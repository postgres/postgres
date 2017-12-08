/*-------------------------------------------------------------------------
 *
 * prep.h
 *	  prototypes for files in optimizer/prep/
 *
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/prep.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PREP_H
#define PREP_H

#include "nodes/plannodes.h"
#include "nodes/relation.h"


/*
 * prototypes for prepjointree.c
 */
extern void pull_up_sublinks(PlannerInfo *root);
extern void inline_set_returning_functions(PlannerInfo *root);
extern void pull_up_subqueries(PlannerInfo *root);
extern void flatten_simple_union_all(PlannerInfo *root);
extern void reduce_outer_joins(PlannerInfo *root);
extern Relids get_relids_in_jointree(Node *jtnode, bool include_joins);
extern Relids get_relids_for_join(PlannerInfo *root, int joinrelid);

/*
 * prototypes for prepqual.c
 */
extern Node *negate_clause(Node *node);
extern Expr *canonicalize_qual(Expr *qual);

/*
 * prototypes for preptlist.c
 */
extern List *preprocess_targetlist(PlannerInfo *root);

extern PlanRowMark *get_plan_rowmark(List *rowmarks, Index rtindex);

/*
 * prototypes for prepunion.c
 */
extern RelOptInfo *plan_set_operations(PlannerInfo *root);

extern void expand_inherited_tables(PlannerInfo *root);

extern Node *adjust_appendrel_attrs(PlannerInfo *root, Node *node,
					   int nappinfos, AppendRelInfo **appinfos);

extern Node *adjust_appendrel_attrs_multilevel(PlannerInfo *root, Node *node,
								  Relids child_relids,
								  Relids top_parent_relids);

extern AppendRelInfo **find_appinfos_by_relids(PlannerInfo *root,
						Relids relids, int *nappinfos);

extern SpecialJoinInfo *build_child_join_sjinfo(PlannerInfo *root,
						SpecialJoinInfo *parent_sjinfo,
						Relids left_relids, Relids right_relids);
extern Relids adjust_child_relids_multilevel(PlannerInfo *root, Relids relids,
							   Relids child_relids, Relids top_parent_relids);

#endif							/* PREP_H */
