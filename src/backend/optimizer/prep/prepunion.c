/*-------------------------------------------------------------------------
 *
 * prepunion.c
 *	  Routines to plan set-operation queries.  The filename is a leftover
 *	  from a time when only UNIONs were implemented.
 *
 * There are two code paths in the planner for set-operation queries.
 * If a subquery consists entirely of simple UNION ALL operations, it
 * is converted into an "append relation".  Otherwise, it is handled
 * by the general code in this module (plan_set_operations and its
 * subroutines).  There is some support code here for the append-relation
 * case, but most of the heavy lifting for that is done elsewhere,
 * notably in prepjointree.c and allpaths.c.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/prep/prepunion.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/tlist.h"
#include "parser/parse_coerce.h"
#include "utils/selfuncs.h"


static RelOptInfo *recurse_set_operations(Node *setOp, PlannerInfo *root,
										  SetOperationStmt *parentOp,
										  List *colTypes, List *colCollations,
										  List *refnames_tlist,
										  List **pTargetList,
										  bool *istrivial_tlist);
static RelOptInfo *generate_recursion_path(SetOperationStmt *setOp,
										   PlannerInfo *root,
										   List *refnames_tlist,
										   List **pTargetList);
static void build_setop_child_paths(PlannerInfo *root, RelOptInfo *rel,
									bool trivial_tlist, List *child_tlist,
									List *interesting_pathkeys,
									double *pNumGroups);
static RelOptInfo *generate_union_paths(SetOperationStmt *op, PlannerInfo *root,
										List *refnames_tlist,
										List **pTargetList);
static RelOptInfo *generate_nonunion_paths(SetOperationStmt *op, PlannerInfo *root,
										   List *refnames_tlist,
										   List **pTargetList);
static List *plan_union_children(PlannerInfo *root,
								 SetOperationStmt *top_union,
								 List *refnames_tlist,
								 List **tlist_list,
								 List **istrivial_tlist);
static void postprocess_setop_rel(PlannerInfo *root, RelOptInfo *rel);
static List *generate_setop_tlist(List *colTypes, List *colCollations,
								  Index varno,
								  bool hack_constants,
								  List *input_tlist,
								  List *refnames_tlist,
								  bool *trivial_tlist);
static List *generate_append_tlist(List *colTypes, List *colCollations,
								   List *input_tlists,
								   List *refnames_tlist);
static List *generate_setop_grouplist(SetOperationStmt *op, List *targetlist);


/*
 * plan_set_operations
 *
 *	  Plans the queries for a tree of set operations (UNION/INTERSECT/EXCEPT)
 *
 * This routine only deals with the setOperations tree of the given query.
 * Any top-level ORDER BY requested in root->parse->sortClause will be handled
 * when we return to grouping_planner; likewise for LIMIT.
 *
 * What we return is an "upperrel" RelOptInfo containing at least one Path
 * that implements the set-operation tree.  In addition, root->processed_tlist
 * receives a targetlist representing the output of the topmost setop node.
 */
RelOptInfo *
plan_set_operations(PlannerInfo *root)
{
	Query	   *parse = root->parse;
	SetOperationStmt *topop = castNode(SetOperationStmt, parse->setOperations);
	Node	   *node;
	RangeTblEntry *leftmostRTE;
	Query	   *leftmostQuery;
	RelOptInfo *setop_rel;
	List	   *top_tlist;

	Assert(topop);

	/* check for unsupported stuff */
	Assert(parse->jointree->fromlist == NIL);
	Assert(parse->jointree->quals == NULL);
	Assert(parse->groupClause == NIL);
	Assert(parse->havingQual == NULL);
	Assert(parse->windowClause == NIL);
	Assert(parse->distinctClause == NIL);

	/*
	 * In the outer query level, equivalence classes are limited to classes
	 * which define that the top-level target entry is equivalent to the
	 * corresponding child target entry.  There won't be any equivalence class
	 * merging.  Mark that merging is complete to allow us to make pathkeys.
	 */
	Assert(root->eq_classes == NIL);
	root->ec_merging_done = true;

	/*
	 * We'll need to build RelOptInfos for each of the leaf subqueries, which
	 * are RTE_SUBQUERY rangetable entries in this Query.  Prepare the index
	 * arrays for those, and for AppendRelInfos in case they're needed.
	 */
	setup_simple_rel_arrays(root);

	/*
	 * Find the leftmost component Query.  We need to use its column names for
	 * all generated tlists (else SELECT INTO won't work right).
	 */
	node = topop->larg;
	while (node && IsA(node, SetOperationStmt))
		node = ((SetOperationStmt *) node)->larg;
	Assert(node && IsA(node, RangeTblRef));
	leftmostRTE = root->simple_rte_array[((RangeTblRef *) node)->rtindex];
	leftmostQuery = leftmostRTE->subquery;
	Assert(leftmostQuery != NULL);

	/*
	 * If the topmost node is a recursive union, it needs special processing.
	 */
	if (root->hasRecursion)
	{
		setop_rel = generate_recursion_path(topop, root,
											leftmostQuery->targetList,
											&top_tlist);
	}
	else
	{
		bool		trivial_tlist;

		/*
		 * Recurse on setOperations tree to generate paths for set ops. The
		 * final output paths should have just the column types shown as the
		 * output from the top-level node.
		 */
		setop_rel = recurse_set_operations((Node *) topop, root,
										   NULL,	/* no parent */
										   topop->colTypes, topop->colCollations,
										   leftmostQuery->targetList,
										   &top_tlist,
										   &trivial_tlist);
	}

	/* Must return the built tlist into root->processed_tlist. */
	root->processed_tlist = top_tlist;

	return setop_rel;
}

/*
 * recurse_set_operations
 *	  Recursively handle one step in a tree of set operations
 *
 * setOp: current step (could be a SetOperationStmt or a leaf RangeTblRef)
 * parentOp: parent step, or NULL if none (but see below)
 * colTypes: OID list of set-op's result column datatypes
 * colCollations: OID list of set-op's result column collations
 * refnames_tlist: targetlist to take column names from
 *
 * parentOp should be passed as NULL unless that step is interested in
 * getting sorted output from this step.  ("Sorted" means "sorted according
 * to the default btree opclasses of the result column datatypes".)
 *
 * Returns a RelOptInfo for the subtree, as well as these output parameters:
 * *pTargetList: receives the fully-fledged tlist for the subtree's top plan
 * *istrivial_tlist: true if, and only if, datatypes between parent and child
 * match.
 *
 * If setOp is a leaf node, this function plans the sub-query but does
 * not populate the pathlist of the returned RelOptInfo.  The caller will
 * generate SubqueryScan paths using useful path(s) of the subquery (see
 * build_setop_child_paths).  But this function does build the paths for
 * set-operation nodes.
 *
 * The pTargetList output parameter is mostly redundant with the pathtarget
 * of the returned RelOptInfo, but for the moment we need it because much of
 * the logic in this file depends on flag columns being marked resjunk.
 * XXX Now that there are no flag columns and hence no resjunk columns, we
 * could probably refactor this file to deal only in pathtargets.
 *
 * We don't have to care about typmods here: the only allowed difference
 * between set-op input and output typmods is input is a specific typmod
 * and output is -1, and that does not require a coercion.
 */
