/*-------------------------------------------------------------------------
 *
 * createplan.c
 *	  Routines to create the desired plan for processing a query.
 *	  Planning is complete, we just need to convert the selected
 *	  Path into a Plan.
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/createplan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>
#include <math.h>

#include "access/stratnum.h"
#include "access/sysattr.h"
#include "catalog/pg_class.h"
#include "foreign/fdwapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/paths.h"
#include "optimizer/placeholder.h"
#include "optimizer/plancat.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/predtest.h"
#include "optimizer/prep.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/subselect.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"
#include "parser/parse_clause.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"


/*
 * Flag bits that can appear in the flags argument of create_plan_recurse().
 * These can be OR-ed together.
 *
 * CP_EXACT_TLIST specifies that the generated plan node must return exactly
 * the tlist specified by the path's pathtarget (this overrides both
 * CP_SMALL_TLIST and CP_LABEL_TLIST, if those are set).  Otherwise, the
 * plan node is allowed to return just the Vars and PlaceHolderVars needed
 * to evaluate the pathtarget.
 *
 * CP_SMALL_TLIST specifies that a narrower tlist is preferred.  This is
 * passed down by parent nodes such as Sort and Hash, which will have to
 * store the returned tuples.
 *
 * CP_LABEL_TLIST specifies that the plan node must return columns matching
 * any sortgrouprefs specified in its pathtarget, with appropriate
 * ressortgroupref labels.  This is passed down by parent nodes such as Sort
 * and Group, which need these values to be available in their inputs.
 */
#define CP_EXACT_TLIST		0x0001		/* Plan must return specified tlist */
#define CP_SMALL_TLIST		0x0002		/* Prefer narrower tlists */
#define CP_LABEL_TLIST		0x0004		/* tlist must contain sortgrouprefs */


static Plan *create_plan_recurse(PlannerInfo *root, Path *best_path,
					int flags);
static Plan *create_scan_plan(PlannerInfo *root, Path *best_path,
				 int flags);
static List *build_path_tlist(PlannerInfo *root, Path *path);
static bool use_physical_tlist(PlannerInfo *root, Path *path, int flags);
static List *get_gating_quals(PlannerInfo *root, List *quals);
static Plan *create_gating_plan(PlannerInfo *root, Path *path, Plan *plan,
				   List *gating_quals);
static Plan *create_join_plan(PlannerInfo *root, JoinPath *best_path);
static Plan *create_append_plan(PlannerInfo *root, AppendPath *best_path);
static Plan *create_merge_append_plan(PlannerInfo *root, MergeAppendPath *best_path);
static Result *create_result_plan(PlannerInfo *root, ResultPath *best_path);
static Material *create_material_plan(PlannerInfo *root, MaterialPath *best_path,
					 int flags);
static Plan *create_unique_plan(PlannerInfo *root, UniquePath *best_path,
				   int flags);
static Gather *create_gather_plan(PlannerInfo *root, GatherPath *best_path);
static Plan *create_projection_plan(PlannerInfo *root, ProjectionPath *best_path);
static Plan *inject_projection_plan(Plan *subplan, List *tlist);
static Sort *create_sort_plan(PlannerInfo *root, SortPath *best_path, int flags);
static Group *create_group_plan(PlannerInfo *root, GroupPath *best_path);
static Unique *create_upper_unique_plan(PlannerInfo *root, UpperUniquePath *best_path,
						 int flags);
static Agg *create_agg_plan(PlannerInfo *root, AggPath *best_path);
static Plan *create_groupingsets_plan(PlannerInfo *root, GroupingSetsPath *best_path);
static Result *create_minmaxagg_plan(PlannerInfo *root, MinMaxAggPath *best_path);
static WindowAgg *create_windowagg_plan(PlannerInfo *root, WindowAggPath *best_path);
static SetOp *create_setop_plan(PlannerInfo *root, SetOpPath *best_path,
				  int flags);
static RecursiveUnion *create_recursiveunion_plan(PlannerInfo *root, RecursiveUnionPath *best_path);
static void get_column_info_for_window(PlannerInfo *root, WindowClause *wc,
						   List *tlist,
						   int numSortCols, AttrNumber *sortColIdx,
						   int *partNumCols,
						   AttrNumber **partColIdx,
						   Oid **partOperators,
						   int *ordNumCols,
						   AttrNumber **ordColIdx,
						   Oid **ordOperators);
static LockRows *create_lockrows_plan(PlannerInfo *root, LockRowsPath *best_path,
					 int flags);
static ModifyTable *create_modifytable_plan(PlannerInfo *root, ModifyTablePath *best_path);
static Limit *create_limit_plan(PlannerInfo *root, LimitPath *best_path,
				  int flags);
static SeqScan *create_seqscan_plan(PlannerInfo *root, Path *best_path,
					List *tlist, List *scan_clauses);
static SampleScan *create_samplescan_plan(PlannerInfo *root, Path *best_path,
					   List *tlist, List *scan_clauses);
static Scan *create_indexscan_plan(PlannerInfo *root, IndexPath *best_path,
					  List *tlist, List *scan_clauses, bool indexonly);
static BitmapHeapScan *create_bitmap_scan_plan(PlannerInfo *root,
						BitmapHeapPath *best_path,
						List *tlist, List *scan_clauses);
static Plan *create_bitmap_subplan(PlannerInfo *root, Path *bitmapqual,
					  List **qual, List **indexqual, List **indexECs);
static TidScan *create_tidscan_plan(PlannerInfo *root, TidPath *best_path,
					List *tlist, List *scan_clauses);
static SubqueryScan *create_subqueryscan_plan(PlannerInfo *root,
						 SubqueryScanPath *best_path,
						 List *tlist, List *scan_clauses);
static FunctionScan *create_functionscan_plan(PlannerInfo *root, Path *best_path,
						 List *tlist, List *scan_clauses);
static ValuesScan *create_valuesscan_plan(PlannerInfo *root, Path *best_path,
					   List *tlist, List *scan_clauses);
static CteScan *create_ctescan_plan(PlannerInfo *root, Path *best_path,
					List *tlist, List *scan_clauses);
static WorkTableScan *create_worktablescan_plan(PlannerInfo *root, Path *best_path,
						  List *tlist, List *scan_clauses);
static ForeignScan *create_foreignscan_plan(PlannerInfo *root, ForeignPath *best_path,
						List *tlist, List *scan_clauses);
static CustomScan *create_customscan_plan(PlannerInfo *root,
					   CustomPath *best_path,
					   List *tlist, List *scan_clauses);
static NestLoop *create_nestloop_plan(PlannerInfo *root, NestPath *best_path);
static MergeJoin *create_mergejoin_plan(PlannerInfo *root, MergePath *best_path);
static HashJoin *create_hashjoin_plan(PlannerInfo *root, HashPath *best_path);
static Node *replace_nestloop_params(PlannerInfo *root, Node *expr);
static Node *replace_nestloop_params_mutator(Node *node, PlannerInfo *root);
static void process_subquery_nestloop_params(PlannerInfo *root,
								 List *subplan_params);
static List *fix_indexqual_references(PlannerInfo *root, IndexPath *index_path);
static List *fix_indexorderby_references(PlannerInfo *root, IndexPath *index_path);
static Node *fix_indexqual_operand(Node *node, IndexOptInfo *index, int indexcol);
static List *get_switched_clauses(List *clauses, Relids outerrelids);
static List *order_qual_clauses(PlannerInfo *root, List *clauses);
static void copy_generic_path_info(Plan *dest, Path *src);
static void copy_plan_costsize(Plan *dest, Plan *src);
static void label_sort_with_costsize(PlannerInfo *root, Sort *plan,
						 double limit_tuples);
static SeqScan *make_seqscan(List *qptlist, List *qpqual, Index scanrelid);
static SampleScan *make_samplescan(List *qptlist, List *qpqual, Index scanrelid,
				TableSampleClause *tsc);
static IndexScan *make_indexscan(List *qptlist, List *qpqual, Index scanrelid,
			   Oid indexid, List *indexqual, List *indexqualorig,
			   List *indexorderby, List *indexorderbyorig,
			   List *indexorderbyops,
			   ScanDirection indexscandir);
static IndexOnlyScan *make_indexonlyscan(List *qptlist, List *qpqual,
				   Index scanrelid, Oid indexid,
				   List *indexqual, List *indexorderby,
				   List *indextlist,
				   ScanDirection indexscandir);
static BitmapIndexScan *make_bitmap_indexscan(Index scanrelid, Oid indexid,
					  List *indexqual,
					  List *indexqualorig);
static BitmapHeapScan *make_bitmap_heapscan(List *qptlist,
					 List *qpqual,
					 Plan *lefttree,
					 List *bitmapqualorig,
					 Index scanrelid);
static TidScan *make_tidscan(List *qptlist, List *qpqual, Index scanrelid,
			 List *tidquals);
static SubqueryScan *make_subqueryscan(List *qptlist,
				  List *qpqual,
				  Index scanrelid,
				  Plan *subplan);
static FunctionScan *make_functionscan(List *qptlist, List *qpqual,
				  Index scanrelid, List *functions, bool funcordinality);
static ValuesScan *make_valuesscan(List *qptlist, List *qpqual,
				Index scanrelid, List *values_lists);
static CteScan *make_ctescan(List *qptlist, List *qpqual,
			 Index scanrelid, int ctePlanId, int cteParam);
static WorkTableScan *make_worktablescan(List *qptlist, List *qpqual,
				   Index scanrelid, int wtParam);
static Append *make_append(List *appendplans, List *tlist);
static RecursiveUnion *make_recursive_union(List *tlist,
					 Plan *lefttree,
					 Plan *righttree,
					 int wtParam,
					 List *distinctList,
					 long numGroups);
static BitmapAnd *make_bitmap_and(List *bitmapplans);
static BitmapOr *make_bitmap_or(List *bitmapplans);
static NestLoop *make_nestloop(List *tlist,
			  List *joinclauses, List *otherclauses, List *nestParams,
			  Plan *lefttree, Plan *righttree,
			  JoinType jointype);
static HashJoin *make_hashjoin(List *tlist,
			  List *joinclauses, List *otherclauses,
			  List *hashclauses,
			  Plan *lefttree, Plan *righttree,
			  JoinType jointype);
static Hash *make_hash(Plan *lefttree,
		  Oid skewTable,
		  AttrNumber skewColumn,
		  bool skewInherit,
		  Oid skewColType,
		  int32 skewColTypmod);
static MergeJoin *make_mergejoin(List *tlist,
			   List *joinclauses, List *otherclauses,
			   List *mergeclauses,
			   Oid *mergefamilies,
			   Oid *mergecollations,
			   int *mergestrategies,
			   bool *mergenullsfirst,
			   Plan *lefttree, Plan *righttree,
			   JoinType jointype);
static Sort *make_sort(Plan *lefttree, int numCols,
		  AttrNumber *sortColIdx, Oid *sortOperators,
		  Oid *collations, bool *nullsFirst);
static Plan *prepare_sort_from_pathkeys(Plan *lefttree, List *pathkeys,
						   Relids relids,
						   const AttrNumber *reqColIdx,
						   bool adjust_tlist_in_place,
						   int *p_numsortkeys,
						   AttrNumber **p_sortColIdx,
						   Oid **p_sortOperators,
						   Oid **p_collations,
						   bool **p_nullsFirst);
static EquivalenceMember *find_ec_member_for_tle(EquivalenceClass *ec,
					   TargetEntry *tle,
					   Relids relids);
static Sort *make_sort_from_pathkeys(Plan *lefttree, List *pathkeys);
static Sort *make_sort_from_groupcols(List *groupcls,
						 AttrNumber *grpColIdx,
						 Plan *lefttree);
static Material *make_material(Plan *lefttree);
static WindowAgg *make_windowagg(List *tlist, Index winref,
			   int partNumCols, AttrNumber *partColIdx, Oid *partOperators,
			   int ordNumCols, AttrNumber *ordColIdx, Oid *ordOperators,
			   int frameOptions, Node *startOffset, Node *endOffset,
			   Plan *lefttree);
static Group *make_group(List *tlist, List *qual, int numGroupCols,
		   AttrNumber *grpColIdx, Oid *grpOperators,
		   Plan *lefttree);
static Unique *make_unique_from_sortclauses(Plan *lefttree, List *distinctList);
static Unique *make_unique_from_pathkeys(Plan *lefttree,
						  List *pathkeys, int numCols);
static Gather *make_gather(List *qptlist, List *qpqual,
			int nworkers, bool single_copy, Plan *subplan);
static SetOp *make_setop(SetOpCmd cmd, SetOpStrategy strategy, Plan *lefttree,
		   List *distinctList, AttrNumber flagColIdx, int firstFlag,
		   long numGroups);
static LockRows *make_lockrows(Plan *lefttree, List *rowMarks, int epqParam);
static Result *make_result(List *tlist, Node *resconstantqual, Plan *subplan);
static ModifyTable *make_modifytable(PlannerInfo *root,
				 CmdType operation, bool canSetTag,
				 Index nominalRelation,
				 List *resultRelations, List *subplans,
				 List *withCheckOptionLists, List *returningLists,
				 List *rowMarks, OnConflictExpr *onconflict, int epqParam);


/*
 * create_plan
 *	  Creates the access plan for a query by recursively processing the
 *	  desired tree of pathnodes, starting at the node 'best_path'.  For
 *	  every pathnode found, we create a corresponding plan node containing
 *	  appropriate id, target list, and qualification information.
 *
 *	  The tlists and quals in the plan tree are still in planner format,
 *	  ie, Vars still correspond to the parser's numbering.  This will be
 *	  fixed later by setrefs.c.
 *
 *	  best_path is the best access path
 *
 *	  Returns a Plan tree.
 */
Plan *
create_plan(PlannerInfo *root, Path *best_path)
{
	Plan	   *plan;

	/* plan_params should not be in use in current query level */
	Assert(root->plan_params == NIL);

	/* Initialize this module's private workspace in PlannerInfo */
	root->curOuterRels = NULL;
	root->curOuterParams = NIL;

	/* Recursively process the path tree, demanding the correct tlist result */
	plan = create_plan_recurse(root, best_path, CP_EXACT_TLIST);

	/*
	 * Make sure the topmost plan node's targetlist exposes the original
	 * column names and other decorative info.  Targetlists generated within
	 * the planner don't bother with that stuff, but we must have it on the
	 * top-level tlist seen at execution time.  However, ModifyTable plan
	 * nodes don't have a tlist matching the querytree targetlist.
	 */
	if (!IsA(plan, ModifyTable))
		apply_tlist_labeling(plan->targetlist, root->processed_tlist);

	/*
	 * Attach any initPlans created in this query level to the topmost plan
	 * node.  (The initPlans could actually go in any plan node at or above
	 * where they're referenced, but there seems no reason to put them any
	 * lower than the topmost node for the query level.)
	 */
	SS_attach_initplans(root, plan);

	/* Update parallel safety information if needed. */
	if (!best_path->parallel_safe)
		root->glob->wholePlanParallelSafe = false;

	/* Check we successfully assigned all NestLoopParams to plan nodes */
	if (root->curOuterParams != NIL)
		elog(ERROR, "failed to assign all NestLoopParams to plan nodes");

	/*
	 * Reset plan_params to ensure param IDs used for nestloop params are not
	 * re-used later
	 */
	root->plan_params = NIL;

	return plan;
}

/*
 * create_plan_recurse
 *	  Recursive guts of create_plan().
 */
static Plan *
create_plan_recurse(PlannerInfo *root, Path *best_path, int flags)
{
	Plan	   *plan;

	switch (best_path->pathtype)
	{
		case T_SeqScan:
		case T_SampleScan:
		case T_IndexScan:
		case T_IndexOnlyScan:
		case T_BitmapHeapScan:
		case T_TidScan:
		case T_SubqueryScan:
		case T_FunctionScan:
		case T_ValuesScan:
		case T_CteScan:
		case T_WorkTableScan:
		case T_ForeignScan:
		case T_CustomScan:
			plan = create_scan_plan(root, best_path, flags);
			break;
		case T_HashJoin:
		case T_MergeJoin:
		case T_NestLoop:
			plan = create_join_plan(root,
									(JoinPath *) best_path);
			break;
		case T_Append:
			plan = create_append_plan(root,
									  (AppendPath *) best_path);
			break;
		case T_MergeAppend:
			plan = create_merge_append_plan(root,
											(MergeAppendPath *) best_path);
			break;
		case T_Result:
			if (IsA(best_path, ProjectionPath))
			{
				plan = create_projection_plan(root,
											  (ProjectionPath *) best_path);
			}
			else if (IsA(best_path, MinMaxAggPath))
			{
				plan = (Plan *) create_minmaxagg_plan(root,
												(MinMaxAggPath *) best_path);
			}
			else
			{
				Assert(IsA(best_path, ResultPath));
				plan = (Plan *) create_result_plan(root,
												   (ResultPath *) best_path);
			}
			break;
		case T_Material:
			plan = (Plan *) create_material_plan(root,
												 (MaterialPath *) best_path,
												 flags);
			break;
		case T_Unique:
			if (IsA(best_path, UpperUniquePath))
			{
				plan = (Plan *) create_upper_unique_plan(root,
											   (UpperUniquePath *) best_path,
														 flags);
			}
			else
			{
				Assert(IsA(best_path, UniquePath));
				plan = create_unique_plan(root,
										  (UniquePath *) best_path,
										  flags);
			}
			break;
		case T_Gather:
			plan = (Plan *) create_gather_plan(root,
											   (GatherPath *) best_path);
			break;
		case T_Sort:
			plan = (Plan *) create_sort_plan(root,
											 (SortPath *) best_path,
											 flags);
			break;
		case T_Group:
			plan = (Plan *) create_group_plan(root,
											  (GroupPath *) best_path);
			break;
		case T_Agg:
			if (IsA(best_path, GroupingSetsPath))
				plan = create_groupingsets_plan(root,
											 (GroupingSetsPath *) best_path);
			else
			{
				Assert(IsA(best_path, AggPath));
				plan = (Plan *) create_agg_plan(root,
												(AggPath *) best_path);
			}
			break;
		case T_WindowAgg:
			plan = (Plan *) create_windowagg_plan(root,
												(WindowAggPath *) best_path);
			break;
		case T_SetOp:
			plan = (Plan *) create_setop_plan(root,
											  (SetOpPath *) best_path,
											  flags);
			break;
		case T_RecursiveUnion:
			plan = (Plan *) create_recursiveunion_plan(root,
										   (RecursiveUnionPath *) best_path);
			break;
		case T_LockRows:
			plan = (Plan *) create_lockrows_plan(root,
												 (LockRowsPath *) best_path,
												 flags);
			break;
		case T_ModifyTable:
			plan = (Plan *) create_modifytable_plan(root,
											  (ModifyTablePath *) best_path);
			break;
		case T_Limit:
			plan = (Plan *) create_limit_plan(root,
											  (LimitPath *) best_path,
											  flags);
			break;
		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) best_path->pathtype);
			plan = NULL;		/* keep compiler quiet */
			break;
	}

	return plan;
}

/*
 * create_scan_plan
 *	 Create a scan plan for the parent relation of 'best_path'.
 */
static Plan *
create_scan_plan(PlannerInfo *root, Path *best_path, int flags)
{
	RelOptInfo *rel = best_path->parent;
	List	   *scan_clauses;
	List	   *gating_clauses;
	List	   *tlist;
	Plan	   *plan;

	/*
	 * Extract the relevant restriction clauses from the parent relation. The
	 * executor must apply all these restrictions during the scan, except for
	 * pseudoconstants which we'll take care of below.
	 */
	scan_clauses = rel->baserestrictinfo;

	/*
	 * If this is a parameterized scan, we also need to enforce all the join
	 * clauses available from the outer relation(s).
	 *
	 * For paranoia's sake, don't modify the stored baserestrictinfo list.
	 */
	if (best_path->param_info)
		scan_clauses = list_concat(list_copy(scan_clauses),
								   best_path->param_info->ppi_clauses);

	/*
	 * Detect whether we have any pseudoconstant quals to deal with.  Then, if
	 * we'll need a gating Result node, it will be able to project, so there
	 * are no requirements on the child's tlist.
	 */
	gating_clauses = get_gating_quals(root, scan_clauses);
	if (gating_clauses)
		flags = 0;

	/*
	 * For table scans, rather than using the relation targetlist (which is
	 * only those Vars actually needed by the query), we prefer to generate a
	 * tlist containing all Vars in order.  This will allow the executor to
	 * optimize away projection of the table tuples, if possible.
	 */
	if (use_physical_tlist(root, best_path, flags))
	{
		if (best_path->pathtype == T_IndexOnlyScan)
		{
			/* For index-only scan, the preferred tlist is the index's */
			tlist = copyObject(((IndexPath *) best_path)->indexinfo->indextlist);
			/* Transfer any sortgroupref data to the replacement tlist */
			apply_pathtarget_labeling_to_tlist(tlist, best_path->pathtarget);
		}
		else
		{
			tlist = build_physical_tlist(root, rel);
			if (tlist == NIL)
			{
				/* Failed because of dropped cols, so use regular method */
				tlist = build_path_tlist(root, best_path);
			}
			else
			{
				/* Transfer any sortgroupref data to the replacement tlist */
				apply_pathtarget_labeling_to_tlist(tlist, best_path->pathtarget);
			}
		}
	}
	else
	{
		tlist = build_path_tlist(root, best_path);
	}

	switch (best_path->pathtype)
	{
		case T_SeqScan:
			plan = (Plan *) create_seqscan_plan(root,
												best_path,
												tlist,
												scan_clauses);
			break;

		case T_SampleScan:
			plan = (Plan *) create_samplescan_plan(root,
												   best_path,
												   tlist,
												   scan_clauses);
			break;

		case T_IndexScan:
			plan = (Plan *) create_indexscan_plan(root,
												  (IndexPath *) best_path,
												  tlist,
												  scan_clauses,
												  false);
			break;

		case T_IndexOnlyScan:
			plan = (Plan *) create_indexscan_plan(root,
												  (IndexPath *) best_path,
												  tlist,
												  scan_clauses,
												  true);
			break;

		case T_BitmapHeapScan:
			plan = (Plan *) create_bitmap_scan_plan(root,
												(BitmapHeapPath *) best_path,
													tlist,
													scan_clauses);
			break;

		case T_TidScan:
			plan = (Plan *) create_tidscan_plan(root,
												(TidPath *) best_path,
												tlist,
												scan_clauses);
			break;

		case T_SubqueryScan:
			plan = (Plan *) create_subqueryscan_plan(root,
											  (SubqueryScanPath *) best_path,
													 tlist,
													 scan_clauses);
			break;

		case T_FunctionScan:
			plan = (Plan *) create_functionscan_plan(root,
													 best_path,
													 tlist,
													 scan_clauses);
			break;

		case T_ValuesScan:
			plan = (Plan *) create_valuesscan_plan(root,
												   best_path,
												   tlist,
												   scan_clauses);
			break;

		case T_CteScan:
			plan = (Plan *) create_ctescan_plan(root,
												best_path,
												tlist,
												scan_clauses);
			break;

		case T_WorkTableScan:
			plan = (Plan *) create_worktablescan_plan(root,
													  best_path,
													  tlist,
													  scan_clauses);
			break;

		case T_ForeignScan:
			plan = (Plan *) create_foreignscan_plan(root,
													(ForeignPath *) best_path,
													tlist,
													scan_clauses);
			break;

		case T_CustomScan:
			plan = (Plan *) create_customscan_plan(root,
												   (CustomPath *) best_path,
												   tlist,
												   scan_clauses);
			break;

		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) best_path->pathtype);
			plan = NULL;		/* keep compiler quiet */
			break;
	}

	/*
	 * If there are any pseudoconstant clauses attached to this node, insert a
	 * gating Result node that evaluates the pseudoconstants as one-time
	 * quals.
	 */
	if (gating_clauses)
		plan = create_gating_plan(root, best_path, plan, gating_clauses);

	return plan;
}

