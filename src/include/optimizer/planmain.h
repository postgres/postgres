/*-------------------------------------------------------------------------
 *
 * planmain.h
 *	  prototypes for various files in optimizer/plan
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: planmain.h,v 1.23 1999/04/19 01:43:10 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLANMAIN_H
#define PLANMAIN_H

#include "nodes/nodes.h"
#include "nodes/plannodes.h"
#include "nodes/parsenodes.h"
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
extern List *index_outerjoin_references(List *inner_indxqual,
						   List *outer_tlist, Index inner_relid);
extern bool set_agg_tlist_references(Agg *aggNode);
extern void del_agg_tlist_references(List *tlist);
extern List *check_having_qual_for_vars(Node *clause, List *targetlist_so_far);
extern void check_having_for_ungrouped_vars(Node *clause, List *groupClause);
extern void transformKeySetQuery(Query *origNode);

#endif	 /* PLANMAIN_H */