static RelOptInfo *
recurse_set_operations(Node *setOp, PlannerInfo *root,
					   SetOperationStmt *parentOp,
					   List *colTypes, List *colCollations,
					   List *refnames_tlist,
					   List **pTargetList,
					   bool *istrivial_tlist)
{
	RelOptInfo *rel;

	*istrivial_tlist = true;	/* for now */

	/* Guard against stack overflow due to overly complex setop nests */
	check_stack_depth();

	if (IsA(setOp, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) setOp;
		RangeTblEntry *rte = root->simple_rte_array[rtr->rtindex];
		Query	   *subquery = rte->subquery;
		PlannerInfo *subroot;
		List	   *tlist;
		bool		trivial_tlist;

		Assert(subquery != NULL);

		/* Build a RelOptInfo for this leaf subquery. */
		rel = build_simple_rel(root, rtr->rtindex, NULL);

		/* plan_params should not be in use in current query level */
		Assert(root->plan_params == NIL);

		/*
		 * Generate a subroot and Paths for the subquery.  If we have a
		 * parentOp, pass that down to encourage subquery_planner to consider
		 * suitably-sorted Paths.
		 */
		subroot = rel->subroot = subquery_planner(root->glob, subquery, root,
												  false, root->tuple_fraction,
												  parentOp);

		/*
		 * It should not be possible for the primitive query to contain any
		 * cross-references to other primitive queries in the setop tree.
		 */
		if (root->plan_params)
			elog(ERROR, "unexpected outer reference in set operation subquery");

		/* Figure out the appropriate target list for this subquery. */
		tlist = generate_setop_tlist(colTypes, colCollations,
									 rtr->rtindex,
									 true,
									 subroot->processed_tlist,
									 refnames_tlist,
									 &trivial_tlist);
		rel->reltarget = create_pathtarget(root, tlist);

		/* Return the fully-fledged tlist to caller, too */
		*pTargetList = tlist;
		*istrivial_tlist = trivial_tlist;
	}
	else if (IsA(setOp, SetOperationStmt))
	{
		SetOperationStmt *op = (SetOperationStmt *) setOp;

		/* UNIONs are much different from INTERSECT/EXCEPT */
		if (op->op == SETOP_UNION)
			rel = generate_union_paths(op, root,
									   refnames_tlist,
									   pTargetList);
		else
			rel = generate_nonunion_paths(op, root,
										  refnames_tlist,
										  pTargetList);

		/*
		 * If necessary, add a Result node to project the caller-requested
		 * output columns.
		 *
		 * XXX you don't really want to know about this: setrefs.c will apply
		 * fix_upper_expr() to the Result node's tlist. This would fail if the
		 * Vars generated by generate_setop_tlist() were not exactly equal()
		 * to the corresponding tlist entries of the subplan. However, since
		 * the subplan was generated by generate_union_paths() or
		 * generate_nonunion_paths(), and hence its tlist was generated by
		 * generate_append_tlist() or generate_setop_tlist(), this will work.
		 * We just tell generate_setop_tlist() to use varno 0.
		 */
		if (!tlist_same_datatypes(*pTargetList, colTypes, false) ||
			!tlist_same_collations(*pTargetList, colCollations, false))
		{
			PathTarget *target;
			bool		trivial_tlist;
			ListCell   *lc;

			*pTargetList = generate_setop_tlist(colTypes, colCollations,
												0,
												false,
												*pTargetList,
												refnames_tlist,
												&trivial_tlist);
			*istrivial_tlist = trivial_tlist;
			target = create_pathtarget(root, *pTargetList);

			/* Apply projection to each path */
			foreach(lc, rel->pathlist)
			{
				Path	   *subpath = (Path *) lfirst(lc);
				Path	   *path;

				Assert(subpath->param_info == NULL);
				path = apply_projection_to_path(root, subpath->parent,
												subpath, target);
				/* If we had to add a Result, path is different from subpath */
				if (path != subpath)
					lfirst(lc) = path;
			}

			/* Apply projection to each partial path */
			foreach(lc, rel->partial_pathlist)
			{
				Path	   *subpath = (Path *) lfirst(lc);
				Path	   *path;

				Assert(subpath->param_info == NULL);

				/* avoid apply_projection_to_path, in case of multiple refs */
				path = (Path *) create_projection_path(root, subpath->parent,
													   subpath, target);
				lfirst(lc) = path;
			}
		}
		postprocess_setop_rel(root, rel);
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(setOp));
		*pTargetList = NIL;
		rel = NULL;				/* keep compiler quiet */
	}

	return rel;
}

/*
 * Generate paths for a recursive UNION node
 */
