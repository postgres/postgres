/*-------------------------------------------------------------------------
 *
 * planmain.h
 *	  prototypes for various files in optimizer/plan
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: planmain.h,v 1.75.4.1 2005/09/28 21:17:50 tgl Exp $
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
extern void query_planner(Query *root, List *tlist, double tuple_fraction,
			  Path **cheapest_path, Path **sorted_path);

/*
 * prototypes for plan/createplan.c
 */
extern Plan *create_plan(Query *root, Path *best_path);
extern SubqueryScan *make_subqueryscan(List *qptlist, List *qpqual,
				  Index scanrelid, Plan *subplan);
extern Append *make_append(List *appendplans, bool isTarget, List *tlist);
extern Sort *make_sort_from_sortclauses(Query *root, List *tlist,
						   Plan *lefttree, List *sortcls);
extern Sort *make_sort_from_groupcols(Query *root, List *groupcls,
						 AttrNumber *grpColIdx, Plan *lefttree);
extern Agg *make_agg(Query *root, List *tlist, List *qual,
		 AggStrategy aggstrategy,
		 int numGroupCols, AttrNumber *grpColIdx,
		 long numGroups, int numAggs,
		 Plan *lefttree);
extern Group *make_group(Query *root, List *tlist,
		   int numGroupCols, AttrNumber *grpColIdx,
		   double numGroups,
		   Plan *lefttree);
extern Material *make_material(List *tlist, Plan *lefttree);
extern Plan *materialize_finished_plan(Plan *subplan);
extern Unique *make_unique(List *tlist, Plan *lefttree, List *distinctList);
extern Limit *make_limit(List *tlist, Plan *lefttree,
		   Node *limitOffset, Node *limitCount);
extern SetOp *make_setop(SetOpCmd cmd, List *tlist, Plan *lefttree,
		   List *distinctList, AttrNumber flagColIdx);
extern Result *make_result(List *tlist, Node *resconstantqual, Plan *subplan);

/*
 * prototypes for plan/initsplan.c
 */
extern void add_base_rels_to_query(Query *root, Node *jtnode);
extern void build_base_rel_tlists(Query *root, List *final_tlist);
extern Relids distribute_quals_to_rels(Query *root, Node *jtnode,
									   bool below_outer_join);
extern void process_implied_equality(Query *root,
						 Node *item1, Node *item2,
						 Oid sortop1, Oid sortop2,
						 Relids item1_relids, Relids item2_relids,
						 bool delete_it);

/*
 * prototypes for plan/setrefs.c
 */
extern void set_plan_references(Plan *plan, List *rtable);
extern void fix_opfuncids(Node *node);
extern void set_opfuncid(OpExpr *opexpr);

#endif   /* PLANMAIN_H */
