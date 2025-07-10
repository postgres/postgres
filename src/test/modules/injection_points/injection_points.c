/*--------------------------------------------------------------------------
 *
 * injection_points.c
 *		Code for testing injection points.
 *
 * Injection points are able to trigger user-defined callbacks in pre-defined
 * code paths.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
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
#include "injection_stats.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "nodes/value.h"
#include "storage/condition_variable.h"
#include "storage/dsm_registry.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"
#include "utils/wait_event.h"

PG_MODULE_MAGIC;

/* Maximum number of waits usable in injection points at once */
#define INJ_MAX_WAIT	8
#define INJ_NAME_MAXLEN	64

/*
 * Conditions related to injection points.  This tracks in shared memory the
 * runtime conditions under which an injection point is allowed to run,
 * stored as private_data when an injection point is attached, and passed as
 * argument to the callback.
 *
 * If more types of runtime conditions need to be tracked, this structure
 * should be expanded.
 */
typedef enum InjectionPointConditionType
{
	INJ_CONDITION_ALWAYS = 0,	/* always run */
	INJ_CONDITION_PID,			/* PID restriction */
} InjectionPointConditionType;

typedef struct InjectionPointCondition
{
	/* Type of the condition */
	InjectionPointConditionType type;

	/* ID of the process where the injection point is allowed to run */
	int			pid;
} InjectionPointCondition;

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
	uint32		wait_counts[INJ_MAX_WAIT];

	/* Names of injection points attached to wait counters */
	char		name[INJ_MAX_WAIT][INJ_NAME_MAXLEN];

	/* Condition variable used for waits and wakeups */
	ConditionVariable wait_point;
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

/*
 * GUC variable
 *
 * This GUC is useful to control if statistics should be enabled or not
 * during a test with injection points, like for example if a test relies
 * on a callback run in a critical section where no allocation should happen.
 */
bool		inj_stats_enabled = false;

/* Shared memory init callbacks */
static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

/*
 * Routine for shared memory area initialization, used as a callback
 * when initializing dynamically with a DSM or when loading the module.
 */
static void
injection_point_init_state(void *ptr)
{
	InjectionPointSharedState *state = (InjectionPointSharedState *) ptr;

	SpinLockInit(&state->lock);
	memset(state->wait_counts, 0, sizeof(state->wait_counts));
	memset(state->name, 0, sizeof(state->name));
	ConditionVariableInit(&state->wait_point);
}

/* Shared memory initialization when loading module */
static void
injection_shmem_request(void)
{
	Size		size;

	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	size = MAXALIGN(sizeof(InjectionPointSharedState));
	RequestAddinShmemSpace(size);
}

static void
injection_shmem_startup(void)
{
	bool		found;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	/* Create or attach to the shared memory state */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	inj_state = ShmemInitStruct("injection_points",
								sizeof(InjectionPointSharedState),
								&found);

	if (!found)
	{
		/*
		 * First time through, so initialize.  This is shared with the dynamic
		 * initialization using a DSM.
		 */
		injection_point_init_state(inj_state);
	}

	LWLockRelease(AddinShmemInitLock);
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
								   &found);
}

/*
 * Check runtime conditions associated to an injection point.
 *
 * Returns true if the named injection point is allowed to run, and false
 * otherwise.
 */
static bool
injection_point_allowed(InjectionPointCondition *condition)
{
	bool		result = true;

	switch (condition->type)
	{
		case INJ_CONDITION_PID:
			if (MyProcPid != condition->pid)
				result = false;
			break;
		case INJ_CONDITION_ALWAYS:
			break;
	}

	return result;
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

		/* Remove stats entry */
		pgstat_drop_inj(name);
	}
}

