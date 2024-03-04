/*--------------------------------------------------------------------------
 *
 * injection_points.c
 *		Code for testing injection points.
 *
 * Injection points are able to trigger user-defined callbacks in pre-defined
 * code paths.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/test/modules/injection_points/injection_points.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"
#include "storage/condition_variable.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/dsm_registry.h"
#include "utils/builtins.h"
#include "utils/injection_point.h"
#include "utils/wait_event.h"

PG_MODULE_MAGIC;

/* Maximum number of waits usable in injection points at once */
#define INJ_MAX_WAIT	8
#define INJ_NAME_MAXLEN	64

/* Shared state information for injection points. */
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

extern PGDLLEXPORT void injection_error(const char *name);
extern PGDLLEXPORT void injection_notice(const char *name);
extern PGDLLEXPORT void injection_wait(const char *name);


/*
 * Callback for shared memory area initialization.
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

/*
 * Initialize shared memory area for this module.
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

/* Set of callbacks available to be attached to an injection point. */
void
injection_error(const char *name)
{
	elog(ERROR, "error triggered for injection point %s", name);
}

void
injection_notice(const char *name)
{
	elog(NOTICE, "notice triggered for injection point %s", name);
}

/* Wait on a condition variable, awaken by injection_points_wakeup() */
void
injection_wait(const char *name)
{
	uint32		old_wait_counts = 0;
	int			index = -1;
	uint32		injection_wait_event = 0;

	if (inj_state == NULL)
		injection_init_shmem();

	/*
	 * Use the injection point name for this custom wait event.  Note that
	 * this custom wait event name is not released, but we don't care much for
	 * testing as this should be short-lived.
	 */
	injection_wait_event = WaitEventExtensionNew(name);

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

	if (strcmp(action, "error") == 0)
		function = "injection_error";
	else if (strcmp(action, "notice") == 0)
		function = "injection_notice";
	else if (strcmp(action, "wait") == 0)
		function = "injection_wait";
	else
		elog(ERROR, "incorrect action \"%s\" for injection point creation", action);

	InjectionPointAttach(name, "injection_points", function);

	PG_RETURN_VOID();
}

/*
 * SQL function for triggering an injection point.
 */
PG_FUNCTION_INFO_V1(injection_points_run);
Datum
injection_points_run(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));

	INJECTION_POINT(name);

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
 * SQL function for dropping an injection point.
 */
PG_FUNCTION_INFO_V1(injection_points_detach);
Datum
injection_points_detach(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));

	InjectionPointDetach(name);

	PG_RETURN_VOID();
}
