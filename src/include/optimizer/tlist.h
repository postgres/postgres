/*-------------------------------------------------------------------------
 *
 * tlist.h
 *	  prototypes for tlist.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: tlist.h,v 1.21 1999/08/21 03:49:15 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TLIST_H
#define TLIST_H

#include "nodes/relation.h"

extern TargetEntry *tlistentry_member(Var *var, List *targetlist);
extern Expr *matching_tlist_var(Var *var, List *targetlist);
extern void add_var_to_tlist(RelOptInfo *rel, Var *var);
extern TargetEntry *create_tl_element(Var *var, int resdomno);
extern List *get_actual_tlist(List *tlist);
extern Resdom *tlist_member(Var *var, List *tlist);

extern TargetEntry *match_varid(Var *test_var, List *tlist);
extern List *new_unsorted_tlist(List *targetlist);
extern List *copy_vars(List *target, List *source);
extern List *flatten_tlist(List *tlist);
extern List *add_to_flat_tlist(List *tlist, List *vars);
extern List *unflatten_tlist(List *full_tlist,
							 List *flat_tlist);

extern Var *get_expr(TargetEntry *tle);
extern Node *get_sortgroupclause_expr(SortClause *sortClause,
									  List *targetList);

#endif	 /* TLIST_H */