/* Set of callbacks available to be attached to an injection point. */
void
injection_error(const char *name, const void *private_data, void *arg)
{
	InjectionPointCondition *condition = (InjectionPointCondition *) private_data;
	char	   *argstr = (char *) arg;

	if (!injection_point_allowed(condition))
		return;

	pgstat_report_inj(name);

	if (argstr)
		elog(ERROR, "error triggered for injection point %s (%s)",
			 name, argstr);
	else
		elog(ERROR, "error triggered for injection point %s", name);
}

void
injection_notice(const char *name, const void *private_data, void *arg)
{
	InjectionPointCondition *condition = (InjectionPointCondition *) private_data;
	char	   *argstr = (char *) arg;

	if (!injection_point_allowed(condition))
		return;

	pgstat_report_inj(name);

	if (argstr)
		elog(NOTICE, "notice triggered for injection point %s (%s)",
			 name, argstr);
	else
		elog(NOTICE, "notice triggered for injection point %s", name);
}

/* Wait on a condition variable, awaken by injection_points_wakeup() */
void
injection_wait(const char *name, const void *private_data, void *arg)
{
	uint32		old_wait_counts = 0;
	int			index = -1;
	uint32		injection_wait_event = 0;
	InjectionPointCondition *condition = (InjectionPointCondition *) private_data;

	if (inj_state == NULL)
		injection_init_shmem();

	if (!injection_point_allowed(condition))
		return;

	pgstat_report_inj(name);

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
			old_wait_counts = inj_state->wait_counts[i];
			break;
		}
	}
	SpinLockRelease(&inj_state->lock);

	if (index < 0)
		elog(ERROR, "could not find free slot for wait of injection point %s ",
			 name);

	/* And sleep.. */
	ConditionVariablePrepareToSleep(&inj_state->wait_point);
	for (;;)
	{
		uint32		new_wait_counts;

		SpinLockAcquire(&inj_state->lock);
		new_wait_counts = inj_state->wait_counts[index];
		SpinLockRelease(&inj_state->lock);

		if (old_wait_counts != new_wait_counts)
			break;
		ConditionVariableSleep(&inj_state->wait_point, injection_wait_event);
	}
	ConditionVariableCancelSleep();

	/* Remove this injection point from the waiters. */
	SpinLockAcquire(&inj_state->lock);
	inj_state->name[index][0] = '\0';
	SpinLockRelease(&inj_state->lock);
}

/*
 * SQL function for creating an injection point.
 */
PG_FUNCTION_INFO_V1(injection_points_attach);
Datum
injection_points_attach(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *action = text_to_cstring(PG_GETARG_TEXT_PP(1));
	char	   *function;
	InjectionPointCondition condition = {0};

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
		condition.type = INJ_CONDITION_PID;
		condition.pid = MyProcPid;
	}

	pgstat_report_inj_fixed(1, 0, 0, 0, 0);
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

	/* Add entry for stats */
	pgstat_create_inj(name);

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

	pgstat_report_inj_fixed(0, 0, 0, 0, 1);
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

	pgstat_report_inj_fixed(0, 0, 1, 0, 0);
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

	pgstat_report_inj_fixed(0, 0, 0, 1, 0);
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

	/* First bump the wait counter for the injection point to wake up */
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
	inj_state->wait_counts[index]++;
	SpinLockRelease(&inj_state->lock);

	/* And broadcast the change to the waiters */
	ConditionVariableBroadcast(&inj_state->wait_point);
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

	pgstat_report_inj_fixed(0, 1, 0, 0, 0);
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

	/* Remove stats entry */
	pgstat_drop_inj(name);

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

	DefineCustomBoolVariable("injection_points.stats",
							 "Enables statistics for injection points.",
							 NULL,
							 &inj_stats_enabled,
							 false,
							 PGC_POSTMASTER,
							 0,
							 NULL,
							 NULL,
							 NULL);

	MarkGUCPrefixReserved("injection_points");

	/* Shared memory initialization */
	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = injection_shmem_request;
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = injection_shmem_startup;

	pgstat_register_inj();
	pgstat_register_inj_fixed();
}
