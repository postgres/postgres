/*-------------------------------------------------------------------------
 *
 * pg_plan_advice.c
 *	  main entrypoints for generating and applying planner advice
 *
 * Copyright (c) 2016-2026, PostgreSQL Global Development Group
 *
 *	  contrib/pg_plan_advice/pg_plan_advice.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "pg_plan_advice.h"
#include "pgpa_ast.h"
#include "pgpa_identifier.h"
#include "pgpa_output.h"
#include "pgpa_planner.h"
#include "pgpa_trove.h"
#include "pgpa_walker.h"

#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "funcapi.h"
#include "optimizer/planner.h"
#include "storage/dsm_registry.h"
#include "utils/guc.h"

PG_MODULE_MAGIC;

/* GUC variables */
char	   *pg_plan_advice_advice = NULL;
bool		pg_plan_advice_always_store_advice_details = false;
static bool pg_plan_advice_always_explain_supplied_advice = true;
bool		pg_plan_advice_feedback_warnings = false;
bool		pg_plan_advice_trace_mask = false;

/* Saved hook value */
static explain_per_plan_hook_type prev_explain_per_plan = NULL;

/* Other file-level globals */
static int	es_extension_id;
static MemoryContext pgpa_memory_context = NULL;
static List *advisor_hook_list = NIL;

static void pg_plan_advice_explain_option_handler(ExplainState *es,
												  DefElem *opt,
												  ParseState *pstate);
static void pg_plan_advice_explain_per_plan_hook(PlannedStmt *plannedstmt,
												 IntoClause *into,
												 ExplainState *es,
												 const char *queryString,
												 ParamListInfo params,
												 QueryEnvironment *queryEnv);
static bool pg_plan_advice_advice_check_hook(char **newval, void **extra,
											 GucSource source);
static DefElem *find_defelem_by_defname(List *deflist, char *defname);

/*
 * Initialize this module.
 */
