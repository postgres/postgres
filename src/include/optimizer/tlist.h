/*-------------------------------------------------------------------------
 *
 * tlist.h
 *	  prototypes for tlist.c.
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/tlist.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TLIST_H
#define TLIST_H

#include "nodes/pathnodes.h"


extern TargetEntry *tlist_member(Expr *node, List *targetlist);
extern TargetEntry *tlist_member_ignore_relabel(Expr *node, List *targetlist);

extern List *add_to_flat_tlist(List *tlist, List *exprs);

extern List *get_tlist_exprs(List *tlist, bool includeJunk);

extern bool tlist_same_exprs(List *tlist1, List *tlist2);

extern bool tlist_same_datatypes(List *tlist, List *colTypes, bool junkOK);
extern bool tlist_same_collations(List *tlist, List *colCollations, bool junkOK);

extern void apply_tlist_labeling(List *dest_tlist, List *src_tlist);

extern Oid *extract_grouping_ops(List *groupClause);
extern Oid *extract_grouping_collations(List *groupClause, List *tlist);
extern AttrNumber *extract_grouping_cols(List *groupClause, List *tlist);
extern bool grouping_is_sortable(List *groupClause);
extern bool grouping_is_hashable(List *groupClause);

extern PathTarget *make_pathtarget_from_tlist(List *tlist);
extern List *make_tlist_from_pathtarget(PathTarget *target);
extern PathTarget *copy_pathtarget(PathTarget *src);
extern PathTarget *create_empty_pathtarget(void);
extern void add_column_to_pathtarget(PathTarget *target,
									 Expr *expr, Index sortgroupref);
extern void add_new_column_to_pathtarget(PathTarget *target, Expr *expr);
extern void add_new_columns_to_pathtarget(PathTarget *target, List *exprs);
extern void apply_pathtarget_labeling_to_tlist(List *tlist, PathTarget *target);
extern void split_pathtarget_at_srfs(PlannerInfo *root,
									 PathTarget *target, PathTarget *input_target,
									 List **targets, List **targets_contain_srfs);

/* Convenience macro to get a PathTarget with valid cost/width fields */
#define create_pathtarget(root, tlist) \
	set_pathtarget_cost_width(root, make_pathtarget_from_tlist(tlist))

#endif							/* TLIST_H */