/*
 * Build a target list (ie, a list of TargetEntry) for the Path's output.
 *
 * This is almost just make_tlist_from_pathtarget(), but we also have to
 * deal with replacing nestloop params.
 */
static List *
build_path_tlist(PlannerInfo *root, Path *path)
{
	List	   *tlist = NIL;
	Index	   *sortgrouprefs = path->pathtarget->sortgrouprefs;
	int			resno = 1;
	ListCell   *v;

	foreach(v, path->pathtarget->exprs)
	{
		Node	   *node = (Node *) lfirst(v);
		TargetEntry *tle;

		/*
		 * If it's a parameterized path, there might be lateral references in
		 * the tlist, which need to be replaced with Params.  There's no need
		 * to remake the TargetEntry nodes, so apply this to each list item
		 * separately.
		 */
		if (path->param_info)
			node = replace_nestloop_params(root, node);

		tle = makeTargetEntry((Expr *) node,
							  resno,
							  NULL,
							  false);
		if (sortgrouprefs)
			tle->ressortgroupref = sortgrouprefs[resno - 1];

		tlist = lappend(tlist, tle);
		resno++;
	}
	return tlist;
}

/*
 * use_physical_tlist
 *		Decide whether to use a tlist matching relation structure,
 *		rather than only those Vars actually referenced.
 */
static bool
use_physical_tlist(PlannerInfo *root, Path *path, int flags)
{
	RelOptInfo *rel = path->parent;
	int			i;
	ListCell   *lc;

	/*
	 * Forget it if either exact tlist or small tlist is demanded.
	 */
	if (flags & (CP_EXACT_TLIST | CP_SMALL_TLIST))
		return false;

	/*
	 * We can do this for real relation scans, subquery scans, function scans,
	 * values scans, and CTE scans (but not for, eg, joins).
	 */
	if (rel->rtekind != RTE_RELATION &&
		rel->rtekind != RTE_SUBQUERY &&
		rel->rtekind != RTE_FUNCTION &&
		rel->rtekind != RTE_VALUES &&
		rel->rtekind != RTE_CTE)
		return false;

	/*
	 * Can't do it with inheritance cases either (mainly because Append
	 * doesn't project; this test may be unnecessary now that
	 * create_append_plan instructs its children to return an exact tlist).
	 */
	if (rel->reloptkind != RELOPT_BASEREL)
		return false;

	/*
	 * Can't do it if any system columns or whole-row Vars are requested.
	 * (This could possibly be fixed but would take some fragile assumptions
	 * in setrefs.c, I think.)
	 */
	for (i = rel->min_attr; i <= 0; i++)
	{
		if (!bms_is_empty(rel->attr_needed[i - rel->min_attr]))
			return false;
	}

	/*
	 * Can't do it if the rel is required to emit any placeholder expressions,
	 * either.
	 */
	foreach(lc, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = (PlaceHolderInfo *) lfirst(lc);

		if (bms_nonempty_difference(phinfo->ph_needed, rel->relids) &&
			bms_is_subset(phinfo->ph_eval_at, rel->relids))
			return false;
	}

	/*
	 * Also, can't do it if CP_LABEL_TLIST is specified and path is requested
	 * to emit any sort/group columns that are not simple Vars.  (If they are
	 * simple Vars, they should appear in the physical tlist, and
	 * apply_pathtarget_labeling_to_tlist will take care of getting them
	 * labeled again.)
	 */
	if ((flags & CP_LABEL_TLIST) && path->pathtarget->sortgrouprefs)
	{
		i = 0;
		foreach(lc, path->pathtarget->exprs)
		{
			Expr	   *expr = (Expr *) lfirst(lc);

			if (path->pathtarget->sortgrouprefs[i])
			{
				if (expr && IsA(expr, Var))
					 /* okay */ ;
				else
					return false;
			}
			i++;
		}
	}

	return true;
}

/*
 * get_gating_quals
 *	  See if there are pseudoconstant quals in a node's quals list
 *
 * If the node's quals list includes any pseudoconstant quals,
 * return just those quals.
 */
static List *
get_gating_quals(PlannerInfo *root, List *quals)
{
	/* No need to look if we know there are no pseudoconstants */
	if (!root->hasPseudoConstantQuals)
		return NIL;

	/* Sort into desirable execution order while still in RestrictInfo form */
	quals = order_qual_clauses(root, quals);

	/* Pull out any pseudoconstant quals from the RestrictInfo list */
	return extract_actual_clauses(quals, true);
}

/*
 * create_gating_plan
 *	  Deal with pseudoconstant qual clauses
 *
 * Add a gating Result node atop the already-built plan.
 */
static Plan *
create_gating_plan(PlannerInfo *root, Path *path, Plan *plan,
				   List *gating_quals)
{
	Plan	   *gplan;

	Assert(gating_quals);

	/*
	 * Since we need a Result node anyway, always return the path's requested
	 * tlist; that's never a wrong choice, even if the parent node didn't ask
	 * for CP_EXACT_TLIST.
	 */
	gplan = (Plan *) make_result(build_path_tlist(root, path),
								 (Node *) gating_quals,
								 plan);

	/*
	 * Notice that we don't change cost or size estimates when doing gating.
	 * The costs of qual eval were already included in the subplan's cost.
	 * Leaving the size alone amounts to assuming that the gating qual will
	 * succeed, which is the conservative estimate for planning upper queries.
	 * We certainly don't want to assume the output size is zero (unless the
	 * gating qual is actually constant FALSE, and that case is dealt with in
	 * clausesel.c).  Interpolating between the two cases is silly, because it
	 * doesn't reflect what will really happen at runtime, and besides which
	 * in most cases we have only a very bad idea of the probability of the
	 * gating qual being true.
	 */
	copy_plan_costsize(gplan, plan);

	return gplan;
}

/*
 * create_join_plan
 *	  Create a join plan for 'best_path' and (recursively) plans for its
 *	  inner and outer paths.
 */
static Plan *
create_join_plan(PlannerInfo *root, JoinPath *best_path)
{
	Plan	   *plan;
	List	   *gating_clauses;

	switch (best_path->path.pathtype)
	{
		case T_MergeJoin:
			plan = (Plan *) create_mergejoin_plan(root,
												  (MergePath *) best_path);
			break;
		case T_HashJoin:
			plan = (Plan *) create_hashjoin_plan(root,
												 (HashPath *) best_path);
			break;
		case T_NestLoop:
			plan = (Plan *) create_nestloop_plan(root,
												 (NestPath *) best_path);
			break;
		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) best_path->path.pathtype);
			plan = NULL;		/* keep compiler quiet */
			break;
	}

	/*
	 * If there are any pseudoconstant clauses attached to this node, insert a
	 * gating Result node that evaluates the pseudoconstants as one-time
	 * quals.
	 */
	gating_clauses = get_gating_quals(root, best_path->joinrestrictinfo);
	if (gating_clauses)
		plan = create_gating_plan(root, (Path *) best_path, plan,
								  gating_clauses);

#ifdef NOT_USED

	/*
	 * * Expensive function pullups may have pulled local predicates * into
	 * this path node.  Put them in the qpqual of the plan node. * JMH,
	 * 6/15/92
	 */
	if (get_loc_restrictinfo(best_path) != NIL)
		set_qpqual((Plan) plan,
				   list_concat(get_qpqual((Plan) plan),
					   get_actual_clauses(get_loc_restrictinfo(best_path))));
#endif

	return plan;
}

/*
 * create_append_plan
 *	  Create an Append plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 *
 *	  Returns a Plan node.
 */
static Plan *
create_append_plan(PlannerInfo *root, AppendPath *best_path)
{
	Append	   *plan;
	List	   *tlist = build_path_tlist(root, &best_path->path);
	List	   *subplans = NIL;
	ListCell   *subpaths;

	/*
	 * The subpaths list could be empty, if every child was proven empty by
	 * constraint exclusion.  In that case generate a dummy plan that returns
	 * no rows.
	 *
	 * Note that an AppendPath with no members is also generated in certain
	 * cases where there was no appending construct at all, but we know the
	 * relation is empty (see set_dummy_rel_pathlist).
	 */
	if (best_path->subpaths == NIL)
	{
		/* Generate a Result plan with constant-FALSE gating qual */
		Plan	   *plan;

		plan = (Plan *) make_result(tlist,
									(Node *) list_make1(makeBoolConst(false,
																	  false)),
									NULL);

		copy_generic_path_info(plan, (Path *) best_path);

		return plan;
	}

	/* Build the plan for each child */
	foreach(subpaths, best_path->subpaths)
	{
		Path	   *subpath = (Path *) lfirst(subpaths);
		Plan	   *subplan;

		/* Must insist that all children return the same tlist */
		subplan = create_plan_recurse(root, subpath, CP_EXACT_TLIST);

		subplans = lappend(subplans, subplan);
	}

	/*
	 * XXX ideally, if there's just one child, we'd not bother to generate an
	 * Append node but just return the single child.  At the moment this does
	 * not work because the varno of the child scan plan won't match the
	 * parent-rel Vars it'll be asked to emit.
	 */

	plan = make_append(subplans, tlist);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return (Plan *) plan;
}

/*
 * create_merge_append_plan
 *	  Create a MergeAppend plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 *
 *	  Returns a Plan node.
 */
static Plan *
create_merge_append_plan(PlannerInfo *root, MergeAppendPath *best_path)
{
	MergeAppend *node = makeNode(MergeAppend);
	Plan	   *plan = &node->plan;
	List	   *tlist = build_path_tlist(root, &best_path->path);
	List	   *pathkeys = best_path->path.pathkeys;
	List	   *subplans = NIL;
	ListCell   *subpaths;

	/*
	 * We don't have the actual creation of the MergeAppend node split out
	 * into a separate make_xxx function.  This is because we want to run
	 * prepare_sort_from_pathkeys on it before we do so on the individual
	 * child plans, to make cross-checking the sort info easier.
	 */
	copy_generic_path_info(plan, (Path *) best_path);
	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = NULL;
	plan->righttree = NULL;

	/* Compute sort column info, and adjust MergeAppend's tlist as needed */
	(void) prepare_sort_from_pathkeys(plan, pathkeys,
									  best_path->path.parent->relids,
									  NULL,
									  true,
									  &node->numCols,
									  &node->sortColIdx,
									  &node->sortOperators,
									  &node->collations,
									  &node->nullsFirst);

	/*
	 * Now prepare the child plans.  We must apply prepare_sort_from_pathkeys
	 * even to subplans that don't need an explicit sort, to make sure they
	 * are returning the same sort key columns the MergeAppend expects.
	 */
	foreach(subpaths, best_path->subpaths)
	{
		Path	   *subpath = (Path *) lfirst(subpaths);
		Plan	   *subplan;
		int			numsortkeys;
		AttrNumber *sortColIdx;
		Oid		   *sortOperators;
		Oid		   *collations;
		bool	   *nullsFirst;

		/* Build the child plan */
		/* Must insist that all children return the same tlist */
		subplan = create_plan_recurse(root, subpath, CP_EXACT_TLIST);

		/* Compute sort column info, and adjust subplan's tlist as needed */
		subplan = prepare_sort_from_pathkeys(subplan, pathkeys,
											 subpath->parent->relids,
											 node->sortColIdx,
											 false,
											 &numsortkeys,
											 &sortColIdx,
											 &sortOperators,
											 &collations,
											 &nullsFirst);

		/*
		 * Check that we got the same sort key information.  We just Assert
		 * that the sortops match, since those depend only on the pathkeys;
		 * but it seems like a good idea to check the sort column numbers
		 * explicitly, to ensure the tlists really do match up.
		 */
		Assert(numsortkeys == node->numCols);
		if (memcmp(sortColIdx, node->sortColIdx,
				   numsortkeys * sizeof(AttrNumber)) != 0)
			elog(ERROR, "MergeAppend child's targetlist doesn't match MergeAppend");
		Assert(memcmp(sortOperators, node->sortOperators,
					  numsortkeys * sizeof(Oid)) == 0);
		Assert(memcmp(collations, node->collations,
					  numsortkeys * sizeof(Oid)) == 0);
		Assert(memcmp(nullsFirst, node->nullsFirst,
					  numsortkeys * sizeof(bool)) == 0);

		/* Now, insert a Sort node if subplan isn't sufficiently ordered */
		if (!pathkeys_contained_in(pathkeys, subpath->pathkeys))
		{
			Sort	   *sort = make_sort(subplan, numsortkeys,
										 sortColIdx, sortOperators,
										 collations, nullsFirst);

			label_sort_with_costsize(root, sort, best_path->limit_tuples);
			subplan = (Plan *) sort;
		}

		subplans = lappend(subplans, subplan);
	}

	node->mergeplans = subplans;

	return (Plan *) node;
}

/*
 * create_result_plan
 *	  Create a Result plan for 'best_path'.
 *	  This is only used for degenerate cases, such as a query with an empty
 *	  jointree.
 *
 *	  Returns a Plan node.
 */