static RelOptInfo *
generate_recursion_path(SetOperationStmt *setOp, PlannerInfo *root,
						List *refnames_tlist,
						List **pTargetList)
{
	RelOptInfo *result_rel;
	Path	   *path;
	RelOptInfo *lrel,
			   *rrel;
	Path	   *lpath;
	Path	   *rpath;
	List	   *lpath_tlist;
	bool		lpath_trivial_tlist;
	List	   *rpath_tlist;
	bool		rpath_trivial_tlist;
	List	   *tlist;
	List	   *groupList;
	double		dNumGroups;

	/* Parser should have rejected other cases */
	if (setOp->op != SETOP_UNION)
		elog(ERROR, "only UNION queries can be recursive");
	/* Worktable ID should be assigned */
	Assert(root->wt_param_id >= 0);

	/*
	 * Unlike a regular UNION node, process the left and right inputs
	 * separately without any intention of combining them into one Append.
	 */
	lrel = recurse_set_operations(setOp->larg, root,
								  NULL, /* no value in sorted results */
								  setOp->colTypes, setOp->colCollations,
								  refnames_tlist,
								  &lpath_tlist,
								  &lpath_trivial_tlist);
	if (lrel->rtekind == RTE_SUBQUERY)
		build_setop_child_paths(root, lrel, lpath_trivial_tlist, lpath_tlist,
								NIL, NULL);
	lpath = lrel->cheapest_total_path;
	/* The right path will want to look at the left one ... */
	root->non_recursive_path = lpath;
	rrel = recurse_set_operations(setOp->rarg, root,
								  NULL, /* no value in sorted results */
								  setOp->colTypes, setOp->colCollations,
								  refnames_tlist,
								  &rpath_tlist,
								  &rpath_trivial_tlist);
	if (rrel->rtekind == RTE_SUBQUERY)
		build_setop_child_paths(root, rrel, rpath_trivial_tlist, rpath_tlist,
								NIL, NULL);
	rpath = rrel->cheapest_total_path;
	root->non_recursive_path = NULL;

	/*
	 * Generate tlist for RecursiveUnion path node --- same as in Append cases
	 */
	tlist = generate_append_tlist(setOp->colTypes, setOp->colCollations,
								  list_make2(lpath_tlist, rpath_tlist),
								  refnames_tlist);

	*pTargetList = tlist;

	/* Build result relation. */
	result_rel = fetch_upper_rel(root, UPPERREL_SETOP,
								 bms_union(lrel->relids, rrel->relids));
	result_rel->reltarget = create_pathtarget(root, tlist);

	/*
	 * If UNION, identify the grouping operators
	 */
	if (setOp->all)
	{
		groupList = NIL;
		dNumGroups = 0;
	}
	else
	{
		/* Identify the grouping semantics */
		groupList = generate_setop_grouplist(setOp, tlist);

		/* We only support hashing here */
		if (!grouping_is_hashable(groupList))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("could not implement recursive UNION"),
					 errdetail("All column datatypes must be hashable.")));

		/*
		 * For the moment, take the number of distinct groups as equal to the
		 * total input size, ie, the worst case.
		 */
		dNumGroups = lpath->rows + rpath->rows * 10;
	}

	/*
	 * And make the path node.
	 */
	path = (Path *) create_recursiveunion_path(root,
											   result_rel,
											   lpath,
											   rpath,
											   result_rel->reltarget,
											   groupList,
											   root->wt_param_id,
											   dNumGroups);

	add_path(result_rel, path);
	postprocess_setop_rel(root, result_rel);
	return result_rel;
}

/*
 * build_setop_child_paths
 *		Build paths for the set op child relation denoted by 'rel'.
 *
 * 'rel' is an RTE_SUBQUERY relation.  We have already generated paths within
 * the subquery's subroot; the task here is to create SubqueryScan paths for
 * 'rel', representing scans of the useful subquery paths.
 *
 * interesting_pathkeys: if not NIL, also include paths that suit these
 * pathkeys, sorting any unsorted paths as required.
 * *pNumGroups: if not NULL, we estimate the number of distinct groups
 * in the result, and store it there.
 */
static void
build_setop_child_paths(PlannerInfo *root, RelOptInfo *rel,
						bool trivial_tlist, List *child_tlist,
						List *interesting_pathkeys, double *pNumGroups)
{
	RelOptInfo *final_rel;
	List	   *setop_pathkeys = rel->subroot->setop_pathkeys;
	ListCell   *lc;

	/* it can't be a set op child rel if it's not a subquery */
	Assert(rel->rtekind == RTE_SUBQUERY);

	/* when sorting is needed, add child rel equivalences */
	if (interesting_pathkeys != NIL)
		add_setop_child_rel_equivalences(root,
										 rel,
										 child_tlist,
										 interesting_pathkeys);

	/*
	 * Mark rel with estimated output rows, width, etc.  Note that we have to
	 * do this before generating outer-query paths, else cost_subqueryscan is
	 * not happy.
	 */
	set_subquery_size_estimates(root, rel);

	/*
	 * Since we may want to add a partial path to this relation, we must set
	 * its consider_parallel flag correctly.
	 */
	final_rel = fetch_upper_rel(rel->subroot, UPPERREL_FINAL, NULL);
	rel->consider_parallel = final_rel->consider_parallel;

	/* Generate subquery scan paths for any interesting path in final_rel */
	foreach(lc, final_rel->pathlist)
	{
		Path	   *subpath = (Path *) lfirst(lc);
		List	   *pathkeys;
		Path	   *cheapest_input_path = final_rel->cheapest_total_path;
		bool		is_sorted;
		int			presorted_keys;

		/*
		 * Include the cheapest path as-is so that the set operation can be
		 * cheaply implemented using a method which does not require the input
		 * to be sorted.
		 */
		if (subpath == cheapest_input_path)
		{
			/* Convert subpath's pathkeys to outer representation */
			pathkeys = convert_subquery_pathkeys(root, rel, subpath->pathkeys,
												 make_tlist_from_pathtarget(subpath->pathtarget));

			/* Generate outer path using this subpath */
			add_path(rel, (Path *) create_subqueryscan_path(root,
															rel,
															subpath,
															trivial_tlist,
															pathkeys,
															NULL));
		}

		/* skip dealing with sorted paths if the setop doesn't need them */
		if (interesting_pathkeys == NIL)
			continue;

		/*
		 * Create paths to suit final sort order required for setop_pathkeys.
		 * Here we'll sort the cheapest input path (if not sorted already) and
		 * incremental sort any paths which are partially sorted.
		 */
		is_sorted = pathkeys_count_contained_in(setop_pathkeys,
												subpath->pathkeys,
												&presorted_keys);

		if (!is_sorted)
		{
			double		limittuples = rel->subroot->limit_tuples;

			/*
			 * Try at least sorting the cheapest path and also try
			 * incrementally sorting any path which is partially sorted
			 * already (no need to deal with paths which have presorted keys
			 * when incremental sort is disabled unless it's the cheapest
			 * input path).
			 */
			if (subpath != cheapest_input_path &&
				(presorted_keys == 0 || !enable_incremental_sort))
				continue;

			/*
			 * We've no need to consider both a sort and incremental sort.
			 * We'll just do a sort if there are no presorted keys and an
			 * incremental sort when there are presorted keys.
			 */
			if (presorted_keys == 0 || !enable_incremental_sort)
				subpath = (Path *) create_sort_path(rel->subroot,
													final_rel,
													subpath,
													setop_pathkeys,
													limittuples);
			else
				subpath = (Path *) create_incremental_sort_path(rel->subroot,
																final_rel,
																subpath,
																setop_pathkeys,
																presorted_keys,
																limittuples);
		}

		/*
		 * subpath is now sorted, so add it to the pathlist.  We already added
		 * the cheapest_input_path above, so don't add it again unless we just
		 * sorted it.
		 */
		if (subpath != cheapest_input_path)
		{
			/* Convert subpath's pathkeys to outer representation */
			pathkeys = convert_subquery_pathkeys(root, rel, subpath->pathkeys,
												 make_tlist_from_pathtarget(subpath->pathtarget));

			/* Generate outer path using this subpath */
			add_path(rel, (Path *) create_subqueryscan_path(root,
															rel,
															subpath,
															trivial_tlist,
															pathkeys,
															NULL));
		}
	}

	/* if consider_parallel is false, there should be no partial paths */
	Assert(final_rel->consider_parallel ||
		   final_rel->partial_pathlist == NIL);

	/*
	 * If we have a partial path for the child relation, we can use that to
	 * build a partial path for this relation.  But there's no point in
	 * considering any path but the cheapest.
	 */
	if (rel->consider_parallel && bms_is_empty(rel->lateral_relids) &&
		final_rel->partial_pathlist != NIL)
	{
		Path	   *partial_subpath;
		Path	   *partial_path;

		partial_subpath = linitial(final_rel->partial_pathlist);
		partial_path = (Path *)
			create_subqueryscan_path(root, rel, partial_subpath,
									 trivial_tlist,
									 NIL, NULL);
		add_partial_path(rel, partial_path);
	}

	postprocess_setop_rel(root, rel);

	/*
	 * Estimate number of groups if caller wants it.  If the subquery used
	 * grouping or aggregation, its output is probably mostly unique anyway;
	 * otherwise do statistical estimation.
	 *
	 * XXX you don't really want to know about this: we do the estimation
	 * using the subroot->parse's original targetlist expressions, not the
	 * subroot->processed_tlist which might seem more appropriate.  The reason
	 * is that if the subquery is itself a setop, it may return a
	 * processed_tlist containing "varno 0" Vars generated by
	 * generate_append_tlist, and those would confuse estimate_num_groups
	 * mightily.  We ought to get rid of the "varno 0" hack, but that requires
	 * a redesign of the parsetree representation of setops, so that there can
	 * be an RTE corresponding to each setop's output. Note, we use this not
	 * subquery's targetlist but subroot->parse's targetlist, because it was
	 * revised by self-join removal.  subquery's targetlist might contain the
	 * references to the removed relids.
	 */
	if (pNumGroups)
	{
		PlannerInfo *subroot = rel->subroot;
		Query	   *subquery = subroot->parse;

		if (subquery->groupClause || subquery->groupingSets ||
			subquery->distinctClause || subroot->hasHavingQual ||
			subquery->hasAggs)
			*pNumGroups = rel->cheapest_total_path->rows;
		else
			*pNumGroups = estimate_num_groups(subroot,
											  get_tlist_exprs(subroot->parse->targetList, false),
											  rel->cheapest_total_path->rows,
											  NULL,
											  NULL);
	}
}

