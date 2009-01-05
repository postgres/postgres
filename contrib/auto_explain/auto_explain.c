/*-------------------------------------------------------------------------
 *
 * auto_explain.c
 *
 *
 * Copyright (c) 2008-2009, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/contrib/auto_explain/auto_explain.c,v 1.4 2009/01/05 13:35:38 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/explain.h"
#include "executor/instrument.h"
#include "utils/guc.h"

PG_MODULE_MAGIC;

/* GUC variables */
static int	auto_explain_log_min_duration = -1;		/* msec or -1 */
static bool auto_explain_log_analyze = false;
static bool auto_explain_log_verbose = false;
static bool auto_explain_log_nested_statements = false;

/* Current nesting depth of ExecutorRun calls */
static int	nesting_level = 0;

/* Saved hook values in case of unload */
static ExecutorStart_hook_type	prev_ExecutorStart = NULL;
static ExecutorRun_hook_type	prev_ExecutorRun = NULL;
static ExecutorEnd_hook_type	prev_ExecutorEnd = NULL;

#define auto_explain_enabled() \
	(auto_explain_log_min_duration >= 0 && \
	 (nesting_level == 0 || auto_explain_log_nested_statements))

void	_PG_init(void);
void	_PG_fini(void);

static void explain_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void explain_ExecutorRun(QueryDesc *queryDesc,
								ScanDirection direction,
								long count);
static void explain_ExecutorEnd(QueryDesc *queryDesc);


/*
 * Module load callback
 */
void
_PG_init(void)
{
	/* Define custom GUC variables. */
	DefineCustomIntVariable("auto_explain.log_min_duration",
							"Sets the minimum execution time above which plans will be logged.",
							"Zero prints all plans. -1 turns this feature off.",
							&auto_explain_log_min_duration,
							-1,
							-1, INT_MAX / 1000,
							PGC_SUSET,
							GUC_UNIT_MS,
							NULL,
							NULL);

	DefineCustomBoolVariable("auto_explain.log_analyze",
							 "Use EXPLAIN ANALYZE for plan logging.",
							 NULL,
							 &auto_explain_log_analyze,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("auto_explain.log_verbose",
							 "Use EXPLAIN VERBOSE for plan logging.",
							 NULL,
							 &auto_explain_log_verbose,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("auto_explain.log_nested_statements",
							 "Log nested statements.",
							 NULL,
							 &auto_explain_log_nested_statements,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL);

	EmitWarningsOnPlaceholders("auto_explain");

	/* Install hooks. */
	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = explain_ExecutorStart;
	prev_ExecutorRun = ExecutorRun_hook;
	ExecutorRun_hook = explain_ExecutorRun;
	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = explain_ExecutorEnd;
}

/*
 * Module unload callback
 */
void
_PG_fini(void)
{
	/* Uninstall hooks. */
	ExecutorStart_hook = prev_ExecutorStart;
	ExecutorRun_hook = prev_ExecutorRun;
	ExecutorEnd_hook = prev_ExecutorEnd;
}

/*
 * ExecutorStart hook: start up logging if needed
 */
void
explain_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	if (auto_explain_enabled())
	{
		/* Enable per-node instrumentation iff log_analyze is required. */
		if (auto_explain_log_analyze && (eflags & EXEC_FLAG_EXPLAIN_ONLY) == 0)
			queryDesc->doInstrument = true;
	}

	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

	if (auto_explain_enabled())
	{
		/*
		 * Set up to track total elapsed time in ExecutorRun.  Make sure
		 * the space is allocated in the per-query context so it will go
		 * away at ExecutorEnd.
		 */
		if (queryDesc->totaltime == NULL)
		{
			MemoryContext oldcxt;

			oldcxt = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);
			queryDesc->totaltime = InstrAlloc(1);
			MemoryContextSwitchTo(oldcxt);
		}
	}
}

/*
 * ExecutorRun hook: all we need do is track nesting depth
 */
void
explain_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, long count)
{
	nesting_level++;
	PG_TRY();
	{
		if (prev_ExecutorRun)
			prev_ExecutorRun(queryDesc, direction, count);
		else
			standard_ExecutorRun(queryDesc, direction, count);
		nesting_level--;
	}
	PG_CATCH();
	{
		nesting_level--;
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * ExecutorEnd hook: log results if needed
 */
void
explain_ExecutorEnd(QueryDesc *queryDesc)
{
	if (queryDesc->totaltime && auto_explain_enabled())
	{
		double	msec;

		/*
		 * Make sure stats accumulation is done.  (Note: it's okay if
		 * several levels of hook all do this.)
		 */
		InstrEndLoop(queryDesc->totaltime);

		/* Log plan if duration is exceeded. */
		msec = queryDesc->totaltime->total * 1000.0;
		if (msec >= auto_explain_log_min_duration)
		{
			StringInfoData	buf;

			initStringInfo(&buf);
			ExplainPrintPlan(&buf, queryDesc,
							 queryDesc->doInstrument && auto_explain_log_analyze,
							 auto_explain_log_verbose);

			/* Remove last line break */
			if (buf.len > 0 && buf.data[buf.len - 1] == '\n')
				buf.data[--buf.len] = '\0';

			/*
			 * Note: we rely on the existing logging of context or
			 * debug_query_string to identify just which statement is being
			 * reported.  This isn't ideal but trying to do it here would
			 * often result in duplication.
			 */
			ereport(LOG,
					(errmsg("duration: %.3f ms  plan:\n%s",
							msec, buf.data)));

			pfree(buf.data);
		}
	}

	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}