static Result *
create_result_plan(PlannerInfo *root, ResultPath *best_path)
{
	Result	   *plan;
	List	   *tlist;
	List	   *quals;

	tlist = build_path_tlist(root, &best_path->path);

	/* best_path->quals is just bare clauses */
	quals = order_qual_clauses(root, best_path->quals);

	plan = make_result(tlist, (Node *) quals, NULL);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * create_material_plan
 *	  Create a Material plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 *
 *	  Returns a Plan node.
 */
static Material *
create_material_plan(PlannerInfo *root, MaterialPath *best_path, int flags)
{
	Material   *plan;
	Plan	   *subplan;

	/*
	 * We don't want any excess columns in the materialized tuples, so request
	 * a smaller tlist.  Otherwise, since Material doesn't project, tlist
	 * requirements pass through.
	 */
	subplan = create_plan_recurse(root, best_path->subpath,
								  flags | CP_SMALL_TLIST);

	plan = make_material(subplan);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * create_unique_plan
 *	  Create a Unique plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 *
 *	  Returns a Plan node.
 */
static Plan *
create_unique_plan(PlannerInfo *root, UniquePath *best_path, int flags)
{
	Plan	   *plan;
	Plan	   *subplan;
	List	   *in_operators;
	List	   *uniq_exprs;
	List	   *newtlist;
	int			nextresno;
	bool		newitems;
	int			numGroupCols;
	AttrNumber *groupColIdx;
	int			groupColPos;
	ListCell   *l;

	/* Unique doesn't project, so tlist requirements pass through */
	subplan = create_plan_recurse(root, best_path->subpath, flags);

	/* Done if we don't need to do any actual unique-ifying */
	if (best_path->umethod == UNIQUE_PATH_NOOP)
		return subplan;

	/*
	 * As constructed, the subplan has a "flat" tlist containing just the Vars
	 * needed here and at upper levels.  The values we are supposed to
	 * unique-ify may be expressions in these variables.  We have to add any
	 * such expressions to the subplan's tlist.
	 *
	 * The subplan may have a "physical" tlist if it is a simple scan plan. If
	 * we're going to sort, this should be reduced to the regular tlist, so
	 * that we don't sort more data than we need to.  For hashing, the tlist
	 * should be left as-is if we don't need to add any expressions; but if we
	 * do have to add expressions, then a projection step will be needed at
	 * runtime anyway, so we may as well remove unneeded items. Therefore
	 * newtlist starts from build_path_tlist() not just a copy of the
	 * subplan's tlist; and we don't install it into the subplan unless we are
	 * sorting or stuff has to be added.
	 */
	in_operators = best_path->in_operators;
	uniq_exprs = best_path->uniq_exprs;

	/* initialize modified subplan tlist as just the "required" vars */
	newtlist = build_path_tlist(root, &best_path->path);
	nextresno = list_length(newtlist) + 1;
	newitems = false;

	foreach(l, uniq_exprs)
	{
		Node	   *uniqexpr = lfirst(l);
		TargetEntry *tle;

		tle = tlist_member(uniqexpr, newtlist);
		if (!tle)
		{
			tle = makeTargetEntry((Expr *) uniqexpr,
								  nextresno,
								  NULL,
								  false);
			newtlist = lappend(newtlist, tle);
			nextresno++;
			newitems = true;
		}
	}

	if (newitems || best_path->umethod == UNIQUE_PATH_SORT)
	{
		/*
		 * If the top plan node can't do projections and its existing target
		 * list isn't already what we need, we need to add a Result node to
		 * help it along.
		 */
		if (!is_projection_capable_plan(subplan) &&
			!tlist_same_exprs(newtlist, subplan->targetlist))
			subplan = inject_projection_plan(subplan, newtlist);
		else
			subplan->targetlist = newtlist;
	}

	/*
	 * Build control information showing which subplan output columns are to
	 * be examined by the grouping step.  Unfortunately we can't merge this
	 * with the previous loop, since we didn't then know which version of the
	 * subplan tlist we'd end up using.
	 */
	newtlist = subplan->targetlist;
	numGroupCols = list_length(uniq_exprs);
	groupColIdx = (AttrNumber *) palloc(numGroupCols * sizeof(AttrNumber));

	groupColPos = 0;
	foreach(l, uniq_exprs)
	{
		Node	   *uniqexpr = lfirst(l);
		TargetEntry *tle;

		tle = tlist_member(uniqexpr, newtlist);
		if (!tle)				/* shouldn't happen */
			elog(ERROR, "failed to find unique expression in subplan tlist");
		groupColIdx[groupColPos++] = tle->resno;
	}

	if (best_path->umethod == UNIQUE_PATH_HASH)
	{
		Oid		   *groupOperators;

		/*
		 * Get the hashable equality operators for the Agg node to use.
		 * Normally these are the same as the IN clause operators, but if
		 * those are cross-type operators then the equality operators are the
		 * ones for the IN clause operators' RHS datatype.
		 */
		groupOperators = (Oid *) palloc(numGroupCols * sizeof(Oid));
		groupColPos = 0;
		foreach(l, in_operators)
		{
			Oid			in_oper = lfirst_oid(l);
			Oid			eq_oper;

			if (!get_compatible_hash_operators(in_oper, NULL, &eq_oper))
				elog(ERROR, "could not find compatible hash operator for operator %u",
					 in_oper);
			groupOperators[groupColPos++] = eq_oper;
		}

		/*
		 * Since the Agg node is going to project anyway, we can give it the
		 * minimum output tlist, without any stuff we might have added to the
		 * subplan tlist.
		 */
		plan = (Plan *) make_agg(build_path_tlist(root, &best_path->path),
								 NIL,
								 AGG_HASHED,
								 false,
								 true,
								 numGroupCols,
								 groupColIdx,
								 groupOperators,
								 NIL,
								 NIL,
								 best_path->path.rows,
								 subplan);
	}
	else
	{
		List	   *sortList = NIL;
		Sort	   *sort;

		/* Create an ORDER BY list to sort the input compatibly */
		groupColPos = 0;
		foreach(l, in_operators)
		{
			Oid			in_oper = lfirst_oid(l);
			Oid			sortop;
			Oid			eqop;
			TargetEntry *tle;
			SortGroupClause *sortcl;

			sortop = get_ordering_op_for_equality_op(in_oper, false);
			if (!OidIsValid(sortop))	/* shouldn't happen */
				elog(ERROR, "could not find ordering operator for equality operator %u",
					 in_oper);

			/*
			 * The Unique node will need equality operators.  Normally these
			 * are the same as the IN clause operators, but if those are
			 * cross-type operators then the equality operators are the ones
			 * for the IN clause operators' RHS datatype.
			 */
			eqop = get_equality_op_for_ordering_op(sortop, NULL);
			if (!OidIsValid(eqop))		/* shouldn't happen */
				elog(ERROR, "could not find equality operator for ordering operator %u",
					 sortop);

			tle = get_tle_by_resno(subplan->targetlist,
								   groupColIdx[groupColPos]);
			Assert(tle != NULL);

			sortcl = makeNode(SortGroupClause);
			sortcl->tleSortGroupRef = assignSortGroupRef(tle,
														 subplan->targetlist);
			sortcl->eqop = eqop;
			sortcl->sortop = sortop;
			sortcl->nulls_first = false;
			sortcl->hashable = false;	/* no need to make this accurate */
			sortList = lappend(sortList, sortcl);
			groupColPos++;
		}
		sort = make_sort_from_sortclauses(sortList, subplan);
		label_sort_with_costsize(root, sort, -1.0);
		plan = (Plan *) make_unique_from_sortclauses((Plan *) sort, sortList);
	}

	/* Copy cost data from Path to Plan */
	copy_generic_path_info(plan, &best_path->path);

	return plan;
}

/*
 * create_gather_plan
 *
 *	  Create a Gather plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 */
static Gather *
create_gather_plan(PlannerInfo *root, GatherPath *best_path)
{
	Gather	   *gather_plan;
	Plan	   *subplan;
	List	   *tlist;

	/*
	 * Although the Gather node can project, we prefer to push down such work
	 * to its child node, so demand an exact tlist from the child.
	 */
	subplan = create_plan_recurse(root, best_path->subpath, CP_EXACT_TLIST);

	tlist = build_path_tlist(root, &best_path->path);

	gather_plan = make_gather(tlist,
							  NIL,
							  best_path->path.parallel_degree,
							  best_path->single_copy,
							  subplan);

	copy_generic_path_info(&gather_plan->plan, &best_path->path);

	/* use parallel mode for parallel plans. */
	root->glob->parallelModeNeeded = true;

	return gather_plan;
}

/*
 * create_projection_plan
 *
 *	  Create a Result node to do a projection step and (recursively) plans
 *	  for its subpaths.
 */
static Plan *
create_projection_plan(PlannerInfo *root, ProjectionPath *best_path)
{
	Plan	   *plan;
	Plan	   *subplan;
	List	   *tlist;

	/* Since we intend to project, we don't need to constrain child tlist */
	subplan = create_plan_recurse(root, best_path->subpath, 0);

	tlist = build_path_tlist(root, &best_path->path);

	/*
	 * Although the ProjectionPath node wouldn't have been made unless its
	 * pathtarget is different from the subpath's, it can still happen that
	 * the constructed tlist matches the subplan's.  (An example is that
	 * MergeAppend doesn't project, so we would have thought that we needed a
	 * projection to attach resjunk sort columns to its output ... but
	 * create_merge_append_plan might have added those same resjunk sort
	 * columns to both MergeAppend and its children.)  So, if the desired
	 * tlist is the same expression-wise as the subplan's, just jam it in
	 * there.  We'll have charged for a Result that doesn't actually appear in
	 * the plan, but that's better than having a Result we don't need.
	 */
	if (tlist_same_exprs(tlist, subplan->targetlist))
	{
		plan = subplan;
		plan->targetlist = tlist;

		/* Adjust cost to match what we thought during planning */
		plan->startup_cost = best_path->path.startup_cost;
		plan->total_cost = best_path->path.total_cost;
		/* ... but be careful not to munge subplan's parallel-aware flag */
	}
	else
	{
		plan = (Plan *) make_result(tlist, NULL, subplan);

		copy_generic_path_info(plan, (Path *) best_path);
	}

	return plan;
}

/*
 * inject_projection_plan
 *	  Insert a Result node to do a projection step.
 *
 * This is used in a few places where we decide on-the-fly that we need a
 * projection step as part of the tree generated for some Path node.
 * We should try to get rid of this in favor of doing it more honestly.
 */
static Plan *
inject_projection_plan(Plan *subplan, List *tlist)
{
	Plan	   *plan;

	plan = (Plan *) make_result(tlist, NULL, subplan);

	/*
	 * In principle, we should charge tlist eval cost plus cpu_per_tuple per
	 * row for the Result node.  But the former has probably been factored in
	 * already and the latter was not accounted for during Path construction,
	 * so being formally correct might just make the EXPLAIN output look less
	 * consistent not more so.  Hence, just copy the subplan's cost.
	 */
	copy_plan_costsize(plan, subplan);

	return plan;
}

/*
 * create_sort_plan
 *
 *	  Create a Sort plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 */
static Sort *
create_sort_plan(PlannerInfo *root, SortPath *best_path, int flags)
{
	Sort	   *plan;
	Plan	   *subplan;

	/*
	 * We don't want any excess columns in the sorted tuples, so request a
	 * smaller tlist.  Otherwise, since Sort doesn't project, tlist
	 * requirements pass through.
	 */
	subplan = create_plan_recurse(root, best_path->subpath,
								  flags | CP_SMALL_TLIST);

	plan = make_sort_from_pathkeys(subplan, best_path->path.pathkeys);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * create_group_plan
 *
 *	  Create a Group plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 */
static Group *
create_group_plan(PlannerInfo *root, GroupPath *best_path)
{
	Group	   *plan;
	Plan	   *subplan;
	List	   *tlist;
	List	   *quals;

	/*
	 * Group can project, so no need to be terribly picky about child tlist,
	 * but we do need grouping columns to be available
	 */
	subplan = create_plan_recurse(root, best_path->subpath, CP_LABEL_TLIST);

	tlist = build_path_tlist(root, &best_path->path);

	quals = order_qual_clauses(root, best_path->qual);

	plan = make_group(tlist,
					  quals,
					  list_length(best_path->groupClause),
					  extract_grouping_cols(best_path->groupClause,
											subplan->targetlist),
					  extract_grouping_ops(best_path->groupClause),
					  subplan);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * create_upper_unique_plan
 *
 *	  Create a Unique plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 */
static Unique *
create_upper_unique_plan(PlannerInfo *root, UpperUniquePath *best_path, int flags)
{
	Unique	   *plan;
	Plan	   *subplan;

	/*
	 * Unique doesn't project, so tlist requirements pass through; moreover we
	 * need grouping columns to be labeled.
	 */
	subplan = create_plan_recurse(root, best_path->subpath,
								  flags | CP_LABEL_TLIST);

	plan = make_unique_from_pathkeys(subplan,
									 best_path->path.pathkeys,
									 best_path->numkeys);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * create_agg_plan
 *
 *	  Create an Agg plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 */
static Agg *
create_agg_plan(PlannerInfo *root, AggPath *best_path)
{
	Agg		   *plan;
	Plan	   *subplan;
	List	   *tlist;
	List	   *quals;

	/*
	 * Agg can project, so no need to be terribly picky about child tlist, but
	 * we do need grouping columns to be available
	 */
	subplan = create_plan_recurse(root, best_path->subpath, CP_LABEL_TLIST);

	tlist = build_path_tlist(root, &best_path->path);

	quals = order_qual_clauses(root, best_path->qual);

	plan = make_agg(tlist, quals,
					best_path->aggstrategy,
					false,
					true,
					list_length(best_path->groupClause),
					extract_grouping_cols(best_path->groupClause,
										  subplan->targetlist),
					extract_grouping_ops(best_path->groupClause),
					NIL,
					NIL,
					best_path->numGroups,
					subplan);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * Given a groupclause for a collection of grouping sets, produce the
 * corresponding groupColIdx.
 *
 * root->grouping_map maps the tleSortGroupRef to the actual column position in
 * the input tuple. So we get the ref from the entries in the groupclause and
 * look them up there.
 */
static AttrNumber *
remap_groupColIdx(PlannerInfo *root, List *groupClause)
{
	AttrNumber *grouping_map = root->grouping_map;
	AttrNumber *new_grpColIdx;
	ListCell   *lc;
	int			i;

	Assert(grouping_map);

	new_grpColIdx = palloc0(sizeof(AttrNumber) * list_length(groupClause));

	i = 0;
	foreach(lc, groupClause)
	{
		SortGroupClause *clause = lfirst(lc);

		new_grpColIdx[i++] = grouping_map[clause->tleSortGroupRef];
	}

	return new_grpColIdx;
}

/*
 * create_groupingsets_plan
 *	  Create a plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 *
 *	  What we emit is an Agg plan with some vestigial Agg and Sort nodes
 *	  hanging off the side.  The top Agg implements the last grouping set
 *	  specified in the GroupingSetsPath, and any additional grouping sets
 *	  each give rise to a subsidiary Agg and Sort node in the top Agg's
 *	  "chain" list.  These nodes don't participate in the plan directly,
 *	  but they are a convenient way to represent the required data for
 *	  the extra steps.
 *
 *	  Returns a Plan node.
 */
static Plan *
create_groupingsets_plan(PlannerInfo *root, GroupingSetsPath *best_path)
{
	Agg		   *plan;
	Plan	   *subplan;
	List	   *rollup_groupclauses = best_path->rollup_groupclauses;
	List	   *rollup_lists = best_path->rollup_lists;
	AttrNumber *grouping_map;
	int			maxref;
	List	   *chain;
	ListCell   *lc,
			   *lc2;

	/* Shouldn't get here without grouping sets */
	Assert(root->parse->groupingSets);
	Assert(rollup_lists != NIL);
	Assert(list_length(rollup_lists) == list_length(rollup_groupclauses));

	/*
	 * Agg can project, so no need to be terribly picky about child tlist, but
	 * we do need grouping columns to be available
	 */
	subplan = create_plan_recurse(root, best_path->subpath, CP_LABEL_TLIST);

	/*
	 * Compute the mapping from tleSortGroupRef to column index in the child's
	 * tlist.  First, identify max SortGroupRef in groupClause, for array
	 * sizing.
	 */
	maxref = 0;
	foreach(lc, root->parse->groupClause)
	{
		SortGroupClause *gc = (SortGroupClause *) lfirst(lc);

		if (gc->tleSortGroupRef > maxref)
			maxref = gc->tleSortGroupRef;
	}

	grouping_map = (AttrNumber *) palloc0((maxref + 1) * sizeof(AttrNumber));

	/* Now look up the column numbers in the child's tlist */
	foreach(lc, root->parse->groupClause)
	{
		SortGroupClause *gc = (SortGroupClause *) lfirst(lc);
		TargetEntry *tle = get_sortgroupclause_tle(gc, subplan->targetlist);

		grouping_map[gc->tleSortGroupRef] = tle->resno;
	}

	/*
	 * During setrefs.c, we'll need the grouping_map to fix up the cols lists
	 * in GroupingFunc nodes.  Save it for setrefs.c to use.
	 *
	 * This doesn't work if we're in an inheritance subtree (see notes in
	 * create_modifytable_plan).  Fortunately we can't be because there would
	 * never be grouping in an UPDATE/DELETE; but let's Assert that.
	 */
	Assert(!root->hasInheritedTarget);
	Assert(root->grouping_map == NULL);
	root->grouping_map = grouping_map;

	/*
	 * Generate the side nodes that describe the other sort and group
	 * operations besides the top one.  Note that we don't worry about putting
	 * accurate cost estimates in the side nodes; only the topmost Agg node's
	 * costs will be shown by EXPLAIN.
	 */
	chain = NIL;
	if (list_length(rollup_groupclauses) > 1)
	{
		forboth(lc, rollup_groupclauses, lc2, rollup_lists)
		{
			List	   *groupClause = (List *) lfirst(lc);
			List	   *gsets = (List *) lfirst(lc2);
			AttrNumber *new_grpColIdx;
			Plan	   *sort_plan;
			Plan	   *agg_plan;

			/* We want to iterate over all but the last rollup list elements */
			if (lnext(lc) == NULL)
				break;

			new_grpColIdx = remap_groupColIdx(root, groupClause);

			sort_plan = (Plan *)
				make_sort_from_groupcols(groupClause,
										 new_grpColIdx,
										 subplan);

			agg_plan = (Plan *) make_agg(NIL,
										 NIL,
										 AGG_SORTED,
										 false,
										 true,
									   list_length((List *) linitial(gsets)),
										 new_grpColIdx,
										 extract_grouping_ops(groupClause),
										 gsets,
										 NIL,
										 0,		/* numGroups not needed */
										 sort_plan);

			/*
			 * Nuke stuff we don't need to avoid bloating debug output.
			 */
			sort_plan->targetlist = NIL;
			sort_plan->lefttree = NULL;

			chain = lappend(chain, agg_plan);
		}
	}

	/*
	 * Now make the final Agg node
	 */
	{
		List	   *groupClause = (List *) llast(rollup_groupclauses);
		List	   *gsets = (List *) llast(rollup_lists);
		AttrNumber *top_grpColIdx;
		int			numGroupCols;

		top_grpColIdx = remap_groupColIdx(root, groupClause);

		numGroupCols = list_length((List *) linitial(gsets));

		plan = make_agg(build_path_tlist(root, &best_path->path),
						best_path->qual,
						(numGroupCols > 0) ? AGG_SORTED : AGG_PLAIN,
						false,
						true,
						numGroupCols,
						top_grpColIdx,
						extract_grouping_ops(groupClause),
						gsets,
						chain,
						0,		/* numGroups not needed */
						subplan);

		/* Copy cost data from Path to Plan */
		copy_generic_path_info(&plan->plan, &best_path->path);
	}

	return (Plan *) plan;
}

/*
 * create_minmaxagg_plan
 *
 *	  Create a Result plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 */
static Result *
create_minmaxagg_plan(PlannerInfo *root, MinMaxAggPath *best_path)
{
	Result	   *plan;
	List	   *tlist;
	ListCell   *lc;

	/* Prepare an InitPlan for each aggregate's subquery. */
	foreach(lc, best_path->mmaggregates)
	{
		MinMaxAggInfo *mminfo = (MinMaxAggInfo *) lfirst(lc);
		PlannerInfo *subroot = mminfo->subroot;
		Query	   *subparse = subroot->parse;
		Plan	   *plan;

		/*
		 * Generate the plan for the subquery. We already have a Path, but we
		 * have to convert it to a Plan and attach a LIMIT node above it.
		 * Since we are entering a different planner context (subroot),
		 * recurse to create_plan not create_plan_recurse.
		 */
		plan = create_plan(subroot, mminfo->path);

		plan = (Plan *) make_limit(plan,
								   subparse->limitOffset,
								   subparse->limitCount);

		/* Must apply correct cost/width data to Limit node */
		plan->startup_cost = mminfo->path->startup_cost;
		plan->total_cost = mminfo->pathcost;
		plan->plan_rows = 1;
		plan->plan_width = mminfo->path->pathtarget->width;
		plan->parallel_aware = false;

		/* Convert the plan into an InitPlan in the outer query. */
		SS_make_initplan_from_plan(root, subroot, plan, mminfo->param);
	}

	/* Generate the output plan --- basically just a Result */
	tlist = build_path_tlist(root, &best_path->path);

	plan = make_result(tlist, (Node *) best_path->quals, NULL);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	/*
	 * During setrefs.c, we'll need to replace references to the Agg nodes
	 * with InitPlan output params.  (We can't just do that locally in the
	 * MinMaxAgg node, because path nodes above here may have Agg references
	 * as well.)  Save the mmaggregates list to tell setrefs.c to do that.
	 *
	 * This doesn't work if we're in an inheritance subtree (see notes in
	 * create_modifytable_plan).  Fortunately we can't be because there would
	 * never be aggregates in an UPDATE/DELETE; but let's Assert that.
	 */
	Assert(!root->hasInheritedTarget);
	Assert(root->minmax_aggs == NIL);
	root->minmax_aggs = best_path->mmaggregates;

	return plan;
}

/*
 * create_windowagg_plan
 *
 *	  Create a WindowAgg plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 */
static WindowAgg *
create_windowagg_plan(PlannerInfo *root, WindowAggPath *best_path)
{
	WindowAgg  *plan;
	WindowClause *wc = best_path->winclause;
	Plan	   *subplan;
	List	   *tlist;
	int			numsortkeys;
	AttrNumber *sortColIdx;
	Oid		   *sortOperators;
	Oid		   *collations;
	bool	   *nullsFirst;
	int			partNumCols;
	AttrNumber *partColIdx;
	Oid		   *partOperators;
	int			ordNumCols;
	AttrNumber *ordColIdx;
	Oid		   *ordOperators;

	/*
	 * WindowAgg can project, so no need to be terribly picky about child
	 * tlist, but we do need grouping columns to be available
	 */
	subplan = create_plan_recurse(root, best_path->subpath, CP_LABEL_TLIST);

	tlist = build_path_tlist(root, &best_path->path);

	/*
	 * We shouldn't need to actually sort, but it's convenient to use
	 * prepare_sort_from_pathkeys to identify the input's sort columns.
	 */
	subplan = prepare_sort_from_pathkeys(subplan,
										 best_path->winpathkeys,
										 NULL,
										 NULL,
										 false,
										 &numsortkeys,
										 &sortColIdx,
										 &sortOperators,
										 &collations,
										 &nullsFirst);

	/* Now deconstruct that into partition and ordering portions */
	get_column_info_for_window(root,
							   wc,
							   subplan->targetlist,
							   numsortkeys,
							   sortColIdx,
							   &partNumCols,
							   &partColIdx,
							   &partOperators,
							   &ordNumCols,
							   &ordColIdx,
							   &ordOperators);

	/* And finally we can make the WindowAgg node */
	plan = make_windowagg(tlist,
						  wc->winref,
						  partNumCols,
						  partColIdx,
						  partOperators,
						  ordNumCols,
						  ordColIdx,
						  ordOperators,
						  wc->frameOptions,
						  wc->startOffset,
						  wc->endOffset,
						  subplan);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * get_column_info_for_window
 *		Get the partitioning/ordering column numbers and equality operators
 *		for a WindowAgg node.
 *
 * This depends on the behavior of planner.c's make_pathkeys_for_window!
 *
 * We are given the target WindowClause and an array of the input column
 * numbers associated with the resulting pathkeys.  In the easy case, there
 * are the same number of pathkey columns as partitioning + ordering columns
 * and we just have to copy some data around.  However, it's possible that
 * some of the original partitioning + ordering columns were eliminated as
 * redundant during the transformation to pathkeys.  (This can happen even
 * though the parser gets rid of obvious duplicates.  A typical scenario is a
 * window specification "PARTITION BY x ORDER BY y" coupled with a clause
 * "WHERE x = y" that causes the two sort columns to be recognized as
 * redundant.)	In that unusual case, we have to work a lot harder to
 * determine which keys are significant.
 *
 * The method used here is a bit brute-force: add the sort columns to a list
 * one at a time and note when the resulting pathkey list gets longer.  But
 * it's a sufficiently uncommon case that a faster way doesn't seem worth
 * the amount of code refactoring that'd be needed.
 */
static void
get_column_info_for_window(PlannerInfo *root, WindowClause *wc, List *tlist,
						   int numSortCols, AttrNumber *sortColIdx,
						   int *partNumCols,
						   AttrNumber **partColIdx,
						   Oid **partOperators,
						   int *ordNumCols,
						   AttrNumber **ordColIdx,
						   Oid **ordOperators)
{
	int			numPart = list_length(wc->partitionClause);
	int			numOrder = list_length(wc->orderClause);

	if (numSortCols == numPart + numOrder)
	{
		/* easy case */
		*partNumCols = numPart;
		*partColIdx = sortColIdx;
		*partOperators = extract_grouping_ops(wc->partitionClause);
		*ordNumCols = numOrder;
		*ordColIdx = sortColIdx + numPart;
		*ordOperators = extract_grouping_ops(wc->orderClause);
	}
	else
	{
		List	   *sortclauses;
		List	   *pathkeys;
		int			scidx;
		ListCell   *lc;

		/* first, allocate what's certainly enough space for the arrays */
		*partNumCols = 0;
		*partColIdx = (AttrNumber *) palloc(numPart * sizeof(AttrNumber));
		*partOperators = (Oid *) palloc(numPart * sizeof(Oid));
		*ordNumCols = 0;
		*ordColIdx = (AttrNumber *) palloc(numOrder * sizeof(AttrNumber));
		*ordOperators = (Oid *) palloc(numOrder * sizeof(Oid));
		sortclauses = NIL;
		pathkeys = NIL;
		scidx = 0;
		foreach(lc, wc->partitionClause)
		{
			SortGroupClause *sgc = (SortGroupClause *) lfirst(lc);
			List	   *new_pathkeys;

			sortclauses = lappend(sortclauses, sgc);
			new_pathkeys = make_pathkeys_for_sortclauses(root,
														 sortclauses,
														 tlist);
			if (list_length(new_pathkeys) > list_length(pathkeys))
			{
				/* this sort clause is actually significant */
				(*partColIdx)[*partNumCols] = sortColIdx[scidx++];
				(*partOperators)[*partNumCols] = sgc->eqop;
				(*partNumCols)++;
				pathkeys = new_pathkeys;
			}
		}
		foreach(lc, wc->orderClause)
		{
			SortGroupClause *sgc = (SortGroupClause *) lfirst(lc);
			List	   *new_pathkeys;

			sortclauses = lappend(sortclauses, sgc);
			new_pathkeys = make_pathkeys_for_sortclauses(root,
														 sortclauses,
														 tlist);
			if (list_length(new_pathkeys) > list_length(pathkeys))
			{
				/* this sort clause is actually significant */
				(*ordColIdx)[*ordNumCols] = sortColIdx[scidx++];
				(*ordOperators)[*ordNumCols] = sgc->eqop;
				(*ordNumCols)++;
				pathkeys = new_pathkeys;
			}
		}
		/* complain if we didn't eat exactly the right number of sort cols */
		if (scidx != numSortCols)
			elog(ERROR, "failed to deconstruct sort operators into partitioning/ordering operators");
	}
}

/*
 * create_setop_plan
 *
 *	  Create a SetOp plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 */
static SetOp *
create_setop_plan(PlannerInfo *root, SetOpPath *best_path, int flags)
{
	SetOp	   *plan;
	Plan	   *subplan;
	long		numGroups;

	/*
	 * SetOp doesn't project, so tlist requirements pass through; moreover we
	 * need grouping columns to be labeled.
	 */
	subplan = create_plan_recurse(root, best_path->subpath,
								  flags | CP_LABEL_TLIST);

	/* Convert numGroups to long int --- but 'ware overflow! */
	numGroups = (long) Min(best_path->numGroups, (double) LONG_MAX);

	plan = make_setop(best_path->cmd,
					  best_path->strategy,
					  subplan,
					  best_path->distinctList,
					  best_path->flagColIdx,
					  best_path->firstFlag,
					  numGroups);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * create_recursiveunion_plan
 *
 *	  Create a RecursiveUnion plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 */
static RecursiveUnion *
create_recursiveunion_plan(PlannerInfo *root, RecursiveUnionPath *best_path)
{
	RecursiveUnion *plan;
	Plan	   *leftplan;
	Plan	   *rightplan;
	List	   *tlist;
	long		numGroups;

	/* Need both children to produce same tlist, so force it */
	leftplan = create_plan_recurse(root, best_path->leftpath, CP_EXACT_TLIST);
	rightplan = create_plan_recurse(root, best_path->rightpath, CP_EXACT_TLIST);

	tlist = build_path_tlist(root, &best_path->path);

	/* Convert numGroups to long int --- but 'ware overflow! */
	numGroups = (long) Min(best_path->numGroups, (double) LONG_MAX);

	plan = make_recursive_union(tlist,
								leftplan,
								rightplan,
								best_path->wtParam,
								best_path->distinctList,
								numGroups);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * create_lockrows_plan
 *
 *	  Create a LockRows plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 */
static LockRows *
create_lockrows_plan(PlannerInfo *root, LockRowsPath *best_path,
					 int flags)
{
	LockRows   *plan;
	Plan	   *subplan;

	/* LockRows doesn't project, so tlist requirements pass through */
	subplan = create_plan_recurse(root, best_path->subpath, flags);

	plan = make_lockrows(subplan, best_path->rowMarks, best_path->epqParam);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * create_modifytable_plan
 *	  Create a ModifyTable plan for 'best_path'.
 *
 *	  Returns a Plan node.
 */
static ModifyTable *
create_modifytable_plan(PlannerInfo *root, ModifyTablePath *best_path)
{
	ModifyTable *plan;
	List	   *subplans = NIL;
	ListCell   *subpaths,
			   *subroots;

	/* Build the plan for each input path */
	forboth(subpaths, best_path->subpaths,
			subroots, best_path->subroots)
	{
		Path	   *subpath = (Path *) lfirst(subpaths);
		PlannerInfo *subroot = (PlannerInfo *) lfirst(subroots);
		Plan	   *subplan;

		/*
		 * In an inherited UPDATE/DELETE, reference the per-child modified
		 * subroot while creating Plans from Paths for the child rel.  This is
		 * a kluge, but otherwise it's too hard to ensure that Plan creation
		 * functions (particularly in FDWs) don't depend on the contents of
		 * "root" matching what they saw at Path creation time.  The main
		 * downside is that creation functions for Plans that might appear
		 * below a ModifyTable cannot expect to modify the contents of "root"
		 * and have it "stick" for subsequent processing such as setrefs.c.
		 * That's not great, but it seems better than the alternative.
		 */
		subplan = create_plan_recurse(subroot, subpath, CP_EXACT_TLIST);

		/* Transfer resname/resjunk labeling, too, to keep executor happy */
		apply_tlist_labeling(subplan->targetlist, subroot->processed_tlist);

		subplans = lappend(subplans, subplan);
	}

	plan = make_modifytable(root,
							best_path->operation,
							best_path->canSetTag,
							best_path->nominalRelation,
							best_path->resultRelations,
							subplans,
							best_path->withCheckOptionLists,
							best_path->returningLists,
							best_path->rowMarks,
							best_path->onconflict,
							best_path->epqParam);

	copy_generic_path_info(&plan->plan, &best_path->path);

	return plan;
}

/*
 * create_limit_plan
 *
 *	  Create a Limit plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 */
static Limit *
create_limit_plan(PlannerInfo *root, LimitPath *best_path, int flags)
{
	Limit	   *plan;
	Plan	   *subplan;

	/* Limit doesn't project, so tlist requirements pass through */
	subplan = create_plan_recurse(root, best_path->subpath, flags);

	plan = make_limit(subplan,
					  best_path->limitOffset,
					  best_path->limitCount);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}


/*****************************************************************************
 *
 *	BASE-RELATION SCAN METHODS
 *
 *****************************************************************************/


/*
 * create_seqscan_plan
 *	 Returns a seqscan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static SeqScan *
create_seqscan_plan(PlannerInfo *root, Path *best_path,
					List *tlist, List *scan_clauses)
{
	SeqScan    *scan_plan;
	Index		scan_relid = best_path->parent->relid;

	/* it should be a base rel... */
	Assert(scan_relid > 0);
	Assert(best_path->parent->rtekind == RTE_RELATION);

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* Replace any outer-relation variables with nestloop params */
	if (best_path->param_info)
	{
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
	}

	scan_plan = make_seqscan(tlist,
							 scan_clauses,
							 scan_relid);

	copy_generic_path_info(&scan_plan->plan, best_path);

	return scan_plan;
}

/*
 * create_samplescan_plan
 *	 Returns a samplescan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static SampleScan *
create_samplescan_plan(PlannerInfo *root, Path *best_path,
					   List *tlist, List *scan_clauses)
{
	SampleScan *scan_plan;
	Index		scan_relid = best_path->parent->relid;
	RangeTblEntry *rte;
	TableSampleClause *tsc;

	/* it should be a base rel with a tablesample clause... */
	Assert(scan_relid > 0);
	rte = planner_rt_fetch(scan_relid, root);
	Assert(rte->rtekind == RTE_RELATION);
	tsc = rte->tablesample;
	Assert(tsc != NULL);

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* Replace any outer-relation variables with nestloop params */
	if (best_path->param_info)
	{
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
		tsc = (TableSampleClause *)
			replace_nestloop_params(root, (Node *) tsc);
	}

	scan_plan = make_samplescan(tlist,
								scan_clauses,
								scan_relid,
								tsc);

	copy_generic_path_info(&scan_plan->scan.plan, best_path);

	return scan_plan;
}

/*
 * create_indexscan_plan
 *	  Returns an indexscan plan for the base relation scanned by 'best_path'
 *	  with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 *
 * We use this for both plain IndexScans and IndexOnlyScans, because the
 * qual preprocessing work is the same for both.  Note that the caller tells
 * us which to build --- we don't look at best_path->path.pathtype, because
 * create_bitmap_subplan needs to be able to override the prior decision.
 */
static Scan *
create_indexscan_plan(PlannerInfo *root,
					  IndexPath *best_path,
					  List *tlist,
					  List *scan_clauses,
					  bool indexonly)
{
	Scan	   *scan_plan;
	List	   *indexquals = best_path->indexquals;
	List	   *indexorderbys = best_path->indexorderbys;
	Index		baserelid = best_path->path.parent->relid;
	Oid			indexoid = best_path->indexinfo->indexoid;
	List	   *qpqual;
	List	   *stripped_indexquals;
	List	   *fixed_indexquals;
	List	   *fixed_indexorderbys;
	List	   *indexorderbyops = NIL;
	ListCell   *l;

	/* it should be a base rel... */
	Assert(baserelid > 0);
	Assert(best_path->path.parent->rtekind == RTE_RELATION);

	/*
	 * Build "stripped" indexquals structure (no RestrictInfos) to pass to
	 * executor as indexqualorig
	 */
	stripped_indexquals = get_actual_clauses(indexquals);

	/*
	 * The executor needs a copy with the indexkey on the left of each clause
	 * and with index Vars substituted for table ones.
	 */
	fixed_indexquals = fix_indexqual_references(root, best_path);

	/*
	 * Likewise fix up index attr references in the ORDER BY expressions.
	 */
	fixed_indexorderbys = fix_indexorderby_references(root, best_path);

	/*
	 * The qpqual list must contain all restrictions not automatically handled
	 * by the index, other than pseudoconstant clauses which will be handled
	 * by a separate gating plan node.  All the predicates in the indexquals
	 * will be checked (either by the index itself, or by nodeIndexscan.c),
	 * but if there are any "special" operators involved then they must be
	 * included in qpqual.  The upshot is that qpqual must contain
	 * scan_clauses minus whatever appears in indexquals.
	 *
	 * In normal cases simple pointer equality checks will be enough to spot
	 * duplicate RestrictInfos, so we try that first.
	 *
	 * Another common case is that a scan_clauses entry is generated from the
	 * same EquivalenceClass as some indexqual, and is therefore redundant
	 * with it, though not equal.  (This happens when indxpath.c prefers a
	 * different derived equality than what generate_join_implied_equalities
	 * picked for a parameterized scan's ppi_clauses.)
	 *
	 * In some situations (particularly with OR'd index conditions) we may
	 * have scan_clauses that are not equal to, but are logically implied by,
	 * the index quals; so we also try a predicate_implied_by() check to see
	 * if we can discard quals that way.  (predicate_implied_by assumes its
	 * first input contains only immutable functions, so we have to check
	 * that.)
	 *
	 * We can also discard quals that are implied by a partial index's
	 * predicate, but only in a plain SELECT; when scanning a target relation
	 * of UPDATE/DELETE/SELECT FOR UPDATE, we must leave such quals in the
	 * plan so that they'll be properly rechecked by EvalPlanQual testing.
	 *
	 * Note: if you change this bit of code you should also look at
	 * extract_nonindex_conditions() in costsize.c.
	 */
	qpqual = NIL;
	foreach(l, scan_clauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

		Assert(IsA(rinfo, RestrictInfo));
		if (rinfo->pseudoconstant)
			continue;			/* we may drop pseudoconstants here */
		if (list_member_ptr(indexquals, rinfo))
			continue;			/* simple duplicate */
		if (is_redundant_derived_clause(rinfo, indexquals))
			continue;			/* derived from same EquivalenceClass */
		if (!contain_mutable_functions((Node *) rinfo->clause))
		{
			List	   *clausel = list_make1(rinfo->clause);

			if (predicate_implied_by(clausel, indexquals))
				continue;		/* provably implied by indexquals */
			if (best_path->indexinfo->indpred)
			{
				if (baserelid != root->parse->resultRelation &&
					get_plan_rowmark(root->rowMarks, baserelid) == NULL)
					if (predicate_implied_by(clausel,
											 best_path->indexinfo->indpred))
						continue;		/* implied by index predicate */
			}
		}
		qpqual = lappend(qpqual, rinfo);
	}

	/* Sort clauses into best execution order */
	qpqual = order_qual_clauses(root, qpqual);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	qpqual = extract_actual_clauses(qpqual, false);

	/*
	 * We have to replace any outer-relation variables with nestloop params in
	 * the indexqualorig, qpqual, and indexorderbyorig expressions.  A bit
	 * annoying to have to do this separately from the processing in
	 * fix_indexqual_references --- rethink this when generalizing the inner
	 * indexscan support.  But note we can't really do this earlier because
	 * it'd break the comparisons to predicates above ... (or would it?  Those
	 * wouldn't have outer refs)
	 */
	if (best_path->path.param_info)
	{
		stripped_indexquals = (List *)
			replace_nestloop_params(root, (Node *) stripped_indexquals);
		qpqual = (List *)
			replace_nestloop_params(root, (Node *) qpqual);
		indexorderbys = (List *)
			replace_nestloop_params(root, (Node *) indexorderbys);
	}

	/*
	 * If there are ORDER BY expressions, look up the sort operators for their
	 * result datatypes.
	 */
	if (indexorderbys)
	{
		ListCell   *pathkeyCell,
				   *exprCell;

		/*
		 * PathKey contains OID of the btree opfamily we're sorting by, but
		 * that's not quite enough because we need the expression's datatype
		 * to look up the sort operator in the operator family.
		 */
		Assert(list_length(best_path->path.pathkeys) == list_length(indexorderbys));
		forboth(pathkeyCell, best_path->path.pathkeys, exprCell, indexorderbys)
		{
			PathKey    *pathkey = (PathKey *) lfirst(pathkeyCell);
			Node	   *expr = (Node *) lfirst(exprCell);
			Oid			exprtype = exprType(expr);
			Oid			sortop;

			/* Get sort operator from opfamily */
			sortop = get_opfamily_member(pathkey->pk_opfamily,
										 exprtype,
										 exprtype,
										 pathkey->pk_strategy);
			if (!OidIsValid(sortop))
				elog(ERROR, "failed to find sort operator for ORDER BY expression");
			indexorderbyops = lappend_oid(indexorderbyops, sortop);
		}
	}

	/* Finally ready to build the plan node */
	if (indexonly)
		scan_plan = (Scan *) make_indexonlyscan(tlist,
												qpqual,
												baserelid,
												indexoid,
												fixed_indexquals,
												fixed_indexorderbys,
											best_path->indexinfo->indextlist,
												best_path->indexscandir);
	else
		scan_plan = (Scan *) make_indexscan(tlist,
											qpqual,
											baserelid,
											indexoid,
											fixed_indexquals,
											stripped_indexquals,
											fixed_indexorderbys,
											indexorderbys,
											indexorderbyops,
											best_path->indexscandir);

	copy_generic_path_info(&scan_plan->plan, &best_path->path);

	return scan_plan;
}

/*
 * create_bitmap_scan_plan
 *	  Returns a bitmap scan plan for the base relation scanned by 'best_path'
 *	  with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static BitmapHeapScan *
create_bitmap_scan_plan(PlannerInfo *root,
						BitmapHeapPath *best_path,
						List *tlist,
						List *scan_clauses)
{
	Index		baserelid = best_path->path.parent->relid;
	Plan	   *bitmapqualplan;
	List	   *bitmapqualorig;
	List	   *indexquals;
	List	   *indexECs;
	List	   *qpqual;
	ListCell   *l;
	BitmapHeapScan *scan_plan;

	/* it should be a base rel... */
	Assert(baserelid > 0);
	Assert(best_path->path.parent->rtekind == RTE_RELATION);

	/* Process the bitmapqual tree into a Plan tree and qual lists */
	bitmapqualplan = create_bitmap_subplan(root, best_path->bitmapqual,
										   &bitmapqualorig, &indexquals,
										   &indexECs);

	/*
	 * The qpqual list must contain all restrictions not automatically handled
	 * by the index, other than pseudoconstant clauses which will be handled
	 * by a separate gating plan node.  All the predicates in the indexquals
	 * will be checked (either by the index itself, or by
	 * nodeBitmapHeapscan.c), but if there are any "special" operators
	 * involved then they must be added to qpqual.  The upshot is that qpqual
	 * must contain scan_clauses minus whatever appears in indexquals.
	 *
	 * This loop is similar to the comparable code in create_indexscan_plan(),
	 * but with some differences because it has to compare the scan clauses to
	 * stripped (no RestrictInfos) indexquals.  See comments there for more
	 * info.
	 *
	 * In normal cases simple equal() checks will be enough to spot duplicate
	 * clauses, so we try that first.  We next see if the scan clause is
	 * redundant with any top-level indexqual by virtue of being generated
	 * from the same EC.  After that, try predicate_implied_by().
	 *
	 * Unlike create_indexscan_plan(), we need take no special thought here
	 * for partial index predicates; this is because the predicate conditions
	 * are already listed in bitmapqualorig and indexquals.  Bitmap scans have
	 * to do it that way because predicate conditions need to be rechecked if
	 * the scan becomes lossy, so they have to be included in bitmapqualorig.
	 */
	qpqual = NIL;
	foreach(l, scan_clauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);
		Node	   *clause = (Node *) rinfo->clause;

		Assert(IsA(rinfo, RestrictInfo));
		if (rinfo->pseudoconstant)
			continue;			/* we may drop pseudoconstants here */
		if (list_member(indexquals, clause))
			continue;			/* simple duplicate */
		if (rinfo->parent_ec && list_member_ptr(indexECs, rinfo->parent_ec))
			continue;			/* derived from same EquivalenceClass */
		if (!contain_mutable_functions(clause))
		{
			List	   *clausel = list_make1(clause);

			if (predicate_implied_by(clausel, indexquals))
				continue;		/* provably implied by indexquals */
		}
		qpqual = lappend(qpqual, rinfo);
	}

	/* Sort clauses into best execution order */
	qpqual = order_qual_clauses(root, qpqual);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	qpqual = extract_actual_clauses(qpqual, false);

	/*
	 * When dealing with special operators, we will at this point have
	 * duplicate clauses in qpqual and bitmapqualorig.  We may as well drop
	 * 'em from bitmapqualorig, since there's no point in making the tests
	 * twice.
	 */
	bitmapqualorig = list_difference_ptr(bitmapqualorig, qpqual);

	/*
	 * We have to replace any outer-relation variables with nestloop params in
	 * the qpqual and bitmapqualorig expressions.  (This was already done for
	 * expressions attached to plan nodes in the bitmapqualplan tree.)
	 */
	if (best_path->path.param_info)
	{
		qpqual = (List *)
			replace_nestloop_params(root, (Node *) qpqual);
		bitmapqualorig = (List *)
			replace_nestloop_params(root, (Node *) bitmapqualorig);
	}

	/* Finally ready to build the plan node */
	scan_plan = make_bitmap_heapscan(tlist,
									 qpqual,
									 bitmapqualplan,
									 bitmapqualorig,
									 baserelid);

	copy_generic_path_info(&scan_plan->scan.plan, &best_path->path);

	return scan_plan;
}

/*
 * Given a bitmapqual tree, generate the Plan tree that implements it
 *
 * As byproducts, we also return in *qual and *indexqual the qual lists
 * (in implicit-AND form, without RestrictInfos) describing the original index
 * conditions and the generated indexqual conditions.  (These are the same in
 * simple cases, but when special index operators are involved, the former
 * list includes the special conditions while the latter includes the actual
 * indexable conditions derived from them.)  Both lists include partial-index
 * predicates, because we have to recheck predicates as well as index
 * conditions if the bitmap scan becomes lossy.
 *
 * In addition, we return a list of EquivalenceClass pointers for all the
 * top-level indexquals that were possibly-redundantly derived from ECs.
 * This allows removal of scan_clauses that are redundant with such quals.
 * (We do not attempt to detect such redundancies for quals that are within
 * OR subtrees.  This could be done in a less hacky way if we returned the
 * indexquals in RestrictInfo form, but that would be slower and still pretty
 * messy, since we'd have to build new RestrictInfos in many cases.)
 */
static Plan *
create_bitmap_subplan(PlannerInfo *root, Path *bitmapqual,
					  List **qual, List **indexqual, List **indexECs)
{
	Plan	   *plan;

	if (IsA(bitmapqual, BitmapAndPath))
	{
		BitmapAndPath *apath = (BitmapAndPath *) bitmapqual;
		List	   *subplans = NIL;
		List	   *subquals = NIL;
		List	   *subindexquals = NIL;
		List	   *subindexECs = NIL;
		ListCell   *l;

		/*
		 * There may well be redundant quals among the subplans, since a
		 * top-level WHERE qual might have gotten used to form several
		 * different index quals.  We don't try exceedingly hard to eliminate
		 * redundancies, but we do eliminate obvious duplicates by using
		 * list_concat_unique.
		 */
		foreach(l, apath->bitmapquals)
		{
			Plan	   *subplan;
			List	   *subqual;
			List	   *subindexqual;
			List	   *subindexEC;

			subplan = create_bitmap_subplan(root, (Path *) lfirst(l),
											&subqual, &subindexqual,
											&subindexEC);
			subplans = lappend(subplans, subplan);
			subquals = list_concat_unique(subquals, subqual);
			subindexquals = list_concat_unique(subindexquals, subindexqual);
			/* Duplicates in indexECs aren't worth getting rid of */
			subindexECs = list_concat(subindexECs, subindexEC);
		}
		plan = (Plan *) make_bitmap_and(subplans);
		plan->startup_cost = apath->path.startup_cost;
		plan->total_cost = apath->path.total_cost;
		plan->plan_rows =
			clamp_row_est(apath->bitmapselectivity * apath->path.parent->tuples);
		plan->plan_width = 0;	/* meaningless */
		plan->parallel_aware = false;
		*qual = subquals;
		*indexqual = subindexquals;
		*indexECs = subindexECs;
	}
	else if (IsA(bitmapqual, BitmapOrPath))
	{
		BitmapOrPath *opath = (BitmapOrPath *) bitmapqual;
		List	   *subplans = NIL;
		List	   *subquals = NIL;
		List	   *subindexquals = NIL;
		bool		const_true_subqual = false;
		bool		const_true_subindexqual = false;
		ListCell   *l;

		/*
		 * Here, we only detect qual-free subplans.  A qual-free subplan would
		 * cause us to generate "... OR true ..."  which we may as well reduce
		 * to just "true".  We do not try to eliminate redundant subclauses
		 * because (a) it's not as likely as in the AND case, and (b) we might
		 * well be working with hundreds or even thousands of OR conditions,
		 * perhaps from a long IN list.  The performance of list_append_unique
		 * would be unacceptable.
		 */
		foreach(l, opath->bitmapquals)
		{
			Plan	   *subplan;
			List	   *subqual;
			List	   *subindexqual;
			List	   *subindexEC;

			subplan = create_bitmap_subplan(root, (Path *) lfirst(l),
											&subqual, &subindexqual,
											&subindexEC);
			subplans = lappend(subplans, subplan);
			if (subqual == NIL)
				const_true_subqual = true;
			else if (!const_true_subqual)
				subquals = lappend(subquals,
								   make_ands_explicit(subqual));
			if (subindexqual == NIL)
				const_true_subindexqual = true;
			else if (!const_true_subindexqual)
				subindexquals = lappend(subindexquals,
										make_ands_explicit(subindexqual));
		}

		/*
		 * In the presence of ScalarArrayOpExpr quals, we might have built
		 * BitmapOrPaths with just one subpath; don't add an OR step.
		 */
		if (list_length(subplans) == 1)
		{
			plan = (Plan *) linitial(subplans);
		}
		else
		{
			plan = (Plan *) make_bitmap_or(subplans);
			plan->startup_cost = opath->path.startup_cost;
			plan->total_cost = opath->path.total_cost;
			plan->plan_rows =
				clamp_row_est(opath->bitmapselectivity * opath->path.parent->tuples);
			plan->plan_width = 0;		/* meaningless */
			plan->parallel_aware = false;
		}

		/*
		 * If there were constant-TRUE subquals, the OR reduces to constant
		 * TRUE.  Also, avoid generating one-element ORs, which could happen
		 * due to redundancy elimination or ScalarArrayOpExpr quals.
		 */
		if (const_true_subqual)
			*qual = NIL;
		else if (list_length(subquals) <= 1)
			*qual = subquals;
		else
			*qual = list_make1(make_orclause(subquals));
		if (const_true_subindexqual)
			*indexqual = NIL;
		else if (list_length(subindexquals) <= 1)
			*indexqual = subindexquals;
		else
			*indexqual = list_make1(make_orclause(subindexquals));
		*indexECs = NIL;
	}
	else if (IsA(bitmapqual, IndexPath))
	{
		IndexPath  *ipath = (IndexPath *) bitmapqual;
		IndexScan  *iscan;
		List	   *subindexECs;
		ListCell   *l;

		/* Use the regular indexscan plan build machinery... */
		iscan = (IndexScan *) create_indexscan_plan(root, ipath,
													NIL, NIL, false);
		Assert(IsA(iscan, IndexScan));
		/* then convert to a bitmap indexscan */
		plan = (Plan *) make_bitmap_indexscan(iscan->scan.scanrelid,
											  iscan->indexid,
											  iscan->indexqual,
											  iscan->indexqualorig);
		/* and set its cost/width fields appropriately */
		plan->startup_cost = 0.0;
		plan->total_cost = ipath->indextotalcost;
		plan->plan_rows =
			clamp_row_est(ipath->indexselectivity * ipath->path.parent->tuples);
		plan->plan_width = 0;	/* meaningless */
		plan->parallel_aware = false;
		*qual = get_actual_clauses(ipath->indexclauses);
		*indexqual = get_actual_clauses(ipath->indexquals);
		foreach(l, ipath->indexinfo->indpred)
		{
			Expr	   *pred = (Expr *) lfirst(l);

			/*
			 * We know that the index predicate must have been implied by the
			 * query condition as a whole, but it may or may not be implied by
			 * the conditions that got pushed into the bitmapqual.  Avoid
			 * generating redundant conditions.
			 */
			if (!predicate_implied_by(list_make1(pred), ipath->indexclauses))
			{
				*qual = lappend(*qual, pred);
				*indexqual = lappend(*indexqual, pred);
			}
		}
		subindexECs = NIL;
		foreach(l, ipath->indexquals)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

			if (rinfo->parent_ec)
				subindexECs = lappend(subindexECs, rinfo->parent_ec);
		}
		*indexECs = subindexECs;
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d", nodeTag(bitmapqual));
		plan = NULL;			/* keep compiler quiet */
	}

	return plan;
}

/*
 * create_tidscan_plan
 *	 Returns a tidscan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static TidScan *
create_tidscan_plan(PlannerInfo *root, TidPath *best_path,
					List *tlist, List *scan_clauses)
{
	TidScan    *scan_plan;
	Index		scan_relid = best_path->path.parent->relid;
	List	   *tidquals = best_path->tidquals;
	List	   *ortidquals;

	/* it should be a base rel... */
	Assert(scan_relid > 0);
	Assert(best_path->path.parent->rtekind == RTE_RELATION);

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* Replace any outer-relation variables with nestloop params */
	if (best_path->path.param_info)
	{
		tidquals = (List *)
			replace_nestloop_params(root, (Node *) tidquals);
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
	}

	/*
	 * Remove any clauses that are TID quals.  This is a bit tricky since the
	 * tidquals list has implicit OR semantics.
	 */
	ortidquals = tidquals;
	if (list_length(ortidquals) > 1)
		ortidquals = list_make1(make_orclause(ortidquals));
	scan_clauses = list_difference(scan_clauses, ortidquals);

	scan_plan = make_tidscan(tlist,
							 scan_clauses,
							 scan_relid,
							 tidquals);

	copy_generic_path_info(&scan_plan->scan.plan, &best_path->path);

	return scan_plan;
}

/*
 * create_subqueryscan_plan
 *	 Returns a subqueryscan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static SubqueryScan *
create_subqueryscan_plan(PlannerInfo *root, SubqueryScanPath *best_path,
						 List *tlist, List *scan_clauses)
{
	SubqueryScan *scan_plan;
	RelOptInfo *rel = best_path->path.parent;
	Index		scan_relid = rel->relid;
	Plan	   *subplan;

	/* it should be a subquery base rel... */
	Assert(scan_relid > 0);
	Assert(rel->rtekind == RTE_SUBQUERY);

	/*
	 * Recursively create Plan from Path for subquery.  Since we are entering
	 * a different planner context (subroot), recurse to create_plan not
	 * create_plan_recurse.
	 */
	subplan = create_plan(rel->subroot, best_path->subpath);

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* Replace any outer-relation variables with nestloop params */
	if (best_path->path.param_info)
	{
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
		process_subquery_nestloop_params(root,
										 rel->subplan_params);
	}

	scan_plan = make_subqueryscan(tlist,
								  scan_clauses,
								  scan_relid,
								  subplan);

	copy_generic_path_info(&scan_plan->scan.plan, &best_path->path);

	return scan_plan;
}

/*
 * create_functionscan_plan
 *	 Returns a functionscan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static FunctionScan *
create_functionscan_plan(PlannerInfo *root, Path *best_path,
						 List *tlist, List *scan_clauses)
{
	FunctionScan *scan_plan;
	Index		scan_relid = best_path->parent->relid;
	RangeTblEntry *rte;
	List	   *functions;

	/* it should be a function base rel... */
	Assert(scan_relid > 0);
	rte = planner_rt_fetch(scan_relid, root);
	Assert(rte->rtekind == RTE_FUNCTION);
	functions = rte->functions;

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* Replace any outer-relation variables with nestloop params */
	if (best_path->param_info)
	{
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
		/* The function expressions could contain nestloop params, too */
		functions = (List *) replace_nestloop_params(root, (Node *) functions);
	}

	scan_plan = make_functionscan(tlist, scan_clauses, scan_relid,
								  functions, rte->funcordinality);

	copy_generic_path_info(&scan_plan->scan.plan, best_path);

	return scan_plan;
}

/*
 * create_valuesscan_plan
 *	 Returns a valuesscan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static ValuesScan *
create_valuesscan_plan(PlannerInfo *root, Path *best_path,
					   List *tlist, List *scan_clauses)
{
	ValuesScan *scan_plan;
	Index		scan_relid = best_path->parent->relid;
	RangeTblEntry *rte;
	List	   *values_lists;

	/* it should be a values base rel... */
	Assert(scan_relid > 0);
	rte = planner_rt_fetch(scan_relid, root);
	Assert(rte->rtekind == RTE_VALUES);
	values_lists = rte->values_lists;

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* Replace any outer-relation variables with nestloop params */
	if (best_path->param_info)
	{
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
		/* The values lists could contain nestloop params, too */
		values_lists = (List *)
			replace_nestloop_params(root, (Node *) values_lists);
	}

	scan_plan = make_valuesscan(tlist, scan_clauses, scan_relid,
								values_lists);

	copy_generic_path_info(&scan_plan->scan.plan, best_path);

	return scan_plan;
}

/*
 * create_ctescan_plan
 *	 Returns a ctescan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static CteScan *
create_ctescan_plan(PlannerInfo *root, Path *best_path,
					List *tlist, List *scan_clauses)
{
	CteScan    *scan_plan;
	Index		scan_relid = best_path->parent->relid;
	RangeTblEntry *rte;
	SubPlan    *ctesplan = NULL;
	int			plan_id;
	int			cte_param_id;
	PlannerInfo *cteroot;
	Index		levelsup;
	int			ndx;
	ListCell   *lc;

	Assert(scan_relid > 0);
	rte = planner_rt_fetch(scan_relid, root);
	Assert(rte->rtekind == RTE_CTE);
	Assert(!rte->self_reference);

	/*
	 * Find the referenced CTE, and locate the SubPlan previously made for it.
	 */
	levelsup = rte->ctelevelsup;
	cteroot = root;
	while (levelsup-- > 0)
	{
		cteroot = cteroot->parent_root;
		if (!cteroot)			/* shouldn't happen */
			elog(ERROR, "bad levelsup for CTE \"%s\"", rte->ctename);
	}

	/*
	 * Note: cte_plan_ids can be shorter than cteList, if we are still working
	 * on planning the CTEs (ie, this is a side-reference from another CTE).
	 * So we mustn't use forboth here.
	 */
	ndx = 0;
	foreach(lc, cteroot->parse->cteList)
	{
		CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);

		if (strcmp(cte->ctename, rte->ctename) == 0)
			break;
		ndx++;
	}
	if (lc == NULL)				/* shouldn't happen */
		elog(ERROR, "could not find CTE \"%s\"", rte->ctename);
	if (ndx >= list_length(cteroot->cte_plan_ids))
		elog(ERROR, "could not find plan for CTE \"%s\"", rte->ctename);
	plan_id = list_nth_int(cteroot->cte_plan_ids, ndx);
	Assert(plan_id > 0);
	foreach(lc, cteroot->init_plans)
	{
		ctesplan = (SubPlan *) lfirst(lc);
		if (ctesplan->plan_id == plan_id)
			break;
	}
	if (lc == NULL)				/* shouldn't happen */
		elog(ERROR, "could not find plan for CTE \"%s\"", rte->ctename);

	/*
	 * We need the CTE param ID, which is the sole member of the SubPlan's
	 * setParam list.
	 */
	cte_param_id = linitial_int(ctesplan->setParam);

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* Replace any outer-relation variables with nestloop params */
	if (best_path->param_info)
	{
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
	}

	scan_plan = make_ctescan(tlist, scan_clauses, scan_relid,
							 plan_id, cte_param_id);

	copy_generic_path_info(&scan_plan->scan.plan, best_path);

	return scan_plan;
}

