/*-------------------------------------------------------------------------
 *
 * clauses.h
 *	  prototypes for clauses.c.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: clauses.h,v 1.68 2003/08/08 21:42:50 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef CLAUSES_H
#define CLAUSES_H

#include "nodes/relation.h"



#define is_opclause(clause)		((clause) != NULL && IsA(clause, OpExpr))
#define is_funcclause(clause)	((clause) != NULL && IsA(clause, FuncExpr))
#define is_subplan(clause)		((clause) != NULL && IsA(clause, SubPlan))


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
extern bool contain_distinct_agg_clause(Node *clause);
extern int	count_agg_clause(Node *clause);

extern bool expression_returns_set(Node *clause);

extern bool contain_subplans(Node *clause);

extern bool contain_mutable_functions(Node *clause);
extern bool contain_volatile_functions(Node *clause);
extern bool contain_nonstrict_functions(Node *clause);

extern bool is_pseudo_constant_clause(Node *clause);
extern List *pull_constant_clauses(List *quals, List **constantQual);

extern bool has_distinct_on_clause(Query *query);

extern void clause_get_relids_vars(Node *clause, Relids *relids, List **vars);
extern int	NumRelids(Node *clause);
extern void CommuteClause(OpExpr *clause);

extern Node *eval_const_expressions(Node *node);

extern bool expression_tree_walker(Node *node, bool (*walker) (),
											   void *context);
extern Node *expression_tree_mutator(Node *node, Node *(*mutator) (),
												 void *context);

/* flags bits for query_tree_walker and query_tree_mutator */
#define QTW_IGNORE_RT_SUBQUERIES	0x01		/* subqueries in rtable */
#define QTW_IGNORE_JOINALIASES		0x02		/* JOIN alias var lists */
#define QTW_DONT_COPY_QUERY			0x04		/* do not copy top Query */

extern bool query_tree_walker(Query *query, bool (*walker) (),
										  void *context, int flags);
extern Query *query_tree_mutator(Query *query, Node *(*mutator) (),
											 void *context, int flags);

extern bool query_or_expression_tree_walker(Node *node, bool (*walker) (),
											   void *context, int flags);
extern Node *query_or_expression_tree_mutator(Node *node, Node *(*mutator) (),
											   void *context, int flags);

#endif   /* CLAUSES_H */
