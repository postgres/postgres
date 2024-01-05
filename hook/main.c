//
// Created by hexiang on 6/12/23.
//
#include "postgres.h"
#include "executor/executor.h"
#include "cc.h"
#include "optimizer/planner.h"
#include "access/xact.h"

PG_MODULE_MAGIC;
void _PG_init(void);
void _PG_fini(void);

static void cc_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void cc_ExecutorEnd(QueryDesc *queryDesc);


static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;

void _PG_init(void) {
    prev_ExecutorStart = ExecutorStart_hook;
    prev_ExecutorEnd = ExecutorEnd_hook;
    ExecutorStart_hook = cc_ExecutorStart;
    prev_ExecutorRun = ExecutorRun_hook;
}

void _PG_fini(void) {
    ExecutorStart_hook = prev_ExecutorStart;
    ExecutorEnd_hook = prev_ExecutorEnd;
}

static void cc_ExecutorStart(QueryDesc *queryDesc, int eflags) {
    if (prev_ExecutorStart)
        prev_ExecutorStart(queryDesc, eflags);
    else
        standard_ExecutorStart(queryDesc, eflags);
}

// This will store the previous planner hook, so that we can restore it on unload
// or forward calls to it if it is set.
static planner_hook_type prev_planner_hook = NULL;


/*
 * Planner hook: if planner_hook has already been overridden, forward to the
 * overriding planner (which we saved to prev_planner_hook in _PG_init).
 * If planner_hook has not been overriden (e.g.: is 0), then forward to the regular planner.
 */
static PlannedStmt *
pg_minimal_planner(Query *parse, int cursorOptions, ParamListInfo boundParams)
{
    PlannedStmt *result;

    // Example of sending a notice to the client
    elog(NOTICE, "Running hook pg_minimal_planner");

    // WARNING this simple example doesn't handle all special cases (e.g.: nesting),
    // See contrib/pg_stat_statements in the postgresql codebase for an example of how to handle
    // some special cases.
    if (prev_planner_hook)
    {
        result = prev_planner_hook(parse, cursorOptions, boundParams);
    }
    else
    {
        result = standard_planner(parse, cursorOptions, boundParams);
    }

    return result;
}


static void cc_ExecutorRun(QueryDesc *queryDesc,
                           ScanDirection direction, uint64 count, bool execute_once) {
    if (prev_ExecutorRun)
        prev_ExecutorRun(queryDesc, direction, count, execute_once);
    else
        standard_ExecutorRun(queryDesc, direction, count, execute_once);

    if (default_cc_alg == NO_WAIT_2PL) {
    } else {

    }

    if (queryDesc->operation == CMD_SELECT) {
        exit(0);
    } else {

    }
}

static void cc_ExecutorEnd(QueryDesc *queryDesc) {
    if (prev_ExecutorEnd)
        prev_ExecutorEnd(queryDesc);
    else
        standard_ExecutorEnd(queryDesc);
}