/*
 * create_worktablescan_plan
 *	 Returns a worktablescan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static WorkTableScan *
create_worktablescan_plan(PlannerInfo *root, Path *best_path,
						  List *tlist, List *scan_clauses)
{
	WorkTableScan *scan_plan;
	Index		scan_relid = best_path->parent->relid;
	RangeTblEntry *rte;
	Index		levelsup;
	PlannerInfo *cteroot;

	Assert(scan_relid > 0);
	rte = planner_rt_fetch(scan_relid, root);
	Assert(rte->rtekind == RTE_CTE);
	Assert(rte->self_reference);

	/*
	 * We need to find the worktable param ID, which is in the plan level
	 * that's processing the recursive UNION, which is one level *below* where
	 * the CTE comes from.
	 */
	levelsup = rte->ctelevelsup;
	if (levelsup == 0)			/* shouldn't happen */
		elog(ERROR, "bad levelsup for CTE \"%s\"", rte->ctename);
	levelsup--;
	cteroot = root;
	while (levelsup-- > 0)
	{
		cteroot = cteroot->parent_root;
		if (!cteroot)			/* shouldn't happen */
			elog(ERROR, "bad levelsup for CTE \"%s\"", rte->ctename);
	}
	if (cteroot->wt_param_id < 0)		/* shouldn't happen */
		elog(ERROR, "could not find param ID for CTE \"%s\"", rte->ctename);

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* Replace any outer-relation variables with nestloop params */
	if (best_path->param_info)
	{
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
	}

	scan_plan = make_worktablescan(tlist, scan_clauses, scan_relid,
								   cteroot->wt_param_id);

	copy_generic_path_info(&scan_plan->scan.plan, best_path);

	return scan_plan;
}

