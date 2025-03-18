/*-------------------------------------------------------------------------
 *
 * auto_explain.c
 *
 *
 * Copyright (c) 2008-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/auto_explain/auto_explain.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/parallel.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "common/pg_prng.h"
#include "executor/instrument.h"
#include "utils/guc.h"

PG_MODULE_MAGIC;

/* GUC variables */
static int	auto_explain_log_min_duration = -1; /* msec or -1 */
static int	auto_explain_log_parameter_max_length = -1; /* bytes or -1 */
static bool auto_explain_log_analyze = false;
static bool auto_explain_log_verbose = false;
static bool auto_explain_log_buffers = false;
static bool auto_explain_log_wal = false;
static bool auto_explain_log_triggers = false;
static bool auto_explain_log_timing = true;
static bool auto_explain_log_settings = false;
static int	auto_explain_log_format = EXPLAIN_FORMAT_TEXT;
static int	auto_explain_log_level = LOG;
static bool auto_explain_log_nested_statements = false;
static double auto_explain_sample_rate = 1;

static const struct config_enum_entry format_options[] = {
	{"text", EXPLAIN_FORMAT_TEXT, false},
	{"xml", EXPLAIN_FORMAT_XML, false},
	{"json", EXPLAIN_FORMAT_JSON, false},
	{"yaml", EXPLAIN_FORMAT_YAML, false},
	{NULL, 0, false}
};

static const struct config_enum_entry loglevel_options[] = {
	{"debug5", DEBUG5, false},
	{"debug4", DEBUG4, false},
	{"debug3", DEBUG3, false},
	{"debug2", DEBUG2, false},
	{"debug1", DEBUG1, false},
	{"debug", DEBUG2, true},
	{"info", INFO, false},
	{"notice", NOTICE, false},
	{"warning", WARNING, false},
	{"log", LOG, false},
	{NULL, 0, false}
};

/* Current nesting depth of ExecutorRun calls */
static int	nesting_level = 0;

/* Is the current top-level query to be sampled? */
static bool current_query_sampled = false;

#define auto_explain_enabled() \
	(auto_explain_log_min_duration >= 0 && \
	 (nesting_level == 0 || auto_explain_log_nested_statements) && \
	 current_query_sampled)

/* Saved hook values */
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;

static bool explain_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void explain_ExecutorRun(QueryDesc *queryDesc,
								ScanDirection direction,
								uint64 count);