/*
 * Generate paths for a UNION or UNION ALL node
 */
static RelOptInfo *
generate_union_paths(SetOperationStmt *op, PlannerInfo *root,
					 List *refnames_tlist,
					 List **pTargetList)
{
	Relids		relids = NULL;
	RelOptInfo *result_rel;
	ListCell   *lc;
	ListCell   *lc2;
	ListCell   *lc3;
	List	   *cheapest_pathlist = NIL;
	List	   *ordered_pathlist = NIL;
	List	   *partial_pathlist = NIL;
	bool		partial_paths_valid = true;
	bool		consider_parallel = true;
	List	   *rellist;
	List	   *tlist_list;
	List	   *trivial_tlist_list;
	List	   *tlist;
	List	   *groupList = NIL;
	Path	   *apath;
	Path	   *gpath = NULL;
	bool		try_sorted = false;
	List	   *union_pathkeys = NIL;

	/*
	 * If any of my children are identical UNION nodes (same op, all-flag, and
	 * colTypes/colCollations) then they can be merged into this node so that
	 * we generate only one Append/MergeAppend and unique-ification for the
	 * lot.  Recurse to find such nodes.
	 */
	rellist = plan_union_children(root,
								  op,
								  refnames_tlist,
								  &tlist_list,
								  &trivial_tlist_list);

	/*
	 * Generate tlist for Append/MergeAppend plan node.
	 *
	 * The tlist for an Append plan isn't important as far as the Append is
	 * concerned, but we must make it look real anyway for the benefit of the
	 * next plan level up.
	 */
	tlist = generate_append_tlist(op->colTypes, op->colCollations,
								  tlist_list, refnames_tlist);
	*pTargetList = tlist;

	/* For UNIONs (not UNION ALL), try sorting, if sorting is possible */
	if (!op->all)
	{
		/* Identify the grouping semantics */
		groupList = generate_setop_grouplist(op, tlist);

		if (grouping_is_sortable(op->groupClauses))
		{
			try_sorted = true;
			/* Determine the pathkeys for sorting by the whole target list */
			union_pathkeys = make_pathkeys_for_sortclauses(root, groupList,
														   tlist);

			root->query_pathkeys = union_pathkeys;
		}
	}

	/*
	 * Now that we've got the append target list, we can build the union child
	 * paths.
	 */
	forthree(lc, rellist, lc2, trivial_tlist_list, lc3, tlist_list)
	{
		RelOptInfo *rel = lfirst(lc);
		bool		trivial_tlist = lfirst_int(lc2);
		List	   *child_tlist = lfirst_node(List, lc3);

		/* only build paths for the union children */
		if (rel->rtekind == RTE_SUBQUERY)
			build_setop_child_paths(root, rel, trivial_tlist, child_tlist,
									union_pathkeys, NULL);
	}

	/* Build path lists and relid set. */
	foreach(lc, rellist)
	{
		RelOptInfo *rel = lfirst(lc);
		Path	   *ordered_path;

		cheapest_pathlist = lappend(cheapest_pathlist,
									rel->cheapest_total_path);

		if (try_sorted)
		{
			ordered_path = get_cheapest_path_for_pathkeys(rel->pathlist,
														  union_pathkeys,
														  NULL,
														  TOTAL_COST,
														  false);

			if (ordered_path != NULL)
				ordered_pathlist = lappend(ordered_pathlist, ordered_path);
			else
			{
				/*
				 * If we can't find a sorted path, just give up trying to
				 * generate a list of correctly sorted child paths.  This can
				 * happen when type coercion was added to the targetlist due
				 * to mismatching types from the union children.
				 */
				try_sorted = false;
			}
		}

		if (consider_parallel)
		{
			if (!rel->consider_parallel)
			{
				consider_parallel = false;
				partial_paths_valid = false;
			}
			else if (rel->partial_pathlist == NIL)
				partial_paths_valid = false;
			else
				partial_pathlist = lappend(partial_pathlist,
										   linitial(rel->partial_pathlist));
		}

		relids = bms_union(relids, rel->relids);
	}

	/* Build result relation. */
	result_rel = fetch_upper_rel(root, UPPERREL_SETOP, relids);
	result_rel->reltarget = create_pathtarget(root, tlist);
	result_rel->consider_parallel = consider_parallel;
	result_rel->consider_startup = (root->tuple_fraction > 0);

	/*
	 * Append the child results together using the cheapest paths from each
	 * union child.
	 */
	apath = (Path *) create_append_path(root, result_rel, cheapest_pathlist,
										NIL, NIL, NULL, 0, false, -1);

	/*
	 * Estimate number of groups.  For now we just assume the output is unique
	 * --- this is certainly true for the UNION case, and we want worst-case
	 * estimates anyway.
	 */
	result_rel->rows = apath->rows;

	/*
	 * Now consider doing the same thing using the partial paths plus Append
	 * plus Gather.
	 */
	if (partial_paths_valid)
	{
		Path	   *papath;
		int			parallel_workers = 0;

		/* Find the highest number of workers requested for any subpath. */
		foreach(lc, partial_pathlist)
		{
			Path	   *subpath = lfirst(lc);

			parallel_workers = Max(parallel_workers,
								   subpath->parallel_workers);
		}
		Assert(parallel_workers > 0);

		/*
		 * If the use of parallel append is permitted, always request at least
		 * log2(# of children) paths.  We assume it can be useful to have
		 * extra workers in this case because they will be spread out across
		 * the children.  The precise formula is just a guess; see
		 * add_paths_to_append_rel.
		 */
		if (enable_parallel_append)
		{
			parallel_workers = Max(parallel_workers,
								   pg_leftmost_one_pos32(list_length(partial_pathlist)) + 1);
			parallel_workers = Min(parallel_workers,
								   max_parallel_workers_per_gather);
		}
		Assert(parallel_workers > 0);

		papath = (Path *)
			create_append_path(root, result_rel, NIL, partial_pathlist,
							   NIL, NULL, parallel_workers,
							   enable_parallel_append, -1);
		gpath = (Path *)
			create_gather_path(root, result_rel, papath,
							   result_rel->reltarget, NULL, NULL);
	}

	if (!op->all)
	{
		double		dNumGroups;
		bool		can_sort = grouping_is_sortable(groupList);
		bool		can_hash = grouping_is_hashable(groupList);

		/*
		 * XXX for the moment, take the number of distinct groups as equal to
		 * the total input size, i.e., the worst case.  This is too
		 * conservative, but it's not clear how to get a decent estimate of
		 * the true size.  One should note as well the propensity of novices
		 * to write UNION rather than UNION ALL even when they don't expect
		 * any duplicates...
		 */
		dNumGroups = apath->rows;

		if (can_hash)
		{
			Path	   *path;

			/*
			 * Try a hash aggregate plan on 'apath'.  This is the cheapest
			 * available path containing each append child.
			 */
			path = (Path *) create_agg_path(root,
											result_rel,
											apath,
											create_pathtarget(root, tlist),
											AGG_HASHED,
											AGGSPLIT_SIMPLE,
											groupList,
											NIL,
											NULL,
											dNumGroups);
			add_path(result_rel, path);

			/* Try hash aggregate on the Gather path, if valid */
			if (gpath != NULL)
			{
				/* Hashed aggregate plan --- no sort needed */
				path = (Path *) create_agg_path(root,
												result_rel,
												gpath,
												create_pathtarget(root, tlist),
												AGG_HASHED,
												AGGSPLIT_SIMPLE,
												groupList,
												NIL,
												NULL,
												dNumGroups);
				add_path(result_rel, path);
			}
		}

		if (can_sort)
		{
			Path	   *path = apath;

			/* Try Sort -> Unique on the Append path */
			if (groupList != NIL)
				path = (Path *) create_sort_path(root, result_rel, path,
												 make_pathkeys_for_sortclauses(root, groupList, tlist),
												 -1.0);

			path = (Path *) create_upper_unique_path(root,
													 result_rel,
													 path,
													 list_length(path->pathkeys),
													 dNumGroups);

			add_path(result_rel, path);

			/* Try Sort -> Unique on the Gather path, if set */
			if (gpath != NULL)
			{
				path = gpath;

				path = (Path *) create_sort_path(root, result_rel, path,
												 make_pathkeys_for_sortclauses(root, groupList, tlist),
												 -1.0);

				path = (Path *) create_upper_unique_path(root,
														 result_rel,
														 path,
														 list_length(path->pathkeys),
														 dNumGroups);
				add_path(result_rel, path);
			}
		}

		/*
		 * Try making a MergeAppend path if we managed to find a path with the
		 * correct pathkeys in each union child query.
		 */
		if (try_sorted && groupList != NIL)
		{
			Path	   *path;

			path = (Path *) create_merge_append_path(root,
													 result_rel,
													 ordered_pathlist,
													 union_pathkeys,
													 NULL);

			/* and make the MergeAppend unique */
			path = (Path *) create_upper_unique_path(root,
													 result_rel,
													 path,
													 list_length(tlist),
													 dNumGroups);

			add_path(result_rel, path);
		}
	}
	else
	{
		/* UNION ALL */
		add_path(result_rel, apath);

		if (gpath != NULL)
			add_path(result_rel, gpath);
	}

	return result_rel;
}

