/*-------------------------------------------------------------------------
 *
 * clauses.h--
 *	  prototypes for clauses.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: clauses.h,v 1.7 1997/09/08 21:53:06 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef CLAUSES_H
#define CLAUSES_H

#include <nodes/primnodes.h>

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

extern List *pull_constant_clauses(List *quals, List **constantQual);
extern void clause_relids_vars(Node *clause, List **relids, List **vars);
extern int	NumRelids(Node *clause);
extern bool contains_not(Node *clause);
extern bool join_clause_p(Node *clause);
extern bool qual_clause_p(Node *clause);
extern void fix_opid(Node *clause);
extern List *fix_opids(List *clauses);
extern void
get_relattval(Node *clause, int *relid,
			  AttrNumber *attno, Datum *constval, int *flag);
extern void
get_rels_atts(Node *clause, int *relid1,
			  AttrNumber *attno1, int *relid2, AttrNumber *attno2);
extern void CommuteClause(Node *clause);

#endif							/* CLAUSES_H */
