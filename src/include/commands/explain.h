/*-------------------------------------------------------------------------
 *
 * explain.h
 *	  prototypes for explain.c
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * src/include/commands/explain.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXPLAIN_H
#define EXPLAIN_H

#include "executor/executor.h"
#include "parser/parse_node.h"

struct ExplainState;			/* defined in explain_state.h */

/* Hook for plugins to get control in ExplainOneQuery() */
typedef void (*ExplainOneQuery_hook_type) (Query *query,
										   int cursorOptions,
										   IntoClause *into,
										   struct ExplainState *es,
										   const char *queryString,
										   ParamListInfo params,
										   QueryEnvironment *queryEnv);
extern PGDLLIMPORT ExplainOneQuery_hook_type ExplainOneQuery_hook;

/* Hook for EXPLAIN plugins to print extra information for each plan */
typedef void (*explain_per_plan_hook_type) (PlannedStmt *plannedstmt,
											IntoClause *into,
											struct ExplainState *es,
											const char *queryString,
											ParamListInfo params,
											QueryEnvironment *queryEnv);
extern PGDLLIMPORT explain_per_plan_hook_type explain_per_plan_hook;

/* Hook for EXPLAIN plugins to print extra fields on individual plan nodes */
typedef void (*explain_per_node_hook_type) (PlanState *planstate,
											List *ancestors,
											const char *relationship,
											const char *plan_name,
											struct ExplainState *es);
extern PGDLLIMPORT explain_per_node_hook_type explain_per_node_hook;

/* Hook for plugins to get control in explain_get_index_name() */
typedef const char *(*explain_get_index_name_hook_type) (Oid indexId);
extern PGDLLIMPORT explain_get_index_name_hook_type explain_get_index_name_hook;


extern void ExplainQuery(ParseState *pstate, ExplainStmt *stmt,
						 ParamListInfo params, DestReceiver *dest);
extern void standard_ExplainOneQuery(Query *query, int cursorOptions,
									 IntoClause *into, struct ExplainState *es,
									 const char *queryString, ParamListInfo params,
									 QueryEnvironment *queryEnv);

extern TupleDesc ExplainResultDesc(ExplainStmt *stmt);

extern void ExplainOneUtility(Node *utilityStmt, IntoClause *into,
							  struct ExplainState *es, ParseState *pstate,
							  ParamListInfo params);

extern void ExplainOnePlan(PlannedStmt *plannedstmt, CachedPlan *cplan,
						   CachedPlanSource *plansource, int plan_index,
						   IntoClause *into, struct ExplainState *es,
						   const char *queryString,
						   ParamListInfo params, QueryEnvironment *queryEnv,
						   const instr_time *planduration,
						   const BufferUsage *bufusage,
						   const MemoryContextCounters *mem_counters);

extern void ExplainPrintPlan(struct ExplainState *es, QueryDesc *queryDesc);
extern void ExplainPrintTriggers(struct ExplainState *es,
								 QueryDesc *queryDesc);

extern void ExplainPrintJITSummary(struct ExplainState *es,
								   QueryDesc *queryDesc);

extern void ExplainQueryText(struct ExplainState *es, QueryDesc *queryDesc);
extern void ExplainQueryParameters(struct ExplainState *es,
								   ParamListInfo params, int maxlen);

#endif							/* EXPLAIN_H */
