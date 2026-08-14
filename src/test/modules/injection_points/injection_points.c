/*--------------------------------------------------------------------------
 *
 * injection_points.c
 *		Code for testing injection points.
 *
 * Injection points are able to trigger user-defined callbacks in pre-defined
 * code paths.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/test/modules/injection_points/injection_points.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"
#include "funcapi.h"
#include "injection_points.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "nodes/value.h"
#include "storage/dsm_registry.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"
#include "utils/tuplestore.h"
#include "utils/wait_event.h"

PG_MODULE_MAGIC;

/* Maximum number of waits usable in injection points at once */
#define INJ_MAX_WAIT	8
#define INJ_NAME_MAXLEN	64

/* Thresholds of waits */
#define INJ_WAIT_INITIAL_US		10	/* 10us */
#define INJ_WAIT_MAX_US			100000	/* 100ms */

/*
 * List of injection points stored in TopMemoryContext attached
 * locally to this process.
 */
static List *inj_list_local = NIL;

/*
 * Shared state information for injection points.
 *
 * This state data can be initialized in two ways: dynamically with a DSM
 * or when loading the module.
 */
typedef struct InjectionPointSharedState
{
	/* Protects access to other fields */
	slock_t		lock;

	/* Counters advancing when injection_points_wakeup() is called */
	pg_atomic_uint32 wait_counts[INJ_MAX_WAIT];

	/* Names of injection points attached to wait counters */
	char		name[INJ_MAX_WAIT][INJ_NAME_MAXLEN];
} InjectionPointSharedState;

/* Pointer to shared-memory state. */
static InjectionPointSharedState *inj_state = NULL;

extern PGDLLEXPORT void injection_error(const char *name,
										const void *private_data,
										void *arg);
extern PGDLLEXPORT void injection_notice(const char *name,
										 const void *private_data,
										 void *arg);
extern PGDLLEXPORT void injection_wait(const char *name,
									   const void *private_data,
									   void *arg);

/* track if injection points attached in this process are linked to it */
static bool injection_point_local = false;

static void injection_shmem_request(void *arg);
static void injection_shmem_init(void *arg);

static const ShmemCallbacks injection_shmem_callbacks = {
	.request_fn = injection_shmem_request,
	.init_fn = injection_shmem_init,
};

/*
 * Routine for shared memory area initialization, used as a callback
 * when initializing dynamically with a DSM or when loading the module.
 */
static void
injection_point_init_state(void *ptr, void *arg)
{
	InjectionPointSharedState *state = (InjectionPointSharedState *) ptr;

	SpinLockInit(&state->lock);
	memset(state->name, 0, sizeof(state->name));
	for (int i = 0; i < INJ_MAX_WAIT; i++)
		pg_atomic_init_u32(&state->wait_counts[i], 0);
}

static void
injection_shmem_request(void *arg)
{
	ShmemRequestStruct(.name = "injection_points",
					   .size = sizeof(InjectionPointSharedState),
					   .ptr = (void **) &inj_state,
		);
}

static void
injection_shmem_init(void *arg)
{
	/*
	 * First time through, so initialize.  This is shared with the dynamic
	 * initialization using a DSM.
	 */
	injection_point_init_state(inj_state, NULL);
}

/*
 * Initialize shared memory area for this module through DSM.
 */
static void
injection_init_shmem(void)
{
	bool		found;

	if (inj_state != NULL)
		return;

	inj_state = GetNamedDSMSegment("injection_points",
								   sizeof(InjectionPointSharedState),
								   injection_point_init_state,
								   &found, NULL);
}

/*
 * Check runtime conditions associated to an injection point.
 *
 * Returns true if the named injection point is allowed to run, and false
 * otherwise.
 */
static bool
injection_point_allowed(const InjectionPointCondition *condition,
						const char *arg)
{
	/* Does not match the condition PID? */
	if ((condition->type & INJ_CONDITION_PID) &&
		MyProcPid != condition->pid)
		return false;

	/* Does not match the condition string? */
	if ((condition->type & INJ_CONDITION_STRING) &&
		(arg == NULL || strcmp(condition->str, arg) != 0))
		return false;

	return true;
}

