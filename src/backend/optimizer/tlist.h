/*-------------------------------------------------------------------------
 *
 * tlist.h--
 *    prototypes for tlist.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: tlist.h,v 1.1.1.1 1996/07/09 06:21:34 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TLIST_H
#define TLIST_H

extern int exec_tlist_length(List *targelist);
extern TargetEntry *tlistentry_member(Var *var, List *targetlist);
extern Expr *matching_tlvar(Var *var, List *targetlist);
extern void add_tl_element(Rel *rel, Var *var);
extern TargetEntry *create_tl_element(Var *var, int resdomno);
extern List *get_actual_tlist(List *tlist);
extern Resdom *tlist_member(Var *var, List *tlist);
extern Resdom *tlist_resdom(List *tlist, Resdom *resnode);

extern TargetEntry *MakeTLE(Resdom *resdom, Node *expr);
extern Var *get_expr(TargetEntry *tle);

extern TargetEntry *match_varid(Var *test_var, List *tlist);
extern List *new_unsorted_tlist(List *targetlist);
extern List *copy_vars(List *target, List *source);
extern List *flatten_tlist(List *tlist);
extern List *flatten_tlist_vars(List *full_tlist,
				List *flat_tlist);
extern void AddGroupAttrToTlist(List *tlist, List *grpCl);

#endif	/* TLIST_H */
