/*-------------------------------------------------------------------------
 *
 * createplan.c
 *	  Routines to create the desired plan for processing a query.
 *	  Planning is complete, we just need to convert the selected
 *	  Path into a Plan.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/createplan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/sysattr.h"
#include "catalog/pg_class.h"
#include "foreign/fdwapi.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/paramassign.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/placeholder.h"
#include "optimizer/plancat.h"
#include "optimizer/planmain.h"
#include "optimizer/prep.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/subselect.h"
#include "optimizer/tlist.h"
#include "parser/parse_clause.h"
#include "parser/parsetree.h"
#include "partitioning/partprune.h"
#include "tcop/tcopprot.h"
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
 *
 * CP_IGNORE_TLIST specifies that the caller plans to replace the targetlist,
 * and therefore it doesn't matter a bit what target list gets generated.
 */
#define CP_EXACT_TLIST		0x0001	/* Plan must return specified tlist */
#define CP_SMALL_TLIST		0x0002	/* Prefer narrower tlists */
#define CP_LABEL_TLIST		0x0004	/* tlist must contain sortgrouprefs */
#define CP_IGNORE_TLIST		0x0008	/* caller will replace tlist */


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
static bool mark_async_capable_plan(Plan *plan, Path *path);
static Plan *create_append_plan(PlannerInfo *root, AppendPath *best_path,
								int flags);
static Plan *create_merge_append_plan(PlannerInfo *root, MergeAppendPath *best_path,
									  int flags);
static Result *create_group_result_plan(PlannerInfo *root,
										GroupResultPath *best_path);
static ProjectSet *create_project_set_plan(PlannerInfo *root, ProjectSetPath *best_path);
static Material *create_material_plan(PlannerInfo *root, MaterialPath *best_path,
									  int flags);
static Memoize *create_memoize_plan(PlannerInfo *root, MemoizePath *best_path,
									int flags);
static Plan *create_unique_plan(PlannerInfo *root, UniquePath *best_path,
								int flags);
static Gather *create_gather_plan(PlannerInfo *root, GatherPath *best_path);
static Plan *create_projection_plan(PlannerInfo *root,
									ProjectionPath *best_path,
									int flags);
static Plan *inject_projection_plan(Plan *subplan, List *tlist, bool parallel_safe);
static Sort *create_sort_plan(PlannerInfo *root, SortPath *best_path, int flags);
static IncrementalSort *create_incrementalsort_plan(PlannerInfo *root,
													IncrementalSortPath *best_path, int flags);
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
static void bitmap_subplan_mark_shared(Plan *plan);
static TidScan *create_tidscan_plan(PlannerInfo *root, TidPath *best_path,
									List *tlist, List *scan_clauses);
static TidRangeScan *create_tidrangescan_plan(PlannerInfo *root,
											  TidRangePath *best_path,
											  List *tlist,
											  List *scan_clauses);
static SubqueryScan *create_subqueryscan_plan(PlannerInfo *root,
											  SubqueryScanPath *best_path,
											  List *tlist, List *scan_clauses);
static FunctionScan *create_functionscan_plan(PlannerInfo *root, Path *best_path,
											  List *tlist, List *scan_clauses);
static ValuesScan *create_valuesscan_plan(PlannerInfo *root, Path *best_path,
										  List *tlist, List *scan_clauses);
static TableFuncScan *create_tablefuncscan_plan(PlannerInfo *root, Path *best_path,
												List *tlist, List *scan_clauses);
static CteScan *create_ctescan_plan(PlannerInfo *root, Path *best_path,
									List *tlist, List *scan_clauses);
static NamedTuplestoreScan *create_namedtuplestorescan_plan(PlannerInfo *root,
															Path *best_path, List *tlist, List *scan_clauses);
static Result *create_resultscan_plan(PlannerInfo *root, Path *best_path,
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
static void fix_indexqual_references(PlannerInfo *root, IndexPath *index_path,
									 List **stripped_indexquals_p,
									 List **fixed_indexquals_p);
static List *fix_indexorderby_references(PlannerInfo *root, IndexPath *index_path);
static Node *fix_indexqual_clause(PlannerInfo *root,
								  IndexOptInfo *index, int indexcol,
								  Node *clause, List *indexcolnos);
static Node *fix_indexqual_operand(Node *node, IndexOptInfo *index, int indexcol);
static List *get_switched_clauses(List *clauses, Relids outerrelids);
static List *order_qual_clauses(PlannerInfo *root, List *clauses);
static void copy_generic_path_info(Plan *dest, Path *src);
static void copy_plan_costsize(Plan *dest, Plan *src);
static void label_sort_with_costsize(PlannerInfo *root, Sort *plan,
									 double limit_tuples);
static void label_incrementalsort_with_costsize(PlannerInfo *root, IncrementalSort *plan,
												List *pathkeys, double limit_tuples);
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
										 List *indexqual, List *recheckqual,
										 List *indexorderby,
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
static TidRangeScan *make_tidrangescan(List *qptlist, List *qpqual,
									   Index scanrelid, List *tidrangequals);
static SubqueryScan *make_subqueryscan(List *qptlist,
									   List *qpqual,
									   Index scanrelid,
									   Plan *subplan);
static FunctionScan *make_functionscan(List *qptlist, List *qpqual,
									   Index scanrelid, List *functions, bool funcordinality);
static ValuesScan *make_valuesscan(List *qptlist, List *qpqual,
								   Index scanrelid, List *values_lists);
static TableFuncScan *make_tablefuncscan(List *qptlist, List *qpqual,
										 Index scanrelid, TableFunc *tablefunc);
static CteScan *make_ctescan(List *qptlist, List *qpqual,
							 Index scanrelid, int ctePlanId, int cteParam);
static NamedTuplestoreScan *make_namedtuplestorescan(List *qptlist, List *qpqual,
													 Index scanrelid, char *enrname);
static WorkTableScan *make_worktablescan(List *qptlist, List *qpqual,
										 Index scanrelid, int wtParam);
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
							   JoinType jointype, bool inner_unique);
static HashJoin *make_hashjoin(List *tlist,
							   List *joinclauses, List *otherclauses,
							   List *hashclauses,
							   List *hashoperators, List *hashcollations,
							   List *hashkeys,
							   Plan *lefttree, Plan *righttree,
							   JoinType jointype, bool inner_unique);
static Hash *make_hash(Plan *lefttree,
					   List *hashkeys,
					   Oid skewTable,
					   AttrNumber skewColumn,
					   bool skewInherit);
static MergeJoin *make_mergejoin(List *tlist,
								 List *joinclauses, List *otherclauses,
								 List *mergeclauses,
								 Oid *mergefamilies,
								 Oid *mergecollations,
								 bool *mergereversals,
								 bool *mergenullsfirst,
								 Plan *lefttree, Plan *righttree,
								 JoinType jointype, bool inner_unique,
								 bool skip_mark_restore);
static Sort *make_sort(Plan *lefttree, int numCols,
					   AttrNumber *sortColIdx, Oid *sortOperators,
					   Oid *collations, bool *nullsFirst);