/*
 * before_shmem_exit callback to remove injection points linked to a
 * specific process.
 */
static void
injection_points_cleanup(int code, Datum arg)
{
	ListCell   *lc;

	/* Leave if nothing is tracked locally */
	if (!injection_point_local)
		return;

	/* Detach all the local points */
	foreach(lc, inj_list_local)
	{
		char	   *name = strVal(lfirst(lc));

		(void) InjectionPointDetach(name);
	}
}

/* Set of callbacks available to be attached to an injection point. */
void
injection_error(const char *name, const void *private_data, void *arg)
{
	const InjectionPointCondition *condition = private_data;
	char	   *argstr = arg;

	if (!injection_point_allowed(condition, argstr))
		return;

	if (argstr)
		elog(ERROR, "error triggered for injection point %s (%s)",
			 name, argstr);
	else
		elog(ERROR, "error triggered for injection point %s", name);
}

void
injection_notice(const char *name, const void *private_data, void *arg)
{
	const InjectionPointCondition *condition = private_data;
	char	   *argstr = arg;

	if (!injection_point_allowed(condition, argstr))
		return;

	if (argstr)
		elog(NOTICE, "notice triggered for injection point %s (%s)",
			 name, argstr);
	else
		elog(NOTICE, "notice triggered for injection point %s", name);
}

/*
 * Error cleanup callback for injection point waits.
 */
static void
injection_wait_cleanup(int code, Datum arg)
{
	int			index = DatumGetInt32(arg);

	SpinLockAcquire(&inj_state->lock);
	inj_state->name[index][0] = '\0';
	SpinLockRelease(&inj_state->lock);
}

/* Wait until injection_points_wakeup() is called */
void
injection_wait(const char *name, const void *private_data, void *arg)
{
	uint32		old_wait_counts = 0;
	int			index = -1;
	uint32		injection_wait_event = 0;
	const InjectionPointCondition *condition = private_data;
	char	   *argstr = arg;
	int			delay_us = 0;

	if (inj_state == NULL)
		injection_init_shmem();

	if (!injection_point_allowed(condition, argstr))
		return;

	/*
	 * Use the injection point name for this custom wait event.  Note that
	 * this custom wait event name is not released, but we don't care much for
	 * testing as this should be short-lived.
	 */
	injection_wait_event = WaitEventInjectionPointNew(name);

	/*
	 * Find a free slot to wait for, and register this injection point's name.
	 */
	SpinLockAcquire(&inj_state->lock);
	for (int i = 0; i < INJ_MAX_WAIT; i++)
	{
		if (inj_state->name[i][0] == '\0')
		{
			index = i;
			strlcpy(inj_state->name[i], name, INJ_NAME_MAXLEN);
			old_wait_counts = pg_atomic_read_u32(&inj_state->wait_counts[i]);
			break;
		}
	}
	SpinLockRelease(&inj_state->lock);

	if (index < 0)
		elog(ERROR, "could not find free slot for wait of injection point %s",
			 name);

	/*
	 * Wait until the counter is bumped by injection_points_wakeup().
	 *
	 * This loop starts with a short delay for responsiveness, enlarged to
	 * ease the CPU workload in slower environments.
	 */
	delay_us = INJ_WAIT_INITIAL_US;

	pgstat_report_wait_start(injection_wait_event);
	PG_ENSURE_ERROR_CLEANUP(injection_wait_cleanup, Int32GetDatum(index));
	{
		while (pg_atomic_read_u32(&inj_state->wait_counts[index]) == old_wait_counts)
		{
			CHECK_FOR_INTERRUPTS();
			pg_usleep(delay_us);
			if (delay_us < INJ_WAIT_MAX_US)
				delay_us = Min(delay_us * 2, INJ_WAIT_MAX_US);
		}
	}
	PG_END_ENSURE_ERROR_CLEANUP(injection_wait_cleanup, Int32GetDatum(index));
	pgstat_report_wait_end();

	/* Remove this injection point from the waiters. */
	injection_wait_cleanup(0, Int32GetDatum(index));
}

/*
 * SQL function for creating an injection point.
 */
