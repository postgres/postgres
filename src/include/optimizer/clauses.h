/*-------------------------------------------------------------------------
 *
 * clauses.h
 *	  prototypes for clauses.c.
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/optimizer/clauses.h,v 1.101 2010/02/26 02:01:26 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef CLAUSES_H
#define CLAUSES_H

#include "nodes/relation.h"


#define is_opclause(clause)		((clause) != NULL && IsA(clause, OpExpr))
#define is_funcclause(clause)	((clause) != NULL && IsA(clause, FuncExpr))

typedef struct
{
	int			numAggs;		/* total number of aggregate calls */
	int			numOrderedAggs; /* number that use DISTINCT or ORDER BY */
	Size		transitionSpace;	/* for pass-by-ref transition data */
} AggClauseCounts;

typedef struct
{
	int			numWindowFuncs; /* total number of WindowFuncs found */
	Index		maxWinRef;		/* windowFuncs[] is indexed 0 .. maxWinRef */
	List	  **windowFuncs;	/* lists of WindowFuncs for each winref */
} WindowFuncLists;


extern Expr *make_opclause(Oid opno, Oid opresulttype, bool opretset,
			  Expr *leftop, Expr *rightop);
extern Node *get_leftop(Expr *clause);
extern Node *get_rightop(Expr *clause);

extern bool not_clause(Node *clause);
extern Expr *make_notclause(Expr *notclause);
extern Expr *get_notclausearg(Expr *notclause);

extern bool or_clause(Node *clause);
extern Expr *make_orclause(List *orclauses);

extern bool and_clause(Node *clause);
extern Expr *make_andclause(List *andclauses);
extern Node *make_and_qual(Node *qual1, Node *qual2);
extern Expr *make_ands_explicit(List *andclauses);
extern List *make_ands_implicit(Expr *clause);

extern bool contain_agg_clause(Node *clause);
extern List *pull_agg_clause(Node *clause);
extern void count_agg_clauses(Node *clause, AggClauseCounts *counts);

extern bool contain_window_function(Node *clause);
extern WindowFuncLists *find_window_functions(Node *clause, Index maxWinRef);

extern double expression_returns_set_rows(Node *clause);

extern bool contain_subplans(Node *clause);

extern bool contain_mutable_functions(Node *clause);
extern bool contain_volatile_functions(Node *clause);
extern bool contain_nonstrict_functions(Node *clause);
extern Relids find_nonnullable_rels(Node *clause);
extern List *find_nonnullable_vars(Node *clause);
extern List *find_forced_null_vars(Node *clause);
extern Var *find_forced_null_var(Node *clause);

extern bool is_pseudo_constant_clause(Node *clause);
extern bool is_pseudo_constant_clause_relids(Node *clause, Relids relids);

extern int	NumRelids(Node *clause);

extern void CommuteOpExpr(OpExpr *clause);
extern void CommuteRowCompareExpr(RowCompareExpr *clause);

extern Node *strip_implicit_coercions(Node *node);

extern void set_coercionform_dontcare(Node *node);

extern Node *eval_const_expressions(PlannerInfo *root, Node *node);

extern Node *estimate_expression_value(PlannerInfo *root, Node *node);

extern Query *inline_set_returning_function(PlannerInfo *root,
							  RangeTblEntry *rte);

#endif   /* CLAUSES_H */
