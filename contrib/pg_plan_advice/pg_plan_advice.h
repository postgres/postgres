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

/*
 * Flags used in plan advice feedback.
 *
 * PGPA_FB_MATCH_PARTIAL means that we found some part of the query that at
 * least partially matched the target; e.g. given JOIN_ORDER(a b), this would
 * be set if we ever saw any joinrel including either "a" or "b".
 *
 * PGPA_FB_MATCH_FULL means that we found an exact match for the target; e.g.
 * given JOIN_ORDER(a b), this would be set if we saw a joinrel containing
 * exactly "a" and "b" and nothing else.
 *
 * PGPA_FB_INAPPLICABLE means that the advice doesn't properly apply to the
 * target; e.g. INDEX_SCAN(foo bar_idx) would be so marked if bar_idx does not
 * exist on foo. The fact that this bit has been set does not mean that the
 * advice had no effect.
 *
 * PGPA_FB_CONFLICTING means that a conflict was detected between what this
 * advice wants and what some other plan advice wants; e.g. JOIN_ORDER(a b)
 * would conflict with HASH_JOIN(a), because the former requires "a" to be the
 * outer table while the latter requires it to be the inner table.
 *
 * PGPA_FB_FAILED means that the resulting plan did not conform to the advice.
 */
#define PGPA_FB_MATCH_PARTIAL		0x0001
#define PGPA_FB_MATCH_FULL			0x0002
#define PGPA_FB_INAPPLICABLE		0x0004
#define PGPA_FB_CONFLICTING			0x0008
#define PGPA_FB_FAILED				0x0010

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
