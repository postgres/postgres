/*-------------------------------------------------------------------------
 *
 * placeholder.h
 *	  prototypes for optimizer/util/placeholder.c.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/placeholder.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLACEHOLDER_H
#define PLACEHOLDER_H

#include "nodes/pathnodes.h"


extern PlaceHolderVar *make_placeholder_expr(PlannerInfo *root, Expr *expr,
											 Relids phrels);
extern PlaceHolderInfo *find_placeholder_info(PlannerInfo *root,
											  PlaceHolderVar *phv);
extern void find_placeholders_in_jointree(PlannerInfo *root);
extern void fix_placeholder_input_needed_levels(PlannerInfo *root);
extern void add_placeholders_to_base_rels(PlannerInfo *root);
extern void add_placeholders_to_joinrel(PlannerInfo *root, RelOptInfo *joinrel,
										RelOptInfo *outer_rel, RelOptInfo *inner_rel,
										SpecialJoinInfo *sjinfo);
extern bool contain_placeholder_references_to(PlannerInfo *root, Node *clause,
											  int relid);

#endif							/* PLACEHOLDER_H */