/*
 * Generate paths for an INTERSECT, INTERSECT ALL, EXCEPT, or EXCEPT ALL node
 */
static RelOptInfo *
generate_nonunion_paths(SetOperationStmt *op, PlannerInfo *root,
						List *refnames_tlist,
						List **pTargetList)
{
	RelOptInfo *result_rel;
	RelOptInfo *lrel,
			   *rrel;
	double		save_fraction = root->tuple_fraction;
	Path	   *lpath,
			   *rpath,
			   *path;
	List	   *lpath_tlist,
			   *rpath_tlist,
			   *tlist,
			   *groupList;
	bool		lpath_trivial_tlist,
				rpath_trivial_tlist,
				result_trivial_tlist;
	List	   *nonunion_pathkeys = NIL;
	double		dLeftGroups,
				dRightGroups,
				dNumGroups,
				dNumOutputRows;
	bool		can_sort;
	bool		can_hash;
	SetOpCmd	cmd;

	/*
	 * Tell children to fetch all tuples.
	 */
	root->tuple_fraction = 0.0;

	/* Recurse on children */
	lrel = recurse_set_operations(op->larg, root,
								  op,
								  op->colTypes, op->colCollations,
								  refnames_tlist,
								  &lpath_tlist,
								  &lpath_trivial_tlist);

	rrel = recurse_set_operations(op->rarg, root,
								  op,
								  op->colTypes, op->colCollations,
								  refnames_tlist,
								  &rpath_tlist,
								  &rpath_trivial_tlist);

	/*
	 * Generate tlist for SetOp plan node.
	 *
	 * The tlist for a SetOp plan isn't important so far as the SetOp is
	 * concerned, but we must make it look real anyway for the benefit of the
	 * next plan level up.
	 */
	tlist = generate_setop_tlist(op->colTypes, op->colCollations,
								 0, false, lpath_tlist, refnames_tlist,
								 &result_trivial_tlist);

	/* We should not have needed any type coercions in the tlist */
	Assert(result_trivial_tlist);

	*pTargetList = tlist;

	/* Identify the grouping semantics */
	groupList = generate_setop_grouplist(op, tlist);

	/* Check whether the operators support sorting or hashing */
	can_sort = grouping_is_sortable(groupList);
	can_hash = grouping_is_hashable(groupList);
	if (!can_sort && !can_hash)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/* translator: %s is INTERSECT or EXCEPT */
				 errmsg("could not implement %s",
						(op->op == SETOP_INTERSECT) ? "INTERSECT" : "EXCEPT"),
				 errdetail("Some of the datatypes only support hashing, while others only support sorting.")));

	if (can_sort)
	{
		/* Determine the pathkeys for sorting by the whole target list */
		nonunion_pathkeys = make_pathkeys_for_sortclauses(root, groupList,
														  tlist);

		root->query_pathkeys = nonunion_pathkeys;
	}

	/*
	 * Now that we've got all that info, we can build the child paths.
	 */
	if (lrel->rtekind == RTE_SUBQUERY)
		build_setop_child_paths(root, lrel, lpath_trivial_tlist, lpath_tlist,
								nonunion_pathkeys, &dLeftGroups);
	else
		dLeftGroups = lrel->rows;
	if (rrel->rtekind == RTE_SUBQUERY)
		build_setop_child_paths(root, rrel, rpath_trivial_tlist, rpath_tlist,
								nonunion_pathkeys, &dRightGroups);
	else
		dRightGroups = rrel->rows;

	/* Undo effects of forcing tuple_fraction to 0 */
	root->tuple_fraction = save_fraction;

	/*
	 * For EXCEPT, we must put the left input first.  For INTERSECT, either
	 * order should give the same results, and we prefer to put the smaller
	 * input first in order to (a) minimize the size of the hash table in the
	 * hashing case, and (b) improve our chances of exploiting the executor's
	 * fast path for empty left-hand input.  "Smaller" means the one with the
	 * fewer groups.
	 */
	if (op->op != SETOP_EXCEPT && dLeftGroups > dRightGroups)
	{
		/* need to swap the two inputs */
		RelOptInfo *tmprel;
		List	   *tmplist;
		double		tmpd;

		tmprel = lrel;
		lrel = rrel;
		rrel = tmprel;
		tmplist = lpath_tlist;
		lpath_tlist = rpath_tlist;
		rpath_tlist = tmplist;
		tmpd = dLeftGroups;
		dLeftGroups = dRightGroups;
		dRightGroups = tmpd;
	}

	lpath = lrel->cheapest_total_path;
	rpath = rrel->cheapest_total_path;

	/* Build result relation. */
	result_rel = fetch_upper_rel(root, UPPERREL_SETOP,
								 bms_union(lrel->relids, rrel->relids));
	result_rel->reltarget = create_pathtarget(root, tlist);

	/*
	 * Estimate number of distinct groups that we'll need hashtable entries
	 * for; this is the size of the left-hand input for EXCEPT, or the smaller
	 * input for INTERSECT.  Also estimate the number of eventual output rows.
	 * In non-ALL cases, we estimate each group produces one output row; in
	 * ALL cases use the relevant relation size.  These are worst-case
	 * estimates, of course, but we need to be conservative.
	 */
	if (op->op == SETOP_EXCEPT)
	{
		dNumGroups = dLeftGroups;
		dNumOutputRows = op->all ? lpath->rows : dNumGroups;
	}
	else
	{
		dNumGroups = dLeftGroups;
		dNumOutputRows = op->all ? Min(lpath->rows, rpath->rows) : dNumGroups;
	}
	result_rel->rows = dNumOutputRows;

	/* Select the SetOpCmd type */
	switch (op->op)
	{
		case SETOP_INTERSECT:
			cmd = op->all ? SETOPCMD_INTERSECT_ALL : SETOPCMD_INTERSECT;
			break;
		case SETOP_EXCEPT:
			cmd = op->all ? SETOPCMD_EXCEPT_ALL : SETOPCMD_EXCEPT;
			break;
		default:
			elog(ERROR, "unrecognized set op: %d", (int) op->op);
			cmd = SETOPCMD_INTERSECT;	/* keep compiler quiet */
			break;
	}

	/*
	 * If we can hash, that just requires a SetOp atop the cheapest inputs.
	 */
	if (can_hash)
	{
		path = (Path *) create_setop_path(root,
										  result_rel,
										  lpath,
										  rpath,
										  cmd,
										  SETOP_HASHED,
										  groupList,
										  dNumGroups,
										  dNumOutputRows);
		add_path(result_rel, path);
	}

	/*
	 * If we can sort, generate the cheapest sorted input paths, and add a
	 * SetOp atop those.
	 */
	if (can_sort)
	{
		List	   *pathkeys;
		Path	   *slpath,
				   *srpath;

		/* First the left input ... */
		pathkeys = make_pathkeys_for_sortclauses(root,
												 groupList,
												 lpath_tlist);
		if (pathkeys_contained_in(pathkeys, lpath->pathkeys))
			slpath = lpath;		/* cheapest path is already sorted */
		else
		{
			slpath = get_cheapest_path_for_pathkeys(lrel->pathlist,
													nonunion_pathkeys,
													NULL,
													TOTAL_COST,
													false);
			/* Subquery failed to produce any presorted paths? */
			if (slpath == NULL)
				slpath = (Path *) create_sort_path(root,
												   lpath->parent,
												   lpath,
												   pathkeys,
												   -1.0);
		}

		/* and now the same for the right. */
		pathkeys = make_pathkeys_for_sortclauses(root,
												 groupList,
												 rpath_tlist);
		if (pathkeys_contained_in(pathkeys, rpath->pathkeys))
			srpath = rpath;		/* cheapest path is already sorted */
		else
		{
			srpath = get_cheapest_path_for_pathkeys(rrel->pathlist,
													nonunion_pathkeys,
													NULL,
													TOTAL_COST,
													false);
			/* Subquery failed to produce any presorted paths? */
			if (srpath == NULL)
				srpath = (Path *) create_sort_path(root,
												   rpath->parent,
												   rpath,
												   pathkeys,
												   -1.0);
		}

		path = (Path *) create_setop_path(root,
										  result_rel,
										  slpath,
										  srpath,
										  cmd,
										  SETOP_SORTED,
										  groupList,
										  dNumGroups,
										  dNumOutputRows);
		add_path(result_rel, path);
	}

	return result_rel;
}

