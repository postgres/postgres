/*-------------------------------------------------------------------------
 *
 * planmain.h
 *	  prototypes for various files in optimizer/plan
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: planmain.h,v 1.30 1999/08/09 00:56:04 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLANMAIN_H
#define PLANMAIN_H

#include "nodes/plannodes.h"
#include "nodes/relation.h"

/*
 * prototypes for plan/planmain.c
 */
extern Plan *query_planner(Query *root,
			  int command_type, List *tlist, List *qual);


/*
 * prototypes for plan/createplan.c
 */
extern Plan *create_plan(Path *best_path);
extern SeqScan *make_seqscan(List *qptlist, List *qpqual, Index scanrelid,
			 Plan *lefttree);
extern Sort *make_sort(List *tlist, Oid nonameid, Plan *lefttree,
		  int keycount);
extern Agg *make_agg(List *tlist, Plan *lefttree);
extern Group *make_group(List *tlist, bool tuplePerGroup, int ngrp,
		   AttrNumber *grpColIdx, Sort *lefttree);
extern Unique *make_unique(List *tlist, Plan *lefttree, char *uniqueAttr);

/*
 * prototypes for plan/initsplan.c
 */
extern void make_var_only_tlist(Query *root, List *tlist);
extern void add_restrict_and_join_to_rels(Query *root, List *clauses);
extern void set_joininfo_mergeable_hashable(List *rel_list);
extern void add_missing_vars_to_tlist(Query *root, List *tlist);

/*
 * prototypes for plan/setrefs.c
 */
extern void set_tlist_references(Plan *plan);
extern List *join_references(List *clauses, List *outer_tlist,
							 List *inner_tlist);
extern void replace_tlist_with_subplan_refs(List *tlist,
								Index subvarno,
								List *subplanTargetList);
extern bool set_agg_tlist_references(Agg *aggNode);
extern void check_having_for_ungrouped_vars(Node *clause,
								List *groupClause,
								List *targetList);
extern void transformKeySetQuery(Query *origNode);

#endif	 /* PLANMAIN_H */
