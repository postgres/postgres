/*-------------------------------------------------------------------------
 *
 * planmain.h--
 *	  prototypes for various files in optimizer/plan
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: planmain.h,v 1.16 1998/09/03 02:34:35 momjian Exp $
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
extern Sort *make_sort(List *tlist, Oid tempid, Plan *lefttree,
		  int keycount);
extern Agg *make_agg(List *tlist, Plan *lefttree);
extern Group *make_group(List *tlist, bool tuplePerGroup, int ngrp,
		   AttrNumber *grpColIdx, Sort *lefttree);
extern Unique *make_unique(List *tlist, Plan *lefttree, char *uniqueAttr);

/*
 * prototypes for plan/initsplan.c
 */
extern void init_base_rels_tlist(Query *root, List *tlist);
extern void init_base_rels_qual(Query *root, List *clauses);
extern void init_join_info(List *rel_list);
extern void add_missing_vars_to_tlist(Query *root, List *tlist);

/*
 * prototypes for plan/setrefs.c
 */
extern void set_tlist_references(Plan *plan);
extern List *join_references(List *clauses, List *outer_tlist,
				List *inner_tlist);
extern List *index_outerjoin_references(List *inner_indxqual,
						   List *outer_tlist, Index inner_relid);
extern void set_result_tlist_references(Result *resultNode);
extern List *set_agg_tlist_references(Agg *aggNode);
extern void set_agg_agglist_references(Agg *aggNode);
extern void del_agg_tlist_references(List *tlist);
extern List *check_having_qual_for_aggs(Node *clause,
						   List *subplanTargetList, List *groupClause);
extern List *check_having_qual_for_vars(Node *clause, List *targetlist_so_far);
extern void transformKeySetQuery(Query *origNode);

#endif	 /* PLANMAIN_H */