/*
 * create_foreignscan_plan
 *	 Returns a foreignscan plan for the relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static ForeignScan *
create_foreignscan_plan(PlannerInfo *root, ForeignPath *best_path,
						List *tlist, List *scan_clauses)
{
	ForeignScan *scan_plan;
	RelOptInfo *rel = best_path->path.parent;
	Index		scan_relid = rel->relid;
	Oid			rel_oid = InvalidOid;
	Plan	   *outer_plan = NULL;

	Assert(rel->fdwroutine != NULL);

	/* transform the child path if any */
	if (best_path->fdw_outerpath)
		outer_plan = create_plan_recurse(root, best_path->fdw_outerpath,
										 CP_EXACT_TLIST);

	/*
	 * If we're scanning a base relation, fetch its OID.  (Irrelevant if
	 * scanning a join relation.)
	 */
	if (scan_relid > 0)
	{
		RangeTblEntry *rte;

		Assert(rel->rtekind == RTE_RELATION);
		rte = planner_rt_fetch(scan_relid, root);
		Assert(rte->rtekind == RTE_RELATION);
		rel_oid = rte->relid;
	}

	/*
	 * Sort clauses into best execution order.  We do this first since the FDW
	 * might have more info than we do and wish to adjust the ordering.
	 */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/*
	 * Let the FDW perform its processing on the restriction clauses and
	 * generate the plan node.  Note that the FDW might remove restriction
	 * clauses that it intends to execute remotely, or even add more (if it
	 * has selected some join clauses for remote use but also wants them
	 * rechecked locally).
	 */
	scan_plan = rel->fdwroutine->GetForeignPlan(root, rel, rel_oid,
												best_path,
												tlist, scan_clauses,
												outer_plan);

	/* Copy cost data from Path to Plan; no need to make FDW do this */
	copy_generic_path_info(&scan_plan->scan.plan, &best_path->path);

	/* Copy foreign server OID; likewise, no need to make FDW do this */
	scan_plan->fs_server = rel->serverid;

	/* Likewise, copy the relids that are represented by this foreign scan */
	scan_plan->fs_relids = best_path->path.parent->relids;

	/*
	 * If a join between foreign relations was pushed down, remember it. The
	 * push-down safety of the join depends upon the server and user mapping
	 * being same. That can change between planning and execution time, in which
	 * case the plan should be invalidated.
	 */
	if (scan_relid == 0)
		root->glob->hasForeignJoin = true;

	/*
	 * Replace any outer-relation variables with nestloop params in the qual,
	 * fdw_exprs and fdw_recheck_quals expressions.  We do this last so that
	 * the FDW doesn't have to be involved.  (Note that parts of fdw_exprs
	 * or fdw_recheck_quals could have come from join clauses, so doing this
	 * beforehand on the scan_clauses wouldn't work.)  We assume
	 * fdw_scan_tlist contains no such variables.
	 */
	if (best_path->path.param_info)
	{
		scan_plan->scan.plan.qual = (List *)
			replace_nestloop_params(root, (Node *) scan_plan->scan.plan.qual);
		scan_plan->fdw_exprs = (List *)
			replace_nestloop_params(root, (Node *) scan_plan->fdw_exprs);
		scan_plan->fdw_recheck_quals = (List *)
			replace_nestloop_params(root,
									(Node *) scan_plan->fdw_recheck_quals);
	}

	/*
	 * If rel is a base relation, detect whether any system columns are
	 * requested from the rel.  (If rel is a join relation, rel->relid will be
	 * 0, but there can be no Var with relid 0 in the rel's targetlist or the
	 * restriction clauses, so we skip this in that case.  Note that any such
	 * columns in base relations that were joined are assumed to be contained
	 * in fdw_scan_tlist.)  This is a bit of a kluge and might go away someday,
	 * so we intentionally leave it out of the API presented to FDWs.
	 */
	scan_plan->fsSystemCol = false;
	if (scan_relid > 0)
	{
		Bitmapset  *attrs_used = NULL;
		ListCell   *lc;
		int			i;

		/*
		 * First, examine all the attributes needed for joins or final output.
		 * Note: we must look at rel's targetlist, not the attr_needed data,
		 * because attr_needed isn't computed for inheritance child rels.
		 */
		pull_varattnos((Node *) rel->reltarget.exprs, scan_relid, &attrs_used);

		/* Add all the attributes used by restriction clauses. */
		foreach(lc, rel->baserestrictinfo)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

			pull_varattnos((Node *) rinfo->clause, scan_relid, &attrs_used);
		}

		/* Now, are any system columns requested from rel? */
		for (i = FirstLowInvalidHeapAttributeNumber + 1; i < 0; i++)
		{
			if (bms_is_member(i - FirstLowInvalidHeapAttributeNumber, attrs_used))
			{
				scan_plan->fsSystemCol = true;
				break;
			}
		}

		bms_free(attrs_used);
	}

	return scan_plan;
}

/*
 * create_custom_plan
 *
 * Transform a CustomPath into a Plan.
 */
static CustomScan *
create_customscan_plan(PlannerInfo *root, CustomPath *best_path,
					   List *tlist, List *scan_clauses)
{
	CustomScan *cplan;
	RelOptInfo *rel = best_path->path.parent;
	List	   *custom_plans = NIL;
	ListCell   *lc;

	/* Recursively transform child paths. */
	foreach(lc, best_path->custom_paths)
	{
		Plan	   *plan = create_plan_recurse(root, (Path *) lfirst(lc),
											   CP_EXACT_TLIST);

		custom_plans = lappend(custom_plans, plan);
	}

	/*
	 * Sort clauses into the best execution order, although custom-scan
	 * provider can reorder them again.
	 */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/*
	 * Invoke custom plan provider to create the Plan node represented by the
	 * CustomPath.
	 */
	cplan = (CustomScan *) best_path->methods->PlanCustomPath(root,
															  rel,
															  best_path,
															  tlist,
															  scan_clauses,
															  custom_plans);
	Assert(IsA(cplan, CustomScan));

	/*
	 * Copy cost data from Path to Plan; no need to make custom-plan providers
	 * do this
	 */
	copy_generic_path_info(&cplan->scan.plan, &best_path->path);

	/* Likewise, copy the relids that are represented by this custom scan */
	cplan->custom_relids = best_path->path.parent->relids;

	/*
	 * Replace any outer-relation variables with nestloop params in the qual
	 * and custom_exprs expressions.  We do this last so that the custom-plan
	 * provider doesn't have to be involved.  (Note that parts of custom_exprs
	 * could have come from join clauses, so doing this beforehand on the
	 * scan_clauses wouldn't work.)  We assume custom_scan_tlist contains no
	 * such variables.
	 */
	if (best_path->path.param_info)
	{
		cplan->scan.plan.qual = (List *)
			replace_nestloop_params(root, (Node *) cplan->scan.plan.qual);
		cplan->custom_exprs = (List *)
			replace_nestloop_params(root, (Node *) cplan->custom_exprs);
	}

	return cplan;
}


/*****************************************************************************
 *
 *	JOIN METHODS
 *
 *****************************************************************************/

static NestLoop *
create_nestloop_plan(PlannerInfo *root,
					 NestPath *best_path)
{
	NestLoop   *join_plan;
	Plan	   *outer_plan;
	Plan	   *inner_plan;
	List	   *tlist = build_path_tlist(root, &best_path->path);
	List	   *joinrestrictclauses = best_path->joinrestrictinfo;
	List	   *joinclauses;
	List	   *otherclauses;
	Relids		outerrelids;
	List	   *nestParams;
	Relids		saveOuterRels = root->curOuterRels;
	ListCell   *cell;
	ListCell   *prev;
	ListCell   *next;

	/* NestLoop can project, so no need to be picky about child tlists */
	outer_plan = create_plan_recurse(root, best_path->outerjoinpath, 0);

	/* For a nestloop, include outer relids in curOuterRels for inner side */
	root->curOuterRels = bms_union(root->curOuterRels,
								   best_path->outerjoinpath->parent->relids);

	inner_plan = create_plan_recurse(root, best_path->innerjoinpath, 0);

	/* Restore curOuterRels */
	bms_free(root->curOuterRels);
	root->curOuterRels = saveOuterRels;

	/* Sort join qual clauses into best execution order */
	joinrestrictclauses = order_qual_clauses(root, joinrestrictclauses);

	/* Get the join qual clauses (in plain expression form) */
	/* Any pseudoconstant clauses are ignored here */
	if (IS_OUTER_JOIN(best_path->jointype))
	{
		extract_actual_join_clauses(joinrestrictclauses,
									&joinclauses, &otherclauses);
	}
	else
	{
		/* We can treat all clauses alike for an inner join */
		joinclauses = extract_actual_clauses(joinrestrictclauses, false);
		otherclauses = NIL;
	}

	/* Replace any outer-relation variables with nestloop params */
	if (best_path->path.param_info)
	{
		joinclauses = (List *)
			replace_nestloop_params(root, (Node *) joinclauses);
		otherclauses = (List *)
			replace_nestloop_params(root, (Node *) otherclauses);
	}

	/*
	 * Identify any nestloop parameters that should be supplied by this join
	 * node, and move them from root->curOuterParams to the nestParams list.
	 */
	outerrelids = best_path->outerjoinpath->parent->relids;
	nestParams = NIL;
	prev = NULL;
	for (cell = list_head(root->curOuterParams); cell; cell = next)
	{
		NestLoopParam *nlp = (NestLoopParam *) lfirst(cell);

		next = lnext(cell);
		if (IsA(nlp->paramval, Var) &&
			bms_is_member(nlp->paramval->varno, outerrelids))
		{
			root->curOuterParams = list_delete_cell(root->curOuterParams,
													cell, prev);
			nestParams = lappend(nestParams, nlp);
		}
		else if (IsA(nlp->paramval, PlaceHolderVar) &&
				 bms_overlap(((PlaceHolderVar *) nlp->paramval)->phrels,
							 outerrelids) &&
				 bms_is_subset(find_placeholder_info(root,
											(PlaceHolderVar *) nlp->paramval,
													 false)->ph_eval_at,
							   outerrelids))
		{
			root->curOuterParams = list_delete_cell(root->curOuterParams,
													cell, prev);
			nestParams = lappend(nestParams, nlp);
		}
		else
			prev = cell;
	}

	join_plan = make_nestloop(tlist,
							  joinclauses,
							  otherclauses,
							  nestParams,
							  outer_plan,
							  inner_plan,
							  best_path->jointype);

	copy_generic_path_info(&join_plan->join.plan, &best_path->path);

	return join_plan;
}

static MergeJoin *
create_mergejoin_plan(PlannerInfo *root,
					  MergePath *best_path)
{
	MergeJoin  *join_plan;
	Plan	   *outer_plan;
	Plan	   *inner_plan;
	List	   *tlist = build_path_tlist(root, &best_path->jpath.path);
	List	   *joinclauses;
	List	   *otherclauses;
	List	   *mergeclauses;
	List	   *outerpathkeys;
	List	   *innerpathkeys;
	int			nClauses;
	Oid		   *mergefamilies;
	Oid		   *mergecollations;
	int		   *mergestrategies;
	bool	   *mergenullsfirst;
	int			i;
	ListCell   *lc;
	ListCell   *lop;
	ListCell   *lip;

	/*
	 * MergeJoin can project, so we don't have to demand exact tlists from the
	 * inputs.  However, if we're intending to sort an input's result, it's
	 * best to request a small tlist so we aren't sorting more data than
	 * necessary.
	 */
	outer_plan = create_plan_recurse(root, best_path->jpath.outerjoinpath,
					 (best_path->outersortkeys != NIL) ? CP_SMALL_TLIST : 0);

	inner_plan = create_plan_recurse(root, best_path->jpath.innerjoinpath,
					 (best_path->innersortkeys != NIL) ? CP_SMALL_TLIST : 0);

	/* Sort join qual clauses into best execution order */
	/* NB: do NOT reorder the mergeclauses */
	joinclauses = order_qual_clauses(root, best_path->jpath.joinrestrictinfo);

	/* Get the join qual clauses (in plain expression form) */
	/* Any pseudoconstant clauses are ignored here */
	if (IS_OUTER_JOIN(best_path->jpath.jointype))
	{
		extract_actual_join_clauses(joinclauses,
									&joinclauses, &otherclauses);
	}
	else
	{
		/* We can treat all clauses alike for an inner join */
		joinclauses = extract_actual_clauses(joinclauses, false);
		otherclauses = NIL;
	}

	/*
	 * Remove the mergeclauses from the list of join qual clauses, leaving the
	 * list of quals that must be checked as qpquals.
	 */
	mergeclauses = get_actual_clauses(best_path->path_mergeclauses);
	joinclauses = list_difference(joinclauses, mergeclauses);

	/*
	 * Replace any outer-relation variables with nestloop params.  There
	 * should not be any in the mergeclauses.
	 */
	if (best_path->jpath.path.param_info)
	{
		joinclauses = (List *)
			replace_nestloop_params(root, (Node *) joinclauses);
		otherclauses = (List *)
			replace_nestloop_params(root, (Node *) otherclauses);
	}

	/*
	 * Rearrange mergeclauses, if needed, so that the outer variable is always
	 * on the left; mark the mergeclause restrictinfos with correct
	 * outer_is_left status.
	 */
	mergeclauses = get_switched_clauses(best_path->path_mergeclauses,
							 best_path->jpath.outerjoinpath->parent->relids);

	/*
	 * Create explicit sort nodes for the outer and inner paths if necessary.
	 */
	if (best_path->outersortkeys)
	{
		Sort	   *sort = make_sort_from_pathkeys(outer_plan,
												   best_path->outersortkeys);

		label_sort_with_costsize(root, sort, -1.0);
		outer_plan = (Plan *) sort;
		outerpathkeys = best_path->outersortkeys;
	}
	else
		outerpathkeys = best_path->jpath.outerjoinpath->pathkeys;

	if (best_path->innersortkeys)
	{
		Sort	   *sort = make_sort_from_pathkeys(inner_plan,
												   best_path->innersortkeys);

		label_sort_with_costsize(root, sort, -1.0);
		inner_plan = (Plan *) sort;
		innerpathkeys = best_path->innersortkeys;
	}
	else
		innerpathkeys = best_path->jpath.innerjoinpath->pathkeys;

	/*
	 * If specified, add a materialize node to shield the inner plan from the
	 * need to handle mark/restore.
	 */
	if (best_path->materialize_inner)
	{
		Plan	   *matplan = (Plan *) make_material(inner_plan);

		/*
		 * We assume the materialize will not spill to disk, and therefore
		 * charge just cpu_operator_cost per tuple.  (Keep this estimate in
		 * sync with final_cost_mergejoin.)
		 */
		copy_plan_costsize(matplan, inner_plan);
		matplan->total_cost += cpu_operator_cost * matplan->plan_rows;

		inner_plan = matplan;
	}

	/*
	 * Compute the opfamily/collation/strategy/nullsfirst arrays needed by the
	 * executor.  The information is in the pathkeys for the two inputs, but
	 * we need to be careful about the possibility of mergeclauses sharing a
	 * pathkey (compare find_mergeclauses_for_pathkeys()).
	 */
	nClauses = list_length(mergeclauses);
	Assert(nClauses == list_length(best_path->path_mergeclauses));
	mergefamilies = (Oid *) palloc(nClauses * sizeof(Oid));
	mergecollations = (Oid *) palloc(nClauses * sizeof(Oid));
	mergestrategies = (int *) palloc(nClauses * sizeof(int));
	mergenullsfirst = (bool *) palloc(nClauses * sizeof(bool));

	lop = list_head(outerpathkeys);
	lip = list_head(innerpathkeys);
	i = 0;
	foreach(lc, best_path->path_mergeclauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
		EquivalenceClass *oeclass;
		EquivalenceClass *ieclass;
		PathKey    *opathkey;
		PathKey    *ipathkey;
		EquivalenceClass *opeclass;
		EquivalenceClass *ipeclass;
		ListCell   *l2;

		/* fetch outer/inner eclass from mergeclause */
		Assert(IsA(rinfo, RestrictInfo));
		if (rinfo->outer_is_left)
		{
			oeclass = rinfo->left_ec;
			ieclass = rinfo->right_ec;
		}
		else
		{
			oeclass = rinfo->right_ec;
			ieclass = rinfo->left_ec;
		}
		Assert(oeclass != NULL);
		Assert(ieclass != NULL);

		/*
		 * For debugging purposes, we check that the eclasses match the paths'
		 * pathkeys.  In typical cases the merge clauses are one-to-one with
		 * the pathkeys, but when dealing with partially redundant query
		 * conditions, we might have clauses that re-reference earlier path
		 * keys.  The case that we need to reject is where a pathkey is
		 * entirely skipped over.
		 *
		 * lop and lip reference the first as-yet-unused pathkey elements;
		 * it's okay to match them, or any element before them.  If they're
		 * NULL then we have found all pathkey elements to be used.
		 */
		if (lop)
		{
			opathkey = (PathKey *) lfirst(lop);
			opeclass = opathkey->pk_eclass;
			if (oeclass == opeclass)
			{
				/* fast path for typical case */
				lop = lnext(lop);
			}
			else
			{
				/* redundant clauses ... must match something before lop */
				foreach(l2, outerpathkeys)
				{
					if (l2 == lop)
						break;
					opathkey = (PathKey *) lfirst(l2);
					opeclass = opathkey->pk_eclass;
					if (oeclass == opeclass)
						break;
				}
				if (oeclass != opeclass)
					elog(ERROR, "outer pathkeys do not match mergeclauses");
			}
		}
		else
		{
			/* redundant clauses ... must match some already-used pathkey */
			opathkey = NULL;
			opeclass = NULL;
			foreach(l2, outerpathkeys)
			{
				opathkey = (PathKey *) lfirst(l2);
				opeclass = opathkey->pk_eclass;
				if (oeclass == opeclass)
					break;
			}
			if (l2 == NULL)
				elog(ERROR, "outer pathkeys do not match mergeclauses");
		}

		if (lip)
		{
			ipathkey = (PathKey *) lfirst(lip);
			ipeclass = ipathkey->pk_eclass;
			if (ieclass == ipeclass)
			{
				/* fast path for typical case */
				lip = lnext(lip);
			}
			else
			{
				/* redundant clauses ... must match something before lip */
				foreach(l2, innerpathkeys)
				{
					if (l2 == lip)
						break;
					ipathkey = (PathKey *) lfirst(l2);
					ipeclass = ipathkey->pk_eclass;
					if (ieclass == ipeclass)
						break;
				}
				if (ieclass != ipeclass)
					elog(ERROR, "inner pathkeys do not match mergeclauses");
			}
		}
		else
		{
			/* redundant clauses ... must match some already-used pathkey */
			ipathkey = NULL;
			ipeclass = NULL;
			foreach(l2, innerpathkeys)
			{
				ipathkey = (PathKey *) lfirst(l2);
				ipeclass = ipathkey->pk_eclass;
				if (ieclass == ipeclass)
					break;
			}
			if (l2 == NULL)
				elog(ERROR, "inner pathkeys do not match mergeclauses");
		}

		/* pathkeys should match each other too (more debugging) */
		if (opathkey->pk_opfamily != ipathkey->pk_opfamily ||
			opathkey->pk_eclass->ec_collation != ipathkey->pk_eclass->ec_collation ||
			opathkey->pk_strategy != ipathkey->pk_strategy ||
			opathkey->pk_nulls_first != ipathkey->pk_nulls_first)
			elog(ERROR, "left and right pathkeys do not match in mergejoin");

		/* OK, save info for executor */
		mergefamilies[i] = opathkey->pk_opfamily;
		mergecollations[i] = opathkey->pk_eclass->ec_collation;
		mergestrategies[i] = opathkey->pk_strategy;
		mergenullsfirst[i] = opathkey->pk_nulls_first;
		i++;
	}

	/*
	 * Note: it is not an error if we have additional pathkey elements (i.e.,
	 * lop or lip isn't NULL here).  The input paths might be better-sorted
	 * than we need for the current mergejoin.
	 */

	/*
	 * Now we can build the mergejoin node.
	 */
	join_plan = make_mergejoin(tlist,
							   joinclauses,
							   otherclauses,
							   mergeclauses,
							   mergefamilies,
							   mergecollations,
							   mergestrategies,
							   mergenullsfirst,
							   outer_plan,
							   inner_plan,
							   best_path->jpath.jointype);

	/* Costs of sort and material steps are included in path cost already */
	copy_generic_path_info(&join_plan->join.plan, &best_path->jpath.path);

	return join_plan;
}