void
_PG_init(void)
{
	DefineCustomStringVariable("pg_plan_advice.advice",
							   "advice to apply during query planning",
							   NULL,
							   &pg_plan_advice_advice,
							   NULL,
							   PGC_USERSET,
							   0,
							   pg_plan_advice_advice_check_hook,
							   NULL,
							   NULL);

	DefineCustomBoolVariable("pg_plan_advice.always_explain_supplied_advice",
							 "EXPLAIN output includes supplied advice even without EXPLAIN (PLAN_ADVICE)",
							 NULL,
							 &pg_plan_advice_always_explain_supplied_advice,
							 true,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("pg_plan_advice.always_store_advice_details",
							 "Generate advice strings even when seemingly not required",
							 "Use this option to see generated advice for prepared queries.",
							 &pg_plan_advice_always_store_advice_details,
							 false,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("pg_plan_advice.feedback_warnings",
							 "Warn when supplied advice does not apply cleanly",
							 NULL,
							 &pg_plan_advice_feedback_warnings,
							 false,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("pg_plan_advice.trace_mask",
							 "Emit debugging messages showing the computed strategy mask for each relation",
							 NULL,
							 &pg_plan_advice_trace_mask,
							 false,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	MarkGUCPrefixReserved("pg_plan_advice");

	/* Get an ID that we can use to cache data in an ExplainState. */
	es_extension_id = GetExplainExtensionId("pg_plan_advice");

	/* Register the new EXPLAIN options implemented by this module. */
	RegisterExtensionExplainOption("plan_advice",
								   pg_plan_advice_explain_option_handler);

	/* Install hooks */
	pgpa_planner_install_hooks();
	prev_explain_per_plan = explain_per_plan_hook;
	explain_per_plan_hook = pg_plan_advice_explain_per_plan_hook;
}

/*
 * Return a pointer to a memory context where long-lived data managed by this
 * module can be stored.
 */
MemoryContext
pg_plan_advice_get_mcxt(void)
{
	if (pgpa_memory_context == NULL)
		pgpa_memory_context = AllocSetContextCreate(TopMemoryContext,
													"pg_plan_advice",
													ALLOCSET_DEFAULT_SIZES);

	return pgpa_memory_context;
}

/*
 * Was the PLAN_ADVICE option specified and not set to false?
 */
bool
pg_plan_advice_should_explain(ExplainState *es)
{
	bool	   *plan_advice = NULL;

	if (es != NULL)
		plan_advice = GetExplainExtensionState(es, es_extension_id);
	return plan_advice != NULL && *plan_advice;
}

/*
 * Get the advice that should be used while planning a particular query.
 */
char *
pg_plan_advice_get_supplied_query_advice(PlannerGlobal *glob,
										 Query *parse,
										 const char *query_string,
										 int cursorOptions,
										 ExplainState *es)
{
	ListCell   *lc;

	/*
	 * If any advisors are loaded, consult them. The first one that produces a
	 * non-NULL string wins.
	 */
	foreach(lc, advisor_hook_list)
	{
		pg_plan_advice_advisor_hook hook = lfirst(lc);
		char	   *advice_string;

		advice_string = (*hook) (glob, parse, query_string, cursorOptions, es);
		if (advice_string != NULL)
			return advice_string;
	}

	/* Otherwise, just use the value of the GUC. */
	return pg_plan_advice_advice;
}

/*
 * Add an advisor, which can supply advice strings to be used during future
 * query planning operations.
 *
 * The advisor should return NULL if it has no advice string to offer for a
 * given query. If multiple advisors are added, they will be consulted in the
 * order added until one of them returns a non-NULL value.
 */
void
pg_plan_advice_add_advisor(pg_plan_advice_advisor_hook hook)
{
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(pg_plan_advice_get_mcxt());
	advisor_hook_list = lappend(advisor_hook_list, hook);
	MemoryContextSwitchTo(oldcontext);
}

/*
 * Remove an advisor.
 */
void
pg_plan_advice_remove_advisor(pg_plan_advice_advisor_hook hook)
{
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(pg_plan_advice_get_mcxt());
	advisor_hook_list = list_delete_ptr(advisor_hook_list, hook);
	MemoryContextSwitchTo(oldcontext);
}

/*
 * Other loadable modules can use this function to trigger advice generation.
 *
 * Calling this function with activate = true requests that any queries
 * planned afterwards should generate plan advice, which will be stored in the
 * PlannedStmt. Calling this function with activate = false revokes that
 * request. Multiple loadable modules could be using this simultaneously, so
 * make sure to only revoke your own requests.
 *
 * Note that you can't use this function to *suppress* advice generation,
 * which can occur for other reasons, such as the use of EXPLAIN (PLAN_ADVICE),
 * regardless. It's a way of turning advice generation on, not a way of turning
 * it off.
 */
void
pg_plan_advice_request_advice_generation(bool activate)
{
	if (activate)
		pgpa_planner_generate_advice++;
	else
	{
		Assert(pgpa_planner_generate_advice > 0);
		pgpa_planner_generate_advice--;
	}
}

/*
 * Handler for EXPLAIN (PLAN_ADVICE).
 */
static void
pg_plan_advice_explain_option_handler(ExplainState *es, DefElem *opt,
									  ParseState *pstate)
{
	bool	   *plan_advice;

	plan_advice = GetExplainExtensionState(es, es_extension_id);

	if (plan_advice == NULL)
	{
		plan_advice = palloc0_object(bool);
		SetExplainExtensionState(es, es_extension_id, plan_advice);
	}

	*plan_advice = defGetBoolean(opt);
}

/*
 * Display a string that is likely to consist of multiple lines in EXPLAIN
 * output.
 */
static void
pg_plan_advice_explain_text_multiline(ExplainState *es, char *qlabel,
									  char *value)
{
	char	   *s;

	/* For non-text formats, it's best not to add any special handling. */
	if (es->format != EXPLAIN_FORMAT_TEXT)
	{
		ExplainPropertyText(qlabel, value, es);
		return;
	}

	/* In text format, if there is no data, display nothing. */
	if (*value == '\0')
		return;

	/*
	 * It looks nicest to indent each line of the advice separately, beginning
	 * on the line below the label.
	 */
	ExplainIndentText(es);
	appendStringInfo(es->str, "%s:\n", qlabel);
	es->indent++;
	while ((s = strchr(value, '\n')) != NULL)
	{
		ExplainIndentText(es);
		appendBinaryStringInfo(es->str, value, (s - value) + 1);
		value = s + 1;
	}

	/* Don't interpret a terminal newline as a request for an empty line. */
	if (*value != '\0')
	{
		ExplainIndentText(es);
		appendStringInfo(es->str, "%s\n", value);
	}

	es->indent--;
}

/*
 * Add advice feedback to the EXPLAIN output.
 */
static void
pg_plan_advice_explain_feedback(ExplainState *es, List *feedback)
{
	StringInfoData buf;

	initStringInfo(&buf);
	foreach_node(DefElem, item, feedback)
	{
		int			flags = defGetInt32(item);

		appendStringInfo(&buf, "%s /* ", item->defname);
		pgpa_trove_append_flags(&buf, flags);
		appendStringInfo(&buf, " */\n");
	}

	pg_plan_advice_explain_text_multiline(es, "Supplied Plan Advice",
										  buf.data);
}

/*
 * Add relevant details, if any, to the EXPLAIN output for a single plan.
 */
static void
pg_plan_advice_explain_per_plan_hook(PlannedStmt *plannedstmt,
									 IntoClause *into,
									 ExplainState *es,
									 const char *queryString,
									 ParamListInfo params,
									 QueryEnvironment *queryEnv)
{
	bool		should_explain;
	DefElem    *pgpa_item;
	List	   *pgpa_list;

	if (prev_explain_per_plan)
		prev_explain_per_plan(plannedstmt, into, es, queryString, params,
							  queryEnv);

	/* Should an advice string be part of the EXPLAIN output? */
	should_explain = pg_plan_advice_should_explain(es);

	/* Find any data pgpa_planner_shutdown stashed in the PlannedStmt. */
	pgpa_item = find_defelem_by_defname(plannedstmt->extension_state,
										"pg_plan_advice");
	pgpa_list = pgpa_item == NULL ? NULL : (List *) pgpa_item->arg;

	/*
	 * By default, if there is a record of attempting to apply advice during
	 * query planning, we always output that information, but the user can set
	 * pg_plan_advice.always_explain_supplied_advice = false to suppress that
	 * behavior. If they do, we'll only display it when the PLAN_ADVICE option
	 * was specified and not set to false.
	 *
	 * NB: If we're explaining a query planned beforehand -- i.e. a prepared
	 * statement -- the application of query advice may not have been
	 * recorded, and therefore this won't be able to show anything. Use
	 * pg_plan_advice.always_store_advice_details = true to work around this.
	 */
	if (pgpa_list != NULL && (pg_plan_advice_always_explain_supplied_advice ||
							  should_explain))
	{
		DefElem    *feedback;

		feedback = find_defelem_by_defname(pgpa_list, "feedback");
		if (feedback != NULL)
			pg_plan_advice_explain_feedback(es, (List *) feedback->arg);
	}

	/*
	 * If the PLAN_ADVICE option was specified -- and not set to FALSE -- show
	 * generated advice.
	 */
	if (should_explain)
	{
		DefElem    *advice_string_item;
		char	   *advice_string = NULL;

		advice_string_item =
			find_defelem_by_defname(pgpa_list, "advice_string");
		if (advice_string_item != NULL)
		{
			advice_string = strVal(advice_string_item->arg);
			pg_plan_advice_explain_text_multiline(es, "Generated Plan Advice",
												  advice_string);
		}
	}
}

/*
 * Check hook for pg_plan_advice.advice
 */
static bool
pg_plan_advice_advice_check_hook(char **newval, void **extra, GucSource source)
{
	MemoryContext oldcontext;
	MemoryContext tmpcontext;
	char	   *error;

	if (*newval == NULL)
		return true;

	tmpcontext = AllocSetContextCreate(CurrentMemoryContext,
									   "pg_plan_advice.advice",
									   ALLOCSET_DEFAULT_SIZES);
	oldcontext = MemoryContextSwitchTo(tmpcontext);

	/*
	 * It would be nice to save the parse tree that we construct here for
	 * eventual use when planning with this advice, but *extra can only point
	 * to a single guc_malloc'd chunk, and our parse tree involves an
	 * arbitrary number of memory allocations.
	 */
	(void) pgpa_parse(*newval, &error);

	if (error != NULL)
		GUC_check_errdetail("Could not parse advice: %s", error);

	MemoryContextSwitchTo(oldcontext);
	MemoryContextDelete(tmpcontext);

	return (error == NULL);
}

/*
 * Search a list of DefElem objects for a given defname.
 */
static DefElem *
find_defelem_by_defname(List *deflist, char *defname)
{
	foreach_node(DefElem, item, deflist)
	{
		if (strcmp(item->defname, defname) == 0)
			return item;
	}

	return NULL;
}
