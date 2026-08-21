/*-------------------------------------------------------------------------
 *
 * clauses.h
 *	  prototypes for clauses.c.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/clauses.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CLAUSES_H
#define CLAUSES_H

#include "nodes/pathnodes.h"

typedef struct
{
	int			numWindowFuncs; /* total number of WindowFuncs found */
	Index		maxWinRef;		/* windowFuncs[] is indexed 0 .. maxWinRef */
	List	  **windowFuncs;	/* lists of WindowFuncs for each winref */
} WindowFuncLists;

/*
 * Callback used by expression_has_grouping_conflict below.  Given a Var, the
 * callback returns the equality operator that the relevant grouping mechanism
 * (GROUP BY, DISTINCT, DISTINCT ON, window PARTITION BY, or set operation)
 * uses for the column the Var references, or InvalidOid if the Var does not
 * participate in that grouping.  Returning InvalidOid signals "not a grouping
 * column" to both the opfamily and collation checks.
 */
typedef Oid (*grouping_eqop_callback) (Var *var, void *context);

extern bool contain_agg_clause(Node *clause);

extern bool contain_window_function(Node *clause);
extern WindowFuncLists *find_window_functions(Node *clause, Index maxWinRef);

extern double expression_returns_set_rows(PlannerInfo *root, Node *clause);

extern bool contain_subplans(Node *clause);

extern char max_parallel_hazard(Query *parse);
extern bool is_parallel_safe(PlannerInfo *root, Node *node);
extern bool contain_nonstrict_functions(Node *clause);
extern bool contain_exec_param(Node *clause, List *param_ids);
extern bool contain_leaked_vars(Node *clause);

extern Relids find_nonnullable_rels(Node *clause);
extern List *find_nonnullable_vars(Node *clause);
extern List *find_forced_null_vars(Node *node);
extern Var *find_forced_null_var(Node *node);
extern void find_safe_quals(Node *jtnode, List **safe_quals);
extern bool query_outputs_are_not_nullable(Query *query);

extern bool is_pseudo_constant_clause(Node *clause);
extern bool is_pseudo_constant_clause_relids(Node *clause, Relids relids);

extern int	NumRelids(PlannerInfo *root, Node *clause);

extern void CommuteOpExpr(OpExpr *clause);

extern Query *inline_function_in_from(PlannerInfo *root,
									  RangeTblEntry *rte);

extern Bitmapset *pull_paramids(Expr *expr);

extern bool expression_has_grouping_conflict(Node *expr,
											 grouping_eqop_callback get_eqop,
											 void *context);

#endif							/* CLAUSES_H */