static HashJoin *
create_hashjoin_plan(PlannerInfo *root,
					 HashPath *best_path)
{
	HashJoin   *join_plan;
	Hash	   *hash_plan;
	Plan	   *outer_plan;
	Plan	   *inner_plan;
	List	   *tlist = build_path_tlist(root, &best_path->jpath.path);
	List	   *joinclauses;
	List	   *otherclauses;
	List	   *hashclauses;
	Oid			skewTable = InvalidOid;
	AttrNumber	skewColumn = InvalidAttrNumber;
	bool		skewInherit = false;
	Oid			skewColType = InvalidOid;
	int32		skewColTypmod = -1;

	/*
	 * HashJoin can project, so we don't have to demand exact tlists from the
	 * inputs.  However, it's best to request a small tlist from the inner
	 * side, so that we aren't storing more data than necessary.  Likewise, if
	 * we anticipate batching, request a small tlist from the outer side so
	 * that we don't put extra data in the outer batch files.
	 */
	outer_plan = create_plan_recurse(root, best_path->jpath.outerjoinpath,
						  (best_path->num_batches > 1) ? CP_SMALL_TLIST : 0);

	inner_plan = create_plan_recurse(root, best_path->jpath.innerjoinpath,
									 CP_SMALL_TLIST);

	/* Sort join qual clauses into best execution order */
	joinclauses = order_qual_clauses(root, best_path->jpath.joinrestrictinfo);
	/* There's no point in sorting the hash clauses ... */

	/* Get the join qual clauses (in plain expression form) */
	/* Any pseudoconstant clauses are ignored here */
	if (IS_OUTER_JOIN(best_path->jpath.jointype))
	{
		extract_actual_join_clauses(joinclauses,
									&joinclauses, &otherclauses);
	}
	else
	{
		/* We can treat all clauses alike for an inner join */
		joinclauses = extract_actual_clauses(joinclauses, false);
		otherclauses = NIL;
	}

	/*
	 * Remove the hashclauses from the list of join qual clauses, leaving the
	 * list of quals that must be checked as qpquals.
	 */
	hashclauses = get_actual_clauses(best_path->path_hashclauses);
	joinclauses = list_difference(joinclauses, hashclauses);

	/*
	 * Replace any outer-relation variables with nestloop params.  There
	 * should not be any in the hashclauses.
	 */
	if (best_path->jpath.path.param_info)
	{
		joinclauses = (List *)
			replace_nestloop_params(root, (Node *) joinclauses);
		otherclauses = (List *)
			replace_nestloop_params(root, (Node *) otherclauses);
	}

	/*
	 * Rearrange hashclauses, if needed, so that the outer variable is always
	 * on the left.
	 */
	hashclauses = get_switched_clauses(best_path->path_hashclauses,
							 best_path->jpath.outerjoinpath->parent->relids);

	/*
	 * If there is a single join clause and we can identify the outer variable
	 * as a simple column reference, supply its identity for possible use in
	 * skew optimization.  (Note: in principle we could do skew optimization
	 * with multiple join clauses, but we'd have to be able to determine the
	 * most common combinations of outer values, which we don't currently have
	 * enough stats for.)
	 */
	if (list_length(hashclauses) == 1)
	{
		OpExpr	   *clause = (OpExpr *) linitial(hashclauses);
		Node	   *node;

		Assert(is_opclause(clause));
		node = (Node *) linitial(clause->args);
		if (IsA(node, RelabelType))
			node = (Node *) ((RelabelType *) node)->arg;
		if (IsA(node, Var))
		{
			Var		   *var = (Var *) node;
			RangeTblEntry *rte;

			rte = root->simple_rte_array[var->varno];
			if (rte->rtekind == RTE_RELATION)
			{
				skewTable = rte->relid;
				skewColumn = var->varattno;
				skewInherit = rte->inh;
				skewColType = var->vartype;
				skewColTypmod = var->vartypmod;
			}
		}
	}

	/*
	 * Build the hash node and hash join node.
	 */
	hash_plan = make_hash(inner_plan,
						  skewTable,
						  skewColumn,
						  skewInherit,
						  skewColType,
						  skewColTypmod);

	/*
	 * Set Hash node's startup & total costs equal to total cost of input
	 * plan; this only affects EXPLAIN display not decisions.
	 */
	copy_plan_costsize(&hash_plan->plan, inner_plan);
	hash_plan->plan.startup_cost = hash_plan->plan.total_cost;

	join_plan = make_hashjoin(tlist,
							  joinclauses,
							  otherclauses,
							  hashclauses,
							  outer_plan,
							  (Plan *) hash_plan,
							  best_path->jpath.jointype);

	copy_generic_path_info(&join_plan->join.plan, &best_path->jpath.path);

	return join_plan;
}


/*****************************************************************************
 *
 *	SUPPORTING ROUTINES
 *
 *****************************************************************************/

/*
 * replace_nestloop_params
 *	  Replace outer-relation Vars and PlaceHolderVars in the given expression
 *	  with nestloop Params
 *
 * All Vars and PlaceHolderVars belonging to the relation(s) identified by
 * root->curOuterRels are replaced by Params, and entries are added to
 * root->curOuterParams if not already present.
 */
static Node *
replace_nestloop_params(PlannerInfo *root, Node *expr)
{
	/* No setup needed for tree walk, so away we go */
	return replace_nestloop_params_mutator(expr, root);
}

static Node *
replace_nestloop_params_mutator(Node *node, PlannerInfo *root)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;
		Param	   *param;
		NestLoopParam *nlp;
		ListCell   *lc;

		/* Upper-level Vars should be long gone at this point */
		Assert(var->varlevelsup == 0);
		/* If not to be replaced, we can just return the Var unmodified */
		if (!bms_is_member(var->varno, root->curOuterRels))
			return node;
		/* Create a Param representing the Var */
		param = assign_nestloop_param_var(root, var);
		/* Is this param already listed in root->curOuterParams? */
		foreach(lc, root->curOuterParams)
		{
			nlp = (NestLoopParam *) lfirst(lc);
			if (nlp->paramno == param->paramid)
			{
				Assert(equal(var, nlp->paramval));
				/* Present, so we can just return the Param */
				return (Node *) param;
			}
		}
		/* No, so add it */
		nlp = makeNode(NestLoopParam);
		nlp->paramno = param->paramid;
		nlp->paramval = var;
		root->curOuterParams = lappend(root->curOuterParams, nlp);
		/* And return the replacement Param */
		return (Node *) param;
	}
	if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;
		Param	   *param;
		NestLoopParam *nlp;
		ListCell   *lc;

		/* Upper-level PlaceHolderVars should be long gone at this point */
		Assert(phv->phlevelsup == 0);

		/*
		 * Check whether we need to replace the PHV.  We use bms_overlap as a
		 * cheap/quick test to see if the PHV might be evaluated in the outer
		 * rels, and then grab its PlaceHolderInfo to tell for sure.
		 */
		if (!bms_overlap(phv->phrels, root->curOuterRels) ||
		  !bms_is_subset(find_placeholder_info(root, phv, false)->ph_eval_at,
						 root->curOuterRels))
		{
			/*
			 * We can't replace the whole PHV, but we might still need to
			 * replace Vars or PHVs within its expression, in case it ends up
			 * actually getting evaluated here.  (It might get evaluated in
			 * this plan node, or some child node; in the latter case we don't
			 * really need to process the expression here, but we haven't got
			 * enough info to tell if that's the case.)  Flat-copy the PHV
			 * node and then recurse on its expression.
			 *
			 * Note that after doing this, we might have different
			 * representations of the contents of the same PHV in different
			 * parts of the plan tree.  This is OK because equal() will just
			 * match on phid/phlevelsup, so setrefs.c will still recognize an
			 * upper-level reference to a lower-level copy of the same PHV.
			 */
			PlaceHolderVar *newphv = makeNode(PlaceHolderVar);

			memcpy(newphv, phv, sizeof(PlaceHolderVar));
			newphv->phexpr = (Expr *)
				replace_nestloop_params_mutator((Node *) phv->phexpr,
												root);
			return (Node *) newphv;
		}
		/* Create a Param representing the PlaceHolderVar */
		param = assign_nestloop_param_placeholdervar(root, phv);
		/* Is this param already listed in root->curOuterParams? */
		foreach(lc, root->curOuterParams)
		{
			nlp = (NestLoopParam *) lfirst(lc);
			if (nlp->paramno == param->paramid)
			{
				Assert(equal(phv, nlp->paramval));
				/* Present, so we can just return the Param */
				return (Node *) param;
			}
		}
		/* No, so add it */
		nlp = makeNode(NestLoopParam);
		nlp->paramno = param->paramid;
		nlp->paramval = (Var *) phv;
		root->curOuterParams = lappend(root->curOuterParams, nlp);
		/* And return the replacement Param */
		return (Node *) param;
	}
	return expression_tree_mutator(node,
								   replace_nestloop_params_mutator,
								   (void *) root);
}

/*
 * process_subquery_nestloop_params
 *	  Handle params of a parameterized subquery that need to be fed
 *	  from an outer nestloop.
 *
 * Currently, that would be *all* params that a subquery in FROM has demanded
 * from the current query level, since they must be LATERAL references.
 *
 * The subplan's references to the outer variables are already represented
 * as PARAM_EXEC Params, so we need not modify the subplan here.  What we
 * do need to do is add entries to root->curOuterParams to signal the parent
 * nestloop plan node that it must provide these values.
 */
static void
process_subquery_nestloop_params(PlannerInfo *root, List *subplan_params)
{
	ListCell   *ppl;

	foreach(ppl, subplan_params)
	{
		PlannerParamItem *pitem = (PlannerParamItem *) lfirst(ppl);

		if (IsA(pitem->item, Var))
		{
			Var		   *var = (Var *) pitem->item;
			NestLoopParam *nlp;
			ListCell   *lc;

			/* If not from a nestloop outer rel, complain */
			if (!bms_is_member(var->varno, root->curOuterRels))
				elog(ERROR, "non-LATERAL parameter required by subquery");
			/* Is this param already listed in root->curOuterParams? */
			foreach(lc, root->curOuterParams)
			{
				nlp = (NestLoopParam *) lfirst(lc);
				if (nlp->paramno == pitem->paramId)
				{
					Assert(equal(var, nlp->paramval));
					/* Present, so nothing to do */
					break;
				}
			}
			if (lc == NULL)
			{
				/* No, so add it */
				nlp = makeNode(NestLoopParam);
				nlp->paramno = pitem->paramId;
				nlp->paramval = copyObject(var);
				root->curOuterParams = lappend(root->curOuterParams, nlp);
			}
		}
		else if (IsA(pitem->item, PlaceHolderVar))
		{
			PlaceHolderVar *phv = (PlaceHolderVar *) pitem->item;
			NestLoopParam *nlp;
			ListCell   *lc;

			/* If not from a nestloop outer rel, complain */
			if (!bms_is_subset(find_placeholder_info(root, phv, false)->ph_eval_at,
							   root->curOuterRels))
				elog(ERROR, "non-LATERAL parameter required by subquery");
			/* Is this param already listed in root->curOuterParams? */
			foreach(lc, root->curOuterParams)
			{
				nlp = (NestLoopParam *) lfirst(lc);
				if (nlp->paramno == pitem->paramId)
				{
					Assert(equal(phv, nlp->paramval));
					/* Present, so nothing to do */
					break;
				}
			}
			if (lc == NULL)
			{
				/* No, so add it */
				nlp = makeNode(NestLoopParam);
				nlp->paramno = pitem->paramId;
				nlp->paramval = copyObject(phv);
				root->curOuterParams = lappend(root->curOuterParams, nlp);
			}
		}
		else
			elog(ERROR, "unexpected type of subquery parameter");
	}
}

/*
 * fix_indexqual_references
 *	  Adjust indexqual clauses to the form the executor's indexqual
 *	  machinery needs.
 *
 * We have four tasks here:
 *	* Remove RestrictInfo nodes from the input clauses.
 *	* Replace any outer-relation Var or PHV nodes with nestloop Params.
 *	  (XXX eventually, that responsibility should go elsewhere?)
 *	* Index keys must be represented by Var nodes with varattno set to the
 *	  index's attribute number, not the attribute number in the original rel.
 *	* If the index key is on the right, commute the clause to put it on the
 *	  left.
 *
 * The result is a modified copy of the path's indexquals list --- the
 * original is not changed.  Note also that the copy shares no substructure
 * with the original; this is needed in case there is a subplan in it (we need
 * two separate copies of the subplan tree, or things will go awry).
 */
static List *
fix_indexqual_references(PlannerInfo *root, IndexPath *index_path)
{
	IndexOptInfo *index = index_path->indexinfo;
	List	   *fixed_indexquals;
	ListCell   *lcc,
			   *lci;

	fixed_indexquals = NIL;

	forboth(lcc, index_path->indexquals, lci, index_path->indexqualcols)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lcc);
		int			indexcol = lfirst_int(lci);
		Node	   *clause;

		Assert(IsA(rinfo, RestrictInfo));

		/*
		 * Replace any outer-relation variables with nestloop params.
		 *
		 * This also makes a copy of the clause, so it's safe to modify it
		 * in-place below.
		 */
		clause = replace_nestloop_params(root, (Node *) rinfo->clause);

		if (IsA(clause, OpExpr))
		{
			OpExpr	   *op = (OpExpr *) clause;

			if (list_length(op->args) != 2)
				elog(ERROR, "indexqual clause is not binary opclause");

			/*
			 * Check to see if the indexkey is on the right; if so, commute
			 * the clause.  The indexkey should be the side that refers to
			 * (only) the base relation.
			 */
			if (!bms_equal(rinfo->left_relids, index->rel->relids))
				CommuteOpExpr(op);

			/*
			 * Now replace the indexkey expression with an index Var.
			 */
			linitial(op->args) = fix_indexqual_operand(linitial(op->args),
													   index,
													   indexcol);
		}
		else if (IsA(clause, RowCompareExpr))
		{
			RowCompareExpr *rc = (RowCompareExpr *) clause;
			Expr	   *newrc;
			List	   *indexcolnos;
			bool		var_on_left;
			ListCell   *lca,
					   *lcai;

			/*
			 * Re-discover which index columns are used in the rowcompare.
			 */
			newrc = adjust_rowcompare_for_index(rc,
												index,
												indexcol,
												&indexcolnos,
												&var_on_left);

			/*
			 * Trouble if adjust_rowcompare_for_index thought the
			 * RowCompareExpr didn't match the index as-is; the clause should
			 * have gone through that routine already.
			 */
			if (newrc != (Expr *) rc)
				elog(ERROR, "inconsistent results from adjust_rowcompare_for_index");

			/*
			 * Check to see if the indexkey is on the right; if so, commute
			 * the clause.
			 */
			if (!var_on_left)
				CommuteRowCompareExpr(rc);

			/*
			 * Now replace the indexkey expressions with index Vars.
			 */
			Assert(list_length(rc->largs) == list_length(indexcolnos));
			forboth(lca, rc->largs, lcai, indexcolnos)
			{
				lfirst(lca) = fix_indexqual_operand(lfirst(lca),
													index,
													lfirst_int(lcai));
			}
		}
		else if (IsA(clause, ScalarArrayOpExpr))
		{
			ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;

			/* Never need to commute... */

			/* Replace the indexkey expression with an index Var. */
			linitial(saop->args) = fix_indexqual_operand(linitial(saop->args),
														 index,
														 indexcol);
		}
		else if (IsA(clause, NullTest))
		{
			NullTest   *nt = (NullTest *) clause;

			/* Replace the indexkey expression with an index Var. */
			nt->arg = (Expr *) fix_indexqual_operand((Node *) nt->arg,
													 index,
													 indexcol);
		}
		else
			elog(ERROR, "unsupported indexqual type: %d",
				 (int) nodeTag(clause));

		fixed_indexquals = lappend(fixed_indexquals, clause);
	}

	return fixed_indexquals;
}

/*
 * fix_indexorderby_references
 *	  Adjust indexorderby clauses to the form the executor's index
 *	  machinery needs.
 *
 * This is a simplified version of fix_indexqual_references.  The input does
 * not have RestrictInfo nodes, and we assume that indxpath.c already
 * commuted the clauses to put the index keys on the left.  Also, we don't
 * bother to support any cases except simple OpExprs, since nothing else
 * is allowed for ordering operators.
 */
static List *
fix_indexorderby_references(PlannerInfo *root, IndexPath *index_path)
{
	IndexOptInfo *index = index_path->indexinfo;
	List	   *fixed_indexorderbys;
	ListCell   *lcc,
			   *lci;

	fixed_indexorderbys = NIL;

	forboth(lcc, index_path->indexorderbys, lci, index_path->indexorderbycols)
	{
		Node	   *clause = (Node *) lfirst(lcc);
		int			indexcol = lfirst_int(lci);

		/*
		 * Replace any outer-relation variables with nestloop params.
		 *
		 * This also makes a copy of the clause, so it's safe to modify it
		 * in-place below.
		 */
		clause = replace_nestloop_params(root, clause);

		if (IsA(clause, OpExpr))
		{
			OpExpr	   *op = (OpExpr *) clause;

			if (list_length(op->args) != 2)
				elog(ERROR, "indexorderby clause is not binary opclause");

			/*
			 * Now replace the indexkey expression with an index Var.
			 */
			linitial(op->args) = fix_indexqual_operand(linitial(op->args),
													   index,
													   indexcol);
		}
		else
			elog(ERROR, "unsupported indexorderby type: %d",
				 (int) nodeTag(clause));

		fixed_indexorderbys = lappend(fixed_indexorderbys, clause);
	}

	return fixed_indexorderbys;
}

/*
 * fix_indexqual_operand
 *	  Convert an indexqual expression to a Var referencing the index column.
 *
 * We represent index keys by Var nodes having varno == INDEX_VAR and varattno
 * equal to the index's attribute number (index column position).
 *
 * Most of the code here is just for sanity cross-checking that the given
 * expression actually matches the index column it's claimed to.
 */
static Node *
fix_indexqual_operand(Node *node, IndexOptInfo *index, int indexcol)
{
	Var		   *result;
	int			pos;
	ListCell   *indexpr_item;

	/*
	 * Remove any binary-compatible relabeling of the indexkey
	 */
	if (IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;

	Assert(indexcol >= 0 && indexcol < index->ncolumns);

	if (index->indexkeys[indexcol] != 0)
	{
		/* It's a simple index column */
		if (IsA(node, Var) &&
			((Var *) node)->varno == index->rel->relid &&
			((Var *) node)->varattno == index->indexkeys[indexcol])
		{
			result = (Var *) copyObject(node);
			result->varno = INDEX_VAR;
			result->varattno = indexcol + 1;
			return (Node *) result;
		}
		else
			elog(ERROR, "index key does not match expected index column");
	}

	/* It's an index expression, so find and cross-check the expression */
	indexpr_item = list_head(index->indexprs);
	for (pos = 0; pos < index->ncolumns; pos++)
	{
		if (index->indexkeys[pos] == 0)
		{
			if (indexpr_item == NULL)
				elog(ERROR, "too few entries in indexprs list");
			if (pos == indexcol)
			{
				Node	   *indexkey;

				indexkey = (Node *) lfirst(indexpr_item);
				if (indexkey && IsA(indexkey, RelabelType))
					indexkey = (Node *) ((RelabelType *) indexkey)->arg;
				if (equal(node, indexkey))
				{
					result = makeVar(INDEX_VAR, indexcol + 1,
									 exprType(lfirst(indexpr_item)), -1,
									 exprCollation(lfirst(indexpr_item)),
									 0);
					return (Node *) result;
				}
				else
					elog(ERROR, "index key does not match expected index column");
			}
			indexpr_item = lnext(indexpr_item);
		}
	}

	/* Ooops... */
	elog(ERROR, "index key does not match expected index column");
	return NULL;				/* keep compiler quiet */
}

/*
 * get_switched_clauses
 *	  Given a list of merge or hash joinclauses (as RestrictInfo nodes),
 *	  extract the bare clauses, and rearrange the elements within the
 *	  clauses, if needed, so the outer join variable is on the left and
 *	  the inner is on the right.  The original clause data structure is not
 *	  touched; a modified list is returned.  We do, however, set the transient
 *	  outer_is_left field in each RestrictInfo to show which side was which.
 */
static List *
get_switched_clauses(List *clauses, Relids outerrelids)
{
	List	   *t_list = NIL;
	ListCell   *l;

	foreach(l, clauses)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(l);
		OpExpr	   *clause = (OpExpr *) restrictinfo->clause;

		Assert(is_opclause(clause));
		if (bms_is_subset(restrictinfo->right_relids, outerrelids))
		{
			/*
			 * Duplicate just enough of the structure to allow commuting the
			 * clause without changing the original list.  Could use
			 * copyObject, but a complete deep copy is overkill.
			 */
			OpExpr	   *temp = makeNode(OpExpr);

			temp->opno = clause->opno;
			temp->opfuncid = InvalidOid;
			temp->opresulttype = clause->opresulttype;
			temp->opretset = clause->opretset;
			temp->opcollid = clause->opcollid;
			temp->inputcollid = clause->inputcollid;
			temp->args = list_copy(clause->args);
			temp->location = clause->location;
			/* Commute it --- note this modifies the temp node in-place. */
			CommuteOpExpr(temp);
			t_list = lappend(t_list, temp);
			restrictinfo->outer_is_left = false;
		}
		else
		{
			Assert(bms_is_subset(restrictinfo->left_relids, outerrelids));
			t_list = lappend(t_list, clause);
			restrictinfo->outer_is_left = true;
		}
	}
	return t_list;
}