/*
 * Pull up children of a UNION node that are identically-propertied UNIONs,
 * and perform planning of the queries underneath the N-way UNION.
 *
 * The result is a list of RelOptInfos containing Paths for sub-nodes, with
 * one entry for each descendant that is a leaf query or non-identical setop.
 * We also return parallel lists of the childrens' targetlists and
 * is-trivial-tlist flags.
 *
 * NOTE: we can also pull a UNION ALL up into a UNION, since the distinct
 * output rows will be lost anyway.
 */
static List *
plan_union_children(PlannerInfo *root,
					SetOperationStmt *top_union,
					List *refnames_tlist,
					List **tlist_list,
					List **istrivial_tlist)
{
	List	   *pending_rels = list_make1(top_union);
	List	   *result = NIL;
	List	   *child_tlist;
	bool		trivial_tlist;

	*tlist_list = NIL;
	*istrivial_tlist = NIL;

	while (pending_rels != NIL)
	{
		Node	   *setOp = linitial(pending_rels);

		pending_rels = list_delete_first(pending_rels);

		if (IsA(setOp, SetOperationStmt))
		{
			SetOperationStmt *op = (SetOperationStmt *) setOp;

			if (op->op == top_union->op &&
				(op->all == top_union->all || op->all) &&
				equal(op->colTypes, top_union->colTypes) &&
				equal(op->colCollations, top_union->colCollations))
			{
				/* Same UNION, so fold children into parent */
				pending_rels = lcons(op->rarg, pending_rels);
				pending_rels = lcons(op->larg, pending_rels);
				continue;
			}
		}

		/*
		 * Not same, so plan this child separately.
		 *
		 * If top_union isn't a UNION ALL, then we are interested in sorted
		 * output from the child, so pass top_union as parentOp.  Note that
		 * this isn't necessarily the child node's immediate SetOperationStmt
		 * parent, but that's fine: it's the effective parent.
		 */
		result = lappend(result, recurse_set_operations(setOp, root,
														top_union->all ? NULL : top_union,
														top_union->colTypes,
														top_union->colCollations,
														refnames_tlist,
														&child_tlist,
														&trivial_tlist));
		*tlist_list = lappend(*tlist_list, child_tlist);
		*istrivial_tlist = lappend_int(*istrivial_tlist, trivial_tlist);
	}

	return result;
}

