/*-------------------------------------------------------------------------
 *
 * clauses.h
 *	  prototypes for clauses.c.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: clauses.h,v 1.42 2001/01/24 19:43:26 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef CLAUSES_H
#define CLAUSES_H

#include "nodes/relation.h"

/*
 *	Flag bits returned by get_relattval().
 *	These are used in selectivity-estimation routines, too.
 */
#define	SEL_CONSTANT 	1		/* operator's non-var arg is a constant */
#define	SEL_RIGHT		2		/* operator's non-var arg is on the right */


extern Expr *make_clause(int type, Node *oper, List *args);

extern bool is_opclause(Node *clause);
extern Expr *make_opclause(Oper *op, Var *leftop, Var *rightop);
extern Var *get_leftop(Expr *clause);
extern Var *get_rightop(Expr *clause);

extern bool is_funcclause(Node *clause);
extern Expr *make_funcclause(Func *func, List *funcargs);

extern bool or_clause(Node *clause);
extern Expr *make_orclause(List *orclauses);

extern bool not_clause(Node *clause);
extern Expr *make_notclause(Expr *notclause);
extern Expr *get_notclausearg(Expr *notclause);

extern bool and_clause(Node *clause);
extern Expr *make_andclause(List *andclauses);
extern Node *make_and_qual(Node *qual1, Node *qual2);
extern Expr *make_ands_explicit(List *andclauses);
extern List *make_ands_implicit(Expr *clause);

extern bool contain_agg_clause(Node *clause);
extern List *pull_agg_clause(Node *clause);

extern bool contain_subplans(Node *clause);
extern List *pull_subplans(Node *clause);
extern void check_subplans_for_ungrouped_vars(Node *clause, Query *query);

extern bool contain_noncachable_functions(Node *clause);

extern bool is_pseudo_constant_clause(Node *clause);
extern List *pull_constant_clauses(List *quals, List **constantQual);

extern void clause_get_relids_vars(Node *clause, Relids *relids, List **vars);
extern int	NumRelids(Node *clause);
extern void get_relattval(Node *clause, int targetrelid,
			  int *relid, AttrNumber *attno,
			  Datum *constval, int *flag);
extern void get_rels_atts(Node *clause, int *relid1,
			  AttrNumber *attno1, int *relid2, AttrNumber *attno2);
extern void CommuteClause(Expr *clause);

extern Node *eval_const_expressions(Node *node);

extern bool expression_tree_walker(Node *node, bool (*walker) (),
								   void *context);
extern Node *expression_tree_mutator(Node *node, Node *(*mutator) (),
									 void *context);
extern bool query_tree_walker(Query *query, bool (*walker) (),
							  void *context, bool visitQueryRTEs);
extern void query_tree_mutator(Query *query, Node *(*mutator) (),
							   void *context, bool visitQueryRTEs);

#define is_subplan(clause)	((clause) != NULL && \
							 IsA(clause, Expr) && \
							 ((Expr *) (clause))->opType == SUBPLAN_EXPR)

#endif	 /* CLAUSES_H */
