/*-------------------------------------------------------------------------
 *
 * planner.h
 *	  prototypes for planner.c.
 *
 * Note that the primary entry points for planner.c are declared in
 * optimizer/optimizer.h, because they're intended to be called from
 * non-planner code.  Declarations here are meant for use by other
 * planner modules.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/planner.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLANNER_H
#define PLANNER_H

#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"


typedef struct ExplainState ExplainState;	/* defined in explain_state.h */

/* Hook for plugins to get control in planner() */
typedef PlannedStmt *(*planner_hook_type) (Query *parse,
										   const char *query_string,
										   int cursorOptions,
										   ParamListInfo boundParams,
										   ExplainState *es);
extern PGDLLIMPORT planner_hook_type planner_hook;

/* Hook for plugins to get control after PlannerGlobal is initialized */
typedef void (*planner_setup_hook_type) (PlannerGlobal *glob, Query *parse,
										 const char *query_string,
										 int cursorOptions,
										 double *tuple_fraction,
										 ExplainState *es);
extern PGDLLIMPORT planner_setup_hook_type planner_setup_hook;

/* Hook for plugins to get control before PlannerGlobal is discarded */
typedef void (*planner_shutdown_hook_type) (PlannerGlobal *glob, Query *parse,
											const char *query_string,
											PlannedStmt *pstmt);
extern PGDLLIMPORT planner_shutdown_hook_type planner_shutdown_hook;

/* Hook for plugins to get control when grouping_planner() plans upper rels */
typedef void (*create_upper_paths_hook_type) (PlannerInfo *root,
											  UpperRelationKind stage,
											  RelOptInfo *input_rel,
											  RelOptInfo *output_rel,
											  void *extra);
extern PGDLLIMPORT create_upper_paths_hook_type create_upper_paths_hook;


extern PlannedStmt *standard_planner(Query *parse, const char *query_string,
									 int cursorOptions,
									 ParamListInfo boundParams,
									 ExplainState *es);

extern PlannerInfo *subquery_planner(PlannerGlobal *glob, Query *parse,
									 char *plan_name,
									 PlannerInfo *parent_root,
									 bool hasRecursion, double tuple_fraction,
									 SetOperationStmt *setops);

extern RowMarkType select_rowmark_type(RangeTblEntry *rte,
									   LockClauseStrength strength);

extern bool limit_needed(Query *parse);

extern void mark_partial_aggref(Aggref *agg, AggSplit aggsplit);

extern Path *get_cheapest_fractional_path(RelOptInfo *rel,
										  double tuple_fraction);

extern Expr *preprocess_phv_expression(PlannerInfo *root, Expr *expr);

extern RelOptInfo *create_unique_paths(PlannerInfo *root, RelOptInfo *rel,
									   SpecialJoinInfo *sjinfo);

extern char *choose_plan_name(PlannerGlobal *glob, const char *name,
							  bool always_number);

#endif							/* PLANNER_H */
