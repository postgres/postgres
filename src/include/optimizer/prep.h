/*-------------------------------------------------------------------------
 *
 * prep.h
 *	  prototypes for files in optimizer/prep/
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/prep.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PREP_H
#define PREP_H

#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"


/*
 * prototypes for prepjointree.c
 */
extern void replace_empty_jointree(Query *parse);
extern void pull_up_sublinks(PlannerInfo *root);
extern void inline_set_returning_functions(PlannerInfo *root);
extern void pull_up_subqueries(PlannerInfo *root);
extern void flatten_simple_union_all(PlannerInfo *root);
extern void reduce_outer_joins(PlannerInfo *root);
extern void remove_useless_result_rtes(PlannerInfo *root);
extern Relids get_relids_in_jointree(Node *jtnode, bool include_joins);
extern Relids get_relids_for_join(Query *query, int joinrelid);

/*
 * prototypes for preptlist.c
 */
extern List *preprocess_targetlist(PlannerInfo *root);

extern PlanRowMark *get_plan_rowmark(List *rowmarks, Index rtindex);

/*
 * prototypes for prepunion.c
 */
extern RelOptInfo *plan_set_operations(PlannerInfo *root);

#endif							/* PREP_H */