/*
 * order_qual_clauses
 *		Given a list of qual clauses that will all be evaluated at the same
 *		plan node, sort the list into the order we want to check the quals
 *		in at runtime.
 *
 * Ideally the order should be driven by a combination of execution cost and
 * selectivity, but it's not immediately clear how to account for both,
 * and given the uncertainty of the estimates the reliability of the decisions
 * would be doubtful anyway.  So we just order by estimated per-tuple cost,
 * being careful not to change the order when (as is often the case) the
 * estimates are identical.
 *
 * Although this will work on either bare clauses or RestrictInfos, it's
 * much faster to apply it to RestrictInfos, since it can re-use cost
 * information that is cached in RestrictInfos.
 *
 * Note: some callers pass lists that contain entries that will later be
 * removed; this is the easiest way to let this routine see RestrictInfos
 * instead of bare clauses.  It's OK because we only sort by cost, but
 * a cost/selectivity combination would likely do the wrong thing.
 */
static List *
order_qual_clauses(PlannerInfo *root, List *clauses)
{
	typedef struct
	{
		Node	   *clause;
		Cost		cost;
	} QualItem;
	int			nitems = list_length(clauses);
	QualItem   *items;
	ListCell   *lc;
	int			i;
	List	   *result;

	/* No need to work hard for 0 or 1 clause */
	if (nitems <= 1)
		return clauses;

	/*
	 * Collect the items and costs into an array.  This is to avoid repeated
	 * cost_qual_eval work if the inputs aren't RestrictInfos.
	 */
	items = (QualItem *) palloc(nitems * sizeof(QualItem));
	i = 0;
	foreach(lc, clauses)
	{
		Node	   *clause = (Node *) lfirst(lc);
		QualCost	qcost;

		cost_qual_eval_node(&qcost, clause, root);
		items[i].clause = clause;
		items[i].cost = qcost.per_tuple;
		i++;
	}

	/*
	 * Sort.  We don't use qsort() because it's not guaranteed stable for
	 * equal keys.  The expected number of entries is small enough that a
	 * simple insertion sort should be good enough.
	 */
	for (i = 1; i < nitems; i++)
	{
		QualItem	newitem = items[i];
		int			j;

		/* insert newitem into the already-sorted subarray */
		for (j = i; j > 0; j--)
		{
			if (newitem.cost >= items[j - 1].cost)
				break;
			items[j] = items[j - 1];
		}
		items[j] = newitem;
	}

	/* Convert back to a list */
	result = NIL;
	for (i = 0; i < nitems; i++)
		result = lappend(result, items[i].clause);

	return result;
}

/*
 * Copy cost and size info from a Path node to the Plan node created from it.
 * The executor usually won't use this info, but it's needed by EXPLAIN.
 * Also copy the parallel-aware flag, which the executor *will* use.
 */
static void
copy_generic_path_info(Plan *dest, Path *src)
{
	dest->startup_cost = src->startup_cost;
	dest->total_cost = src->total_cost;
	dest->plan_rows = src->rows;
	dest->plan_width = src->pathtarget->width;
	dest->parallel_aware = src->parallel_aware;
}

/*
 * Copy cost and size info from a lower plan node to an inserted node.
 * (Most callers alter the info after copying it.)
 */
static void
copy_plan_costsize(Plan *dest, Plan *src)
{
	dest->startup_cost = src->startup_cost;
	dest->total_cost = src->total_cost;
	dest->plan_rows = src->plan_rows;
	dest->plan_width = src->plan_width;
	/* Assume the inserted node is not parallel-aware. */
	dest->parallel_aware = false;
}

/*
 * Some places in this file build Sort nodes that don't have a directly
 * corresponding Path node.  The cost of the sort is, or should have been,
 * included in the cost of the Path node we're working from, but since it's
 * not split out, we have to re-figure it using cost_sort().  This is just
 * to label the Sort node nicely for EXPLAIN.
 *
 * limit_tuples is as for cost_sort (in particular, pass -1 if no limit)
 */
static void
label_sort_with_costsize(PlannerInfo *root, Sort *plan, double limit_tuples)
{
	Plan	   *lefttree = plan->plan.lefttree;
	Path		sort_path;		/* dummy for result of cost_sort */

	cost_sort(&sort_path, root, NIL,
			  lefttree->total_cost,
			  lefttree->plan_rows,
			  lefttree->plan_width,
			  0.0,
			  work_mem,
			  limit_tuples);
	plan->plan.startup_cost = sort_path.startup_cost;
	plan->plan.total_cost = sort_path.total_cost;
	plan->plan.plan_rows = lefttree->plan_rows;
	plan->plan.plan_width = lefttree->plan_width;
	plan->plan.parallel_aware = false;
}


/*****************************************************************************
 *
 *	PLAN NODE BUILDING ROUTINES
 *
 * In general, these functions are not passed the original Path and therefore
 * leave it to the caller to fill in the cost/width fields from the Path,
 * typically by calling copy_generic_path_info().  This convention is
 * somewhat historical, but it does support a few places above where we build
 * a plan node without having an exactly corresponding Path node.  Under no
 * circumstances should one of these functions do its own cost calculations,
 * as that would be redundant with calculations done while building Paths.
 *
 *****************************************************************************/

static SeqScan *
make_seqscan(List *qptlist,
			 List *qpqual,
			 Index scanrelid)
{
	SeqScan    *node = makeNode(SeqScan);
	Plan	   *plan = &node->plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scanrelid = scanrelid;

	return node;
}

