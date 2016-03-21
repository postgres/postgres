/*-------------------------------------------------------------------------
 *
 * clauses.h
 *	  prototypes for clauses.c.
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/clauses.h
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
	int			numWindowFuncs; /* total number of WindowFuncs found */
	Index		maxWinRef;		/* windowFuncs[] is indexed 0 .. maxWinRef */
	List	  **windowFuncs;	/* lists of WindowFuncs for each winref */
} WindowFuncLists;

/*
 * PartialAggType
 *	PartialAggType stores whether partial aggregation is allowed and
 *	which context it is allowed in. We require three states here as there are
 *	two different contexts in which partial aggregation is safe. For aggregates
 *	which have an 'stype' of INTERNAL, it is okay to pass a pointer to the
 *  aggregate state within a single process, since the datum is just a
 *	pointer. In cases where the aggregate state must be passed between
 *  different processes, for example during parallel aggregation, passing
 *  pointers directly is not going to work.
 */
typedef enum
{
	PAT_ANY = 0,		/* Any type of partial aggregation is okay. */
	PAT_INTERNAL_ONLY,	/* Some aggregates support only internal mode. */
	PAT_DISABLED		/* Some aggregates don't support partial mode at all */
} PartialAggType;

extern Expr *make_opclause(Oid opno, Oid opresulttype, bool opretset,
			  Expr *leftop, Expr *rightop,
			  Oid opcollid, Oid inputcollid);
extern Node *get_leftop(const Expr *clause);
extern Node *get_rightop(const Expr *clause);

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

extern PartialAggType aggregates_allow_partial(Node *clause);
extern bool contain_agg_clause(Node *clause);
extern void count_agg_clauses(PlannerInfo *root, Node *clause,
				  AggClauseCosts *costs);

extern bool contain_window_function(Node *clause);
extern WindowFuncLists *find_window_functions(Node *clause, Index maxWinRef);

extern double expression_returns_set_rows(Node *clause);
extern double tlist_returns_set_rows(List *tlist);

extern bool contain_subplans(Node *clause);

extern bool contain_mutable_functions(Node *clause);
extern bool contain_volatile_functions(Node *clause);
extern bool contain_volatile_functions_not_nextval(Node *clause);
extern bool has_parallel_hazard(Node *node, bool allow_restricted);
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

extern Query *inline_set_returning_function(PlannerInfo *root,
							  RangeTblEntry *rte);

#endif   /* CLAUSES_H */
