/*-------------------------------------------------------------------------
 *
 * pg_plan_advice.h
 *	  main header file for pg_plan_advice contrib module
 *
 * Copyright (c) 2016-2026, PostgreSQL Global Development Group
 *
 *	  contrib/pg_plan_advice/pg_plan_advice.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_PLAN_ADVICE_H
#define PG_PLAN_ADVICE_H

#include "commands/explain_state.h"
#include "nodes/pathnodes.h"

/* Hook for other plugins to supply advice strings */
typedef char *(*pg_plan_advice_advisor_hook) (PlannerGlobal *glob,
											  Query *parse,
											  const char *query_string,
											  int cursorOptions,
											  ExplainState *es);

/* GUC variables */
extern char *pg_plan_advice_advice;
extern bool pg_plan_advice_always_store_advice_details;
extern bool pg_plan_advice_feedback_warnings;
extern bool pg_plan_advice_trace_mask;

/* Function prototypes (for use by pg_plan_advice itself) */
extern MemoryContext pg_plan_advice_get_mcxt(void);
extern bool pg_plan_advice_should_explain(ExplainState *es);
extern char *pg_plan_advice_get_supplied_query_advice(PlannerGlobal *glob,
													  Query *parse,
													  const char *query_string,
													  int cursorOptions,
													  ExplainState *es);

/* Function prototypes (for use by other plugins) */
extern PGDLLEXPORT void pg_plan_advice_add_advisor(pg_plan_advice_advisor_hook hook);
extern PGDLLEXPORT void pg_plan_advice_remove_advisor(pg_plan_advice_advisor_hook hook);
extern PGDLLEXPORT void pg_plan_advice_request_advice_generation(bool activate);

#endif