static SampleScan *
make_samplescan(List *qptlist,
				List *qpqual,
				Index scanrelid,
				TableSampleClause *tsc)
{
	SampleScan *node = makeNode(SampleScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->tablesample = tsc;

	return node;
}

static IndexScan *
make_indexscan(List *qptlist,
			   List *qpqual,
			   Index scanrelid,
			   Oid indexid,
			   List *indexqual,
			   List *indexqualorig,
			   List *indexorderby,
			   List *indexorderbyorig,
			   List *indexorderbyops,
			   ScanDirection indexscandir)
{
	IndexScan  *node = makeNode(IndexScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->indexid = indexid;
	node->indexqual = indexqual;
	node->indexqualorig = indexqualorig;
	node->indexorderby = indexorderby;
	node->indexorderbyorig = indexorderbyorig;
	node->indexorderbyops = indexorderbyops;
	node->indexorderdir = indexscandir;

	return node;
}

static IndexOnlyScan *
make_indexonlyscan(List *qptlist,
				   List *qpqual,
				   Index scanrelid,
				   Oid indexid,
				   List *indexqual,
				   List *indexorderby,
				   List *indextlist,
				   ScanDirection indexscandir)
{
	IndexOnlyScan *node = makeNode(IndexOnlyScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->indexid = indexid;
	node->indexqual = indexqual;
	node->indexorderby = indexorderby;
	node->indextlist = indextlist;
	node->indexorderdir = indexscandir;

	return node;
}

static BitmapIndexScan *
make_bitmap_indexscan(Index scanrelid,
					  Oid indexid,
					  List *indexqual,
					  List *indexqualorig)
{
	BitmapIndexScan *node = makeNode(BitmapIndexScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = NIL;		/* not used */
	plan->qual = NIL;			/* not used */
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->indexid = indexid;
	node->indexqual = indexqual;
	node->indexqualorig = indexqualorig;

	return node;
}

static BitmapHeapScan *
make_bitmap_heapscan(List *qptlist,
					 List *qpqual,
					 Plan *lefttree,
					 List *bitmapqualorig,
					 Index scanrelid)
{
	BitmapHeapScan *node = makeNode(BitmapHeapScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = lefttree;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->bitmapqualorig = bitmapqualorig;

	return node;
}

static TidScan *
make_tidscan(List *qptlist,
			 List *qpqual,
			 Index scanrelid,
			 List *tidquals)
{
	TidScan    *node = makeNode(TidScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->tidquals = tidquals;

	return node;
}

static SubqueryScan *
make_subqueryscan(List *qptlist,
				  List *qpqual,
				  Index scanrelid,
				  Plan *subplan)
{
	SubqueryScan *node = makeNode(SubqueryScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->subplan = subplan;

	return node;
}

static FunctionScan *
make_functionscan(List *qptlist,
				  List *qpqual,
				  Index scanrelid,
				  List *functions,
				  bool funcordinality)
{
	FunctionScan *node = makeNode(FunctionScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->functions = functions;
	node->funcordinality = funcordinality;

	return node;
}

static ValuesScan *
make_valuesscan(List *qptlist,
				List *qpqual,
				Index scanrelid,
				List *values_lists)
{
	ValuesScan *node = makeNode(ValuesScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->values_lists = values_lists;

	return node;
}

static CteScan *
make_ctescan(List *qptlist,
			 List *qpqual,
			 Index scanrelid,
			 int ctePlanId,
			 int cteParam)
{
	CteScan    *node = makeNode(CteScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->ctePlanId = ctePlanId;
	node->cteParam = cteParam;

	return node;
}

static WorkTableScan *
make_worktablescan(List *qptlist,
				   List *qpqual,
				   Index scanrelid,
				   int wtParam)
{
	WorkTableScan *node = makeNode(WorkTableScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->wtParam = wtParam;

	return node;
}

ForeignScan *
make_foreignscan(List *qptlist,
				 List *qpqual,
				 Index scanrelid,
				 List *fdw_exprs,
				 List *fdw_private,
				 List *fdw_scan_tlist,
				 List *fdw_recheck_quals,
				 Plan *outer_plan)
{
	ForeignScan *node = makeNode(ForeignScan);
	Plan	   *plan = &node->scan.plan;

	/* cost will be filled in by create_foreignscan_plan */
	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = outer_plan;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	/* fs_server will be filled in by create_foreignscan_plan */
	node->fs_server = InvalidOid;
	node->fdw_exprs = fdw_exprs;
	node->fdw_private = fdw_private;
	node->fdw_scan_tlist = fdw_scan_tlist;
	node->fdw_recheck_quals = fdw_recheck_quals;
	/* fs_relids will be filled in by create_foreignscan_plan */
	node->fs_relids = NULL;
	/* fsSystemCol will be filled in by create_foreignscan_plan */
	node->fsSystemCol = false;

	return node;
}

static Append *
make_append(List *appendplans, List *tlist)
{
	Append	   *node = makeNode(Append);
	Plan	   *plan = &node->plan;

	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->appendplans = appendplans;

	return node;
}

static RecursiveUnion *
make_recursive_union(List *tlist,
					 Plan *lefttree,
					 Plan *righttree,
					 int wtParam,
					 List *distinctList,
					 long numGroups)
{
	RecursiveUnion *node = makeNode(RecursiveUnion);
	Plan	   *plan = &node->plan;
	int			numCols = list_length(distinctList);

	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = righttree;
	node->wtParam = wtParam;

	/*
	 * convert SortGroupClause list into arrays of attr indexes and equality
	 * operators, as wanted by executor
	 */
	node->numCols = numCols;
	if (numCols > 0)
	{
		int			keyno = 0;
		AttrNumber *dupColIdx;
		Oid		   *dupOperators;
		ListCell   *slitem;

		dupColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numCols);
		dupOperators = (Oid *) palloc(sizeof(Oid) * numCols);

		foreach(slitem, distinctList)
		{
			SortGroupClause *sortcl = (SortGroupClause *) lfirst(slitem);
			TargetEntry *tle = get_sortgroupclause_tle(sortcl,
													   plan->targetlist);

			dupColIdx[keyno] = tle->resno;
			dupOperators[keyno] = sortcl->eqop;
			Assert(OidIsValid(dupOperators[keyno]));
			keyno++;
		}
		node->dupColIdx = dupColIdx;
		node->dupOperators = dupOperators;
	}
	node->numGroups = numGroups;

	return node;
}

static BitmapAnd *
make_bitmap_and(List *bitmapplans)
{
	BitmapAnd  *node = makeNode(BitmapAnd);
	Plan	   *plan = &node->plan;

	plan->targetlist = NIL;
	plan->qual = NIL;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->bitmapplans = bitmapplans;

	return node;
}

static BitmapOr *
make_bitmap_or(List *bitmapplans)
{
	BitmapOr   *node = makeNode(BitmapOr);
	Plan	   *plan = &node->plan;

	plan->targetlist = NIL;
	plan->qual = NIL;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->bitmapplans = bitmapplans;

	return node;
}

static NestLoop *
make_nestloop(List *tlist,
			  List *joinclauses,
			  List *otherclauses,
			  List *nestParams,
			  Plan *lefttree,
			  Plan *righttree,
			  JoinType jointype)
{
	NestLoop   *node = makeNode(NestLoop);
	Plan	   *plan = &node->join.plan;

	plan->targetlist = tlist;
	plan->qual = otherclauses;
	plan->lefttree = lefttree;
	plan->righttree = righttree;
	node->join.jointype = jointype;
	node->join.joinqual = joinclauses;
	node->nestParams = nestParams;

	return node;
}

static HashJoin *
make_hashjoin(List *tlist,
			  List *joinclauses,
			  List *otherclauses,
			  List *hashclauses,
			  Plan *lefttree,
			  Plan *righttree,
			  JoinType jointype)
{
	HashJoin   *node = makeNode(HashJoin);
	Plan	   *plan = &node->join.plan;

	plan->targetlist = tlist;
	plan->qual = otherclauses;
	plan->lefttree = lefttree;
	plan->righttree = righttree;
	node->hashclauses = hashclauses;
	node->join.jointype = jointype;
	node->join.joinqual = joinclauses;

	return node;
}

static Hash *
make_hash(Plan *lefttree,
		  Oid skewTable,
		  AttrNumber skewColumn,
		  bool skewInherit,
		  Oid skewColType,
		  int32 skewColTypmod)
{
	Hash	   *node = makeNode(Hash);
	Plan	   *plan = &node->plan;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	node->skewTable = skewTable;
	node->skewColumn = skewColumn;
	node->skewInherit = skewInherit;
	node->skewColType = skewColType;
	node->skewColTypmod = skewColTypmod;

	return node;
}

static MergeJoin *
make_mergejoin(List *tlist,
			   List *joinclauses,
			   List *otherclauses,
			   List *mergeclauses,
			   Oid *mergefamilies,
			   Oid *mergecollations,
			   int *mergestrategies,
			   bool *mergenullsfirst,
			   Plan *lefttree,
			   Plan *righttree,
			   JoinType jointype)
{
	MergeJoin  *node = makeNode(MergeJoin);
	Plan	   *plan = &node->join.plan;

	plan->targetlist = tlist;
	plan->qual = otherclauses;
	plan->lefttree = lefttree;
	plan->righttree = righttree;
	node->mergeclauses = mergeclauses;
	node->mergeFamilies = mergefamilies;
	node->mergeCollations = mergecollations;
	node->mergeStrategies = mergestrategies;
	node->mergeNullsFirst = mergenullsfirst;
	node->join.jointype = jointype;
	node->join.joinqual = joinclauses;

	return node;
}

/*
 * make_sort --- basic routine to build a Sort plan node
 *
 * Caller must have built the sortColIdx, sortOperators, collations, and
 * nullsFirst arrays already.
 */
static Sort *
make_sort(Plan *lefttree, int numCols,
		  AttrNumber *sortColIdx, Oid *sortOperators,
		  Oid *collations, bool *nullsFirst)
{
	Sort	   *node = makeNode(Sort);
	Plan	   *plan = &node->plan;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;
	node->numCols = numCols;
	node->sortColIdx = sortColIdx;
	node->sortOperators = sortOperators;
	node->collations = collations;
	node->nullsFirst = nullsFirst;

	return node;
}

/*
 * prepare_sort_from_pathkeys
 *	  Prepare to sort according to given pathkeys
 *
 * This is used to set up for both Sort and MergeAppend nodes.  It calculates
 * the executor's representation of the sort key information, and adjusts the
 * plan targetlist if needed to add resjunk sort columns.
 *
 * Input parameters:
 *	  'lefttree' is the plan node which yields input tuples
 *	  'pathkeys' is the list of pathkeys by which the result is to be sorted
 *	  'relids' identifies the child relation being sorted, if any
 *	  'reqColIdx' is NULL or an array of required sort key column numbers
 *	  'adjust_tlist_in_place' is TRUE if lefttree must be modified in-place
 *
 * We must convert the pathkey information into arrays of sort key column
 * numbers, sort operator OIDs, collation OIDs, and nulls-first flags,
 * which is the representation the executor wants.  These are returned into
 * the output parameters *p_numsortkeys etc.
 *
 * When looking for matches to an EquivalenceClass's members, we will only
 * consider child EC members if they match 'relids'.  This protects against
 * possible incorrect matches to child expressions that contain no Vars.
 *
 * If reqColIdx isn't NULL then it contains sort key column numbers that
 * we should match.  This is used when making child plans for a MergeAppend;
 * it's an error if we can't match the columns.
 *
 * If the pathkeys include expressions that aren't simple Vars, we will
 * usually need to add resjunk items to the input plan's targetlist to
 * compute these expressions, since the Sort/MergeAppend node itself won't
 * do any such calculations.  If the input plan type isn't one that can do
 * projections, this means adding a Result node just to do the projection.
 * However, the caller can pass adjust_tlist_in_place = TRUE to force the
 * lefttree tlist to be modified in-place regardless of whether the node type
 * can project --- we use this for fixing the tlist of MergeAppend itself.
 *
 * Returns the node which is to be the input to the Sort (either lefttree,
 * or a Result stacked atop lefttree).
 */
static Plan *
prepare_sort_from_pathkeys(Plan *lefttree, List *pathkeys,
						   Relids relids,
						   const AttrNumber *reqColIdx,
						   bool adjust_tlist_in_place,
						   int *p_numsortkeys,
						   AttrNumber **p_sortColIdx,
						   Oid **p_sortOperators,
						   Oid **p_collations,
						   bool **p_nullsFirst)
{
	List	   *tlist = lefttree->targetlist;
	ListCell   *i;
	int			numsortkeys;
	AttrNumber *sortColIdx;
	Oid		   *sortOperators;
	Oid		   *collations;
	bool	   *nullsFirst;

	/*
	 * We will need at most list_length(pathkeys) sort columns; possibly less
	 */
	numsortkeys = list_length(pathkeys);
	sortColIdx = (AttrNumber *) palloc(numsortkeys * sizeof(AttrNumber));
	sortOperators = (Oid *) palloc(numsortkeys * sizeof(Oid));
	collations = (Oid *) palloc(numsortkeys * sizeof(Oid));
	nullsFirst = (bool *) palloc(numsortkeys * sizeof(bool));

	numsortkeys = 0;

	foreach(i, pathkeys)
	{
		PathKey    *pathkey = (PathKey *) lfirst(i);
		EquivalenceClass *ec = pathkey->pk_eclass;
		EquivalenceMember *em;
		TargetEntry *tle = NULL;
		Oid			pk_datatype = InvalidOid;
		Oid			sortop;
		ListCell   *j;

		if (ec->ec_has_volatile)
		{
			/*
			 * If the pathkey's EquivalenceClass is volatile, then it must
			 * have come from an ORDER BY clause, and we have to match it to
			 * that same targetlist entry.
			 */
			if (ec->ec_sortref == 0)	/* can't happen */
				elog(ERROR, "volatile EquivalenceClass has no sortref");
			tle = get_sortgroupref_tle(ec->ec_sortref, tlist);
			Assert(tle);
			Assert(list_length(ec->ec_members) == 1);
			pk_datatype = ((EquivalenceMember *) linitial(ec->ec_members))->em_datatype;
		}
		else if (reqColIdx != NULL)
		{
			/*
			 * If we are given a sort column number to match, only consider
			 * the single TLE at that position.  It's possible that there is
			 * no such TLE, in which case fall through and generate a resjunk
			 * targetentry (we assume this must have happened in the parent
			 * plan as well).  If there is a TLE but it doesn't match the
			 * pathkey's EC, we do the same, which is probably the wrong thing
			 * but we'll leave it to caller to complain about the mismatch.
			 */
			tle = get_tle_by_resno(tlist, reqColIdx[numsortkeys]);
			if (tle)
			{
				em = find_ec_member_for_tle(ec, tle, relids);
				if (em)
				{
					/* found expr at right place in tlist */
					pk_datatype = em->em_datatype;
				}
				else
					tle = NULL;
			}
		}
		else
		{
			/*
			 * Otherwise, we can sort by any non-constant expression listed in
			 * the pathkey's EquivalenceClass.  For now, we take the first
			 * tlist item found in the EC. If there's no match, we'll generate
			 * a resjunk entry using the first EC member that is an expression
			 * in the input's vars.  (The non-const restriction only matters
			 * if the EC is below_outer_join; but if it isn't, it won't
			 * contain consts anyway, else we'd have discarded the pathkey as
			 * redundant.)
			 *
			 * XXX if we have a choice, is there any way of figuring out which
			 * might be cheapest to execute?  (For example, int4lt is likely
			 * much cheaper to execute than numericlt, but both might appear
			 * in the same equivalence class...)  Not clear that we ever will
			 * have an interesting choice in practice, so it may not matter.
			 */
			foreach(j, tlist)
			{
				tle = (TargetEntry *) lfirst(j);
				em = find_ec_member_for_tle(ec, tle, relids);
				if (em)
				{
					/* found expr already in tlist */
					pk_datatype = em->em_datatype;
					break;
				}
				tle = NULL;
			}
		}

		if (!tle)
		{
			/*
			 * No matching tlist item; look for a computable expression. Note
			 * that we treat Aggrefs as if they were variables; this is
			 * necessary when attempting to sort the output from an Agg node
			 * for use in a WindowFunc (since grouping_planner will have
			 * treated the Aggrefs as variables, too).  Likewise, if we find a
			 * WindowFunc in a sort expression, treat it as a variable.
			 */
			Expr	   *sortexpr = NULL;

			foreach(j, ec->ec_members)
			{
				EquivalenceMember *em = (EquivalenceMember *) lfirst(j);
				List	   *exprvars;
				ListCell   *k;

				/*
				 * We shouldn't be trying to sort by an equivalence class that
				 * contains a constant, so no need to consider such cases any
				 * further.
				 */
				if (em->em_is_const)
					continue;

				/*
				 * Ignore child members unless they match the rel being
				 * sorted.
				 */
				if (em->em_is_child &&
					!bms_equal(em->em_relids, relids))
					continue;

				sortexpr = em->em_expr;
				exprvars = pull_var_clause((Node *) sortexpr,
										   PVC_INCLUDE_AGGREGATES |
										   PVC_INCLUDE_WINDOWFUNCS |
										   PVC_INCLUDE_PLACEHOLDERS);
				foreach(k, exprvars)
				{
					if (!tlist_member_ignore_relabel(lfirst(k), tlist))
						break;
				}
				list_free(exprvars);
				if (!k)
				{
					pk_datatype = em->em_datatype;
					break;		/* found usable expression */
				}
			}
			if (!j)
				elog(ERROR, "could not find pathkey item to sort");

			/*
			 * Do we need to insert a Result node?
			 */
			if (!adjust_tlist_in_place &&
				!is_projection_capable_plan(lefttree))
			{
				/* copy needed so we don't modify input's tlist below */
				tlist = copyObject(tlist);
				lefttree = inject_projection_plan(lefttree, tlist);
			}

			/* Don't bother testing is_projection_capable_plan again */
			adjust_tlist_in_place = true;

			/*
			 * Add resjunk entry to input's tlist
			 */
			tle = makeTargetEntry(sortexpr,
								  list_length(tlist) + 1,
								  NULL,
								  true);
			tlist = lappend(tlist, tle);
			lefttree->targetlist = tlist;		/* just in case NIL before */
		}

		/*
		 * Look up the correct sort operator from the PathKey's slightly
		 * abstracted representation.
		 */
		sortop = get_opfamily_member(pathkey->pk_opfamily,
									 pk_datatype,
									 pk_datatype,
									 pathkey->pk_strategy);
		if (!OidIsValid(sortop))	/* should not happen */
			elog(ERROR, "could not find member %d(%u,%u) of opfamily %u",
				 pathkey->pk_strategy, pk_datatype, pk_datatype,
				 pathkey->pk_opfamily);

		/* Add the column to the sort arrays */
		sortColIdx[numsortkeys] = tle->resno;
		sortOperators[numsortkeys] = sortop;
		collations[numsortkeys] = ec->ec_collation;
		nullsFirst[numsortkeys] = pathkey->pk_nulls_first;
		numsortkeys++;
	}

	/* Return results */
	*p_numsortkeys = numsortkeys;
	*p_sortColIdx = sortColIdx;
	*p_sortOperators = sortOperators;
	*p_collations = collations;
	*p_nullsFirst = nullsFirst;

	return lefttree;
}

/*
 * find_ec_member_for_tle
 *		Locate an EquivalenceClass member matching the given TLE, if any
 *
 * Child EC members are ignored unless they match 'relids'.
 */
static EquivalenceMember *
find_ec_member_for_tle(EquivalenceClass *ec,
					   TargetEntry *tle,
					   Relids relids)
{
	Expr	   *tlexpr;
	ListCell   *lc;

	/* We ignore binary-compatible relabeling on both ends */
	tlexpr = tle->expr;
	while (tlexpr && IsA(tlexpr, RelabelType))
		tlexpr = ((RelabelType *) tlexpr)->arg;

	foreach(lc, ec->ec_members)
	{
		EquivalenceMember *em = (EquivalenceMember *) lfirst(lc);
		Expr	   *emexpr;

		/*
		 * We shouldn't be trying to sort by an equivalence class that
		 * contains a constant, so no need to consider such cases any further.
		 */
		if (em->em_is_const)
			continue;

		/*
		 * Ignore child members unless they match the rel being sorted.
		 */
		if (em->em_is_child &&
			!bms_equal(em->em_relids, relids))
			continue;

		/* Match if same expression (after stripping relabel) */
		emexpr = em->em_expr;
		while (emexpr && IsA(emexpr, RelabelType))
			emexpr = ((RelabelType *) emexpr)->arg;

		if (equal(emexpr, tlexpr))
			return em;
	}

	return NULL;
}

/*
 * make_sort_from_pathkeys
 *	  Create sort plan to sort according to given pathkeys
 *
 *	  'lefttree' is the node which yields input tuples
 *	  'pathkeys' is the list of pathkeys by which the result is to be sorted
 */
static Sort *
make_sort_from_pathkeys(Plan *lefttree, List *pathkeys)
{
	int			numsortkeys;
	AttrNumber *sortColIdx;
	Oid		   *sortOperators;
	Oid		   *collations;
	bool	   *nullsFirst;

	/* Compute sort column info, and adjust lefttree as needed */
	lefttree = prepare_sort_from_pathkeys(lefttree, pathkeys,
										  NULL,
										  NULL,
										  false,
										  &numsortkeys,
										  &sortColIdx,
										  &sortOperators,
										  &collations,
										  &nullsFirst);

	/* Now build the Sort node */
	return make_sort(lefttree, numsortkeys,
					 sortColIdx, sortOperators,
					 collations, nullsFirst);
}

/*
 * make_sort_from_sortclauses
 *	  Create sort plan to sort according to given sortclauses
 *
 *	  'sortcls' is a list of SortGroupClauses
 *	  'lefttree' is the node which yields input tuples
 */
Sort *
make_sort_from_sortclauses(List *sortcls, Plan *lefttree)
{
	List	   *sub_tlist = lefttree->targetlist;
	ListCell   *l;
	int			numsortkeys;
	AttrNumber *sortColIdx;
	Oid		   *sortOperators;
	Oid		   *collations;
	bool	   *nullsFirst;

	/* Convert list-ish representation to arrays wanted by executor */
	numsortkeys = list_length(sortcls);
	sortColIdx = (AttrNumber *) palloc(numsortkeys * sizeof(AttrNumber));
	sortOperators = (Oid *) palloc(numsortkeys * sizeof(Oid));
	collations = (Oid *) palloc(numsortkeys * sizeof(Oid));
	nullsFirst = (bool *) palloc(numsortkeys * sizeof(bool));

	numsortkeys = 0;
	foreach(l, sortcls)
	{
		SortGroupClause *sortcl = (SortGroupClause *) lfirst(l);
		TargetEntry *tle = get_sortgroupclause_tle(sortcl, sub_tlist);

		sortColIdx[numsortkeys] = tle->resno;
		sortOperators[numsortkeys] = sortcl->sortop;
		collations[numsortkeys] = exprCollation((Node *) tle->expr);
		nullsFirst[numsortkeys] = sortcl->nulls_first;
		numsortkeys++;
	}

	return make_sort(lefttree, numsortkeys,
					 sortColIdx, sortOperators,
					 collations, nullsFirst);
}

/*
 * make_sort_from_groupcols
 *	  Create sort plan to sort based on grouping columns
 *
 * 'groupcls' is the list of SortGroupClauses
 * 'grpColIdx' gives the column numbers to use
 *
 * This might look like it could be merged with make_sort_from_sortclauses,
 * but presently we *must* use the grpColIdx[] array to locate sort columns,
 * because the child plan's tlist is not marked with ressortgroupref info
 * appropriate to the grouping node.  So, only the sort ordering info
 * is used from the SortGroupClause entries.
 */
static Sort *
make_sort_from_groupcols(List *groupcls,
						 AttrNumber *grpColIdx,
						 Plan *lefttree)
{
	List	   *sub_tlist = lefttree->targetlist;
	ListCell   *l;
	int			numsortkeys;
	AttrNumber *sortColIdx;
	Oid		   *sortOperators;
	Oid		   *collations;
	bool	   *nullsFirst;

	/* Convert list-ish representation to arrays wanted by executor */
	numsortkeys = list_length(groupcls);
	sortColIdx = (AttrNumber *) palloc(numsortkeys * sizeof(AttrNumber));
	sortOperators = (Oid *) palloc(numsortkeys * sizeof(Oid));
	collations = (Oid *) palloc(numsortkeys * sizeof(Oid));
	nullsFirst = (bool *) palloc(numsortkeys * sizeof(bool));

	numsortkeys = 0;
	foreach(l, groupcls)
	{
		SortGroupClause *grpcl = (SortGroupClause *) lfirst(l);
		TargetEntry *tle = get_tle_by_resno(sub_tlist, grpColIdx[numsortkeys]);

		if (!tle)
			elog(ERROR, "could not retrieve tle for sort-from-groupcols");

		sortColIdx[numsortkeys] = tle->resno;
		sortOperators[numsortkeys] = grpcl->sortop;
		collations[numsortkeys] = exprCollation((Node *) tle->expr);
		nullsFirst[numsortkeys] = grpcl->nulls_first;
		numsortkeys++;
	}

	return make_sort(lefttree, numsortkeys,
					 sortColIdx, sortOperators,
					 collations, nullsFirst);
}

static Material *
make_material(Plan *lefttree)
{
	Material   *node = makeNode(Material);
	Plan	   *plan = &node->plan;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	return node;
}

/*
 * materialize_finished_plan: stick a Material node atop a completed plan
 *
 * There are a couple of places where we want to attach a Material node
 * after completion of create_plan(), without any MaterialPath path.
 * Those places should probably be refactored someday to do this on the
 * Path representation, but it's not worth the trouble yet.
 */
Plan *
materialize_finished_plan(Plan *subplan)
{
	Plan	   *matplan;
	Path		matpath;		/* dummy for result of cost_material */

	matplan = (Plan *) make_material(subplan);

	/* Set cost data */
	cost_material(&matpath,
				  subplan->startup_cost,
				  subplan->total_cost,
				  subplan->plan_rows,
				  subplan->plan_width);
	matplan->startup_cost = matpath.startup_cost;
	matplan->total_cost = matpath.total_cost;
	matplan->plan_rows = subplan->plan_rows;
	matplan->plan_width = subplan->plan_width;
	matplan->parallel_aware = false;

	return matplan;
}

Agg *
make_agg(List *tlist, List *qual,
		 AggStrategy aggstrategy,
		 bool combineStates, bool finalizeAggs,
		 int numGroupCols, AttrNumber *grpColIdx, Oid *grpOperators,
		 List *groupingSets, List *chain,
		 double dNumGroups, Plan *lefttree)
{
	Agg		   *node = makeNode(Agg);
	Plan	   *plan = &node->plan;
	long		numGroups;

	/* Reduce to long, but 'ware overflow! */
	numGroups = (long) Min(dNumGroups, (double) LONG_MAX);

	node->aggstrategy = aggstrategy;
	node->combineStates = combineStates;
	node->finalizeAggs = finalizeAggs;
	node->numCols = numGroupCols;
	node->grpColIdx = grpColIdx;
	node->grpOperators = grpOperators;
	node->numGroups = numGroups;
	node->groupingSets = groupingSets;
	node->chain = chain;

	plan->qual = qual;
	plan->targetlist = tlist;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	return node;
}

static WindowAgg *
make_windowagg(List *tlist, Index winref,
			   int partNumCols, AttrNumber *partColIdx, Oid *partOperators,
			   int ordNumCols, AttrNumber *ordColIdx, Oid *ordOperators,
			   int frameOptions, Node *startOffset, Node *endOffset,
			   Plan *lefttree)
{
	WindowAgg  *node = makeNode(WindowAgg);
	Plan	   *plan = &node->plan;

	node->winref = winref;
	node->partNumCols = partNumCols;
	node->partColIdx = partColIdx;
	node->partOperators = partOperators;
	node->ordNumCols = ordNumCols;
	node->ordColIdx = ordColIdx;
	node->ordOperators = ordOperators;
	node->frameOptions = frameOptions;
	node->startOffset = startOffset;
	node->endOffset = endOffset;

	plan->targetlist = tlist;
	plan->lefttree = lefttree;
	plan->righttree = NULL;
	/* WindowAgg nodes never have a qual clause */
	plan->qual = NIL;

	return node;
}

static Group *
make_group(List *tlist,
		   List *qual,
		   int numGroupCols,
		   AttrNumber *grpColIdx,
		   Oid *grpOperators,
		   Plan *lefttree)
{
	Group	   *node = makeNode(Group);
	Plan	   *plan = &node->plan;

	node->numCols = numGroupCols;
	node->grpColIdx = grpColIdx;
	node->grpOperators = grpOperators;

	plan->qual = qual;
	plan->targetlist = tlist;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	return node;
}

/*
 * distinctList is a list of SortGroupClauses, identifying the targetlist items
 * that should be considered by the Unique filter.  The input path must
 * already be sorted accordingly.
 */
static Unique *
make_unique_from_sortclauses(Plan *lefttree, List *distinctList)
{
	Unique	   *node = makeNode(Unique);
	Plan	   *plan = &node->plan;
	int			numCols = list_length(distinctList);
	int			keyno = 0;
	AttrNumber *uniqColIdx;
	Oid		   *uniqOperators;
	ListCell   *slitem;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	/*
	 * convert SortGroupClause list into arrays of attr indexes and equality
	 * operators, as wanted by executor
	 */
	Assert(numCols > 0);
	uniqColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numCols);
	uniqOperators = (Oid *) palloc(sizeof(Oid) * numCols);

	foreach(slitem, distinctList)
	{
		SortGroupClause *sortcl = (SortGroupClause *) lfirst(slitem);
		TargetEntry *tle = get_sortgroupclause_tle(sortcl, plan->targetlist);

		uniqColIdx[keyno] = tle->resno;
		uniqOperators[keyno] = sortcl->eqop;
		Assert(OidIsValid(uniqOperators[keyno]));
		keyno++;
	}

	node->numCols = numCols;
	node->uniqColIdx = uniqColIdx;
	node->uniqOperators = uniqOperators;

	return node;
}

/*
 * as above, but use pathkeys to identify the sort columns and semantics
 */
static Unique *
make_unique_from_pathkeys(Plan *lefttree, List *pathkeys, int numCols)
{
	Unique	   *node = makeNode(Unique);
	Plan	   *plan = &node->plan;
	int			keyno = 0;
	AttrNumber *uniqColIdx;
	Oid		   *uniqOperators;
	ListCell   *lc;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	/*
	 * Convert pathkeys list into arrays of attr indexes and equality
	 * operators, as wanted by executor.  This has a lot in common with
	 * prepare_sort_from_pathkeys ... maybe unify sometime?
	 */
	Assert(numCols >= 0 && numCols <= list_length(pathkeys));
	uniqColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numCols);
	uniqOperators = (Oid *) palloc(sizeof(Oid) * numCols);

	foreach(lc, pathkeys)
	{
		PathKey    *pathkey = (PathKey *) lfirst(lc);
		EquivalenceClass *ec = pathkey->pk_eclass;
		EquivalenceMember *em;
		TargetEntry *tle = NULL;
		Oid			pk_datatype = InvalidOid;
		Oid			eqop;
		ListCell   *j;

		/* Ignore pathkeys beyond the specified number of columns */
		if (keyno >= numCols)
			break;

		if (ec->ec_has_volatile)
		{
			/*
			 * If the pathkey's EquivalenceClass is volatile, then it must
			 * have come from an ORDER BY clause, and we have to match it to
			 * that same targetlist entry.
			 */
			if (ec->ec_sortref == 0)	/* can't happen */
				elog(ERROR, "volatile EquivalenceClass has no sortref");
			tle = get_sortgroupref_tle(ec->ec_sortref, plan->targetlist);
			Assert(tle);
			Assert(list_length(ec->ec_members) == 1);
			pk_datatype = ((EquivalenceMember *) linitial(ec->ec_members))->em_datatype;
		}
		else
		{
			/*
			 * Otherwise, we can use any non-constant expression listed in the
			 * pathkey's EquivalenceClass.  For now, we take the first tlist
			 * item found in the EC.
			 */
			foreach(j, plan->targetlist)
			{
				tle = (TargetEntry *) lfirst(j);
				em = find_ec_member_for_tle(ec, tle, NULL);
				if (em)
				{
					/* found expr already in tlist */
					pk_datatype = em->em_datatype;
					break;
				}
				tle = NULL;
			}
		}

		if (!tle)
			elog(ERROR, "could not find pathkey item to sort");

		/*
		 * Look up the correct equality operator from the PathKey's slightly
		 * abstracted representation.
		 */
		eqop = get_opfamily_member(pathkey->pk_opfamily,
								   pk_datatype,
								   pk_datatype,
								   BTEqualStrategyNumber);
		if (!OidIsValid(eqop))	/* should not happen */
			elog(ERROR, "could not find member %d(%u,%u) of opfamily %u",
				 BTEqualStrategyNumber, pk_datatype, pk_datatype,
				 pathkey->pk_opfamily);

		uniqColIdx[keyno] = tle->resno;
		uniqOperators[keyno] = eqop;

		keyno++;
	}

	node->numCols = numCols;
	node->uniqColIdx = uniqColIdx;
	node->uniqOperators = uniqOperators;

	return node;
}

static Gather *
make_gather(List *qptlist,
			List *qpqual,
			int nworkers,
			bool single_copy,
			Plan *subplan)
{
	Gather	   *node = makeNode(Gather);
	Plan	   *plan = &node->plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = subplan;
	plan->righttree = NULL;
	node->num_workers = nworkers;
	node->single_copy = single_copy;
	node->invisible	= false;

	return node;
}

/*
 * distinctList is a list of SortGroupClauses, identifying the targetlist
 * items that should be considered by the SetOp filter.  The input path must
 * already be sorted accordingly.
 */
static SetOp *
make_setop(SetOpCmd cmd, SetOpStrategy strategy, Plan *lefttree,
		   List *distinctList, AttrNumber flagColIdx, int firstFlag,
		   long numGroups)
{
	SetOp	   *node = makeNode(SetOp);
	Plan	   *plan = &node->plan;
	int			numCols = list_length(distinctList);
	int			keyno = 0;
	AttrNumber *dupColIdx;
	Oid		   *dupOperators;
	ListCell   *slitem;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	/*
	 * convert SortGroupClause list into arrays of attr indexes and equality
	 * operators, as wanted by executor
	 */
	Assert(numCols > 0);
	dupColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numCols);
	dupOperators = (Oid *) palloc(sizeof(Oid) * numCols);

	foreach(slitem, distinctList)
	{
		SortGroupClause *sortcl = (SortGroupClause *) lfirst(slitem);
		TargetEntry *tle = get_sortgroupclause_tle(sortcl, plan->targetlist);

		dupColIdx[keyno] = tle->resno;
		dupOperators[keyno] = sortcl->eqop;
		Assert(OidIsValid(dupOperators[keyno]));
		keyno++;
	}

	node->cmd = cmd;
	node->strategy = strategy;
	node->numCols = numCols;
	node->dupColIdx = dupColIdx;
	node->dupOperators = dupOperators;
	node->flagColIdx = flagColIdx;
	node->firstFlag = firstFlag;
	node->numGroups = numGroups;

	return node;
}

/*
 * make_lockrows
 *	  Build a LockRows plan node
 */
static LockRows *
make_lockrows(Plan *lefttree, List *rowMarks, int epqParam)
{
	LockRows   *node = makeNode(LockRows);
	Plan	   *plan = &node->plan;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	node->rowMarks = rowMarks;
	node->epqParam = epqParam;

	return node;
}

/*
 * make_limit
 *	  Build a Limit plan node
 */
Limit *
make_limit(Plan *lefttree, Node *limitOffset, Node *limitCount)
{
	Limit	   *node = makeNode(Limit);
	Plan	   *plan = &node->plan;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	node->limitOffset = limitOffset;
	node->limitCount = limitCount;

	return node;
}

/*
 * make_result
 *	  Build a Result plan node
 */
static Result *
make_result(List *tlist,
			Node *resconstantqual,
			Plan *subplan)
{
	Result	   *node = makeNode(Result);
	Plan	   *plan = &node->plan;

	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = subplan;
	plan->righttree = NULL;
	node->resconstantqual = resconstantqual;

	return node;
}

/*
 * make_modifytable
 *	  Build a ModifyTable plan node
 */
static ModifyTable *
make_modifytable(PlannerInfo *root,
				 CmdType operation, bool canSetTag,
				 Index nominalRelation,
				 List *resultRelations, List *subplans,
				 List *withCheckOptionLists, List *returningLists,
				 List *rowMarks, OnConflictExpr *onconflict, int epqParam)
{
	ModifyTable *node = makeNode(ModifyTable);
	List	   *fdw_private_list;
	ListCell   *lc;
	int			i;

	Assert(list_length(resultRelations) == list_length(subplans));
	Assert(withCheckOptionLists == NIL ||
		   list_length(resultRelations) == list_length(withCheckOptionLists));
	Assert(returningLists == NIL ||
		   list_length(resultRelations) == list_length(returningLists));

	node->plan.lefttree = NULL;
	node->plan.righttree = NULL;
	node->plan.qual = NIL;
	/* setrefs.c will fill in the targetlist, if needed */
	node->plan.targetlist = NIL;

	node->operation = operation;
	node->canSetTag = canSetTag;
	node->nominalRelation = nominalRelation;
	node->resultRelations = resultRelations;
	node->resultRelIndex = -1;	/* will be set correctly in setrefs.c */
	node->plans = subplans;
	if (!onconflict)
	{
		node->onConflictAction = ONCONFLICT_NONE;
		node->onConflictSet = NIL;
		node->onConflictWhere = NULL;
		node->arbiterIndexes = NIL;
		node->exclRelRTI = 0;
		node->exclRelTlist = NIL;
	}
	else
	{
		node->onConflictAction = onconflict->action;
		node->onConflictSet = onconflict->onConflictSet;
		node->onConflictWhere = onconflict->onConflictWhere;

		/*
		 * If a set of unique index inference elements was provided (an
		 * INSERT...ON CONFLICT "inference specification"), then infer
		 * appropriate unique indexes (or throw an error if none are
		 * available).
		 */
		node->arbiterIndexes = infer_arbiter_indexes(root);

		node->exclRelRTI = onconflict->exclRelIndex;
		node->exclRelTlist = onconflict->exclRelTlist;
	}
	node->withCheckOptionLists = withCheckOptionLists;
	node->returningLists = returningLists;
	node->rowMarks = rowMarks;
	node->epqParam = epqParam;

	/*
	 * For each result relation that is a foreign table, allow the FDW to
	 * construct private plan data, and accumulate it all into a list.
	 */
	fdw_private_list = NIL;
	i = 0;
	foreach(lc, resultRelations)
	{
		Index		rti = lfirst_int(lc);
		FdwRoutine *fdwroutine;
		List	   *fdw_private;

		/*
		 * If possible, we want to get the FdwRoutine from our RelOptInfo for
		 * the table.  But sometimes we don't have a RelOptInfo and must get
		 * it the hard way.  (In INSERT, the target relation is not scanned,
		 * so it's not a baserel; and there are also corner cases for
		 * updatable views where the target rel isn't a baserel.)
		 */
		if (rti < root->simple_rel_array_size &&
			root->simple_rel_array[rti] != NULL)
		{
			RelOptInfo *resultRel = root->simple_rel_array[rti];

			fdwroutine = resultRel->fdwroutine;
		}
		else
		{
			RangeTblEntry *rte = planner_rt_fetch(rti, root);

			Assert(rte->rtekind == RTE_RELATION);
			if (rte->relkind == RELKIND_FOREIGN_TABLE)
				fdwroutine = GetFdwRoutineByRelId(rte->relid);
			else
				fdwroutine = NULL;
		}

		if (fdwroutine != NULL &&
			fdwroutine->PlanForeignModify != NULL)
			fdw_private = fdwroutine->PlanForeignModify(root, node, rti, i);
		else
			fdw_private = NIL;
		fdw_private_list = lappend(fdw_private_list, fdw_private);
		i++;
	}
	node->fdwPrivLists = fdw_private_list;

	return node;
}

/*
 * is_projection_capable_path
 *		Check whether a given Path node is able to do projection.
 */
bool
is_projection_capable_path(Path *path)
{
	/* Most plan types can project, so just list the ones that can't */
	switch (path->pathtype)
	{
		case T_Hash:
		case T_Material:
		case T_Sort:
		case T_Unique:
		case T_SetOp:
		case T_LockRows:
		case T_Limit:
		case T_ModifyTable:
		case T_MergeAppend:
		case T_RecursiveUnion:
			return false;
		case T_Append:

			/*
			 * Append can't project, but if it's being used to represent a
			 * dummy path, claim that it can project.  This prevents us from
			 * converting a rel from dummy to non-dummy status by applying a
			 * projection to its dummy path.
			 */
			return IS_DUMMY_PATH(path);
		default:
			break;
	}
	return true;
}

/*
 * is_projection_capable_plan
 *		Check whether a given Plan node is able to do projection.
 */
bool
is_projection_capable_plan(Plan *plan)
{
	/* Most plan types can project, so just list the ones that can't */
	switch (nodeTag(plan))
	{
		case T_Hash:
		case T_Material:
		case T_Sort:
		case T_Unique:
		case T_SetOp:
		case T_LockRows:
		case T_Limit:
		case T_ModifyTable:
		case T_Append:
		case T_MergeAppend:
		case T_RecursiveUnion:
			return false;
		default:
			break;
	}
	return true;
}