PG_FUNCTION_INFO_V1(injection_points_attach);
Datum
injection_points_attach(PG_FUNCTION_ARGS)
{
	char	   *name;
	char	   *action;
	char	   *str;
	char	   *function;
	InjectionPointCondition condition = {0};

	if (PG_ARGISNULL(0))
		elog(ERROR, "injection point name must not be null");

	if (PG_ARGISNULL(1))
		elog(ERROR, "injection point action must not be null");

	name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	action = text_to_cstring(PG_GETARG_TEXT_PP(1));
	str = PG_ARGISNULL(2) ? NULL
		: text_to_cstring(PG_GETARG_TEXT_PP(2));

	if (strcmp(action, "error") == 0)
		function = "injection_error";
	else if (strcmp(action, "notice") == 0)
		function = "injection_notice";
	else if (strcmp(action, "wait") == 0)
		function = "injection_wait";
	else
		elog(ERROR, "incorrect action \"%s\" for injection point creation", action);

	if (injection_point_local)
	{
		condition.type |= INJ_CONDITION_PID;
		condition.pid = MyProcPid;
	}

	if (str)
	{
		if (str[0] == '\0')
			elog(ERROR, "injection point condition string must not be empty");
		if (strlen(str) >= INJ_DATA_MAXLEN)
			elog(ERROR, "injection point condition string too long (maximum of %d characters)",
				 INJ_DATA_MAXLEN - 1);
		condition.type |= INJ_CONDITION_STRING;
		strlcpy(condition.str, str, INJ_DATA_MAXLEN);
	}

	InjectionPointAttach(name, "injection_points", function, &condition,
						 sizeof(InjectionPointCondition));

	if (injection_point_local)
	{
		MemoryContext oldctx;

		/* Local injection point, so track it for automated cleanup */
		oldctx = MemoryContextSwitchTo(TopMemoryContext);
		inj_list_local = lappend(inj_list_local, makeString(pstrdup(name)));
		MemoryContextSwitchTo(oldctx);
	}

	PG_RETURN_VOID();
}

/*
 * SQL function for creating an injection point with library name, function
 * name and private data.
 */
PG_FUNCTION_INFO_V1(injection_points_attach_func);
Datum
injection_points_attach_func(PG_FUNCTION_ARGS)
{
	char	   *name;
	char	   *lib_name;
	char	   *function;
	bytea	   *private_data = NULL;
	int			private_data_size = 0;

	if (PG_ARGISNULL(0))
		elog(ERROR, "injection point name cannot be NULL");
	if (PG_ARGISNULL(1))
		elog(ERROR, "injection point library cannot be NULL");
	if (PG_ARGISNULL(2))
		elog(ERROR, "injection point function cannot be NULL");

	name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	lib_name = text_to_cstring(PG_GETARG_TEXT_PP(1));
	function = text_to_cstring(PG_GETARG_TEXT_PP(2));

	if (!PG_ARGISNULL(3))
	{
		private_data = PG_GETARG_BYTEA_PP(3);
		private_data_size = VARSIZE_ANY_EXHDR(private_data);
	}

	if (private_data != NULL)
		InjectionPointAttach(name, lib_name, function, VARDATA_ANY(private_data),
							 private_data_size);
	else
		InjectionPointAttach(name, lib_name, function, NULL,
							 0);
	PG_RETURN_VOID();
}

/*
 * SQL function for loading an injection point.
 */
PG_FUNCTION_INFO_V1(injection_points_load);
Datum
injection_points_load(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));

	if (inj_state == NULL)
		injection_init_shmem();

	INJECTION_POINT_LOAD(name);

	PG_RETURN_VOID();
}

/*
 * SQL function for triggering an injection point.
 */
PG_FUNCTION_INFO_V1(injection_points_run);
Datum
injection_points_run(PG_FUNCTION_ARGS)
{
	char	   *name;
	char	   *arg = NULL;

	if (PG_ARGISNULL(0))
		PG_RETURN_VOID();
	name = text_to_cstring(PG_GETARG_TEXT_PP(0));

	if (!PG_ARGISNULL(1))
		arg = text_to_cstring(PG_GETARG_TEXT_PP(1));

	INJECTION_POINT(name, arg);

	PG_RETURN_VOID();
}