/*
 * postprocess_setop_rel - perform steps required after adding paths
 */
static void
postprocess_setop_rel(PlannerInfo *root, RelOptInfo *rel)
{
	/*
	 * We don't currently worry about allowing FDWs to contribute paths to
	 * this relation, but give extensions a chance.
	 */
	if (create_upper_paths_hook)
		(*create_upper_paths_hook) (root, UPPERREL_SETOP,
									NULL, rel, NULL);

	/* Select cheapest path */
	set_cheapest(rel);
}

/*
 * Generate targetlist for a set-operation plan node
 *
 * colTypes: OID list of set-op's result column datatypes
 * colCollations: OID list of set-op's result column collations
 * varno: varno to use in generated Vars
 * hack_constants: true to copy up constants (see comments in code)
 * input_tlist: targetlist of this node's input node
 * refnames_tlist: targetlist to take column names from
 * trivial_tlist: output parameter, set to true if targetlist is trivial
 */
static List *
generate_setop_tlist(List *colTypes, List *colCollations,
					 Index varno,
					 bool hack_constants,
					 List *input_tlist,
					 List *refnames_tlist,
					 bool *trivial_tlist)
{
	List	   *tlist = NIL;
	int			resno = 1;
	ListCell   *ctlc,
			   *cclc,
			   *itlc,
			   *rtlc;
	TargetEntry *tle;
	Node	   *expr;

	*trivial_tlist = true;		/* until proven differently */

	forfour(ctlc, colTypes, cclc, colCollations,
			itlc, input_tlist, rtlc, refnames_tlist)
	{
		Oid			colType = lfirst_oid(ctlc);
		Oid			colColl = lfirst_oid(cclc);
		TargetEntry *inputtle = (TargetEntry *) lfirst(itlc);
		TargetEntry *reftle = (TargetEntry *) lfirst(rtlc);

		Assert(inputtle->resno == resno);
		Assert(reftle->resno == resno);
		Assert(!inputtle->resjunk);
		Assert(!reftle->resjunk);

		/*
		 * Generate columns referencing input columns and having appropriate
		 * data types and column names.  Insert datatype coercions where
		 * necessary.
		 *
		 * HACK: constants in the input's targetlist are copied up as-is
		 * rather than being referenced as subquery outputs.  This is mainly
		 * to ensure that when we try to coerce them to the output column's
		 * datatype, the right things happen for UNKNOWN constants.  But do
		 * this only at the first level of subquery-scan plans; we don't want
		 * phony constants appearing in the output tlists of upper-level
		 * nodes!
		 *
		 * Note that copying a constant doesn't in itself require us to mark
		 * the tlist nontrivial; see trivial_subqueryscan() in setrefs.c.
		 */
		if (hack_constants && inputtle->expr && IsA(inputtle->expr, Const))
			expr = (Node *) inputtle->expr;
		else
			expr = (Node *) makeVar(varno,
									inputtle->resno,
									exprType((Node *) inputtle->expr),
									exprTypmod((Node *) inputtle->expr),
									exprCollation((Node *) inputtle->expr),
									0);

		if (exprType(expr) != colType)
		{
			/*
			 * Note: it's not really cool to be applying coerce_to_common_type
			 * here; one notable point is that assign_expr_collations never
			 * gets run on any generated nodes.  For the moment that's not a
			 * problem because we force the correct exposed collation below.
			 * It would likely be best to make the parser generate the correct
			 * output tlist for every set-op to begin with, though.
			 */
			expr = coerce_to_common_type(NULL,	/* no UNKNOWNs here */
										 expr,
										 colType,
										 "UNION/INTERSECT/EXCEPT");
			*trivial_tlist = false; /* the coercion makes it not trivial */
		}

		/*
		 * Ensure the tlist entry's exposed collation matches the set-op. This
		 * is necessary because plan_set_operations() reports the result
		 * ordering as a list of SortGroupClauses, which don't carry collation
		 * themselves but just refer to tlist entries.  If we don't show the
		 * right collation then planner.c might do the wrong thing in
		 * higher-level queries.
		 *
		 * Note we use RelabelType, not CollateExpr, since this expression
		 * will reach the executor without any further processing.
		 */
		if (exprCollation(expr) != colColl)
		{
			expr = applyRelabelType(expr,
									exprType(expr), exprTypmod(expr), colColl,
									COERCE_IMPLICIT_CAST, -1, false);
			*trivial_tlist = false; /* the relabel makes it not trivial */
		}

		tle = makeTargetEntry((Expr *) expr,
							  (AttrNumber) resno++,
							  pstrdup(reftle->resname),
							  false);

		/*
		 * By convention, all output columns in a setop tree have
		 * ressortgroupref equal to their resno.  In some cases the ref isn't
		 * needed, but this is a cleaner way than modifying the tlist later.
		 */
		tle->ressortgroupref = tle->resno;

		tlist = lappend(tlist, tle);
	}

	return tlist;
}