static IncrementalSort *make_incrementalsort(Plan *lefttree,
											 int numCols, int nPresortedCols,
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
static Sort *make_sort_from_pathkeys(Plan *lefttree, List *pathkeys,
									 Relids relids);
static IncrementalSort *make_incrementalsort_from_pathkeys(Plan *lefttree,
														   List *pathkeys, Relids relids, int nPresortedCols);
static Sort *make_sort_from_groupcols(List *groupcls,
									  AttrNumber *grpColIdx,
									  Plan *lefttree);
static Material *make_material(Plan *lefttree);
static Memoize *make_memoize(Plan *lefttree, Oid *hashoperators,
							 Oid *collations, List *param_exprs,
							 bool singlerow, bool binary_mode,
							 uint32 est_entries, Bitmapset *keyparamids);
static WindowAgg *make_windowagg(List *tlist, Index winref,
								 int partNumCols, AttrNumber *partColIdx, Oid *partOperators, Oid *partCollations,
								 int ordNumCols, AttrNumber *ordColIdx, Oid *ordOperators, Oid *ordCollations,
								 int frameOptions, Node *startOffset, Node *endOffset,
								 Oid startInRangeFunc, Oid endInRangeFunc,
								 Oid inRangeColl, bool inRangeAsc, bool inRangeNullsFirst,
								 List *runCondition, List *qual, bool topWindow,
								 Plan *lefttree);
static Group *make_group(List *tlist, List *qual, int numGroupCols,
						 AttrNumber *grpColIdx, Oid *grpOperators, Oid *grpCollations,
						 Plan *lefttree);
static Unique *make_unique_from_sortclauses(Plan *lefttree, List *distinctList);
static Unique *make_unique_from_pathkeys(Plan *lefttree,
										 List *pathkeys, int numCols);
static Gather *make_gather(List *qptlist, List *qpqual,
						   int nworkers, int rescan_param, bool single_copy, Plan *subplan);
static SetOp *make_setop(SetOpCmd cmd, SetOpStrategy strategy, Plan *lefttree,
						 List *distinctList, AttrNumber flagColIdx, int firstFlag,
						 long numGroups);
static LockRows *make_lockrows(Plan *lefttree, List *rowMarks, int epqParam);
static Result *make_result(List *tlist, Node *resconstantqual, Plan *subplan);
static ProjectSet *make_project_set(List *tlist, Plan *subplan);
static ModifyTable *make_modifytable(PlannerInfo *root, Plan *subplan,
									 CmdType operation, bool canSetTag,
									 Index nominalRelation, Index rootRelation,
									 bool partColsUpdated,
									 List *resultRelations,
									 List *updateColnosLists,
									 List *withCheckOptionLists, List *returningLists,
									 List *rowMarks, OnConflictExpr *onconflict,
									 List *mergeActionLists, List *mergeJoinConditions,
									 int epqParam);
static GatherMerge *create_gather_merge_plan(PlannerInfo *root,
											 GatherMergePath *best_path);


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

	/* Initialize this module's workspace in PlannerInfo */
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
	 * node.  (In principle the initplans could go in any plan node at or
	 * above where they're referenced, but there seems no reason to put them
	 * any lower than the topmost node for the query level.  Also, see
	 * comments for SS_finalize_plan before you try to change this.)
	 */
	SS_attach_initplans(root, plan);

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

	/* Guard against stack overflow due to overly complex plans */
	check_stack_depth();

	switch (best_path->pathtype)
	{
		case T_SeqScan:
		case T_SampleScan:
		case T_IndexScan:
		case T_IndexOnlyScan:
		case T_BitmapHeapScan:
		case T_TidScan:
		case T_TidRangeScan:
		case T_SubqueryScan:
		case T_FunctionScan:
		case T_TableFuncScan:
		case T_ValuesScan:
		case T_CteScan:
		case T_WorkTableScan:
		case T_NamedTuplestoreScan:
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
									  (AppendPath *) best_path,
									  flags);
			break;
		case T_MergeAppend:
			plan = create_merge_append_plan(root,
											(MergeAppendPath *) best_path,
											flags);
			break;
		case T_Result:
			if (IsA(best_path, ProjectionPath))
			{
				plan = create_projection_plan(root,
											  (ProjectionPath *) best_path,
											  flags);
			}
			else if (IsA(best_path, MinMaxAggPath))
			{
				plan = (Plan *) create_minmaxagg_plan(root,
													  (MinMaxAggPath *) best_path);
			}
			else if (IsA(best_path, GroupResultPath))
			{
				plan = (Plan *) create_group_result_plan(root,
														 (GroupResultPath *) best_path);
			}
			else
			{
				/* Simple RTE_RESULT base relation */
				Assert(IsA(best_path, Path));
				plan = create_scan_plan(root, best_path, flags);
			}
			break;
		case T_ProjectSet:
			plan = (Plan *) create_project_set_plan(root,
													(ProjectSetPath *) best_path);
			break;
		case T_Material:
			plan = (Plan *) create_material_plan(root,
												 (MaterialPath *) best_path,
												 flags);
			break;
		case T_Memoize:
			plan = (Plan *) create_memoize_plan(root,
												(MemoizePath *) best_path,
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
		case T_IncrementalSort:
			plan = (Plan *) create_incrementalsort_plan(root,
														(IncrementalSortPath *) best_path,
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
		case T_GatherMerge:
			plan = (Plan *) create_gather_merge_plan(root,
													 (GatherMergePath *) best_path);
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
	 *
	 * If this is a plain indexscan or index-only scan, we need not consider
	 * restriction clauses that are implied by the index's predicate, so use
	 * indrestrictinfo not baserestrictinfo.  Note that we can't do that for
	 * bitmap indexscans, since there's not necessarily a single index
	 * involved; but it doesn't matter since create_bitmap_scan_plan() will be
	 * able to get rid of such clauses anyway via predicate proof.
	 */
	switch (best_path->pathtype)
	{
		case T_IndexScan:
		case T_IndexOnlyScan:
			scan_clauses = castNode(IndexPath, best_path)->indexinfo->indrestrictinfo;
			break;
		default:
			scan_clauses = rel->baserestrictinfo;
			break;
	}

	/*
	 * If this is a parameterized scan, we also need to enforce all the join
	 * clauses available from the outer relation(s).
	 *
	 * For paranoia's sake, don't modify the stored baserestrictinfo list.
	 */
	if (best_path->param_info)
		scan_clauses = list_concat_copy(scan_clauses,
										best_path->param_info->ppi_clauses);

	/*
	 * Detect whether we have any pseudoconstant quals to deal with.  Then, if
	 * we'll need a gating Result node, it will be able to project, so there
	 * are no requirements on the child's tlist.
	 *
	 * If this replaces a join, it must be a foreign scan or a custom scan,
	 * and the FDW or the custom scan provider would have stored in the best
	 * path the list of RestrictInfo nodes to apply to the join; check against
	 * that list in that case.
	 */
	if (IS_JOIN_REL(rel))
	{
		List	   *join_clauses;

		Assert(best_path->pathtype == T_ForeignScan ||
			   best_path->pathtype == T_CustomScan);
		if (best_path->pathtype == T_ForeignScan)
			join_clauses = ((ForeignPath *) best_path)->fdw_restrictinfo;
		else
			join_clauses = ((CustomPath *) best_path)->custom_restrictinfo;

		gating_clauses = get_gating_quals(root, join_clauses);
	}
	else
		gating_clauses = get_gating_quals(root, scan_clauses);
	if (gating_clauses)
		flags = 0;

	/*
	 * For table scans, rather than using the relation targetlist (which is
	 * only those Vars actually needed by the query), we prefer to generate a
	 * tlist containing all Vars in order.  This will allow the executor to
	 * optimize away projection of the table tuples, if possible.
	 *
	 * But if the caller is going to ignore our tlist anyway, then don't
	 * bother generating one at all.  We use an exact equality test here, so
	 * that this only applies when CP_IGNORE_TLIST is the only flag set.
	 */
	if (flags == CP_IGNORE_TLIST)
	{
		tlist = NULL;
	}
	else if (use_physical_tlist(root, best_path, flags))
	{
		if (best_path->pathtype == T_IndexOnlyScan)
		{
			/* For index-only scan, the preferred tlist is the index's */
			tlist = copyObject(((IndexPath *) best_path)->indexinfo->indextlist);

			/*
			 * Transfer sortgroupref data to the replacement tlist, if
			 * requested (use_physical_tlist checked that this will work).
			 */
			if (flags & CP_LABEL_TLIST)
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
				/* As above, transfer sortgroupref data to replacement tlist */
				if (flags & CP_LABEL_TLIST)
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

		case T_TidRangeScan:
			plan = (Plan *) create_tidrangescan_plan(root,
													 (TidRangePath *) best_path,
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

		case T_TableFuncScan:
			plan = (Plan *) create_tablefuncscan_plan(root,
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

		case T_NamedTuplestoreScan:
			plan = (Plan *) create_namedtuplestorescan_plan(root,
															best_path,
															tlist,
															scan_clauses);
			break;

		case T_Result:
			plan = (Plan *) create_resultscan_plan(root,
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
	 * tablefunc scans, values scans, and CTE scans (but not for, eg, joins).
	 */
	if (rel->rtekind != RTE_RELATION &&
		rel->rtekind != RTE_SUBQUERY &&
		rel->rtekind != RTE_FUNCTION &&
		rel->rtekind != RTE_TABLEFUNC &&
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
	 * Also, don't do it to a CustomPath; the premise that we're extracting
	 * columns from a simple physical tuple is unlikely to hold for those.
	 * (When it does make sense, the custom path creator can set up the path's
	 * pathtarget that way.)
	 */
	if (IsA(path, CustomPath))
		return false;

	/*
	 * If a bitmap scan's tlist is empty, keep it as-is.  This may allow the
	 * executor to skip heap page fetches, and in any case, the benefit of
	 * using a physical tlist instead would be minimal.
	 */
	if (IsA(path, BitmapHeapPath) &&
		path->pathtarget->exprs == NIL)
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
	 * For an index-only scan, the "physical tlist" is the index's indextlist.
	 * We can only return that without a projection if all the index's columns
	 * are returnable.
	 */
	if (path->pathtype == T_IndexOnlyScan)
	{
		IndexOptInfo *indexinfo = ((IndexPath *) path)->indexinfo;

		for (i = 0; i < indexinfo->ncolumns; i++)
		{
			if (!indexinfo->canreturn[i])
				return false;
		}
	}

	/*
	 * Also, can't do it if CP_LABEL_TLIST is specified and path is requested
	 * to emit any sort/group columns that are not simple Vars.  (If they are
	 * simple Vars, they should appear in the physical tlist, and
	 * apply_pathtarget_labeling_to_tlist will take care of getting them
	 * labeled again.)	We also have to check that no two sort/group columns
	 * are the same Var, else that element of the physical tlist would need
	 * conflicting ressortgroupref labels.
	 */
	if ((flags & CP_LABEL_TLIST) && path->pathtarget->sortgrouprefs)
	{
		Bitmapset  *sortgroupatts = NULL;

		i = 0;
		foreach(lc, path->pathtarget->exprs)
		{
			Expr	   *expr = (Expr *) lfirst(lc);

			if (path->pathtarget->sortgrouprefs[i])
			{
				if (expr && IsA(expr, Var))
				{
					int			attno = ((Var *) expr)->varattno;

					attno -= FirstLowInvalidHeapAttributeNumber;
					if (bms_is_member(attno, sortgroupatts))
						return false;
					sortgroupatts = bms_add_member(sortgroupatts, attno);
				}
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
	Plan	   *splan;

	Assert(gating_quals);

	/*
	 * We might have a trivial Result plan already.  Stacking one Result atop
	 * another is silly, so if that applies, just discard the input plan.
	 * (We're assuming its targetlist is uninteresting; it should be either
	 * the same as the result of build_path_tlist, or a simplified version.)
	 */
	splan = plan;
	if (IsA(plan, Result))
	{
		Result	   *rplan = (Result *) plan;

		if (rplan->plan.lefttree == NULL &&
			rplan->resconstantqual == NULL)
			splan = NULL;
	}

	/*
	 * Since we need a Result node anyway, always return the path's requested
	 * tlist; that's never a wrong choice, even if the parent node didn't ask
	 * for CP_EXACT_TLIST.
	 */
	gplan = (Plan *) make_result(build_path_tlist(root, path),
								 (Node *) gating_quals,
								 splan);

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

	/* Gating quals could be unsafe, so better use the Path's safety flag */
	gplan->parallel_safe = path->parallel_safe;

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
 * mark_async_capable_plan
 *		Check whether the Plan node created from a Path node is async-capable,
 *		and if so, mark the Plan node as such and return true, otherwise
 *		return false.
 */
static bool
mark_async_capable_plan(Plan *plan, Path *path)
{
	switch (nodeTag(path))
	{
		case T_SubqueryScanPath:
			{
				SubqueryScan *scan_plan = (SubqueryScan *) plan;

				/*
				 * If the generated plan node includes a gating Result node,
				 * we can't execute it asynchronously.
				 */
				if (IsA(plan, Result))
					return false;

				/*
				 * If a SubqueryScan node atop of an async-capable plan node
				 * is deletable, consider it as async-capable.
				 */
				if (trivial_subqueryscan(scan_plan) &&
					mark_async_capable_plan(scan_plan->subplan,
											((SubqueryScanPath *) path)->subpath))
					break;
				return false;
			}
		case T_ForeignPath:
			{
				FdwRoutine *fdwroutine = path->parent->fdwroutine;

				/*
				 * If the generated plan node includes a gating Result node,
				 * we can't execute it asynchronously.
				 */
				if (IsA(plan, Result))
					return false;

				Assert(fdwroutine != NULL);
				if (fdwroutine->IsForeignPathAsyncCapable != NULL &&
					fdwroutine->IsForeignPathAsyncCapable((ForeignPath *) path))
					break;
				return false;
			}
		case T_ProjectionPath:

			/*
			 * If the generated plan node includes a Result node for the
			 * projection, we can't execute it asynchronously.
			 */
			if (IsA(plan, Result))
				return false;

			/*
			 * create_projection_plan() would have pulled up the subplan, so
			 * check the capability using the subpath.
			 */
			if (mark_async_capable_plan(plan,
										((ProjectionPath *) path)->subpath))
				return true;
			return false;
		default:
			return false;
	}

	plan->async_capable = true;

	return true;
}

/*
 * create_append_plan
 *	  Create an Append plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 *
 *	  Returns a Plan node.
 */
static Plan *
create_append_plan(PlannerInfo *root, AppendPath *best_path, int flags)
{
	Append	   *plan;
	List	   *tlist = build_path_tlist(root, &best_path->path);
	int			orig_tlist_length = list_length(tlist);
	bool		tlist_was_changed = false;
	List	   *pathkeys = best_path->path.pathkeys;
	List	   *subplans = NIL;
	ListCell   *subpaths;
	int			nasyncplans = 0;
	RelOptInfo *rel = best_path->path.parent;
	PartitionPruneInfo *partpruneinfo = NULL;
	int			nodenumsortkeys = 0;
	AttrNumber *nodeSortColIdx = NULL;
	Oid		   *nodeSortOperators = NULL;
	Oid		   *nodeCollations = NULL;
	bool	   *nodeNullsFirst = NULL;
	bool		consider_async = false;

	/*
	 * The subpaths list could be empty, if every child was proven empty by
	 * constraint exclusion.  In that case generate a dummy plan that returns
	 * no rows.
	 *
	 * Note that an AppendPath with no members is also generated in certain
	 * cases where there was no appending construct at all, but we know the
	 * relation is empty (see set_dummy_rel_pathlist and mark_dummy_rel).
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

	/*
	 * Otherwise build an Append plan.  Note that if there's just one child,
	 * the Append is pretty useless; but we wait till setrefs.c to get rid of
	 * it.  Doing so here doesn't work because the varno of the child scan
	 * plan won't match the parent-rel Vars it'll be asked to emit.
	 *
	 * We don't have the actual creation of the Append node split out into a
	 * separate make_xxx function.  This is because we want to run
	 * prepare_sort_from_pathkeys on it before we do so on the individual
	 * child plans, to make cross-checking the sort info easier.
	 */
	plan = makeNode(Append);
	plan->plan.targetlist = tlist;
	plan->plan.qual = NIL;
	plan->plan.lefttree = NULL;
	plan->plan.righttree = NULL;
	plan->apprelids = rel->relids;

	if (pathkeys != NIL)
	{
		/*
		 * Compute sort column info, and adjust the Append's tlist as needed.
		 * Because we pass adjust_tlist_in_place = true, we may ignore the
		 * function result; it must be the same plan node.  However, we then
		 * need to detect whether any tlist entries were added.
		 */
		(void) prepare_sort_from_pathkeys((Plan *) plan, pathkeys,
										  best_path->path.parent->relids,
										  NULL,
										  true,
										  &nodenumsortkeys,
										  &nodeSortColIdx,
										  &nodeSortOperators,
										  &nodeCollations,
										  &nodeNullsFirst);
		tlist_was_changed = (orig_tlist_length != list_length(plan->plan.targetlist));
	}

	/* If appropriate, consider async append */
	consider_async = (enable_async_append && pathkeys == NIL &&
					  !best_path->path.parallel_safe &&
					  list_length(best_path->subpaths) > 1);

	/* Build the plan for each child */
	foreach(subpaths, best_path->subpaths)
	{
		Path	   *subpath = (Path *) lfirst(subpaths);
		Plan	   *subplan;

		/* Must insist that all children return the same tlist */
		subplan = create_plan_recurse(root, subpath, CP_EXACT_TLIST);

		/*
		 * For ordered Appends, we must insert a Sort node if subplan isn't
		 * sufficiently ordered.
		 */
		if (pathkeys != NIL)
		{
			int			numsortkeys;
			AttrNumber *sortColIdx;
			Oid		   *sortOperators;
			Oid		   *collations;
			bool	   *nullsFirst;

			/*
			 * Compute sort column info, and adjust subplan's tlist as needed.
			 * We must apply prepare_sort_from_pathkeys even to subplans that
			 * don't need an explicit sort, to make sure they are returning
			 * the same sort key columns the Append expects.
			 */
			subplan = prepare_sort_from_pathkeys(subplan, pathkeys,
												 subpath->parent->relids,
												 nodeSortColIdx,
												 false,
												 &numsortkeys,
												 &sortColIdx,
												 &sortOperators,
												 &collations,
												 &nullsFirst);

			/*
			 * Check that we got the same sort key information.  We just
			 * Assert that the sortops match, since those depend only on the
			 * pathkeys; but it seems like a good idea to check the sort
			 * column numbers explicitly, to ensure the tlists match up.
			 */
			Assert(numsortkeys == nodenumsortkeys);
			if (memcmp(sortColIdx, nodeSortColIdx,
					   numsortkeys * sizeof(AttrNumber)) != 0)
				elog(ERROR, "Append child's targetlist doesn't match Append");
			Assert(memcmp(sortOperators, nodeSortOperators,
						  numsortkeys * sizeof(Oid)) == 0);
			Assert(memcmp(collations, nodeCollations,
						  numsortkeys * sizeof(Oid)) == 0);
			Assert(memcmp(nullsFirst, nodeNullsFirst,
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
		}

		/* If needed, check to see if subplan can be executed asynchronously */
		if (consider_async && mark_async_capable_plan(subplan, subpath))
		{
			Assert(subplan->async_capable);
			++nasyncplans;
		}

		subplans = lappend(subplans, subplan);
	}

	/*
	 * If any quals exist, they may be useful to perform further partition
	 * pruning during execution.  Gather information needed by the executor to
	 * do partition pruning.
	 */
	if (enable_partition_pruning)
	{
		List	   *prunequal;

		prunequal = extract_actual_clauses(rel->baserestrictinfo, false);

		if (best_path->path.param_info)
		{
			List	   *prmquals = best_path->path.param_info->ppi_clauses;

			prmquals = extract_actual_clauses(prmquals, false);
			prmquals = (List *) replace_nestloop_params(root,
														(Node *) prmquals);

			prunequal = list_concat(prunequal, prmquals);
		}

		if (prunequal != NIL)
			partpruneinfo =
				make_partition_pruneinfo(root, rel,
										 best_path->subpaths,
										 prunequal);
	}

	plan->appendplans = subplans;
	plan->nasyncplans = nasyncplans;
	plan->first_partial_plan = best_path->first_partial_path;
	plan->part_prune_info = partpruneinfo;

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	/*
	 * If prepare_sort_from_pathkeys added sort columns, but we were told to
	 * produce either the exact tlist or a narrow tlist, we should get rid of
	 * the sort columns again.  We must inject a projection node to do so.
	 */
	if (tlist_was_changed && (flags & (CP_EXACT_TLIST | CP_SMALL_TLIST)))
	{
		tlist = list_copy_head(plan->plan.targetlist, orig_tlist_length);
		return inject_projection_plan((Plan *) plan, tlist,
									  plan->plan.parallel_safe);
	}
	else
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
create_merge_append_plan(PlannerInfo *root, MergeAppendPath *best_path,
						 int flags)
{
	MergeAppend *node = makeNode(MergeAppend);
	Plan	   *plan = &node->plan;
	List	   *tlist = build_path_tlist(root, &best_path->path);
	int			orig_tlist_length = list_length(tlist);
	bool		tlist_was_changed;
	List	   *pathkeys = best_path->path.pathkeys;
	List	   *subplans = NIL;
	ListCell   *subpaths;
	RelOptInfo *rel = best_path->path.parent;
	PartitionPruneInfo *partpruneinfo = NULL;

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
	node->apprelids = rel->relids;

	/*
	 * Compute sort column info, and adjust MergeAppend's tlist as needed.
	 * Because we pass adjust_tlist_in_place = true, we may ignore the
	 * function result; it must be the same plan node.  However, we then need
	 * to detect whether any tlist entries were added.
	 */
	(void) prepare_sort_from_pathkeys(plan, pathkeys,
									  best_path->path.parent->relids,
									  NULL,
									  true,
									  &node->numCols,
									  &node->sortColIdx,
									  &node->sortOperators,
									  &node->collations,
									  &node->nullsFirst);
	tlist_was_changed = (orig_tlist_length != list_length(plan->targetlist));

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

	/*
	 * If any quals exist, they may be useful to perform further partition
	 * pruning during execution.  Gather information needed by the executor to
	 * do partition pruning.
	 */
	if (enable_partition_pruning)
	{
		List	   *prunequal;

		prunequal = extract_actual_clauses(rel->baserestrictinfo, false);

		/* We don't currently generate any parameterized MergeAppend paths */
		Assert(best_path->path.param_info == NULL);

		if (prunequal != NIL)
			partpruneinfo = make_partition_pruneinfo(root, rel,
													 best_path->subpaths,
													 prunequal);
	}

	node->mergeplans = subplans;
	node->part_prune_info = partpruneinfo;

	/*
	 * If prepare_sort_from_pathkeys added sort columns, but we were told to
	 * produce either the exact tlist or a narrow tlist, we should get rid of
	 * the sort columns again.  We must inject a projection node to do so.
	 */
	if (tlist_was_changed && (flags & (CP_EXACT_TLIST | CP_SMALL_TLIST)))
	{
		tlist = list_copy_head(plan->targetlist, orig_tlist_length);
		return inject_projection_plan(plan, tlist, plan->parallel_safe);
	}
	else
		return plan;
}

/*
 * create_group_result_plan
 *	  Create a Result plan for 'best_path'.
 *	  This is only used for degenerate grouping cases.
 *
 *	  Returns a Plan node.
 */
static Result *
create_group_result_plan(PlannerInfo *root, GroupResultPath *best_path)
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
 * create_project_set_plan
 *	  Create a ProjectSet plan for 'best_path'.
 *
 *	  Returns a Plan node.
 */
static ProjectSet *
create_project_set_plan(PlannerInfo *root, ProjectSetPath *best_path)
{
	ProjectSet *plan;
	Plan	   *subplan;
	List	   *tlist;

	/* Since we intend to project, we don't need to constrain child tlist */
	subplan = create_plan_recurse(root, best_path->subpath, 0);

	tlist = build_path_tlist(root, &best_path->path);

	plan = make_project_set(tlist, subplan);

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
 * create_memoize_plan
 *	  Create a Memoize plan for 'best_path' and (recursively) plans for its
 *	  subpaths.
 *
 *	  Returns a Plan node.
 */
static Memoize *
create_memoize_plan(PlannerInfo *root, MemoizePath *best_path, int flags)
{
	Memoize    *plan;
	Bitmapset  *keyparamids;
	Plan	   *subplan;
	Oid		   *operators;
	Oid		   *collations;
	List	   *param_exprs = NIL;
	ListCell   *lc;
	ListCell   *lc2;
	int			nkeys;
	int			i;

	subplan = create_plan_recurse(root, best_path->subpath,
								  flags | CP_SMALL_TLIST);

	param_exprs = (List *) replace_nestloop_params(root, (Node *)
												   best_path->param_exprs);

	nkeys = list_length(param_exprs);
	Assert(nkeys > 0);
	operators = palloc(nkeys * sizeof(Oid));
	collations = palloc(nkeys * sizeof(Oid));

	i = 0;
	forboth(lc, param_exprs, lc2, best_path->hash_operators)
	{
		Expr	   *param_expr = (Expr *) lfirst(lc);
		Oid			opno = lfirst_oid(lc2);

		operators[i] = opno;
		collations[i] = exprCollation((Node *) param_expr);
		i++;
	}

	keyparamids = pull_paramids((Expr *) param_exprs);

	plan = make_memoize(subplan, operators, collations, param_exprs,
						best_path->singlerow, best_path->binary_mode,
						best_path->est_entries, keyparamids);

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
	Oid		   *groupCollations;
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
		Expr	   *uniqexpr = lfirst(l);
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

	/* Use change_plan_targetlist in case we need to insert a Result node */
	if (newitems || best_path->umethod == UNIQUE_PATH_SORT)
		subplan = change_plan_targetlist(subplan, newtlist,
										 best_path->path.parallel_safe);

	/*
	 * Build control information showing which subplan output columns are to
	 * be examined by the grouping step.  Unfortunately we can't merge this
	 * with the previous loop, since we didn't then know which version of the
	 * subplan tlist we'd end up using.
	 */
	newtlist = subplan->targetlist;
	numGroupCols = list_length(uniq_exprs);
	groupColIdx = (AttrNumber *) palloc(numGroupCols * sizeof(AttrNumber));
	groupCollations = (Oid *) palloc(numGroupCols * sizeof(Oid));

	groupColPos = 0;
	foreach(l, uniq_exprs)
	{
		Expr	   *uniqexpr = lfirst(l);
		TargetEntry *tle;

		tle = tlist_member(uniqexpr, newtlist);
		if (!tle)				/* shouldn't happen */
			elog(ERROR, "failed to find unique expression in subplan tlist");
		groupColIdx[groupColPos] = tle->resno;
		groupCollations[groupColPos] = exprCollation((Node *) tle->expr);
		groupColPos++;
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
								 AGGSPLIT_SIMPLE,
								 numGroupCols,
								 groupColIdx,
								 groupOperators,
								 groupCollations,
								 NIL,
								 NIL,
								 best_path->path.rows,
								 0,
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
			if (!OidIsValid(eqop))	/* shouldn't happen */
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
			sortcl->reverse_sort = false;
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
	 * Push projection down to the child node.  That way, the projection work
	 * is parallelized, and there can be no system columns in the result (they
	 * can't travel through a tuple queue because it uses MinimalTuple
	 * representation).
	 */
	subplan = create_plan_recurse(root, best_path->subpath, CP_EXACT_TLIST);

	tlist = build_path_tlist(root, &best_path->path);

	gather_plan = make_gather(tlist,
							  NIL,
							  best_path->num_workers,
							  assign_special_exec_param(root),
							  best_path->single_copy,
							  subplan);

	copy_generic_path_info(&gather_plan->plan, &best_path->path);

	/* use parallel mode for parallel plans. */
	root->glob->parallelModeNeeded = true;

	return gather_plan;
}

/*
 * create_gather_merge_plan
 *
 *	  Create a Gather Merge plan for 'best_path' and (recursively)
 *	  plans for its subpaths.
 */
static GatherMerge *
create_gather_merge_plan(PlannerInfo *root, GatherMergePath *best_path)
{
	GatherMerge *gm_plan;
	Plan	   *subplan;
	List	   *pathkeys = best_path->path.pathkeys;
	List	   *tlist = build_path_tlist(root, &best_path->path);

	/* As with Gather, project away columns in the workers. */
	subplan = create_plan_recurse(root, best_path->subpath, CP_EXACT_TLIST);

	/* Create a shell for a GatherMerge plan. */
	gm_plan = makeNode(GatherMerge);
	gm_plan->plan.targetlist = tlist;
	gm_plan->num_workers = best_path->num_workers;
	copy_generic_path_info(&gm_plan->plan, &best_path->path);

	/* Assign the rescan Param. */
	gm_plan->rescan_param = assign_special_exec_param(root);

	/* Gather Merge is pointless with no pathkeys; use Gather instead. */
	Assert(pathkeys != NIL);

	/* Compute sort column info, and adjust subplan's tlist as needed */
	subplan = prepare_sort_from_pathkeys(subplan, pathkeys,
										 best_path->subpath->parent->relids,
										 gm_plan->sortColIdx,
										 false,
										 &gm_plan->numCols,
										 &gm_plan->sortColIdx,
										 &gm_plan->sortOperators,
										 &gm_plan->collations,
										 &gm_plan->nullsFirst);

	/*
	 * All gather merge paths should have already guaranteed the necessary
	 * sort order.  See create_gather_merge_path.
	 */
	Assert(pathkeys_contained_in(pathkeys, best_path->subpath->pathkeys));

	/* Now insert the subplan under GatherMerge. */
	gm_plan->plan.lefttree = subplan;

	/* use parallel mode for parallel plans. */
	root->glob->parallelModeNeeded = true;

	return gm_plan;
}

/*
 * create_projection_plan
 *
 *	  Create a plan tree to do a projection step and (recursively) plans
 *	  for its subpaths.  We may need a Result node for the projection,
 *	  but sometimes we can just let the subplan do the work.
 */
static Plan *
create_projection_plan(PlannerInfo *root, ProjectionPath *best_path, int flags)
{
	Plan	   *plan;
	Plan	   *subplan;
	List	   *tlist;
	bool		needs_result_node = false;

	/*
	 * Convert our subpath to a Plan and determine whether we need a Result
	 * node.
	 *
	 * In most cases where we don't need to project, create_projection_path
	 * will have set dummypp, but not always.  First, some createplan.c
	 * routines change the tlists of their nodes.  (An example is that
	 * create_merge_append_plan might add resjunk sort columns to a
	 * MergeAppend.)  Second, create_projection_path has no way of knowing
	 * what path node will be placed on top of the projection path and
	 * therefore can't predict whether it will require an exact tlist. For
	 * both of these reasons, we have to recheck here.
	 */
	if (use_physical_tlist(root, &best_path->path, flags))
	{
		/*
		 * Our caller doesn't really care what tlist we return, so we don't
		 * actually need to project.  However, we may still need to ensure
		 * proper sortgroupref labels, if the caller cares about those.
		 */
		subplan = create_plan_recurse(root, best_path->subpath, 0);
		tlist = subplan->targetlist;
		if (flags & CP_LABEL_TLIST)
			apply_pathtarget_labeling_to_tlist(tlist,
											   best_path->path.pathtarget);
	}
	else if (is_projection_capable_path(best_path->subpath))
	{
		/*
		 * Our caller requires that we return the exact tlist, but no separate
		 * result node is needed because the subpath is projection-capable.
		 * Tell create_plan_recurse that we're going to ignore the tlist it
		 * produces.
		 */
		subplan = create_plan_recurse(root, best_path->subpath,
									  CP_IGNORE_TLIST);
		Assert(is_projection_capable_plan(subplan));
		tlist = build_path_tlist(root, &best_path->path);
	}
	else
	{
		/*
		 * It looks like we need a result node, unless by good fortune the
		 * requested tlist is exactly the one the child wants to produce.
		 */
		subplan = create_plan_recurse(root, best_path->subpath, 0);
		tlist = build_path_tlist(root, &best_path->path);
		needs_result_node = !tlist_same_exprs(tlist, subplan->targetlist);
	}

	/*
	 * If we make a different decision about whether to include a Result node
	 * than create_projection_path did, we'll have made slightly wrong cost
	 * estimates; but label the plan with the cost estimates we actually used,
	 * not "corrected" ones.  (XXX this could be cleaned up if we moved more
	 * of the sortcolumn setup logic into Path creation, but that would add
	 * expense to creating Paths we might end up not using.)
	 */
	if (!needs_result_node)
	{
		/* Don't need a separate Result, just assign tlist to subplan */
		plan = subplan;
		plan->targetlist = tlist;

		/* Label plan with the estimated costs we actually used */
		plan->startup_cost = best_path->path.startup_cost;
		plan->total_cost = best_path->path.total_cost;
		plan->plan_rows = best_path->path.rows;
		plan->plan_width = best_path->path.pathtarget->width;
		plan->parallel_safe = best_path->path.parallel_safe;
		/* ... but don't change subplan's parallel_aware flag */
	}
	else
	{
		/* We need a Result node */
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
 *
 * One reason it's ugly is we have to be told the right parallel_safe marking
 * to apply (since the tlist might be unsafe even if the child plan is safe).
 */
static Plan *
inject_projection_plan(Plan *subplan, List *tlist, bool parallel_safe)
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
	plan->parallel_safe = parallel_safe;

	return plan;
}

/*
 * change_plan_targetlist
 *	  Externally available wrapper for inject_projection_plan.
 *
 * This is meant for use by FDW plan-generation functions, which might
 * want to adjust the tlist computed by some subplan tree.  In general,
 * a Result node is needed to compute the new tlist, but we can optimize
 * some cases.
 *
 * In most cases, tlist_parallel_safe can just be passed as the parallel_safe
 * flag of the FDW's own Path node.
 */
Plan *
change_plan_targetlist(Plan *subplan, List *tlist, bool tlist_parallel_safe)
{
	/*
	 * If the top plan node can't do projections and its existing target list
	 * isn't already what we need, we need to add a Result node to help it
	 * along.
	 */
	if (!is_projection_capable_plan(subplan) &&
		!tlist_same_exprs(tlist, subplan->targetlist))
		subplan = inject_projection_plan(subplan, tlist,
										 subplan->parallel_safe &&
										 tlist_parallel_safe);
	else
	{
		/* Else we can just replace the plan node's tlist */
		subplan->targetlist = tlist;
		subplan->parallel_safe &= tlist_parallel_safe;
	}
	return subplan;
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

	/*
	 * make_sort_from_pathkeys indirectly calls find_ec_member_matching_expr,
	 * which will ignore any child EC members that don't belong to the given
	 * relids. Thus, if this sort path is based on a child relation, we must
	 * pass its relids.
	 */
	plan = make_sort_from_pathkeys(subplan, best_path->path.pathkeys,
								   IS_OTHER_REL(best_path->subpath->parent) ?
								   best_path->path.parent->relids : NULL);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
}

/*
 * create_incrementalsort_plan
 *
 *	  Do the same as create_sort_plan, but create IncrementalSort plan.
 */
static IncrementalSort *
create_incrementalsort_plan(PlannerInfo *root, IncrementalSortPath *best_path,
							int flags)
{
	IncrementalSort *plan;
	Plan	   *subplan;

	/* See comments in create_sort_plan() above */
	subplan = create_plan_recurse(root, best_path->spath.subpath,
								  flags | CP_SMALL_TLIST);
	plan = make_incrementalsort_from_pathkeys(subplan,
											  best_path->spath.path.pathkeys,
											  IS_OTHER_REL(best_path->spath.subpath->parent) ?
											  best_path->spath.path.parent->relids : NULL,
											  best_path->nPresortedCols);

	copy_generic_path_info(&plan->sort.plan, (Path *) best_path);

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
					  extract_grouping_collations(best_path->groupClause,
												  subplan->targetlist),
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
					best_path->aggsplit,
					list_length(best_path->groupClause),
					extract_grouping_cols(best_path->groupClause,
										  subplan->targetlist),
					extract_grouping_ops(best_path->groupClause),
					extract_grouping_collations(best_path->groupClause,
												subplan->targetlist),
					NIL,
					NIL,
					best_path->numGroups,
					best_path->transitionSpace,
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
	List	   *rollups = best_path->rollups;
	AttrNumber *grouping_map;
	int			maxref;
	List	   *chain;
	ListCell   *lc;

	/* Shouldn't get here without grouping sets */
	Assert(root->parse->groupingSets);
	Assert(rollups != NIL);

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
	foreach(lc, root->processed_groupClause)
	{
		SortGroupClause *gc = (SortGroupClause *) lfirst(lc);

		if (gc->tleSortGroupRef > maxref)
			maxref = gc->tleSortGroupRef;
	}

	grouping_map = (AttrNumber *) palloc0((maxref + 1) * sizeof(AttrNumber));

	/* Now look up the column numbers in the child's tlist */
	foreach(lc, root->processed_groupClause)
	{
		SortGroupClause *gc = (SortGroupClause *) lfirst(lc);
		TargetEntry *tle = get_sortgroupclause_tle(gc, subplan->targetlist);

		grouping_map[gc->tleSortGroupRef] = tle->resno;
	}

	/*
	 * During setrefs.c, we'll need the grouping_map to fix up the cols lists
	 * in GroupingFunc nodes.  Save it for setrefs.c to use.
	 */
	Assert(root->grouping_map == NULL);
	root->grouping_map = grouping_map;

	/*
	 * Generate the side nodes that describe the other sort and group
	 * operations besides the top one.  Note that we don't worry about putting
	 * accurate cost estimates in the side nodes; only the topmost Agg node's
	 * costs will be shown by EXPLAIN.
	 */
	chain = NIL;
	if (list_length(rollups) > 1)
	{
		bool		is_first_sort = ((RollupData *) linitial(rollups))->is_hashed;

		for_each_from(lc, rollups, 1)
		{
			RollupData *rollup = lfirst(lc);
			AttrNumber *new_grpColIdx;
			Plan	   *sort_plan = NULL;
			Plan	   *agg_plan;
			AggStrategy strat;

			new_grpColIdx = remap_groupColIdx(root, rollup->groupClause);

			if (!rollup->is_hashed && !is_first_sort)
			{
				sort_plan = (Plan *)
					make_sort_from_groupcols(rollup->groupClause,
											 new_grpColIdx,
											 subplan);
			}

			if (!rollup->is_hashed)
				is_first_sort = false;

			if (rollup->is_hashed)
				strat = AGG_HASHED;
			else if (linitial(rollup->gsets) == NIL)
				strat = AGG_PLAIN;
			else
				strat = AGG_SORTED;

			agg_plan = (Plan *) make_agg(NIL,
										 NIL,
										 strat,
										 AGGSPLIT_SIMPLE,
										 list_length((List *) linitial(rollup->gsets)),
										 new_grpColIdx,
										 extract_grouping_ops(rollup->groupClause),
										 extract_grouping_collations(rollup->groupClause, subplan->targetlist),
										 rollup->gsets,
										 NIL,
										 rollup->numGroups,
										 best_path->transitionSpace,
										 sort_plan);

			/*
			 * Remove stuff we don't need to avoid bloating debug output.
			 */
			if (sort_plan)
			{
				sort_plan->targetlist = NIL;
				sort_plan->lefttree = NULL;
			}

			chain = lappend(chain, agg_plan);
		}
	}

	/*
	 * Now make the real Agg node
	 */
	{
		RollupData *rollup = linitial(rollups);
		AttrNumber *top_grpColIdx;
		int			numGroupCols;

		top_grpColIdx = remap_groupColIdx(root, rollup->groupClause);

		numGroupCols = list_length((List *) linitial(rollup->gsets));

		plan = make_agg(build_path_tlist(root, &best_path->path),
						best_path->qual,
						best_path->aggstrategy,
						AGGSPLIT_SIMPLE,
						numGroupCols,
						top_grpColIdx,
						extract_grouping_ops(rollup->groupClause),
						extract_grouping_collations(rollup->groupClause, subplan->targetlist),
						rollup->gsets,
						chain,
						rollup->numGroups,
						best_path->transitionSpace,
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
								   subparse->limitCount,
								   subparse->limitOption,
								   0, NULL, NULL, NULL);

		/* Must apply correct cost/width data to Limit node */
		plan->disabled_nodes = mminfo->path->disabled_nodes;
		plan->startup_cost = mminfo->path->startup_cost;
		plan->total_cost = mminfo->pathcost;
		plan->plan_rows = 1;
		plan->plan_width = mminfo->path->pathtarget->width;
		plan->parallel_aware = false;
		plan->parallel_safe = mminfo->path->parallel_safe;

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
	 */
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
	int			numPart = list_length(wc->partitionClause);
	int			numOrder = list_length(wc->orderClause);
	Plan	   *subplan;
	List	   *tlist;
	int			partNumCols;
	AttrNumber *partColIdx;
	Oid		   *partOperators;
	Oid		   *partCollations;
	int			ordNumCols;
	AttrNumber *ordColIdx;
	Oid		   *ordOperators;
	Oid		   *ordCollations;
	ListCell   *lc;

	/*
	 * Choice of tlist here is motivated by the fact that WindowAgg will be
	 * storing the input rows of window frames in a tuplestore; it therefore
	 * behooves us to request a small tlist to avoid wasting space. We do of
	 * course need grouping columns to be available.
	 */
	subplan = create_plan_recurse(root, best_path->subpath,
								  CP_LABEL_TLIST | CP_SMALL_TLIST);

	tlist = build_path_tlist(root, &best_path->path);

	/*
	 * Convert SortGroupClause lists into arrays of attr indexes and equality
	 * operators, as wanted by executor.
	 */
	partColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numPart);
	partOperators = (Oid *) palloc(sizeof(Oid) * numPart);
	partCollations = (Oid *) palloc(sizeof(Oid) * numPart);

	partNumCols = 0;
	foreach(lc, wc->partitionClause)
	{
		SortGroupClause *sgc = (SortGroupClause *) lfirst(lc);
		TargetEntry *tle = get_sortgroupclause_tle(sgc, subplan->targetlist);

		Assert(OidIsValid(sgc->eqop));
		partColIdx[partNumCols] = tle->resno;
		partOperators[partNumCols] = sgc->eqop;
		partCollations[partNumCols] = exprCollation((Node *) tle->expr);
		partNumCols++;
	}

	ordColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numOrder);
	ordOperators = (Oid *) palloc(sizeof(Oid) * numOrder);
	ordCollations = (Oid *) palloc(sizeof(Oid) * numOrder);

	ordNumCols = 0;
	foreach(lc, wc->orderClause)
	{
		SortGroupClause *sgc = (SortGroupClause *) lfirst(lc);
		TargetEntry *tle = get_sortgroupclause_tle(sgc, subplan->targetlist);

		Assert(OidIsValid(sgc->eqop));
		ordColIdx[ordNumCols] = tle->resno;
		ordOperators[ordNumCols] = sgc->eqop;
		ordCollations[ordNumCols] = exprCollation((Node *) tle->expr);
		ordNumCols++;
	}

	/* And finally we can make the WindowAgg node */
	plan = make_windowagg(tlist,
						  wc->winref,
						  partNumCols,
						  partColIdx,
						  partOperators,
						  partCollations,
						  ordNumCols,
						  ordColIdx,
						  ordOperators,
						  ordCollations,
						  wc->frameOptions,
						  wc->startOffset,
						  wc->endOffset,
						  wc->startInRangeFunc,
						  wc->endInRangeFunc,
						  wc->inRangeColl,
						  wc->inRangeAsc,
						  wc->inRangeNullsFirst,
						  best_path->runCondition,
						  best_path->qual,
						  best_path->topwindow,
						  subplan);

	copy_generic_path_info(&plan->plan, (Path *) best_path);

	return plan;
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
	numGroups = clamp_cardinality_to_long(best_path->numGroups);

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
	numGroups = clamp_cardinality_to_long(best_path->numGroups);

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
	Path	   *subpath = best_path->subpath;
	Plan	   *subplan;

	/* Subplan must produce exactly the specified tlist */
	subplan = create_plan_recurse(root, subpath, CP_EXACT_TLIST);

	/* Transfer resname/resjunk labeling, too, to keep executor happy */
	apply_tlist_labeling(subplan->targetlist, root->processed_tlist);

	plan = make_modifytable(root,
							subplan,
							best_path->operation,
							best_path->canSetTag,
							best_path->nominalRelation,
							best_path->rootRelation,
							best_path->partColsUpdated,
							best_path->resultRelations,
							best_path->updateColnosLists,
							best_path->withCheckOptionLists,
							best_path->returningLists,
							best_path->rowMarks,
							best_path->onconflict,
							best_path->mergeActionLists,
							best_path->mergeJoinConditions,
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
	int			numUniqkeys = 0;
	AttrNumber *uniqColIdx = NULL;
	Oid		   *uniqOperators = NULL;
	Oid		   *uniqCollations = NULL;

	/* Limit doesn't project, so tlist requirements pass through */
	subplan = create_plan_recurse(root, best_path->subpath, flags);

	/* Extract information necessary for comparing rows for WITH TIES. */
	if (best_path->limitOption == LIMIT_OPTION_WITH_TIES)
	{
		Query	   *parse = root->parse;
		ListCell   *l;

		numUniqkeys = list_length(parse->sortClause);
		uniqColIdx = (AttrNumber *) palloc(numUniqkeys * sizeof(AttrNumber));
		uniqOperators = (Oid *) palloc(numUniqkeys * sizeof(Oid));
		uniqCollations = (Oid *) palloc(numUniqkeys * sizeof(Oid));

		numUniqkeys = 0;
		foreach(l, parse->sortClause)
		{
			SortGroupClause *sortcl = (SortGroupClause *) lfirst(l);
			TargetEntry *tle = get_sortgroupclause_tle(sortcl, parse->targetList);

			uniqColIdx[numUniqkeys] = tle->resno;
			uniqOperators[numUniqkeys] = sortcl->eqop;
			uniqCollations[numUniqkeys] = exprCollation((Node *) tle->expr);
			numUniqkeys++;
		}
	}

	plan = make_limit(subplan,
					  best_path->limitOffset,
					  best_path->limitCount,
					  best_path->limitOption,
					  numUniqkeys, uniqColIdx, uniqOperators, uniqCollations);

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

	copy_generic_path_info(&scan_plan->scan.plan, best_path);

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
	List	   *indexclauses = best_path->indexclauses;
	List	   *indexorderbys = best_path->indexorderbys;
	Index		baserelid = best_path->path.parent->relid;
	IndexOptInfo *indexinfo = best_path->indexinfo;
	Oid			indexoid = indexinfo->indexoid;
	List	   *qpqual;
	List	   *stripped_indexquals;
	List	   *fixed_indexquals;
	List	   *fixed_indexorderbys;
	List	   *indexorderbyops = NIL;
	ListCell   *l;

	/* it should be a base rel... */
	Assert(baserelid > 0);
	Assert(best_path->path.parent->rtekind == RTE_RELATION);
	/* check the scan direction is valid */
	Assert(best_path->indexscandir == ForwardScanDirection ||
		   best_path->indexscandir == BackwardScanDirection);

	/*
	 * Extract the index qual expressions (stripped of RestrictInfos) from the
	 * IndexClauses list, and prepare a copy with index Vars substituted for
	 * table Vars.  (This step also does replace_nestloop_params on the
	 * fixed_indexquals.)
	 */
	fix_indexqual_references(root, best_path,
							 &stripped_indexquals,
							 &fixed_indexquals);

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
	 * is_redundant_with_indexclauses() detects cases where a scan clause is
	 * present in the indexclauses list or is generated from the same
	 * EquivalenceClass as some indexclause, and is therefore redundant with
	 * it, though not equal.  (The latter happens when indxpath.c prefers a
	 * different derived equality than what generate_join_implied_equalities
	 * picked for a parameterized scan's ppi_clauses.)  Note that it will not
	 * match to lossy index clauses, which is critical because we have to
	 * include the original clause in qpqual in that case.
	 *
	 * In some situations (particularly with OR'd index conditions) we may
	 * have scan_clauses that are not equal to, but are logically implied by,
	 * the index quals; so we also try a predicate_implied_by() check to see
	 * if we can discard quals that way.  (predicate_implied_by assumes its
	 * first input contains only immutable functions, so we have to check
	 * that.)
	 *
	 * Note: if you change this bit of code you should also look at
	 * extract_nonindex_conditions() in costsize.c.
	 */
	qpqual = NIL;
	foreach(l, scan_clauses)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);

		if (rinfo->pseudoconstant)
			continue;			/* we may drop pseudoconstants here */
		if (is_redundant_with_indexclauses(rinfo, indexclauses))
			continue;			/* dup or derived from same EquivalenceClass */
		if (!contain_mutable_functions((Node *) rinfo->clause) &&
			predicate_implied_by(list_make1(rinfo->clause), stripped_indexquals,
								 false))
			continue;			/* provably implied by indexquals */
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
				elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
					 pathkey->pk_strategy, exprtype, exprtype, pathkey->pk_opfamily);
			indexorderbyops = lappend_oid(indexorderbyops, sortop);
		}
	}

	/*
	 * For an index-only scan, we must mark indextlist entries as resjunk if
	 * they are columns that the index AM can't return; this cues setrefs.c to
	 * not generate references to those columns.
	 */
	if (indexonly)
	{
		int			i = 0;

		foreach(l, indexinfo->indextlist)
		{
			TargetEntry *indextle = (TargetEntry *) lfirst(l);

			indextle->resjunk = !indexinfo->canreturn[i];
			i++;
		}
	}

	/* Finally ready to build the plan node */
	if (indexonly)
		scan_plan = (Scan *) make_indexonlyscan(tlist,
												qpqual,
												baserelid,
												indexoid,
												fixed_indexquals,
												stripped_indexquals,
												fixed_indexorderbys,
												indexinfo->indextlist,
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

	if (best_path->path.parallel_aware)
		bitmap_subplan_mark_shared(bitmapqualplan);

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
	 * Unlike create_indexscan_plan(), the predicate_implied_by() test here is
	 * useful for getting rid of qpquals that are implied by index predicates,
	 * because the predicate conditions are included in the "indexquals"
	 * returned by create_bitmap_subplan().  Bitmap scans have to do it that
	 * way because predicate conditions need to be rechecked if the scan
	 * becomes lossy, so they have to be included in bitmapqualorig.
	 */
	qpqual = NIL;
	foreach(l, scan_clauses)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);
		Node	   *clause = (Node *) rinfo->clause;

		if (rinfo->pseudoconstant)
			continue;			/* we may drop pseudoconstants here */
		if (list_member(indexquals, clause))
			continue;			/* simple duplicate */
		if (rinfo->parent_ec && list_member_ptr(indexECs, rinfo->parent_ec))
			continue;			/* derived from same EquivalenceClass */
		if (!contain_mutable_functions(clause) &&
			predicate_implied_by(list_make1(clause), indexquals, false))
			continue;			/* provably implied by indexquals */
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
		plan->parallel_safe = apath->path.parallel_safe;
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
			plan->plan_width = 0;	/* meaningless */
			plan->parallel_aware = false;
			plan->parallel_safe = opath->path.parallel_safe;
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
		List	   *subquals;
		List	   *subindexquals;
		List	   *subindexECs;
		ListCell   *l;

		/* Use the regular indexscan plan build machinery... */
		iscan = castNode(IndexScan,
						 create_indexscan_plan(root, ipath,
											   NIL, NIL, false));
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
		plan->parallel_safe = ipath->path.parallel_safe;
		/* Extract original index clauses, actual index quals, relevant ECs */
		subquals = NIL;
		subindexquals = NIL;
		subindexECs = NIL;
		foreach(l, ipath->indexclauses)
		{
			IndexClause *iclause = (IndexClause *) lfirst(l);
			RestrictInfo *rinfo = iclause->rinfo;

			Assert(!rinfo->pseudoconstant);
			subquals = lappend(subquals, rinfo->clause);
			subindexquals = list_concat(subindexquals,
										get_actual_clauses(iclause->indexquals));
			if (rinfo->parent_ec)
				subindexECs = lappend(subindexECs, rinfo->parent_ec);
		}
		/* We can add any index predicate conditions, too */
		foreach(l, ipath->indexinfo->indpred)
		{
			Expr	   *pred = (Expr *) lfirst(l);

			/*
			 * We know that the index predicate must have been implied by the
			 * query condition as a whole, but it may or may not be implied by
			 * the conditions that got pushed into the bitmapqual.  Avoid
			 * generating redundant conditions.
			 */
			if (!predicate_implied_by(list_make1(pred), subquals, false))
			{
				subquals = lappend(subquals, pred);
				subindexquals = lappend(subindexquals, pred);
			}
		}
		*qual = subquals;
		*indexqual = subindexquals;
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

	/* it should be a base rel... */
	Assert(scan_relid > 0);
	Assert(best_path->path.parent->rtekind == RTE_RELATION);

	/*
	 * The qpqual list must contain all restrictions not enforced by the
	 * tidquals list.  Since tidquals has OR semantics, we have to be careful
	 * about matching it up to scan_clauses.  It's convenient to handle the
	 * single-tidqual case separately from the multiple-tidqual case.  In the
	 * single-tidqual case, we look through the scan_clauses while they are
	 * still in RestrictInfo form, and drop any that are redundant with the
	 * tidqual.
	 *
	 * In normal cases simple pointer equality checks will be enough to spot
	 * duplicate RestrictInfos, so we try that first.
	 *
	 * Another common case is that a scan_clauses entry is generated from the
	 * same EquivalenceClass as some tidqual, and is therefore redundant with
	 * it, though not equal.
	 *
	 * Unlike indexpaths, we don't bother with predicate_implied_by(); the
	 * number of cases where it could win are pretty small.
	 */
	if (list_length(tidquals) == 1)
	{
		List	   *qpqual = NIL;
		ListCell   *l;

		foreach(l, scan_clauses)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);

			if (rinfo->pseudoconstant)
				continue;		/* we may drop pseudoconstants here */
			if (list_member_ptr(tidquals, rinfo))
				continue;		/* simple duplicate */
			if (is_redundant_derived_clause(rinfo, tidquals))
				continue;		/* derived from same EquivalenceClass */
			qpqual = lappend(qpqual, rinfo);
		}
		scan_clauses = qpqual;
	}

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* Reduce RestrictInfo lists to bare expressions; ignore pseudoconstants */
	tidquals = extract_actual_clauses(tidquals, false);
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/*
	 * If we have multiple tidquals, it's more convenient to remove duplicate
	 * scan_clauses after stripping the RestrictInfos.  In this situation,
	 * because the tidquals represent OR sub-clauses, they could not have come
	 * from EquivalenceClasses so we don't have to worry about matching up
	 * non-identical clauses.  On the other hand, because tidpath.c will have
	 * extracted those sub-clauses from some OR clause and built its own list,
	 * we will certainly not have pointer equality to any scan clause.  So
	 * convert the tidquals list to an explicit OR clause and see if we can
	 * match it via equal() to any scan clause.
	 */
	if (list_length(tidquals) > 1)
		scan_clauses = list_difference(scan_clauses,
									   list_make1(make_orclause(tidquals)));

	/* Replace any outer-relation variables with nestloop params */
	if (best_path->path.param_info)
	{
		tidquals = (List *)
			replace_nestloop_params(root, (Node *) tidquals);
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
	}

	scan_plan = make_tidscan(tlist,
							 scan_clauses,
							 scan_relid,
							 tidquals);

	copy_generic_path_info(&scan_plan->scan.plan, &best_path->path);

	return scan_plan;
}

/*
 * create_tidrangescan_plan
 *	 Returns a tidrangescan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static TidRangeScan *
create_tidrangescan_plan(PlannerInfo *root, TidRangePath *best_path,
						 List *tlist, List *scan_clauses)
{
	TidRangeScan *scan_plan;
	Index		scan_relid = best_path->path.parent->relid;
	List	   *tidrangequals = best_path->tidrangequals;

	/* it should be a base rel... */
	Assert(scan_relid > 0);
	Assert(best_path->path.parent->rtekind == RTE_RELATION);

	/*
	 * The qpqual list must contain all restrictions not enforced by the
	 * tidrangequals list.  tidrangequals has AND semantics, so we can simply
	 * remove any qual that appears in it.
	 */
	{
		List	   *qpqual = NIL;
		ListCell   *l;

		foreach(l, scan_clauses)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);

			if (rinfo->pseudoconstant)
				continue;		/* we may drop pseudoconstants here */
			if (list_member_ptr(tidrangequals, rinfo))
				continue;		/* simple duplicate */
			qpqual = lappend(qpqual, rinfo);
		}
		scan_clauses = qpqual;
	}

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	/* Reduce RestrictInfo lists to bare expressions; ignore pseudoconstants */
	tidrangequals = extract_actual_clauses(tidrangequals, false);
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* Replace any outer-relation variables with nestloop params */
	if (best_path->path.param_info)
	{
		tidrangequals = (List *)
			replace_nestloop_params(root, (Node *) tidrangequals);
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
	}

	scan_plan = make_tidrangescan(tlist,
								  scan_clauses,
								  scan_relid,
								  tidrangequals);

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

	/*
	 * Replace any outer-relation variables with nestloop params.
	 *
	 * We must provide nestloop params for both lateral references of the
	 * subquery and outer vars in the scan_clauses.  It's better to assign the
	 * former first, because that code path requires specific param IDs, while
	 * replace_nestloop_params can adapt to the IDs assigned by
	 * process_subquery_nestloop_params.  This avoids possibly duplicating
	 * nestloop params when the same Var is needed for both reasons.
	 */
	if (best_path->path.param_info)
	{
		process_subquery_nestloop_params(root,
										 rel->subplan_params);
		scan_clauses = (List *)
			replace_nestloop_params(root, (Node *) scan_clauses);
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
 * create_tablefuncscan_plan
 *	 Returns a tablefuncscan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static TableFuncScan *
create_tablefuncscan_plan(PlannerInfo *root, Path *best_path,
						  List *tlist, List *scan_clauses)
{
	TableFuncScan *scan_plan;
	Index		scan_relid = best_path->parent->relid;
	RangeTblEntry *rte;
	TableFunc  *tablefunc;

	/* it should be a function base rel... */
	Assert(scan_relid > 0);
	rte = planner_rt_fetch(scan_relid, root);
	Assert(rte->rtekind == RTE_TABLEFUNC);
	tablefunc = rte->tablefunc;

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
		tablefunc = (TableFunc *) replace_nestloop_params(root, (Node *) tablefunc);
	}

	scan_plan = make_tablefuncscan(tlist, scan_clauses, scan_relid,
								   tablefunc);

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
	if (plan_id <= 0)
		elog(ERROR, "no plan was made for CTE \"%s\"", rte->ctename);
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
 * create_namedtuplestorescan_plan
 *	 Returns a tuplestorescan plan for the base relation scanned by
 *	'best_path' with restriction clauses 'scan_clauses' and targetlist
 *	'tlist'.
 */
static NamedTuplestoreScan *
create_namedtuplestorescan_plan(PlannerInfo *root, Path *best_path,
								List *tlist, List *scan_clauses)
{
	NamedTuplestoreScan *scan_plan;
	Index		scan_relid = best_path->parent->relid;
	RangeTblEntry *rte;

	Assert(scan_relid > 0);
	rte = planner_rt_fetch(scan_relid, root);
	Assert(rte->rtekind == RTE_NAMEDTUPLESTORE);

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

	scan_plan = make_namedtuplestorescan(tlist, scan_clauses, scan_relid,
										 rte->enrname);

	copy_generic_path_info(&scan_plan->scan.plan, best_path);

	return scan_plan;
}

/*
 * create_resultscan_plan
 *	 Returns a Result plan for the RTE_RESULT base relation scanned by
 *	'best_path' with restriction clauses 'scan_clauses' and targetlist
 *	'tlist'.
 */
static Result *
create_resultscan_plan(PlannerInfo *root, Path *best_path,
					   List *tlist, List *scan_clauses)
{
	Result	   *scan_plan;
	Index		scan_relid = best_path->parent->relid;
	RangeTblEntry *rte PG_USED_FOR_ASSERTS_ONLY;

	Assert(scan_relid > 0);
	rte = planner_rt_fetch(scan_relid, root);
	Assert(rte->rtekind == RTE_RESULT);

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

	scan_plan = make_result(tlist, (Node *) scan_clauses, NULL);

	copy_generic_path_info(&scan_plan->plan, best_path);

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
	if (cteroot->wt_param_id < 0)	/* shouldn't happen */
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

	/* Copy user OID to access as; likewise no need to make FDW do this */
	scan_plan->checkAsUser = rel->userid;

	/* Copy foreign server OID; likewise, no need to make FDW do this */
	scan_plan->fs_server = rel->serverid;

	/*
	 * Likewise, copy the relids that are represented by this foreign scan. An
	 * upper rel doesn't have relids set, but it covers all the relations
	 * participating in the underlying scan/join, so use root->all_query_rels.
	 */
	if (rel->reloptkind == RELOPT_UPPER_REL)
		scan_plan->fs_relids = root->all_query_rels;
	else
		scan_plan->fs_relids = best_path->path.parent->relids;

	/*
	 * Join relid sets include relevant outer joins, but FDWs may need to know
	 * which are the included base rels.  That's a bit tedious to get without
	 * access to the plan-time data structures, so compute it here.
	 */
	scan_plan->fs_base_relids = bms_difference(scan_plan->fs_relids,
											   root->outer_join_rels);

	/*
	 * If this is a foreign join, and to make it valid to push down we had to
	 * assume that the current user is the same as some user explicitly named
	 * in the query, mark the finished plan as depending on the current user.
	 */
	if (rel->useridiscurrent)
		root->glob->dependsOnRole = true;

	/*
	 * Replace any outer-relation variables with nestloop params in the qual,
	 * fdw_exprs and fdw_recheck_quals expressions.  We do this last so that
	 * the FDW doesn't have to be involved.  (Note that parts of fdw_exprs or
	 * fdw_recheck_quals could have come from join clauses, so doing this
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
	 * in fdw_scan_tlist.)	This is a bit of a kluge and might go away
	 * someday, so we intentionally leave it out of the API presented to FDWs.
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
		pull_varattnos((Node *) rel->reltarget->exprs, scan_relid, &attrs_used);

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
 * create_customscan_plan
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
	cplan = castNode(CustomScan,
					 best_path->methods->PlanCustomPath(root,
														rel,
														best_path,
														tlist,
														scan_clauses,
														custom_plans));

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
	List	   *tlist = build_path_tlist(root, &best_path->jpath.path);
	List	   *joinrestrictclauses = best_path->jpath.joinrestrictinfo;
	List	   *joinclauses;
	List	   *otherclauses;
	Relids		outerrelids;
	List	   *nestParams;
	Relids		saveOuterRels = root->curOuterRels;

	/*
	 * If the inner path is parameterized by the topmost parent of the outer
	 * rel rather than the outer rel itself, fix that.  (Nothing happens here
	 * if it is not so parameterized.)
	 */
	best_path->jpath.innerjoinpath =
		reparameterize_path_by_child(root,
									 best_path->jpath.innerjoinpath,
									 best_path->jpath.outerjoinpath->parent);

	/*
	 * Failure here probably means that reparameterize_path_by_child() is not
	 * in sync with path_is_reparameterizable_by_child().
	 */
	Assert(best_path->jpath.innerjoinpath != NULL);

	/* NestLoop can project, so no need to be picky about child tlists */
	outer_plan = create_plan_recurse(root, best_path->jpath.outerjoinpath, 0);

	/* For a nestloop, include outer relids in curOuterRels for inner side */
	root->curOuterRels = bms_union(root->curOuterRels,
								   best_path->jpath.outerjoinpath->parent->relids);

	inner_plan = create_plan_recurse(root, best_path->jpath.innerjoinpath, 0);

	/* Restore curOuterRels */
	bms_free(root->curOuterRels);
	root->curOuterRels = saveOuterRels;

	/* Sort join qual clauses into best execution order */
	joinrestrictclauses = order_qual_clauses(root, joinrestrictclauses);

	/* Get the join qual clauses (in plain expression form) */
	/* Any pseudoconstant clauses are ignored here */
	if (IS_OUTER_JOIN(best_path->jpath.jointype))
	{
		extract_actual_join_clauses(joinrestrictclauses,
									best_path->jpath.path.parent->relids,
									&joinclauses, &otherclauses);
	}
	else
	{
		/* We can treat all clauses alike for an inner join */
		joinclauses = extract_actual_clauses(joinrestrictclauses, false);
		otherclauses = NIL;
	}

	/* Replace any outer-relation variables with nestloop params */
	if (best_path->jpath.path.param_info)
	{
		joinclauses = (List *)
			replace_nestloop_params(root, (Node *) joinclauses);
		otherclauses = (List *)
			replace_nestloop_params(root, (Node *) otherclauses);
	}

	/*
	 * Identify any nestloop parameters that should be supplied by this join
	 * node, and remove them from root->curOuterParams.
	 */
	outerrelids = best_path->jpath.outerjoinpath->parent->relids;
	nestParams = identify_current_nestloop_params(root, outerrelids);

	join_plan = make_nestloop(tlist,
							  joinclauses,
							  otherclauses,
							  nestParams,
							  outer_plan,
							  inner_plan,
							  best_path->jpath.jointype,
							  best_path->jpath.inner_unique);

	copy_generic_path_info(&join_plan->join.plan, &best_path->jpath.path);

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
	bool	   *mergereversals;
	bool	   *mergenullsfirst;
	PathKey    *opathkey;
	EquivalenceClass *opeclass;
	int			i;
	ListCell   *lc;
	ListCell   *lop;
	ListCell   *lip;
	Path	   *outer_path = best_path->jpath.outerjoinpath;
	Path	   *inner_path = best_path->jpath.innerjoinpath;

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
									best_path->jpath.path.parent->relids,
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
		Relids		outer_relids = outer_path->parent->relids;
		Plan	   *sort_plan;
		bool		use_incremental_sort = false;
		int			presorted_keys;

		/*
		 * We choose to use incremental sort if it is enabled and there are
		 * presorted keys; otherwise we use full sort.
		 */
		if (enable_incremental_sort)
		{
			bool		is_sorted PG_USED_FOR_ASSERTS_ONLY;

			is_sorted = pathkeys_count_contained_in(best_path->outersortkeys,
													outer_path->pathkeys,
													&presorted_keys);
			Assert(!is_sorted);

			if (presorted_keys > 0)
				use_incremental_sort = true;
		}

		if (!use_incremental_sort)
		{
			sort_plan = (Plan *)
				make_sort_from_pathkeys(outer_plan,
										best_path->outersortkeys,
										outer_relids);

			label_sort_with_costsize(root, (Sort *) sort_plan, -1.0);
		}
		else
		{
			sort_plan = (Plan *)
				make_incrementalsort_from_pathkeys(outer_plan,
												   best_path->outersortkeys,
												   outer_relids,
												   presorted_keys);

			label_incrementalsort_with_costsize(root,
												(IncrementalSort *) sort_plan,
												best_path->outersortkeys,
												-1.0);
		}

		outer_plan = sort_plan;
		outerpathkeys = best_path->outersortkeys;
	}
	else
		outerpathkeys = best_path->jpath.outerjoinpath->pathkeys;

	if (best_path->innersortkeys)
	{
		/*
		 * We do not consider incremental sort for inner path, because
		 * incremental sort does not support mark/restore.
		 */

		Relids		inner_relids = inner_path->parent->relids;
		Sort	   *sort = make_sort_from_pathkeys(inner_plan,
												   best_path->innersortkeys,
												   inner_relids);

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
	 * pathkey, as well as the possibility that the inner pathkeys are not in
	 * an order matching the mergeclauses.
	 */
	nClauses = list_length(mergeclauses);
	Assert(nClauses == list_length(best_path->path_mergeclauses));
	mergefamilies = (Oid *) palloc(nClauses * sizeof(Oid));
	mergecollations = (Oid *) palloc(nClauses * sizeof(Oid));
	mergereversals = (bool *) palloc(nClauses * sizeof(bool));
	mergenullsfirst = (bool *) palloc(nClauses * sizeof(bool));

	opathkey = NULL;
	opeclass = NULL;
	lop = list_head(outerpathkeys);
	lip = list_head(innerpathkeys);
	i = 0;
	foreach(lc, best_path->path_mergeclauses)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
		EquivalenceClass *oeclass;
		EquivalenceClass *ieclass;
		PathKey    *ipathkey = NULL;
		EquivalenceClass *ipeclass = NULL;
		bool		first_inner_match = false;

		/* fetch outer/inner eclass from mergeclause */
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
		 * We must identify the pathkey elements associated with this clause
		 * by matching the eclasses (which should give a unique match, since
		 * the pathkey lists should be canonical).  In typical cases the merge
		 * clauses are one-to-one with the pathkeys, but when dealing with
		 * partially redundant query conditions, things are more complicated.
		 *
		 * lop and lip reference the first as-yet-unmatched pathkey elements.
		 * If they're NULL then all pathkey elements have been matched.
		 *
		 * The ordering of the outer pathkeys should match the mergeclauses,
		 * by construction (see find_mergeclauses_for_outer_pathkeys()). There
		 * could be more than one mergeclause for the same outer pathkey, but
		 * no pathkey may be entirely skipped over.
		 */
		if (oeclass != opeclass)	/* multiple matches are not interesting */
		{
			/* doesn't match the current opathkey, so must match the next */
			if (lop == NULL)
				elog(ERROR, "outer pathkeys do not match mergeclauses");
			opathkey = (PathKey *) lfirst(lop);
			opeclass = opathkey->pk_eclass;
			lop = lnext(outerpathkeys, lop);
			if (oeclass != opeclass)
				elog(ERROR, "outer pathkeys do not match mergeclauses");
		}

		/*
		 * The inner pathkeys likewise should not have skipped-over keys, but
		 * it's possible for a mergeclause to reference some earlier inner
		 * pathkey if we had redundant pathkeys.  For example we might have
		 * mergeclauses like "o.a = i.x AND o.b = i.y AND o.c = i.x".  The
		 * implied inner ordering is then "ORDER BY x, y, x", but the pathkey
		 * mechanism drops the second sort by x as redundant, and this code
		 * must cope.
		 *
		 * It's also possible for the implied inner-rel ordering to be like
		 * "ORDER BY x, y, x DESC".  We still drop the second instance of x as
		 * redundant; but this means that the sort ordering of a redundant
		 * inner pathkey should not be considered significant.  So we must
		 * detect whether this is the first clause matching an inner pathkey.
		 */
		if (lip)
		{
			ipathkey = (PathKey *) lfirst(lip);
			ipeclass = ipathkey->pk_eclass;
			if (ieclass == ipeclass)
			{
				/* successful first match to this inner pathkey */
				lip = lnext(innerpathkeys, lip);
				first_inner_match = true;
			}
		}
		if (!first_inner_match)
		{
			/* redundant clause ... must match something before lip */
			ListCell   *l2;

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

		/*
		 * The pathkeys should always match each other as to opfamily and
		 * collation (which affect equality), but if we're considering a
		 * redundant inner pathkey, its sort ordering might not match.  In
		 * such cases we may ignore the inner pathkey's sort ordering and use
		 * the outer's.  (In effect, we're lying to the executor about the
		 * sort direction of this inner column, but it does not matter since
		 * the run-time row comparisons would only reach this column when
		 * there's equality for the earlier column containing the same eclass.
		 * There could be only one value in this column for the range of inner
		 * rows having a given value in the earlier column, so it does not
		 * matter which way we imagine this column to be ordered.)  But a
		 * non-redundant inner pathkey had better match outer's ordering too.
		 */
		if (opathkey->pk_opfamily != ipathkey->pk_opfamily ||
			opathkey->pk_eclass->ec_collation != ipathkey->pk_eclass->ec_collation)
			elog(ERROR, "left and right pathkeys do not match in mergejoin");
		if (first_inner_match &&
			(opathkey->pk_strategy != ipathkey->pk_strategy ||
			 opathkey->pk_nulls_first != ipathkey->pk_nulls_first))
			elog(ERROR, "left and right pathkeys do not match in mergejoin");

		/* OK, save info for executor */
		mergefamilies[i] = opathkey->pk_opfamily;
		mergecollations[i] = opathkey->pk_eclass->ec_collation;
		mergereversals[i] = (opathkey->pk_strategy == BTGreaterStrategyNumber ? true : false);
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
							   mergereversals,
							   mergenullsfirst,
							   outer_plan,
							   inner_plan,
							   best_path->jpath.jointype,
							   best_path->jpath.inner_unique,
							   best_path->skip_mark_restore);

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
	List	   *hashoperators = NIL;
	List	   *hashcollations = NIL;
	List	   *inner_hashkeys = NIL;
	List	   *outer_hashkeys = NIL;
	Oid			skewTable = InvalidOid;
	AttrNumber	skewColumn = InvalidAttrNumber;
	bool		skewInherit = false;
	ListCell   *lc;

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
									best_path->jpath.path.parent->relids,
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
			}
		}
	}

	/*
	 * Collect hash related information. The hashed expressions are
	 * deconstructed into outer/inner expressions, so they can be computed
	 * separately (inner expressions are used to build the hashtable via Hash,
	 * outer expressions to perform lookups of tuples from HashJoin's outer
	 * plan in the hashtable). Also collect operator information necessary to
	 * build the hashtable.
	 */
	foreach(lc, hashclauses)
	{
		OpExpr	   *hclause = lfirst_node(OpExpr, lc);

		hashoperators = lappend_oid(hashoperators, hclause->opno);
		hashcollations = lappend_oid(hashcollations, hclause->inputcollid);
		outer_hashkeys = lappend(outer_hashkeys, linitial(hclause->args));
		inner_hashkeys = lappend(inner_hashkeys, lsecond(hclause->args));
	}

	/*
	 * Build the hash node and hash join node.
	 */
	hash_plan = make_hash(inner_plan,
						  inner_hashkeys,
						  skewTable,
						  skewColumn,
						  skewInherit);

	/*
	 * Set Hash node's startup & total costs equal to total cost of input
	 * plan; this only affects EXPLAIN display not decisions.
	 */
	copy_plan_costsize(&hash_plan->plan, inner_plan);
	hash_plan->plan.startup_cost = hash_plan->plan.total_cost;

	/*
	 * If parallel-aware, the executor will also need an estimate of the total
	 * number of rows expected from all participants so that it can size the
	 * shared hash table.
	 */
	if (best_path->jpath.path.parallel_aware)
	{
		hash_plan->plan.parallel_aware = true;
		hash_plan->rows_total = best_path->inner_rows_total;
	}

	join_plan = make_hashjoin(tlist,
							  joinclauses,
							  otherclauses,
							  hashclauses,
							  hashoperators,
							  hashcollations,
							  outer_hashkeys,
							  outer_plan,
							  (Plan *) hash_plan,
							  best_path->jpath.jointype,
							  best_path->jpath.inner_unique);

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

		/* Upper-level Vars should be long gone at this point */
		Assert(var->varlevelsup == 0);
		/* If not to be replaced, we can just return the Var unmodified */
		if (IS_SPECIAL_VARNO(var->varno) ||
			!bms_is_member(var->varno, root->curOuterRels))
			return node;
		/* Replace the Var with a nestloop Param */
		return (Node *) replace_nestloop_param_var(root, var);
	}
	if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		/* Upper-level PlaceHolderVars should be long gone at this point */
		Assert(phv->phlevelsup == 0);

		/* Check whether we need to replace the PHV */
		if (!bms_is_subset(find_placeholder_info(root, phv)->ph_eval_at,
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
		/* Replace the PlaceHolderVar with a nestloop Param */
		return (Node *) replace_nestloop_param_placeholdervar(root, phv);
	}
	return expression_tree_mutator(node,
								   replace_nestloop_params_mutator,
								   (void *) root);
}

/*
 * fix_indexqual_references
 *	  Adjust indexqual clauses to the form the executor's indexqual
 *	  machinery needs.
 *
 * We have three tasks here:
 *	* Select the actual qual clauses out of the input IndexClause list,
 *	  and remove RestrictInfo nodes from the qual clauses.
 *	* Replace any outer-relation Var or PHV nodes with nestloop Params.
 *	  (XXX eventually, that responsibility should go elsewhere?)
 *	* Index keys must be represented by Var nodes with varattno set to the
 *	  index's attribute number, not the attribute number in the original rel.
 *
 * *stripped_indexquals_p receives a list of the actual qual clauses.
 *
 * *fixed_indexquals_p receives a list of the adjusted quals.  This is a copy
 * that shares no substructure with the original; this is needed in case there
 * are subplans in it (we need two separate copies of the subplan tree, or
 * things will go awry).
 */
static void
fix_indexqual_references(PlannerInfo *root, IndexPath *index_path,
						 List **stripped_indexquals_p, List **fixed_indexquals_p)
{
	IndexOptInfo *index = index_path->indexinfo;
	List	   *stripped_indexquals;
	List	   *fixed_indexquals;
	ListCell   *lc;

	stripped_indexquals = fixed_indexquals = NIL;

	foreach(lc, index_path->indexclauses)
	{
		IndexClause *iclause = lfirst_node(IndexClause, lc);
		int			indexcol = iclause->indexcol;
		ListCell   *lc2;

		foreach(lc2, iclause->indexquals)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc2);
			Node	   *clause = (Node *) rinfo->clause;

			stripped_indexquals = lappend(stripped_indexquals, clause);
			clause = fix_indexqual_clause(root, index, indexcol,
										  clause, iclause->indexcols);
			fixed_indexquals = lappend(fixed_indexquals, clause);
		}
	}

	*stripped_indexquals_p = stripped_indexquals;
	*fixed_indexquals_p = fixed_indexquals;
}

/*
 * fix_indexorderby_references
 *	  Adjust indexorderby clauses to the form the executor's index
 *	  machinery needs.
 *
 * This is a simplified version of fix_indexqual_references.  The input is
 * bare clauses and a separate indexcol list, instead of IndexClauses.
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

		clause = fix_indexqual_clause(root, index, indexcol, clause, NIL);
		fixed_indexorderbys = lappend(fixed_indexorderbys, clause);
	}

	return fixed_indexorderbys;
}

/*
 * fix_indexqual_clause
 *	  Convert a single indexqual clause to the form needed by the executor.
 *
 * We replace nestloop params here, and replace the index key variables
 * or expressions by index Var nodes.
 */
static Node *
fix_indexqual_clause(PlannerInfo *root, IndexOptInfo *index, int indexcol,
					 Node *clause, List *indexcolnos)
{
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

		/* Replace the indexkey expression with an index Var. */
		linitial(op->args) = fix_indexqual_operand(linitial(op->args),
												   index,
												   indexcol);
	}
	else if (IsA(clause, RowCompareExpr))
	{
		RowCompareExpr *rc = (RowCompareExpr *) clause;
		ListCell   *lca,
				   *lcai;

		/* Replace the indexkey expressions with index Vars. */
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

	return clause;
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
			indexpr_item = lnext(index->indexprs, indexpr_item);
		}
	}

	/* Oops... */
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
 * When security barrier quals are used in the query, we may have quals with
 * different security levels in the list.  Quals of lower security_level
 * must go before quals of higher security_level, except that we can grant
 * exceptions to move up quals that are leakproof.  When security level
 * doesn't force the decision, we prefer to order clauses by estimated
 * execution cost, cheapest first.
 *
 * Ideally the order should be driven by a combination of execution cost and
 * selectivity, but it's not immediately clear how to account for both,
 * and given the uncertainty of the estimates the reliability of the decisions
 * would be doubtful anyway.  So we just order by security level then
 * estimated per-tuple cost, being careful not to change the order when
 * (as is often the case) the estimates are identical.
 *
 * Although this will work on either bare clauses or RestrictInfos, it's
 * much faster to apply it to RestrictInfos, since it can re-use cost
 * information that is cached in RestrictInfos.  XXX in the bare-clause
 * case, we are also not able to apply security considerations.  That is
 * all right for the moment, because the bare-clause case doesn't occur
 * anywhere that barrier quals could be present, but it would be better to
 * get rid of it.
 *
 * Note: some callers pass lists that contain entries that will later be
 * removed; this is the easiest way to let this routine see RestrictInfos
 * instead of bare clauses.  This is another reason why trying to consider
 * selectivity in the ordering would likely do the wrong thing.
 */
static List *
order_qual_clauses(PlannerInfo *root, List *clauses)
{
	typedef struct
	{
		Node	   *clause;
		Cost		cost;
		Index		security_level;
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
		if (IsA(clause, RestrictInfo))
		{
			RestrictInfo *rinfo = (RestrictInfo *) clause;

			/*
			 * If a clause is leakproof, it doesn't have to be constrained by
			 * its nominal security level.  If it's also reasonably cheap
			 * (here defined as 10X cpu_operator_cost), pretend it has
			 * security_level 0, which will allow it to go in front of
			 * more-expensive quals of lower security levels.  Of course, that
			 * will also force it to go in front of cheaper quals of its own
			 * security level, which is not so great, but we can alleviate
			 * that risk by applying the cost limit cutoff.
			 */
			if (rinfo->leakproof && items[i].cost < 10 * cpu_operator_cost)
				items[i].security_level = 0;
			else
				items[i].security_level = rinfo->security_level;
		}
		else
			items[i].security_level = 0;
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
			QualItem   *olditem = &items[j - 1];

			if (newitem.security_level > olditem->security_level ||
				(newitem.security_level == olditem->security_level &&
				 newitem.cost >= olditem->cost))
				break;
			items[j] = *olditem;
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
 * Also copy the parallel-related flags, which the executor *will* use.
 */
static void
copy_generic_path_info(Plan *dest, Path *src)
{
	dest->disabled_nodes = src->disabled_nodes;
	dest->startup_cost = src->startup_cost;
	dest->total_cost = src->total_cost;
	dest->plan_rows = src->rows;
	dest->plan_width = src->pathtarget->width;
	dest->parallel_aware = src->parallel_aware;
	dest->parallel_safe = src->parallel_safe;
}

/*
 * Copy cost and size info from a lower plan node to an inserted node.
 * (Most callers alter the info after copying it.)
 */
static void
copy_plan_costsize(Plan *dest, Plan *src)
{
	dest->disabled_nodes = src->disabled_nodes;
	dest->startup_cost = src->startup_cost;
	dest->total_cost = src->total_cost;
	dest->plan_rows = src->plan_rows;
	dest->plan_width = src->plan_width;
	/* Assume the inserted node is not parallel-aware. */
	dest->parallel_aware = false;
	/* Assume the inserted node is parallel-safe, if child plan is. */
	dest->parallel_safe = src->parallel_safe;
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

	Assert(IsA(plan, Sort));

	cost_sort(&sort_path, root, NIL,
			  plan->plan.disabled_nodes,
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
	plan->plan.parallel_safe = lefttree->parallel_safe;
}

/*
 * Same as label_sort_with_costsize, but labels the IncrementalSort node
 * instead.
 */
static void
label_incrementalsort_with_costsize(PlannerInfo *root, IncrementalSort *plan,
									List *pathkeys, double limit_tuples)
{
	Plan	   *lefttree = plan->sort.plan.lefttree;
	Path		sort_path;		/* dummy for result of cost_incremental_sort */

	Assert(IsA(plan, IncrementalSort));

	cost_incremental_sort(&sort_path, root, pathkeys,
						  plan->nPresortedCols,
						  plan->sort.plan.disabled_nodes,
						  lefttree->startup_cost,
						  lefttree->total_cost,
						  lefttree->plan_rows,
						  lefttree->plan_width,
						  0.0,
						  work_mem,
						  limit_tuples);
	plan->sort.plan.startup_cost = sort_path.startup_cost;
	plan->sort.plan.total_cost = sort_path.total_cost;
	plan->sort.plan.plan_rows = lefttree->plan_rows;
	plan->sort.plan.plan_width = lefttree->plan_width;
	plan->sort.plan.parallel_aware = false;
	plan->sort.plan.parallel_safe = lefttree->parallel_safe;
}

/*
 * bitmap_subplan_mark_shared
 *	 Set isshared flag in bitmap subplan so that it will be created in
 *	 shared memory.
 */
static void
bitmap_subplan_mark_shared(Plan *plan)
{
	if (IsA(plan, BitmapAnd))
		bitmap_subplan_mark_shared(linitial(((BitmapAnd *) plan)->bitmapplans));
	else if (IsA(plan, BitmapOr))
	{
		((BitmapOr *) plan)->isshared = true;
		bitmap_subplan_mark_shared(linitial(((BitmapOr *) plan)->bitmapplans));
	}
	else if (IsA(plan, BitmapIndexScan))
		((BitmapIndexScan *) plan)->isshared = true;
	else
		elog(ERROR, "unrecognized node type: %d", nodeTag(plan));
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
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;

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
				   List *recheckqual,
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
	node->recheckqual = recheckqual;
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

static TidRangeScan *
make_tidrangescan(List *qptlist,
				  List *qpqual,
				  Index scanrelid,
				  List *tidrangequals)
{
	TidRangeScan *node = makeNode(TidRangeScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->tidrangequals = tidrangequals;

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
	node->scanstatus = SUBQUERY_SCAN_UNKNOWN;

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

static TableFuncScan *
make_tablefuncscan(List *qptlist,
				   List *qpqual,
				   Index scanrelid,
				   TableFunc *tablefunc)
{
	TableFuncScan *node = makeNode(TableFuncScan);
	Plan	   *plan = &node->scan.plan;

	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->tablefunc = tablefunc;

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

static NamedTuplestoreScan *
make_namedtuplestorescan(List *qptlist,
						 List *qpqual,
						 Index scanrelid,
						 char *enrname)
{
	NamedTuplestoreScan *node = makeNode(NamedTuplestoreScan);
	Plan	   *plan = &node->scan.plan;

	/* cost should be inserted by caller */
	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->enrname = enrname;

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

	/* these may be overridden by the FDW's PlanDirectModify callback. */
	node->operation = CMD_SELECT;
	node->resultRelation = 0;

	/* checkAsUser, fs_server will be filled in by create_foreignscan_plan */
	node->checkAsUser = InvalidOid;
	node->fs_server = InvalidOid;
	node->fdw_exprs = fdw_exprs;
	node->fdw_private = fdw_private;
	node->fdw_scan_tlist = fdw_scan_tlist;
	node->fdw_recheck_quals = fdw_recheck_quals;
	/* fs_relids, fs_base_relids will be filled by create_foreignscan_plan */
	node->fs_relids = NULL;
	node->fs_base_relids = NULL;
	/* fsSystemCol will be filled in by create_foreignscan_plan */
	node->fsSystemCol = false;

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
		Oid		   *dupCollations;
		ListCell   *slitem;

		dupColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numCols);
		dupOperators = (Oid *) palloc(sizeof(Oid) * numCols);
		dupCollations = (Oid *) palloc(sizeof(Oid) * numCols);

		foreach(slitem, distinctList)
		{
			SortGroupClause *sortcl = (SortGroupClause *) lfirst(slitem);
			TargetEntry *tle = get_sortgroupclause_tle(sortcl,
													   plan->targetlist);

			dupColIdx[keyno] = tle->resno;
			dupOperators[keyno] = sortcl->eqop;
			dupCollations[keyno] = exprCollation((Node *) tle->expr);
			Assert(OidIsValid(dupOperators[keyno]));
			keyno++;
		}
		node->dupColIdx = dupColIdx;
		node->dupOperators = dupOperators;
		node->dupCollations = dupCollations;
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
			  JoinType jointype,
			  bool inner_unique)
{
	NestLoop   *node = makeNode(NestLoop);
	Plan	   *plan = &node->join.plan;

	plan->targetlist = tlist;
	plan->qual = otherclauses;
	plan->lefttree = lefttree;
	plan->righttree = righttree;
	node->join.jointype = jointype;
	node->join.inner_unique = inner_unique;
	node->join.joinqual = joinclauses;
	node->nestParams = nestParams;

	return node;
}

static HashJoin *
make_hashjoin(List *tlist,
			  List *joinclauses,
			  List *otherclauses,
			  List *hashclauses,
			  List *hashoperators,
			  List *hashcollations,
			  List *hashkeys,
			  Plan *lefttree,
			  Plan *righttree,
			  JoinType jointype,
			  bool inner_unique)
{
	HashJoin   *node = makeNode(HashJoin);
	Plan	   *plan = &node->join.plan;

	plan->targetlist = tlist;
	plan->qual = otherclauses;
	plan->lefttree = lefttree;
	plan->righttree = righttree;
	node->hashclauses = hashclauses;
	node->hashoperators = hashoperators;
	node->hashcollations = hashcollations;
	node->hashkeys = hashkeys;
	node->join.jointype = jointype;
	node->join.inner_unique = inner_unique;
	node->join.joinqual = joinclauses;

	return node;
}

static Hash *
make_hash(Plan *lefttree,
		  List *hashkeys,
		  Oid skewTable,
		  AttrNumber skewColumn,
		  bool skewInherit)
{
	Hash	   *node = makeNode(Hash);
	Plan	   *plan = &node->plan;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	node->hashkeys = hashkeys;
	node->skewTable = skewTable;
	node->skewColumn = skewColumn;
	node->skewInherit = skewInherit;

	return node;
}

static MergeJoin *
make_mergejoin(List *tlist,
			   List *joinclauses,
			   List *otherclauses,
			   List *mergeclauses,
			   Oid *mergefamilies,
			   Oid *mergecollations,
			   bool *mergereversals,
			   bool *mergenullsfirst,
			   Plan *lefttree,
			   Plan *righttree,
			   JoinType jointype,
			   bool inner_unique,
			   bool skip_mark_restore)
{
	MergeJoin  *node = makeNode(MergeJoin);
	Plan	   *plan = &node->join.plan;

	plan->targetlist = tlist;
	plan->qual = otherclauses;
	plan->lefttree = lefttree;
	plan->righttree = righttree;
	node->skip_mark_restore = skip_mark_restore;
	node->mergeclauses = mergeclauses;
	node->mergeFamilies = mergefamilies;
	node->mergeCollations = mergecollations;
	node->mergeReversals = mergereversals;
	node->mergeNullsFirst = mergenullsfirst;
	node->join.jointype = jointype;
	node->join.inner_unique = inner_unique;
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
	Sort	   *node;
	Plan	   *plan;

	node = makeNode(Sort);

	plan = &node->plan;
	plan->targetlist = lefttree->targetlist;
	plan->disabled_nodes = lefttree->disabled_nodes + (enable_sort == false);
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
 * make_incrementalsort --- basic routine to build an IncrementalSort plan node
 *
 * Caller must have built the sortColIdx, sortOperators, collations, and
 * nullsFirst arrays already.
 */
static IncrementalSort *
make_incrementalsort(Plan *lefttree, int numCols, int nPresortedCols,
					 AttrNumber *sortColIdx, Oid *sortOperators,
					 Oid *collations, bool *nullsFirst)
{
	IncrementalSort *node;
	Plan	   *plan;

	node = makeNode(IncrementalSort);

	plan = &node->sort.plan;
	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;
	node->nPresortedCols = nPresortedCols;
	node->sort.numCols = numCols;
	node->sort.sortColIdx = sortColIdx;
	node->sort.sortOperators = sortOperators;
	node->sort.collations = collations;
	node->sort.nullsFirst = nullsFirst;

	return node;
}

/*
 * prepare_sort_from_pathkeys
 *	  Prepare to sort according to given pathkeys
 *
 * This is used to set up for Sort, MergeAppend, and Gather Merge nodes.  It
 * calculates the executor's representation of the sort key information, and
 * adjusts the plan targetlist if needed to add resjunk sort columns.
 *
 * Input parameters:
 *	  'lefttree' is the plan node which yields input tuples
 *	  'pathkeys' is the list of pathkeys by which the result is to be sorted
 *	  'relids' identifies the child relation being sorted, if any
 *	  'reqColIdx' is NULL or an array of required sort key column numbers
 *	  'adjust_tlist_in_place' is true if lefttree must be modified in-place
 *
 * We must convert the pathkey information into arrays of sort key column
 * numbers, sort operator OIDs, collation OIDs, and nulls-first flags,
 * which is the representation the executor wants.  These are returned into
 * the output parameters *p_numsortkeys etc.
 *
 * When looking for matches to an EquivalenceClass's members, we will only
 * consider child EC members if they belong to given 'relids'.  This protects
 * against possible incorrect matches to child expressions that contain no
 * Vars.
 *
 * If reqColIdx isn't NULL then it contains sort key column numbers that
 * we should match.  This is used when making child plans for a MergeAppend;
 * it's an error if we can't match the columns.
 *
 * If the pathkeys include expressions that aren't simple Vars, we will
 * usually need to add resjunk items to the input plan's targetlist to
 * compute these expressions, since a Sort or MergeAppend node itself won't
 * do any such calculations.  If the input plan type isn't one that can do
 * projections, this means adding a Result node just to do the projection.
 * However, the caller can pass adjust_tlist_in_place = true to force the
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
				em = find_ec_member_matching_expr(ec, tle->expr, relids);
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
			 * in the input's vars.
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
				em = find_ec_member_matching_expr(ec, tle->expr, relids);
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
			 * No matching tlist item; look for a computable expression.
			 */
			em = find_computable_ec_member(NULL, ec, tlist, relids, false);
			if (!em)
				elog(ERROR, "could not find pathkey item to sort");
			pk_datatype = em->em_datatype;

			/*
			 * Do we need to insert a Result node?
			 */
			if (!adjust_tlist_in_place &&
				!is_projection_capable_plan(lefttree))
			{
				/* copy needed so we don't modify input's tlist below */
				tlist = copyObject(tlist);
				lefttree = inject_projection_plan(lefttree, tlist,
												  lefttree->parallel_safe);
			}

			/* Don't bother testing is_projection_capable_plan again */
			adjust_tlist_in_place = true;

			/*
			 * Add resjunk entry to input's tlist
			 */
			tle = makeTargetEntry(copyObject(em->em_expr),
								  list_length(tlist) + 1,
								  NULL,
								  true);
			tlist = lappend(tlist, tle);
			lefttree->targetlist = tlist;	/* just in case NIL before */
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
			elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
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
 * make_sort_from_pathkeys
 *	  Create sort plan to sort according to given pathkeys
 *
 *	  'lefttree' is the node which yields input tuples
 *	  'pathkeys' is the list of pathkeys by which the result is to be sorted
 *	  'relids' is the set of relations required by prepare_sort_from_pathkeys()
 */
static Sort *
make_sort_from_pathkeys(Plan *lefttree, List *pathkeys, Relids relids)
{
	int			numsortkeys;
	AttrNumber *sortColIdx;
	Oid		   *sortOperators;
	Oid		   *collations;
	bool	   *nullsFirst;

	/* Compute sort column info, and adjust lefttree as needed */
	lefttree = prepare_sort_from_pathkeys(lefttree, pathkeys,
										  relids,
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
 * make_incrementalsort_from_pathkeys
 *	  Create sort plan to sort according to given pathkeys
 *
 *	  'lefttree' is the node which yields input tuples
 *	  'pathkeys' is the list of pathkeys by which the result is to be sorted
 *	  'relids' is the set of relations required by prepare_sort_from_pathkeys()
 *	  'nPresortedCols' is the number of presorted columns in input tuples
 */
static IncrementalSort *
make_incrementalsort_from_pathkeys(Plan *lefttree, List *pathkeys,
								   Relids relids, int nPresortedCols)
{
	int			numsortkeys;
	AttrNumber *sortColIdx;
	Oid		   *sortOperators;
	Oid		   *collations;
	bool	   *nullsFirst;

	/* Compute sort column info, and adjust lefttree as needed */
	lefttree = prepare_sort_from_pathkeys(lefttree, pathkeys,
										  relids,
										  NULL,
										  false,
										  &numsortkeys,
										  &sortColIdx,
										  &sortOperators,
										  &collations,
										  &nullsFirst);

	/* Now build the Sort node */
	return make_incrementalsort(lefttree, numsortkeys, nPresortedCols,
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
	Cost		initplan_cost;
	bool		unsafe_initplans;

	matplan = (Plan *) make_material(subplan);

	/*
	 * XXX horrid kluge: if there are any initPlans attached to the subplan,
	 * move them up to the Material node, which is now effectively the top
	 * plan node in its query level.  This prevents failure in
	 * SS_finalize_plan(), which see for comments.
	 */
	matplan->initPlan = subplan->initPlan;
	subplan->initPlan = NIL;

	/* Move the initplans' cost delta, as well */
	SS_compute_initplan_cost(matplan->initPlan,
							 &initplan_cost, &unsafe_initplans);
	subplan->startup_cost -= initplan_cost;
	subplan->total_cost -= initplan_cost;

	/* Set cost data */
	cost_material(&matpath,
				  subplan->disabled_nodes,
				  subplan->startup_cost,
				  subplan->total_cost,
				  subplan->plan_rows,
				  subplan->plan_width);
	matplan->disabled_nodes = subplan->disabled_nodes;
	matplan->startup_cost = matpath.startup_cost + initplan_cost;
	matplan->total_cost = matpath.total_cost + initplan_cost;
	matplan->plan_rows = subplan->plan_rows;
	matplan->plan_width = subplan->plan_width;
	matplan->parallel_aware = false;
	matplan->parallel_safe = subplan->parallel_safe;

	return matplan;
}

static Memoize *
make_memoize(Plan *lefttree, Oid *hashoperators, Oid *collations,
			 List *param_exprs, bool singlerow, bool binary_mode,
			 uint32 est_entries, Bitmapset *keyparamids)
{
	Memoize    *node = makeNode(Memoize);
	Plan	   *plan = &node->plan;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	node->numKeys = list_length(param_exprs);
	node->hashOperators = hashoperators;
	node->collations = collations;
	node->param_exprs = param_exprs;
	node->singlerow = singlerow;
	node->binary_mode = binary_mode;
	node->est_entries = est_entries;
	node->keyparamids = keyparamids;

	return node;
}

Agg *
make_agg(List *tlist, List *qual,
		 AggStrategy aggstrategy, AggSplit aggsplit,
		 int numGroupCols, AttrNumber *grpColIdx, Oid *grpOperators, Oid *grpCollations,
		 List *groupingSets, List *chain, double dNumGroups,
		 Size transitionSpace, Plan *lefttree)
{
	Agg		   *node = makeNode(Agg);
	Plan	   *plan = &node->plan;
	long		numGroups;

	/* Reduce to long, but 'ware overflow! */
	numGroups = clamp_cardinality_to_long(dNumGroups);

	node->aggstrategy = aggstrategy;
	node->aggsplit = aggsplit;
	node->numCols = numGroupCols;
	node->grpColIdx = grpColIdx;
	node->grpOperators = grpOperators;
	node->grpCollations = grpCollations;
	node->numGroups = numGroups;
	node->transitionSpace = transitionSpace;
	node->aggParams = NULL;		/* SS_finalize_plan() will fill this */
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
			   int partNumCols, AttrNumber *partColIdx, Oid *partOperators, Oid *partCollations,
			   int ordNumCols, AttrNumber *ordColIdx, Oid *ordOperators, Oid *ordCollations,
			   int frameOptions, Node *startOffset, Node *endOffset,
			   Oid startInRangeFunc, Oid endInRangeFunc,
			   Oid inRangeColl, bool inRangeAsc, bool inRangeNullsFirst,
			   List *runCondition, List *qual, bool topWindow, Plan *lefttree)
{
	WindowAgg  *node = makeNode(WindowAgg);
	Plan	   *plan = &node->plan;

	node->winref = winref;
	node->partNumCols = partNumCols;
	node->partColIdx = partColIdx;
	node->partOperators = partOperators;
	node->partCollations = partCollations;
	node->ordNumCols = ordNumCols;
	node->ordColIdx = ordColIdx;
	node->ordOperators = ordOperators;
	node->ordCollations = ordCollations;
	node->frameOptions = frameOptions;
	node->startOffset = startOffset;
	node->endOffset = endOffset;
	node->runCondition = runCondition;
	/* a duplicate of the above for EXPLAIN */
	node->runConditionOrig = runCondition;
	node->startInRangeFunc = startInRangeFunc;
	node->endInRangeFunc = endInRangeFunc;
	node->inRangeColl = inRangeColl;
	node->inRangeAsc = inRangeAsc;
	node->inRangeNullsFirst = inRangeNullsFirst;
	node->topWindow = topWindow;

	plan->targetlist = tlist;
	plan->lefttree = lefttree;
	plan->righttree = NULL;
	plan->qual = qual;

	return node;
}

static Group *
make_group(List *tlist,
		   List *qual,
		   int numGroupCols,
		   AttrNumber *grpColIdx,
		   Oid *grpOperators,
		   Oid *grpCollations,
		   Plan *lefttree)
{
	Group	   *node = makeNode(Group);
	Plan	   *plan = &node->plan;

	node->numCols = numGroupCols;
	node->grpColIdx = grpColIdx;
	node->grpOperators = grpOperators;
	node->grpCollations = grpCollations;

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
	Oid		   *uniqCollations;
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
	uniqCollations = (Oid *) palloc(sizeof(Oid) * numCols);

	foreach(slitem, distinctList)
	{
		SortGroupClause *sortcl = (SortGroupClause *) lfirst(slitem);
		TargetEntry *tle = get_sortgroupclause_tle(sortcl, plan->targetlist);

		uniqColIdx[keyno] = tle->resno;
		uniqOperators[keyno] = sortcl->eqop;
		uniqCollations[keyno] = exprCollation((Node *) tle->expr);
		Assert(OidIsValid(uniqOperators[keyno]));
		keyno++;
	}

	node->numCols = numCols;
	node->uniqColIdx = uniqColIdx;
	node->uniqOperators = uniqOperators;
	node->uniqCollations = uniqCollations;

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
	Oid		   *uniqCollations;
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
	uniqCollations = (Oid *) palloc(sizeof(Oid) * numCols);

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
				em = find_ec_member_matching_expr(ec, tle->expr, NULL);
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
			elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
				 BTEqualStrategyNumber, pk_datatype, pk_datatype,
				 pathkey->pk_opfamily);

		uniqColIdx[keyno] = tle->resno;
		uniqOperators[keyno] = eqop;
		uniqCollations[keyno] = ec->ec_collation;

		keyno++;
	}

	node->numCols = numCols;
	node->uniqColIdx = uniqColIdx;
	node->uniqOperators = uniqOperators;
	node->uniqCollations = uniqCollations;

	return node;
}

static Gather *
make_gather(List *qptlist,
			List *qpqual,
			int nworkers,
			int rescan_param,
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
	node->rescan_param = rescan_param;
	node->single_copy = single_copy;
	node->invisible = false;
	node->initParam = NULL;

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
	Oid		   *dupCollations;
	ListCell   *slitem;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	/*
	 * convert SortGroupClause list into arrays of attr indexes and equality
	 * operators, as wanted by executor
	 */
	dupColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numCols);
	dupOperators = (Oid *) palloc(sizeof(Oid) * numCols);
	dupCollations = (Oid *) palloc(sizeof(Oid) * numCols);

	foreach(slitem, distinctList)
	{
		SortGroupClause *sortcl = (SortGroupClause *) lfirst(slitem);
		TargetEntry *tle = get_sortgroupclause_tle(sortcl, plan->targetlist);

		dupColIdx[keyno] = tle->resno;
		dupOperators[keyno] = sortcl->eqop;
		dupCollations[keyno] = exprCollation((Node *) tle->expr);
		Assert(OidIsValid(dupOperators[keyno]));
		keyno++;
	}

	node->cmd = cmd;
	node->strategy = strategy;
	node->numCols = numCols;
	node->dupColIdx = dupColIdx;
	node->dupOperators = dupOperators;
	node->dupCollations = dupCollations;
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
make_limit(Plan *lefttree, Node *limitOffset, Node *limitCount,
		   LimitOption limitOption, int uniqNumCols, AttrNumber *uniqColIdx,
		   Oid *uniqOperators, Oid *uniqCollations)
{
	Limit	   *node = makeNode(Limit);
	Plan	   *plan = &node->plan;

	plan->targetlist = lefttree->targetlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	node->limitOffset = limitOffset;
	node->limitCount = limitCount;
	node->limitOption = limitOption;
	node->uniqNumCols = uniqNumCols;
	node->uniqColIdx = uniqColIdx;
	node->uniqOperators = uniqOperators;
	node->uniqCollations = uniqCollations;

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
 * make_project_set
 *	  Build a ProjectSet plan node
 */
static ProjectSet *
make_project_set(List *tlist,
				 Plan *subplan)
{
	ProjectSet *node = makeNode(ProjectSet);
	Plan	   *plan = &node->plan;

	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = subplan;
	plan->righttree = NULL;

	return node;
}

/*
 * make_modifytable
 *	  Build a ModifyTable plan node
 */
static ModifyTable *
make_modifytable(PlannerInfo *root, Plan *subplan,
				 CmdType operation, bool canSetTag,
				 Index nominalRelation, Index rootRelation,
				 bool partColsUpdated,
				 List *resultRelations,
				 List *updateColnosLists,
				 List *withCheckOptionLists, List *returningLists,
				 List *rowMarks, OnConflictExpr *onconflict,
				 List *mergeActionLists, List *mergeJoinConditions,
				 int epqParam)
{
	ModifyTable *node = makeNode(ModifyTable);
	List	   *fdw_private_list;
	Bitmapset  *direct_modify_plans;
	ListCell   *lc;
	int			i;

	Assert(operation == CMD_MERGE ||
		   (operation == CMD_UPDATE ?
			list_length(resultRelations) == list_length(updateColnosLists) :
			updateColnosLists == NIL));
	Assert(withCheckOptionLists == NIL ||
		   list_length(resultRelations) == list_length(withCheckOptionLists));
	Assert(returningLists == NIL ||
		   list_length(resultRelations) == list_length(returningLists));

	node->plan.lefttree = subplan;
	node->plan.righttree = NULL;
	node->plan.qual = NIL;
	/* setrefs.c will fill in the targetlist, if needed */
	node->plan.targetlist = NIL;

	node->operation = operation;
	node->canSetTag = canSetTag;
	node->nominalRelation = nominalRelation;
	node->rootRelation = rootRelation;
	node->partColsUpdated = partColsUpdated;
	node->resultRelations = resultRelations;
	if (!onconflict)
	{
		node->onConflictAction = ONCONFLICT_NONE;
		node->onConflictSet = NIL;
		node->onConflictCols = NIL;
		node->onConflictWhere = NULL;
		node->arbiterIndexes = NIL;
		node->exclRelRTI = 0;
		node->exclRelTlist = NIL;
	}
	else
	{
		node->onConflictAction = onconflict->action;

		/*
		 * Here we convert the ON CONFLICT UPDATE tlist, if any, to the
		 * executor's convention of having consecutive resno's.  The actual
		 * target column numbers are saved in node->onConflictCols.  (This
		 * could be done earlier, but there seems no need to.)
		 */
		node->onConflictSet = onconflict->onConflictSet;
		node->onConflictCols =
			extract_update_targetlist_colnos(node->onConflictSet);
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
	node->updateColnosLists = updateColnosLists;
	node->withCheckOptionLists = withCheckOptionLists;
	node->returningLists = returningLists;
	node->rowMarks = rowMarks;
	node->mergeActionLists = mergeActionLists;
	node->mergeJoinConditions = mergeJoinConditions;
	node->epqParam = epqParam;

	/*
	 * For each result relation that is a foreign table, allow the FDW to
	 * construct private plan data, and accumulate it all into a list.
	 */
	fdw_private_list = NIL;
	direct_modify_plans = NULL;
	i = 0;
	foreach(lc, resultRelations)
	{
		Index		rti = lfirst_int(lc);
		FdwRoutine *fdwroutine;
		List	   *fdw_private;
		bool		direct_modify;

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

			if (rte->rtekind == RTE_RELATION &&
				rte->relkind == RELKIND_FOREIGN_TABLE)
			{
				/* Check if the access to foreign tables is restricted */
				if (unlikely((restrict_nonsystem_relation_kind & RESTRICT_RELKIND_FOREIGN_TABLE) != 0))
				{
					/* there must not be built-in foreign tables */
					Assert(rte->relid >= FirstNormalObjectId);
					ereport(ERROR,
							(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
							 errmsg("access to non-system foreign table is restricted")));
				}

				fdwroutine = GetFdwRoutineByRelId(rte->relid);
			}
			else
				fdwroutine = NULL;
		}

		/*
		 * MERGE is not currently supported for foreign tables.  We already
		 * checked that when the table mentioned in the query is foreign; but
		 * we can still get here if a partitioned table has a foreign table as
		 * partition.  Disallow that now, to avoid an uglier error message
		 * later.
		 */
		if (operation == CMD_MERGE && fdwroutine != NULL)
		{
			RangeTblEntry *rte = planner_rt_fetch(rti, root);

			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("cannot execute MERGE on relation \"%s\"",
						   get_rel_name(rte->relid)),
					errdetail_relkind_not_supported(rte->relkind));
		}

		/*
		 * Try to modify the foreign table directly if (1) the FDW provides
		 * callback functions needed for that and (2) there are no local
		 * structures that need to be run for each modified row: row-level
		 * triggers on the foreign table, stored generated columns, WITH CHECK
		 * OPTIONs from parent views.
		 */
		direct_modify = false;
		if (fdwroutine != NULL &&
			fdwroutine->PlanDirectModify != NULL &&
			fdwroutine->BeginDirectModify != NULL &&
			fdwroutine->IterateDirectModify != NULL &&
			fdwroutine->EndDirectModify != NULL &&
			withCheckOptionLists == NIL &&
			!has_row_triggers(root, rti, operation) &&
			!has_stored_generated_columns(root, rti))
			direct_modify = fdwroutine->PlanDirectModify(root, node, rti, i);
		if (direct_modify)
			direct_modify_plans = bms_add_member(direct_modify_plans, i);

		if (!direct_modify &&
			fdwroutine != NULL &&
			fdwroutine->PlanForeignModify != NULL)
			fdw_private = fdwroutine->PlanForeignModify(root, node, rti, i);
		else
			fdw_private = NIL;
		fdw_private_list = lappend(fdw_private_list, fdw_private);
		i++;
	}
	node->fdwPrivLists = fdw_private_list;
	node->fdwDirectModifyPlans = direct_modify_plans;

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
		case T_Memoize:
		case T_Sort:
		case T_IncrementalSort:
		case T_Unique:
		case T_SetOp:
		case T_LockRows:
		case T_Limit:
		case T_ModifyTable:
		case T_MergeAppend:
		case T_RecursiveUnion:
			return false;
		case T_CustomScan:
			if (castNode(CustomPath, path)->flags & CUSTOMPATH_SUPPORT_PROJECTION)
				return true;
			return false;
		case T_Append:

			/*
			 * Append can't project, but if an AppendPath is being used to
			 * represent a dummy path, what will actually be generated is a
			 * Result which can project.
			 */
			return IS_DUMMY_APPEND(path);
		case T_ProjectSet:

			/*
			 * Although ProjectSet certainly projects, say "no" because we
			 * don't want the planner to randomly replace its tlist with
			 * something else; the SRFs have to stay at top level.  This might
			 * get relaxed later.
			 */
			return false;
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
		case T_Memoize:
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
		case T_CustomScan:
			if (((CustomScan *) plan)->flags & CUSTOMPATH_SUPPORT_PROJECTION)
				return true;
			return false;
		case T_ProjectSet:

			/*
			 * Although ProjectSet certainly projects, say "no" because we
			 * don't want the planner to randomly replace its tlist with
			 * something else; the SRFs have to stay at top level.  This might
			 * get relaxed later.
			 */
			return false;
		default:
			break;
	}
	return true;
}