/*
 * SQL function for triggering an injection point from cache.
 */
PG_FUNCTION_INFO_V1(injection_points_cached);
Datum
injection_points_cached(PG_FUNCTION_ARGS)
{
	char	   *name;
	char	   *arg = NULL;

	if (PG_ARGISNULL(0))
		PG_RETURN_VOID();
	name = text_to_cstring(PG_GETARG_TEXT_PP(0));

	if (!PG_ARGISNULL(1))
		arg = text_to_cstring(PG_GETARG_TEXT_PP(1));

	INJECTION_POINT_CACHED(name, arg);

	PG_RETURN_VOID();
}

/*
 * SQL function for waking up an injection point waiting in injection_wait().
 */
PG_FUNCTION_INFO_V1(injection_points_wakeup);
Datum
injection_points_wakeup(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int			index = -1;

	if (inj_state == NULL)
		injection_init_shmem();

	/* Find the injection point then bump its wait counter */
	SpinLockAcquire(&inj_state->lock);
	for (int i = 0; i < INJ_MAX_WAIT; i++)
	{
		if (strcmp(name, inj_state->name[i]) == 0)
		{
			index = i;
			break;
		}
	}
	if (index < 0)
	{
		SpinLockRelease(&inj_state->lock);
		elog(ERROR, "could not find injection point %s to wake up", name);
	}
	SpinLockRelease(&inj_state->lock);

	pg_atomic_fetch_add_u32(&inj_state->wait_counts[index], 1);
	PG_RETURN_VOID();
}

/*
 * injection_points_set_local
 *
 * Track if any injection point created in this process ought to run only
 * in this process.  Such injection points are detached automatically when
 * this process exits.  This is useful to make test suites concurrent-safe.
 */
PG_FUNCTION_INFO_V1(injection_points_set_local);
Datum
injection_points_set_local(PG_FUNCTION_ARGS)
{
	/* Enable flag to add a runtime condition based on this process ID */
	injection_point_local = true;

	if (inj_state == NULL)
		injection_init_shmem();

	/*
	 * Register a before_shmem_exit callback to remove any injection points
	 * linked to this process.
	 */
	before_shmem_exit(injection_points_cleanup, (Datum) 0);

	PG_RETURN_VOID();
}

/*
 * SQL function for dropping an injection point.
 */
PG_FUNCTION_INFO_V1(injection_points_detach);
Datum
injection_points_detach(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));

	if (!InjectionPointDetach(name))
		elog(ERROR, "could not detach injection point \"%s\"", name);

	/* Remove point from local list, if required */
	if (inj_list_local != NIL)
	{
		MemoryContext oldctx;

		oldctx = MemoryContextSwitchTo(TopMemoryContext);
		inj_list_local = list_delete(inj_list_local, makeString(name));
		MemoryContextSwitchTo(oldctx);
	}

	PG_RETURN_VOID();
}

/*
 * SQL function for listing all the injection points attached.
 */
PG_FUNCTION_INFO_V1(injection_points_list);
Datum
injection_points_list(PG_FUNCTION_ARGS)
{
#define NUM_INJECTION_POINTS_LIST 3
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	List	   *inj_points;
	ListCell   *lc;

	/* Build a tuplestore to return our results in */
	InitMaterializedSRF(fcinfo, 0);

	inj_points = InjectionPointList();

	foreach(lc, inj_points)
	{
		Datum		values[NUM_INJECTION_POINTS_LIST];
		bool		nulls[NUM_INJECTION_POINTS_LIST];
		InjectionPointData *inj_point = lfirst(lc);

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		values[0] = PointerGetDatum(cstring_to_text(inj_point->name));
		values[1] = PointerGetDatum(cstring_to_text(inj_point->library));
		values[2] = PointerGetDatum(cstring_to_text(inj_point->function));

		/* shove row into tuplestore */
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	return (Datum) 0;
#undef NUM_INJECTION_POINTS_LIST
}

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		return;

	RegisterShmemCallbacks(&injection_shmem_callbacks);
}