static void explain_ExecutorFinish(QueryDesc *queryDesc);
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
							"-1 disables logging plans. 0 means log all plans.",
							&auto_explain_log_min_duration,
							-1,
							-1, INT_MAX,
							PGC_SUSET,
							GUC_UNIT_MS,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("auto_explain.log_parameter_max_length",
							"Sets the maximum length of query parameter values to log.",
							"-1 means log values in full.",
							&auto_explain_log_parameter_max_length,
							-1,
							-1, INT_MAX,
							PGC_SUSET,
							GUC_UNIT_BYTE,
							NULL,
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
							 NULL,
							 NULL);

	DefineCustomBoolVariable("auto_explain.log_settings",
							 "Log modified configuration parameters affecting query planning.",
							 NULL,
							 &auto_explain_log_settings,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
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
							 NULL,
							 NULL);

	DefineCustomBoolVariable("auto_explain.log_buffers",
							 "Log buffers usage.",
							 NULL,
							 &auto_explain_log_buffers,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("auto_explain.log_wal",
							 "Log WAL usage.",
							 NULL,
							 &auto_explain_log_wal,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("auto_explain.log_triggers",
							 "Include trigger statistics in plans.",
							 "This has no effect unless log_analyze is also set.",
							 &auto_explain_log_triggers,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomEnumVariable("auto_explain.log_format",
							 "EXPLAIN format to be used for plan logging.",
							 NULL,
							 &auto_explain_log_format,
							 EXPLAIN_FORMAT_TEXT,
							 format_options,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomEnumVariable("auto_explain.log_level",
							 "Log level for the plan.",
							 NULL,
							 &auto_explain_log_level,
							 LOG,
							 loglevel_options,
							 PGC_SUSET,
							 0,
							 NULL,
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
							 NULL,
							 NULL);

	DefineCustomBoolVariable("auto_explain.log_timing",
							 "Collect timing data, not just row counts.",
							 NULL,
							 &auto_explain_log_timing,
							 true,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomRealVariable("auto_explain.sample_rate",
							 "Fraction of queries to process.",
							 NULL,
							 &auto_explain_sample_rate,
							 1.0,
							 0.0,
							 1.0,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	MarkGUCPrefixReserved("auto_explain");

	/* Install hooks. */
	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = explain_ExecutorStart;
	prev_ExecutorRun = ExecutorRun_hook;
	ExecutorRun_hook = explain_ExecutorRun;
	prev_ExecutorFinish = ExecutorFinish_hook;
	ExecutorFinish_hook = explain_ExecutorFinish;
	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = explain_ExecutorEnd;
}

/*
 * ExecutorStart hook: start up logging if needed
 */
static bool
explain_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	bool		plan_valid;

	/*
	 * At the beginning of each top-level statement, decide whether we'll
	 * sample this statement.  If nested-statement explaining is enabled,
	 * either all nested statements will be explained or none will.
	 *
	 * When in a parallel worker, we should do nothing, which we can implement
	 * cheaply by pretending we decided not to sample the current statement.
	 * If EXPLAIN is active in the parent session, data will be collected and
	 * reported back to the parent, and it's no business of ours to interfere.
	 */
	if (nesting_level == 0)
	{
		if (auto_explain_log_min_duration >= 0 && !IsParallelWorker())
			current_query_sampled = (pg_prng_double(&pg_global_prng_state) < auto_explain_sample_rate);
		else
			current_query_sampled = false;
	}

	if (auto_explain_enabled())
	{
		/* Enable per-node instrumentation iff log_analyze is required. */
		if (auto_explain_log_analyze && (eflags & EXEC_FLAG_EXPLAIN_ONLY) == 0)
		{
			if (auto_explain_log_timing)
				queryDesc->instrument_options |= INSTRUMENT_TIMER;
			else
				queryDesc->instrument_options |= INSTRUMENT_ROWS;
			if (auto_explain_log_buffers)
				queryDesc->instrument_options |= INSTRUMENT_BUFFERS;
			if (auto_explain_log_wal)
				queryDesc->instrument_options |= INSTRUMENT_WAL;
		}
	}

	if (prev_ExecutorStart)
		plan_valid = prev_ExecutorStart(queryDesc, eflags);
	else
		plan_valid = standard_ExecutorStart(queryDesc, eflags);

	/* The plan may have become invalid during standard_ExecutorStart() */
	if (!plan_valid)
		return false;

	if (auto_explain_enabled())
	{
		/*
		 * Set up to track total elapsed time in ExecutorRun.  Make sure the
		 * space is allocated in the per-query context so it will go away at
		 * ExecutorEnd.
		 */
		if (queryDesc->totaltime == NULL)
		{
			MemoryContext oldcxt;

			oldcxt = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);
			queryDesc->totaltime = InstrAlloc(1, INSTRUMENT_ALL, false);
			MemoryContextSwitchTo(oldcxt);
		}
	}

	return true;
}

/*
 * ExecutorRun hook: all we need do is track nesting depth
 */
static void
explain_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction,
					uint64 count)
{
	nesting_level++;
	PG_TRY();
	{
		if (prev_ExecutorRun)
			prev_ExecutorRun(queryDesc, direction, count);
		else
			standard_ExecutorRun(queryDesc, direction, count);
	}
	PG_FINALLY();
	{
		nesting_level--;
	}
	PG_END_TRY();
}

/*
 * ExecutorFinish hook: all we need do is track nesting depth
 */
static void
explain_ExecutorFinish(QueryDesc *queryDesc)
{
	nesting_level++;
	PG_TRY();
	{
		if (prev_ExecutorFinish)
			prev_ExecutorFinish(queryDesc);
		else
			standard_ExecutorFinish(queryDesc);
	}
	PG_FINALLY();
	{
		nesting_level--;
	}
	PG_END_TRY();
}

/*
 * ExecutorEnd hook: log results if needed
 */
static void
explain_ExecutorEnd(QueryDesc *queryDesc)
{
	if (queryDesc->totaltime && auto_explain_enabled())
	{
		MemoryContext oldcxt;
		double		msec;

		/*
		 * Make sure we operate in the per-query context, so any cruft will be
		 * discarded later during ExecutorEnd.
		 */
		oldcxt = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);

		/*
		 * Make sure stats accumulation is done.  (Note: it's okay if several
		 * levels of hook all do this.)
		 */
		InstrEndLoop(queryDesc->totaltime);

		/* Log plan if duration is exceeded. */
		msec = queryDesc->totaltime->total * 1000.0;
		if (msec >= auto_explain_log_min_duration)
		{
			ExplainState *es = NewExplainState();

			es->analyze = (queryDesc->instrument_options && auto_explain_log_analyze);
			es->verbose = auto_explain_log_verbose;
			es->buffers = (es->analyze && auto_explain_log_buffers);
			es->wal = (es->analyze && auto_explain_log_wal);
			es->timing = (es->analyze && auto_explain_log_timing);
			es->summary = es->analyze;
			/* No support for MEMORY */
			/* es->memory = false; */
			es->format = auto_explain_log_format;
			es->settings = auto_explain_log_settings;

			ExplainBeginOutput(es);
			ExplainQueryText(es, queryDesc);
			ExplainQueryParameters(es, queryDesc->params, auto_explain_log_parameter_max_length);
			ExplainPrintPlan(es, queryDesc);
			if (es->analyze && auto_explain_log_triggers)
				ExplainPrintTriggers(es, queryDesc);
			if (es->costs)
				ExplainPrintJITSummary(es, queryDesc);
			ExplainEndOutput(es);

			/* Remove last line break */
			if (es->str->len > 0 && es->str->data[es->str->len - 1] == '\n')
				es->str->data[--es->str->len] = '\0';

			/* Fix JSON to output an object */
			if (auto_explain_log_format == EXPLAIN_FORMAT_JSON)
			{
				es->str->data[0] = '{';
				es->str->data[es->str->len - 1] = '}';
			}

			/*
			 * Note: we rely on the existing logging of context or
			 * debug_query_string to identify just which statement is being
			 * reported.  This isn't ideal but trying to do it here would
			 * often result in duplication.
			 */
			ereport(auto_explain_log_level,
					(errmsg("duration: %.3f ms  plan:\n%s",
							msec, es->str->data),
					 errhidestmt(true)));
		}

		MemoryContextSwitchTo(oldcxt);
	}

	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}
