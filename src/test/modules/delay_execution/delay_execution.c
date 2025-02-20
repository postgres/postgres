/*-------------------------------------------------------------------------
 *
 * delay_execution.c
 *		Test module to introduce delay at various points during execution of a
 *		query to test that execution proceeds safely in light of concurrent
 *		changes.
 *
 * The delay is implemented by taking and immediately releasing a specified
 * advisory lock.  If another process has previously taken that lock, the
 * current process will be blocked until the lock is released; otherwise,
 * there's no effect.  This allows an isolationtester script to reliably
 * test behaviors where some specified action happens in another backend in
 * a couple of cases: 1) between parsing and execution of any desired query
 * when using the planner_hook, 2) between RevalidateCachedQuery() and
 * ExecutorStart() when using the ExecutorStart_hook.
 *
 * Copyright (c) 2020-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/test/modules/delay_execution/delay_execution.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "executor/executor.h"
#include "optimizer/planner.h"
#include "utils/fmgrprotos.h"
#include "utils/guc.h"
#include "utils/inval.h"


PG_MODULE_MAGIC;

/* GUC: advisory lock ID to use.  Zero disables the feature. */
static int	post_planning_lock_id = 0;
static int	executor_start_lock_id = 0;

/* Save previous hook users to be a good citizen */
static planner_hook_type prev_planner_hook = NULL;
static ExecutorStart_hook_type prev_ExecutorStart_hook = NULL;


/* planner_hook function to provide the desired delay */
static PlannedStmt *
delay_execution_planner(Query *parse, const char *query_string,
						int cursorOptions, ParamListInfo boundParams)
{
	PlannedStmt *result;

	/* Invoke the planner, possibly via a previous hook user */
	if (prev_planner_hook)
		result = prev_planner_hook(parse, query_string, cursorOptions,
								   boundParams);
	else
		result = standard_planner(parse, query_string, cursorOptions,
								  boundParams);

	/* If enabled, delay by taking and releasing the specified lock */
	if (post_planning_lock_id != 0)
	{
		DirectFunctionCall1(pg_advisory_lock_int8,
							Int64GetDatum((int64) post_planning_lock_id));
		DirectFunctionCall1(pg_advisory_unlock_int8,
							Int64GetDatum((int64) post_planning_lock_id));

		/*
		 * Ensure that we notice any pending invalidations, since the advisory
		 * lock functions don't do this.
		 */
		AcceptInvalidationMessages();
	}

	return result;
}

/* ExecutorStart_hook function to provide the desired delay */
static bool
delay_execution_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	bool		plan_valid;

	/* If enabled, delay by taking and releasing the specified lock */
	if (executor_start_lock_id != 0)
	{
		DirectFunctionCall1(pg_advisory_lock_int8,
							Int64GetDatum((int64) executor_start_lock_id));
		DirectFunctionCall1(pg_advisory_unlock_int8,
							Int64GetDatum((int64) executor_start_lock_id));

		/*
		 * Ensure that we notice any pending invalidations, since the advisory
		 * lock functions don't do this.
		 */
		AcceptInvalidationMessages();
	}

	/* Now start the executor, possibly via a previous hook user */
	if (prev_ExecutorStart_hook)
		plan_valid = prev_ExecutorStart_hook(queryDesc, eflags);
	else
		plan_valid = standard_ExecutorStart(queryDesc, eflags);

	if (executor_start_lock_id != 0)
		elog(NOTICE, "Finished ExecutorStart(): CachedPlan is %s",
			 plan_valid ? "valid" : "not valid");

	return plan_valid;
}

/* Module load function */
void
_PG_init(void)
{
	/* Set up GUCs to control which lock is used */
	DefineCustomIntVariable("delay_execution.post_planning_lock_id",
							"Sets the advisory lock ID to be locked/unlocked after planning.",
							"Zero disables the delay.",
							&post_planning_lock_id,
							0,
							0, INT_MAX,
							PGC_USERSET,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("delay_execution.executor_start_lock_id",
							"Sets the advisory lock ID to be locked/unlocked before starting execution.",
							"Zero disables the delay.",
							&executor_start_lock_id,
							0,
							0, INT_MAX,
							PGC_USERSET,
							0,
							NULL,
							NULL,
							NULL);
	MarkGUCPrefixReserved("delay_execution");

	/* Install our hooks. */
	prev_planner_hook = planner_hook;
	planner_hook = delay_execution_planner;
	prev_ExecutorStart_hook = ExecutorStart_hook;
	ExecutorStart_hook = delay_execution_ExecutorStart;
}