/*
 * Generate targetlist for a set-operation Append node
 *
 * colTypes: OID list of set-op's result column datatypes
 * colCollations: OID list of set-op's result column collations
 * input_tlists: list of tlists for sub-plans of the Append
 * refnames_tlist: targetlist to take column names from
 *
 * The entries in the Append's targetlist should always be simple Vars;
 * we just have to make sure they have the right datatypes/typmods/collations.
 * The Vars are always generated with varno 0.
 *
 * XXX a problem with the varno-zero approach is that set_pathtarget_cost_width
 * cannot figure out a realistic width for the tlist we make here.  But we
 * ought to refactor this code to produce a PathTarget directly, anyway.
 */
static List *
generate_append_tlist(List *colTypes, List *colCollations,
					  List *input_tlists,
					  List *refnames_tlist)
{
	List	   *tlist = NIL;
	int			resno = 1;
	ListCell   *curColType;
	ListCell   *curColCollation;
	ListCell   *ref_tl_item;
	int			colindex;
	TargetEntry *tle;
	Node	   *expr;
	ListCell   *tlistl;
	int32	   *colTypmods;

	/*
	 * First extract typmods to use.
	 *
	 * If the inputs all agree on type and typmod of a particular column, use
	 * that typmod; else use -1.
	 */
	colTypmods = (int32 *) palloc(list_length(colTypes) * sizeof(int32));

	foreach(tlistl, input_tlists)
	{
		List	   *subtlist = (List *) lfirst(tlistl);
		ListCell   *subtlistl;

		curColType = list_head(colTypes);
		colindex = 0;
		foreach(subtlistl, subtlist)
		{
			TargetEntry *subtle = (TargetEntry *) lfirst(subtlistl);

			Assert(!subtle->resjunk);
			Assert(curColType != NULL);
			if (exprType((Node *) subtle->expr) == lfirst_oid(curColType))
			{
				/* If first subplan, copy the typmod; else compare */
				int32		subtypmod = exprTypmod((Node *) subtle->expr);

				if (tlistl == list_head(input_tlists))
					colTypmods[colindex] = subtypmod;
				else if (subtypmod != colTypmods[colindex])
					colTypmods[colindex] = -1;
			}
			else
			{
				/* types disagree, so force typmod to -1 */
				colTypmods[colindex] = -1;
			}
			curColType = lnext(colTypes, curColType);
			colindex++;
		}
		Assert(curColType == NULL);
	}

	/*
	 * Now we can build the tlist for the Append.
	 */
	colindex = 0;
	forthree(curColType, colTypes, curColCollation, colCollations,
			 ref_tl_item, refnames_tlist)
	{
		Oid			colType = lfirst_oid(curColType);
		int32		colTypmod = colTypmods[colindex++];
		Oid			colColl = lfirst_oid(curColCollation);
		TargetEntry *reftle = (TargetEntry *) lfirst(ref_tl_item);

		Assert(reftle->resno == resno);
		Assert(!reftle->resjunk);
		expr = (Node *) makeVar(0,
								resno,
								colType,
								colTypmod,
								colColl,
								0);
		tle = makeTargetEntry((Expr *) expr,
							  (AttrNumber) resno++,
							  pstrdup(reftle->resname),
							  false);

		/*
		 * By convention, all output columns in a setop tree have
		 * ressortgroupref equal to their resno.  In some cases the ref isn't
		 * needed, but this is a cleaner way than modifying the tlist later.
		 */
		tle->ressortgroupref = tle->resno;

		tlist = lappend(tlist, tle);
	}

	pfree(colTypmods);

	return tlist;
}

/*
 * generate_setop_grouplist
 *		Build a SortGroupClause list defining the sort/grouping properties
 *		of the setop's output columns.
 *
 * Parse analysis already determined the properties and built a suitable
 * list, except that the entries do not have sortgrouprefs set because
 * the parser output representation doesn't include a tlist for each
 * setop.  So what we need to do here is copy that list and install
 * proper sortgrouprefs into it (copying those from the targetlist).
 */
static List *
generate_setop_grouplist(SetOperationStmt *op, List *targetlist)
{
	List	   *grouplist = copyObject(op->groupClauses);
	ListCell   *lg;
	ListCell   *lt;

	lg = list_head(grouplist);
	foreach(lt, targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lt);
		SortGroupClause *sgc;

		Assert(!tle->resjunk);

		/* non-resjunk columns should have sortgroupref = resno */
		Assert(tle->ressortgroupref == tle->resno);

		/* non-resjunk columns should have grouping clauses */
		Assert(lg != NULL);
		sgc = (SortGroupClause *) lfirst(lg);
		lg = lnext(grouplist, lg);
		Assert(sgc->tleSortGroupRef == 0);

		sgc->tleSortGroupRef = tle->ressortgroupref;
	}
	Assert(lg == NULL);
	return grouplist;
}
