/*-------------------------------------------------------------------------
 *
 * planmain.h
 *	  prototypes for various files in optimizer/plan
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/optimizer/planmain.h,v 1.95.2.1 2007/10/04 20:44:55 tgl Exp $
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
extern void query_planner(PlannerInfo *root, List *tlist,
			  double tuple_fraction,
			  Path **cheapest_path, Path **sorted_path,
			  double *num_groups);

/*
 * prototypes for plan/planagg.c
 */
extern Plan *optimize_minmax_aggregates(PlannerInfo *root, List *tlist,
						   Path *best_path);

/*
 * prototypes for plan/createplan.c
 */
extern Plan *create_plan(PlannerInfo *root, Path *best_path);
extern SubqueryScan *make_subqueryscan(List *qptlist, List *qpqual,
				  Index scanrelid, Plan *subplan);
extern Append *make_append(List *appendplans, bool isTarget, List *tlist);
extern Sort *make_sort_from_sortclauses(PlannerInfo *root, List *sortcls,
						   Plan *lefttree);
extern Sort *make_sort_from_groupcols(PlannerInfo *root, List *groupcls,
						 AttrNumber *grpColIdx, Plan *lefttree);
extern Agg *make_agg(PlannerInfo *root, List *tlist, List *qual,
		 AggStrategy aggstrategy,
		 int numGroupCols, AttrNumber *grpColIdx,
		 long numGroups, int numAggs,
		 Plan *lefttree);
extern Group *make_group(PlannerInfo *root, List *tlist, List *qual,
		   int numGroupCols, AttrNumber *grpColIdx,
		   double numGroups,
		   Plan *lefttree);
extern Material *make_material(Plan *lefttree);
extern Plan *materialize_finished_plan(Plan *subplan);
extern Unique *make_unique(Plan *lefttree, List *distinctList);
extern Limit *make_limit(Plan *lefttree, Node *limitOffset, Node *limitCount,
		   int64 offset_est, int64 count_est);
extern SetOp *make_setop(SetOpCmd cmd, Plan *lefttree,
		   List *distinctList, AttrNumber flagColIdx);
extern Result *make_result(List *tlist, Node *resconstantqual, Plan *subplan);
extern bool is_projection_capable_plan(Plan *plan);

/*
 * prototypes for plan/initsplan.c
 */
extern int	from_collapse_limit;
extern int	join_collapse_limit;

extern void add_base_rels_to_query(PlannerInfo *root, Node *jtnode);
extern void build_base_rel_tlists(PlannerInfo *root, List *final_tlist);
extern void add_IN_vars_to_tlists(PlannerInfo *root);
extern List *deconstruct_jointree(PlannerInfo *root);
extern void process_implied_equality(PlannerInfo *root,
						 Node *item1, Node *item2,
						 Oid sortop1, Oid sortop2,
						 Relids item1_relids, Relids item2_relids,
						 bool delete_it);

/*
 * prototypes for plan/setrefs.c
 */
extern Plan *set_plan_references(Plan *plan, List *rtable);
extern List *set_returning_clause_references(List *rlist,
								Plan *topplan,
								Index resultRelation);
extern void fix_opfuncids(Node *node);
extern void set_opfuncid(OpExpr *opexpr);

#endif   /* PLANMAIN_H */
