/*-------------------------------------------------------------------------
 *
 * clauses.h
 *	  prototypes for clauses.c.
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/clauses.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CLAUSES_H
#define CLAUSES_H

#include "access/htup.h"
#include "nodes/relation.h"

typedef struct
{
	int			numWindowFuncs; /* total number of WindowFuncs found */
	Index		maxWinRef;		/* windowFuncs[] is indexed 0 .. maxWinRef */
	List	  **windowFuncs;	/* lists of WindowFuncs for each winref */
} WindowFuncLists;

extern bool contain_agg_clause(Node *clause);
extern void get_agg_clause_costs(PlannerInfo *root, Node *clause,
					 AggSplit aggsplit, AggClauseCosts *costs);

extern bool contain_window_function(Node *clause);
extern WindowFuncLists *find_window_functions(Node *clause, Index maxWinRef);

extern double expression_returns_set_rows(Node *clause);

extern bool contain_subplans(Node *clause);

extern bool contain_mutable_functions(Node *clause);
extern bool contain_volatile_functions(Node *clause);
extern bool contain_volatile_functions_not_nextval(Node *clause);
extern char max_parallel_hazard(Query *parse);
extern bool is_parallel_safe(PlannerInfo *root, Node *node);
extern bool contain_nonstrict_functions(Node *clause);
extern bool contain_leaked_vars(Node *clause);

extern Relids find_nonnullable_rels(Node *clause);
extern List *find_nonnullable_vars(Node *clause);
extern List *find_forced_null_vars(Node *clause);
extern Var *find_forced_null_var(Node *clause);

extern bool is_pseudo_constant_clause(Node *clause);
extern bool is_pseudo_constant_clause_relids(Node *clause, Relids relids);

extern int	NumRelids(Node *clause);

extern void CommuteOpExpr(OpExpr *clause);
extern void CommuteRowCompareExpr(RowCompareExpr *clause);

extern Node *eval_const_expressions(PlannerInfo *root, Node *node);

extern Node *estimate_expression_value(PlannerInfo *root, Node *node);

extern Expr *evaluate_expr(Expr *expr, Oid result_type, int32 result_typmod,
						   Oid result_collation);

extern Query *inline_set_returning_function(PlannerInfo *root,
							  RangeTblEntry *rte);

extern List *expand_function_arguments(List *args, Oid result_type,
						  HeapTuple func_tuple);

#endif							/* CLAUSES_H */
