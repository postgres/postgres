/*-------------------------------------------------------------------------
 *
 * createplan.c
 *	  Routines to create the desired plan for processing a query.
 *	  Planning is complete, we just need to convert the selected
 *	  Path into a Plan.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/plan/createplan.c,v 1.157.2.3 2004/02/29 17:36:48 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"
#include "parser/parsetree.h"
#include "parser/parse_clause.h"
#include "parser/parse_expr.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


static Scan *create_scan_plan(Query *root, Path *best_path);
static List *build_relation_tlist(RelOptInfo *rel);
static bool use_physical_tlist(RelOptInfo *rel);
static void disuse_physical_tlist(Plan *plan, Path *path);
static Join *create_join_plan(Query *root, JoinPath *best_path);
static Append *create_append_plan(Query *root, AppendPath *best_path);
static Result *create_result_plan(Query *root, ResultPath *best_path);
static Material *create_material_plan(Query *root, MaterialPath *best_path);
static Plan *create_unique_plan(Query *root, UniquePath *best_path);
static SeqScan *create_seqscan_plan(Path *best_path, List *tlist,
					List *scan_clauses);
static IndexScan *create_indexscan_plan(Query *root, IndexPath *best_path,
					  List *tlist, List *scan_clauses);
static TidScan *create_tidscan_plan(TidPath *best_path, List *tlist,
					List *scan_clauses);
static SubqueryScan *create_subqueryscan_plan(Path *best_path,
						 List *tlist, List *scan_clauses);
static FunctionScan *create_functionscan_plan(Path *best_path,
						 List *tlist, List *scan_clauses);
static NestLoop *create_nestloop_plan(Query *root, NestPath *best_path,
					 Plan *outer_plan, Plan *inner_plan);
static MergeJoin *create_mergejoin_plan(Query *root, MergePath *best_path,
					  Plan *outer_plan, Plan *inner_plan);
static HashJoin *create_hashjoin_plan(Query *root, HashPath *best_path,
					 Plan *outer_plan, Plan *inner_plan);
static void fix_indxqual_references(List *indexquals, IndexPath *index_path,
						List **fixed_indexquals,
						List **recheck_indexquals);
static void fix_indxqual_sublist(List *indexqual,
					 Relids baserelids, int baserelid,
					 IndexOptInfo *index,
					 List **fixed_quals, List **recheck_quals);
static Node *fix_indxqual_operand(Node *node, int baserelid,
					 IndexOptInfo *index,
					 Oid *opclass);
static List *get_switched_clauses(List *clauses, Relids outerrelids);
static List *order_qual_clauses(Query *root, List *clauses);
static void copy_path_costsize(Plan *dest, Path *src);
static void copy_plan_costsize(Plan *dest, Plan *src);
static SeqScan *make_seqscan(List *qptlist, List *qpqual, Index scanrelid);
static IndexScan *make_indexscan(List *qptlist, List *qpqual, Index scanrelid,
			   List *indxid, List *indxqual,
			   List *indxqualorig,
			   ScanDirection indexscandir);
static TidScan *make_tidscan(List *qptlist, List *qpqual, Index scanrelid,
			 List *tideval);
static FunctionScan *make_functionscan(List *qptlist, List *qpqual,
				  Index scanrelid);
static NestLoop *make_nestloop(List *tlist,
			  List *joinclauses, List *otherclauses,
			  Plan *lefttree, Plan *righttree,
			  JoinType jointype);
static HashJoin *make_hashjoin(List *tlist,
			  List *joinclauses, List *otherclauses,
			  List *hashclauses,
			  Plan *lefttree, Plan *righttree,
			  JoinType jointype);
static Hash *make_hash(List *tlist, List *hashkeys, Plan *lefttree);
static MergeJoin *make_mergejoin(List *tlist,
			   List *joinclauses, List *otherclauses,
			   List *mergeclauses,
			   Plan *lefttree, Plan *righttree,
			   JoinType jointype);
static Sort *make_sort(Query *root, List *tlist, Plan *lefttree, int numCols,
		  AttrNumber *sortColIdx, Oid *sortOperators);
static Sort *make_sort_from_pathkeys(Query *root, Plan *lefttree,
						List *pathkeys);


/*
 * create_plan
 *	  Creates the access plan for a query by tracing backwards through the
 *	  desired chain of pathnodes, starting at the node 'best_path'.  For
 *	  every pathnode found:
 *	  (1) Create a corresponding plan node containing appropriate id,
 *		  target list, and qualification information.
 *	  (2) Modify qual clauses of join nodes so that subplan attributes are
 *		  referenced using relative values.
 *	  (3) Target lists are not modified, but will be in setrefs.c.
 *
 *	  best_path is the best access path
 *
 *	  Returns a Plan tree.
 */
Plan *
create_plan(Query *root, Path *best_path)
{
	Plan	   *plan;

	switch (best_path->pathtype)
	{
		case T_IndexScan:
		case T_SeqScan:
		case T_TidScan:
		case T_SubqueryScan:
		case T_FunctionScan:
			plan = (Plan *) create_scan_plan(root, best_path);
			break;
		case T_HashJoin:
		case T_MergeJoin:
		case T_NestLoop:
			plan = (Plan *) create_join_plan(root,
											 (JoinPath *) best_path);
			break;
		case T_Append:
			plan = (Plan *) create_append_plan(root,
											   (AppendPath *) best_path);
			break;
		case T_Result:
			plan = (Plan *) create_result_plan(root,
											   (ResultPath *) best_path);
			break;
		case T_Material:
			plan = (Plan *) create_material_plan(root,
											 (MaterialPath *) best_path);
			break;
		case T_Unique:
			plan = (Plan *) create_unique_plan(root,
											   (UniquePath *) best_path);
			break;
		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) best_path->pathtype);
			plan = NULL;		/* keep compiler quiet */
			break;
	}

#ifdef NOT_USED					/* fix xfunc */
	/* sort clauses by cost/(1-selectivity) -- JMH 2/26/92 */
	if (XfuncMode != XFUNC_OFF)
	{
		set_qpqual((Plan) plan,
				   lisp_qsort(get_qpqual((Plan) plan),
							  xfunc_clause_compare));
		if (XfuncMode != XFUNC_NOR)
			/* sort the disjuncts within each clause by cost -- JMH 3/4/92 */
			xfunc_disjunct_sort(plan->qpqual);
	}
#endif

	return plan;
}

/*
 * create_scan_plan
 *	 Create a scan plan for the parent relation of 'best_path'.
 *
 *	 Returns a Plan node.
 */
static Scan *
create_scan_plan(Query *root, Path *best_path)
{
	RelOptInfo *rel = best_path->parent;
	List	   *tlist;
	List	   *scan_clauses;
	Scan	   *plan;

	/*
	 * For table scans, rather than using the relation targetlist (which
	 * is only those Vars actually needed by the query), we prefer to
	 * generate a tlist containing all Vars in order.  This will allow the
	 * executor to optimize away projection of the table tuples, if
	 * possible.  (Note that planner.c may replace the tlist we generate
	 * here, forcing projection to occur.)
	 */
	if (use_physical_tlist(rel))
	{
		tlist = build_physical_tlist(root, rel);
		/* if fail because of dropped cols, use regular method */
		if (tlist == NIL)
			tlist = build_relation_tlist(rel);
	}
	else
		tlist = build_relation_tlist(rel);

	/*
	 * Extract the relevant restriction clauses from the parent relation;
	 * the executor must apply all these restrictions during the scan.
	 */
	scan_clauses = get_actual_clauses(rel->baserestrictinfo);

	/* Sort clauses into best execution order */
	scan_clauses = order_qual_clauses(root, scan_clauses);

	switch (best_path->pathtype)
	{
		case T_SeqScan:
			plan = (Scan *) create_seqscan_plan(best_path,
												tlist,
												scan_clauses);
			break;

		case T_IndexScan:
			plan = (Scan *) create_indexscan_plan(root,
												  (IndexPath *) best_path,
												  tlist,
												  scan_clauses);
			break;

		case T_TidScan:
			plan = (Scan *) create_tidscan_plan((TidPath *) best_path,
												tlist,
												scan_clauses);
			break;

		case T_SubqueryScan:
			plan = (Scan *) create_subqueryscan_plan(best_path,
													 tlist,
													 scan_clauses);
			break;

		case T_FunctionScan:
			plan = (Scan *) create_functionscan_plan(best_path,
													 tlist,
													 scan_clauses);
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
 * Build a target list (ie, a list of TargetEntry) for a relation.
 */
static List *
build_relation_tlist(RelOptInfo *rel)
{
	FastList	tlist;
	int			resdomno = 1;
	List	   *v;

	FastListInit(&tlist);
	foreach(v, FastListValue(&rel->reltargetlist))
	{
		/* Do we really need to copy here?	Not sure */
		Var		   *var = (Var *) copyObject(lfirst(v));

		FastAppend(&tlist, create_tl_element(var, resdomno));
		resdomno++;
	}
	return FastListValue(&tlist);
}

/*
 * use_physical_tlist
 *		Decide whether to use a tlist matching relation structure,
 *		rather than only those Vars actually referenced.
 */
static bool
use_physical_tlist(RelOptInfo *rel)
{
	int			i;

	/*
	 * Currently, can't do this for subquery or function scans.  (This is
	 * mainly because we don't have an equivalent of build_physical_tlist
	 * for them; worth adding?)
	 */
	if (rel->rtekind != RTE_RELATION)
		return false;

	/*
	 * Can't do it with inheritance cases either (mainly because Append
	 * doesn't project).
	 */
	if (rel->reloptkind != RELOPT_BASEREL)
		return false;

	/*
	 * Can't do it if any system columns are requested, either.  (This
	 * could possibly be fixed but would take some fragile assumptions in
	 * setrefs.c, I think.)
	 */
	for (i = rel->min_attr; i <= 0; i++)
	{
		if (!bms_is_empty(rel->attr_needed[i - rel->min_attr]))
			return false;
	}
	return true;
}

/*
 * disuse_physical_tlist
 *		Switch a plan node back to emitting only Vars actually referenced.
 *
 * If the plan node immediately above a scan would prefer to get only
 * needed Vars and not a physical tlist, it must call this routine to
 * undo the decision made by use_physical_tlist().	Currently, Hash, Sort,
 * and Material nodes want this, so they don't have to store useless columns.
 */
static void
disuse_physical_tlist(Plan *plan, Path *path)
{
	/* Only need to undo it for path types handled by create_scan_plan() */
	switch (path->pathtype)
	{
		case T_IndexScan:
		case T_SeqScan:
		case T_TidScan:
		case T_SubqueryScan:
		case T_FunctionScan:
			plan->targetlist = build_relation_tlist(path->parent);
			break;
		default:
			break;
	}
}

/*
 * create_join_plan
 *	  Create a join plan for 'best_path' and (recursively) plans for its
 *	  inner and outer paths.
 *
 *	  Returns a Plan node.
 */
static Join *
create_join_plan(Query *root, JoinPath *best_path)
{
	Plan	   *outer_plan;
	Plan	   *inner_plan;
	Join	   *plan;

	outer_plan = create_plan(root, best_path->outerjoinpath);
	inner_plan = create_plan(root, best_path->innerjoinpath);

	switch (best_path->path.pathtype)
	{
		case T_MergeJoin:
			plan = (Join *) create_mergejoin_plan(root,
												  (MergePath *) best_path,
												  outer_plan,
												  inner_plan);
			break;
		case T_HashJoin:
			plan = (Join *) create_hashjoin_plan(root,
												 (HashPath *) best_path,
												 outer_plan,
												 inner_plan);
			break;
		case T_NestLoop:
			plan = (Join *) create_nestloop_plan(root,
												 (NestPath *) best_path,
												 outer_plan,
												 inner_plan);
			break;
		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) best_path->path.pathtype);
			plan = NULL;		/* keep compiler quiet */
			break;
	}

#ifdef NOT_USED

	/*
	 * * Expensive function pullups may have pulled local predicates *
	 * into this path node.  Put them in the qpqual of the plan node. *
	 * JMH, 6/15/92
	 */
	if (get_loc_restrictinfo(best_path) != NIL)
		set_qpqual((Plan) plan,
				   nconc(get_qpqual((Plan) plan),
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
static Append *
create_append_plan(Query *root, AppendPath *best_path)
{
	Append	   *plan;
	List	   *tlist = build_relation_tlist(best_path->path.parent);
	List	   *subplans = NIL;
	List	   *subpaths;

	foreach(subpaths, best_path->subpaths)
	{
		Path	   *subpath = (Path *) lfirst(subpaths);

		subplans = lappend(subplans, create_plan(root, subpath));
	}

	plan = make_append(subplans, false, tlist);

	return plan;
}

/*
 * create_result_plan
 *	  Create a Result plan for 'best_path' and (recursively) plans
 *	  for its subpaths.
 *
 *	  Returns a Plan node.
 */
static Result *
create_result_plan(Query *root, ResultPath *best_path)
{
	Result	   *plan;
	List	   *tlist;
	List	   *constclauses;
	Plan	   *subplan;

	if (best_path->path.parent)
		tlist = build_relation_tlist(best_path->path.parent);
	else
		tlist = NIL;			/* will be filled in later */

	if (best_path->subpath)
		subplan = create_plan(root, best_path->subpath);
	else
		subplan = NULL;

	constclauses = order_qual_clauses(root, best_path->constantqual);

	plan = make_result(tlist, (Node *) constclauses, subplan);

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
create_material_plan(Query *root, MaterialPath *best_path)
{
	Material   *plan;
	Plan	   *subplan;

	subplan = create_plan(root, best_path->subpath);

	/* We don't want any excess columns in the materialized tuples */
	disuse_physical_tlist(subplan, best_path->subpath);

	plan = make_material(subplan->targetlist, subplan);

	copy_path_costsize(&plan->plan, (Path *) best_path);

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
create_unique_plan(Query *root, UniquePath *best_path)
{
	Plan	   *plan;
	Plan	   *subplan;
	List	   *uniq_exprs;
	int			numGroupCols;
	AttrNumber *groupColIdx;
	int			groupColPos;
	List	   *newtlist;
	int			nextresno;
	bool		newitems;
	List	   *my_tlist;
	List	   *l;

	subplan = create_plan(root, best_path->subpath);

	/*
	 * As constructed, the subplan has a "flat" tlist containing just the
	 * Vars needed here and at upper levels.  The values we are supposed
	 * to unique-ify may be expressions in these variables.  We have to
	 * add any such expressions to the subplan's tlist.  We then build
	 * control information showing which subplan output columns are to be
	 * examined by the grouping step.  (Since we do not remove any
	 * existing subplan outputs, not all the output columns may be used
	 * for grouping.)
	 *
	 * Note: the reason we don't remove any subplan outputs is that there are
	 * scenarios where a Var is needed at higher levels even though it is
	 * not one of the nominal outputs of an IN clause.	Consider WHERE x
	 * IN (SELECT y FROM t1,t2 WHERE y = z) Implied equality deduction
	 * will generate an "x = z" clause, which may get used instead of "x =
	 * y" in the upper join step.  Therefore the sub-select had better
	 * deliver both y and z in its targetlist.	It is sufficient to
	 * unique-ify on y, however.
	 *
	 * To find the correct list of values to unique-ify, we look in the
	 * information saved for IN expressions.  If this code is ever used in
	 * other scenarios, some other way of finding what to unique-ify will
	 * be needed.
	 */
	uniq_exprs = NIL;			/* just to keep compiler quiet */
	foreach(l, root->in_info_list)
	{
		InClauseInfo *ininfo = (InClauseInfo *) lfirst(l);

		if (bms_equal(ininfo->righthand, best_path->path.parent->relids))
		{
			uniq_exprs = ininfo->sub_targetlist;
			break;
		}
	}
	if (l == NIL)				/* fell out of loop? */
		elog(ERROR, "could not find UniquePath in in_info_list");

	/* set up to record positions of unique columns */
	numGroupCols = length(uniq_exprs);
	groupColIdx = (AttrNumber *) palloc(numGroupCols * sizeof(AttrNumber));
	groupColPos = 0;
	/* not sure if tlist might be shared with other nodes, so copy */
	newtlist = copyObject(subplan->targetlist);
	nextresno = length(newtlist) + 1;
	newitems = false;

	foreach(l, uniq_exprs)
	{
		Node	   *uniqexpr = lfirst(l);
		TargetEntry *tle;

		tle = tlistentry_member(uniqexpr, newtlist);
		if (!tle)
		{
			tle = makeTargetEntry(makeResdom(nextresno,
											 exprType(uniqexpr),
											 exprTypmod(uniqexpr),
											 NULL,
											 false),
								  (Expr *) uniqexpr);
			newtlist = lappend(newtlist, tle);
			nextresno++;
			newitems = true;
		}
		groupColIdx[groupColPos++] = tle->resdom->resno;
	}

	if (newitems)
	{
		/*
		 * If the top plan node can't do projections, we need to add a
		 * Result node to help it along.
		 *
		 * Currently, the only non-projection-capable plan type we can see
		 * here is Append.
		 */
		if (IsA(subplan, Append))
			subplan = (Plan *) make_result(newtlist, NULL, subplan);
		else
			subplan->targetlist = newtlist;
	}

	/* Copy tlist again to make one we can put sorting labels on */
	my_tlist = copyObject(subplan->targetlist);

	if (best_path->use_hash)
	{
		long		numGroups;

		numGroups = (long) Min(best_path->rows, (double) LONG_MAX);

		plan = (Plan *) make_agg(root,
								 my_tlist,
								 NIL,
								 AGG_HASHED,
								 numGroupCols,
								 groupColIdx,
								 numGroups,
								 0,
								 subplan);
	}
	else
	{
		List	   *sortList = NIL;

		for (groupColPos = 0; groupColPos < numGroupCols; groupColPos++)
		{
			TargetEntry *tle;

			tle = get_tle_by_resno(my_tlist, groupColIdx[groupColPos]);
			Assert(tle != NULL);
			sortList = addTargetToSortList(NULL, tle,
										   sortList, my_tlist,
										   SORTBY_ASC, NIL, false);
		}
		plan = (Plan *) make_sort_from_sortclauses(root, my_tlist,
												   subplan, sortList);
		plan = (Plan *) make_unique(my_tlist, plan, sortList);
	}

	plan->plan_rows = best_path->rows;

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
create_seqscan_plan(Path *best_path, List *tlist, List *scan_clauses)
{
	SeqScan    *scan_plan;
	Index		scan_relid = best_path->parent->relid;

	/* it should be a base rel... */
	Assert(scan_relid > 0);
	Assert(best_path->parent->rtekind == RTE_RELATION);

	scan_plan = make_seqscan(tlist,
							 scan_clauses,
							 scan_relid);

	copy_path_costsize(&scan_plan->plan, best_path);

	return scan_plan;
}

/*
 * create_indexscan_plan
 *	  Returns a indexscan plan for the base relation scanned by 'best_path'
 *	  with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 *
 * The indexqual of the path contains a sublist of implicitly-ANDed qual
 * conditions for each scan of the index(es); if there is more than one
 * scan then the retrieved tuple sets are ORed together.  The indexqual
 * and indexinfo lists must have the same length, ie, the number of scans
 * that will occur.  Note it is possible for a qual condition sublist
 * to be empty --- then no index restrictions will be applied during that
 * scan.
 */
static IndexScan *
create_indexscan_plan(Query *root,
					  IndexPath *best_path,
					  List *tlist,
					  List *scan_clauses)
{
	List	   *indxqual = best_path->indexqual;
	Index		baserelid = best_path->path.parent->relid;
	List	   *qpqual;
	Expr	   *indxqual_or_expr = NULL;
	List	   *fixed_indxqual;
	List	   *recheck_indxqual;
	FastList	indexids;
	List	   *ixinfo;
	IndexScan  *scan_plan;

	/* it should be a base rel... */
	Assert(baserelid > 0);
	Assert(best_path->path.parent->rtekind == RTE_RELATION);

	/*
	 * Build list of index OIDs.
	 */
	FastListInit(&indexids);
	foreach(ixinfo, best_path->indexinfo)
	{
		IndexOptInfo *index = (IndexOptInfo *) lfirst(ixinfo);

		FastAppendo(&indexids, index->indexoid);
	}

	/*
	 * The qpqual list must contain all restrictions not automatically
	 * handled by the index.  Normally the predicates in the indxqual are
	 * checked fully by the index, but if the index is "lossy" for a
	 * particular operator (as signaled by the amopreqcheck flag in
	 * pg_amop), then we need to double-check that predicate in qpqual,
	 * because the index may return more tuples than match the predicate.
	 *
	 * Since the indexquals were generated from the restriction clauses given
	 * by scan_clauses, there will normally be some duplications between
	 * the lists.  We get rid of the duplicates, then add back if lossy.
	 */
	if (length(indxqual) > 1)
	{
		/*
		 * Build an expression representation of the indexqual, expanding
		 * the implicit OR and AND semantics of the first- and
		 * second-level lists.
		 */
		FastList	orclauses;
		List	   *orclause;

		FastListInit(&orclauses);
		foreach(orclause, indxqual)
			FastAppend(&orclauses, make_ands_explicit(lfirst(orclause)));
		indxqual_or_expr = make_orclause(FastListValue(&orclauses));

		qpqual = set_difference(scan_clauses, makeList1(indxqual_or_expr));
	}
	else if (indxqual != NIL)
	{
		/*
		 * Here, we can simply treat the first sublist as an independent
		 * set of qual expressions, since there is no top-level OR
		 * behavior.
		 */
		qpqual = set_difference(scan_clauses, lfirst(indxqual));
	}
	else
		qpqual = scan_clauses;

	/*
	 * The executor needs a copy with the indexkey on the left of each
	 * clause and with index attr numbers substituted for table ones. This
	 * pass also looks for "lossy" operators.
	 */
	fix_indxqual_references(indxqual, best_path,
							&fixed_indxqual, &recheck_indxqual);

	/*
	 * If there were any "lossy" operators, need to add back the
	 * appropriate qual clauses to the qpqual.	When there is just one
	 * indexscan being performed (ie, we have simple AND semantics), we
	 * can just add the lossy clauses themselves to qpqual.  If we have
	 * OR-of-ANDs, we'd better add the entire original indexqual to make
	 * sure that the semantics are correct.
	 */
	if (recheck_indxqual != NIL)
	{
		if (indxqual_or_expr)
		{
			/* Better do a deep copy of the original scanclauses */
			qpqual = lappend(qpqual, copyObject(indxqual_or_expr));
		}
		else
		{
			/* Subroutine already copied quals, so just append to list */
			Assert(length(recheck_indxqual) == 1);
			qpqual = nconc(qpqual, (List *) lfirst(recheck_indxqual));
		}
	}

	/* Finally ready to build the plan node */
	scan_plan = make_indexscan(tlist,
							   qpqual,
							   baserelid,
							   FastListValue(&indexids),
							   fixed_indxqual,
							   indxqual,
							   best_path->indexscandir);

	copy_path_costsize(&scan_plan->scan.plan, &best_path->path);
	/* use the indexscan-specific rows estimate, not the parent rel's */
	scan_plan->scan.plan.plan_rows = best_path->rows;

	return scan_plan;
}

/*
 * create_tidscan_plan
 *	 Returns a tidscan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static TidScan *
create_tidscan_plan(TidPath *best_path, List *tlist, List *scan_clauses)
{
	TidScan    *scan_plan;
	Index		scan_relid = best_path->path.parent->relid;

	/* it should be a base rel... */
	Assert(scan_relid > 0);
	Assert(best_path->path.parent->rtekind == RTE_RELATION);

	scan_plan = make_tidscan(tlist,
							 scan_clauses,
							 scan_relid,
							 best_path->tideval);

	copy_path_costsize(&scan_plan->scan.plan, &best_path->path);

	return scan_plan;
}

/*
 * create_subqueryscan_plan
 *	 Returns a subqueryscan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static SubqueryScan *
create_subqueryscan_plan(Path *best_path, List *tlist, List *scan_clauses)
{
	SubqueryScan *scan_plan;
	Index		scan_relid = best_path->parent->relid;

	/* it should be a subquery base rel... */
	Assert(scan_relid > 0);
	Assert(best_path->parent->rtekind == RTE_SUBQUERY);

	scan_plan = make_subqueryscan(tlist,
								  scan_clauses,
								  scan_relid,
								  best_path->parent->subplan);

	copy_path_costsize(&scan_plan->scan.plan, best_path);

	return scan_plan;
}

/*
 * create_functionscan_plan
 *	 Returns a functionscan plan for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static FunctionScan *
create_functionscan_plan(Path *best_path, List *tlist, List *scan_clauses)
{
	FunctionScan *scan_plan;
	Index		scan_relid = best_path->parent->relid;

	/* it should be a function base rel... */
	Assert(scan_relid > 0);
	Assert(best_path->parent->rtekind == RTE_FUNCTION);

	scan_plan = make_functionscan(tlist, scan_clauses, scan_relid);

	copy_path_costsize(&scan_plan->scan.plan, best_path);

	return scan_plan;
}

/*****************************************************************************
 *
 *	JOIN METHODS
 *
 *****************************************************************************/

static NestLoop *
create_nestloop_plan(Query *root,
					 NestPath *best_path,
					 Plan *outer_plan,
					 Plan *inner_plan)
{
	List	   *tlist = build_relation_tlist(best_path->path.parent);
	List	   *joinrestrictclauses = best_path->joinrestrictinfo;
	List	   *joinclauses;
	List	   *otherclauses;
	NestLoop   *join_plan;

	if (IsA(best_path->innerjoinpath, IndexPath))
	{
		/*
		 * An index is being used to reduce the number of tuples scanned
		 * in the inner relation.  If there are join clauses being used
		 * with the index, we may remove those join clauses from the list
		 * of clauses that have to be checked as qpquals at the join node
		 * --- but only if there's just one indexscan in the inner path
		 * (otherwise, several different sets of clauses are being ORed
		 * together).
		 *
		 * We can also remove any join clauses that are redundant with those
		 * being used in the index scan; prior redundancy checks will not
		 * have caught this case because the join clauses would never have
		 * been put in the same joininfo list.
		 *
		 * This would be a waste of time if the indexpath was an ordinary
		 * indexpath and not a special innerjoin path.	We will skip it in
		 * that case since indexjoinclauses is NIL in an ordinary
		 * indexpath.
		 */
		IndexPath  *innerpath = (IndexPath *) best_path->innerjoinpath;
		List	   *indexjoinclauses = innerpath->indexjoinclauses;

		if (length(indexjoinclauses) == 1)		/* single indexscan? */
		{
			joinrestrictclauses =
				select_nonredundant_join_clauses(root,
												 joinrestrictclauses,
												 lfirst(indexjoinclauses),
												 best_path->jointype);
		}
	}

	/* Get the join qual clauses (in plain expression form) */
	if (IS_OUTER_JOIN(best_path->jointype))
	{
		get_actual_join_clauses(joinrestrictclauses,
								&joinclauses, &otherclauses);
	}
	else
	{
		/* We can treat all clauses alike for an inner join */
		joinclauses = get_actual_clauses(joinrestrictclauses);
		otherclauses = NIL;
	}

	/* Sort clauses into best execution order */
	joinclauses = order_qual_clauses(root, joinclauses);
	otherclauses = order_qual_clauses(root, otherclauses);

	join_plan = make_nestloop(tlist,
							  joinclauses,
							  otherclauses,
							  outer_plan,
							  inner_plan,
							  best_path->jointype);

	copy_path_costsize(&join_plan->join.plan, &best_path->path);

	return join_plan;
}

static MergeJoin *
create_mergejoin_plan(Query *root,
					  MergePath *best_path,
					  Plan *outer_plan,
					  Plan *inner_plan)
{
	List	   *tlist = build_relation_tlist(best_path->jpath.path.parent);
	List	   *joinclauses;
	List	   *otherclauses;
	List	   *mergeclauses;
	MergeJoin  *join_plan;

	/* Get the join qual clauses (in plain expression form) */
	if (IS_OUTER_JOIN(best_path->jpath.jointype))
	{
		get_actual_join_clauses(best_path->jpath.joinrestrictinfo,
								&joinclauses, &otherclauses);
	}
	else
	{
		/* We can treat all clauses alike for an inner join */
		joinclauses = get_actual_clauses(best_path->jpath.joinrestrictinfo);
		otherclauses = NIL;
	}

	/*
	 * Remove the mergeclauses from the list of join qual clauses, leaving
	 * the list of quals that must be checked as qpquals.
	 */
	mergeclauses = get_actual_clauses(best_path->path_mergeclauses);
	joinclauses = set_difference(joinclauses, mergeclauses);

	/*
	 * Rearrange mergeclauses, if needed, so that the outer variable is
	 * always on the left.
	 */
	mergeclauses = get_switched_clauses(best_path->path_mergeclauses,
						 best_path->jpath.outerjoinpath->parent->relids);

	/* Sort clauses into best execution order */
	/* NB: do NOT reorder the mergeclauses */
	joinclauses = order_qual_clauses(root, joinclauses);
	otherclauses = order_qual_clauses(root, otherclauses);

	/*
	 * Create explicit sort nodes for the outer and inner join paths if
	 * necessary.  The sort cost was already accounted for in the path.
	 * Make sure there are no excess columns in the inputs if sorting.
	 */
	if (best_path->outersortkeys)
	{
		disuse_physical_tlist(outer_plan, best_path->jpath.outerjoinpath);
		outer_plan = (Plan *)
			make_sort_from_pathkeys(root,
									outer_plan,
									best_path->outersortkeys);
	}

	if (best_path->innersortkeys)
	{
		disuse_physical_tlist(inner_plan, best_path->jpath.innerjoinpath);
		inner_plan = (Plan *)
			make_sort_from_pathkeys(root,
									inner_plan,
									best_path->innersortkeys);
	}

	/*
	 * Now we can build the mergejoin node.
	 */
	join_plan = make_mergejoin(tlist,
							   joinclauses,
							   otherclauses,
							   mergeclauses,
							   outer_plan,
							   inner_plan,
							   best_path->jpath.jointype);

	copy_path_costsize(&join_plan->join.plan, &best_path->jpath.path);

	return join_plan;
}

static HashJoin *
create_hashjoin_plan(Query *root,
					 HashPath *best_path,
					 Plan *outer_plan,
					 Plan *inner_plan)
{
	List	   *tlist = build_relation_tlist(best_path->jpath.path.parent);
	List	   *joinclauses;
	List	   *otherclauses;
	List	   *hashclauses;
	HashJoin   *join_plan;
	Hash	   *hash_plan;
	List	   *innerhashkeys;
	List	   *hcl;

	/* Get the join qual clauses (in plain expression form) */
	if (IS_OUTER_JOIN(best_path->jpath.jointype))
	{
		get_actual_join_clauses(best_path->jpath.joinrestrictinfo,
								&joinclauses, &otherclauses);
	}
	else
	{
		/* We can treat all clauses alike for an inner join */
		joinclauses = get_actual_clauses(best_path->jpath.joinrestrictinfo);
		otherclauses = NIL;
	}

	/*
	 * Remove the hashclauses from the list of join qual clauses, leaving
	 * the list of quals that must be checked as qpquals.
	 */
	hashclauses = get_actual_clauses(best_path->path_hashclauses);
	joinclauses = set_difference(joinclauses, hashclauses);

	/*
	 * Rearrange hashclauses, if needed, so that the outer variable is
	 * always on the left.
	 */
	hashclauses = get_switched_clauses(best_path->path_hashclauses,
						 best_path->jpath.outerjoinpath->parent->relids);

	/* Sort clauses into best execution order */
	joinclauses = order_qual_clauses(root, joinclauses);
	otherclauses = order_qual_clauses(root, otherclauses);
	hashclauses = order_qual_clauses(root, hashclauses);

	/*
	 * Extract the inner hash keys (right-hand operands of the hashclauses)
	 * to put in the Hash node.  Must do a deep copy in case there are
	 * subplans in the hash keys.
	 */
	innerhashkeys = NIL;
	foreach(hcl, hashclauses)
		innerhashkeys = lappend(innerhashkeys,
								copyObject(get_rightop(lfirst(hcl))));

	/* We don't want any excess columns in the hashed tuples */
	disuse_physical_tlist(inner_plan, best_path->jpath.innerjoinpath);

	/*
	 * Build the hash node and hash join node.
	 */
	hash_plan = make_hash(inner_plan->targetlist,
						  innerhashkeys,
						  inner_plan);
	join_plan = make_hashjoin(tlist,
							  joinclauses,
							  otherclauses,
							  hashclauses,
							  outer_plan,
							  (Plan *) hash_plan,
							  best_path->jpath.jointype);

	copy_path_costsize(&join_plan->join.plan, &best_path->jpath.path);

	return join_plan;
}


/*****************************************************************************
 *
 *	SUPPORTING ROUTINES
 *
 *****************************************************************************/

/*
 * fix_indxqual_references
 *	  Adjust indexqual clauses to the form the executor's indexqual
 *	  machinery needs, and check for recheckable (lossy) index conditions.
 *
 * We have three tasks here:
 *	* Index keys must be represented by Var nodes with varattno set to the
 *	  index's attribute number, not the attribute number in the original rel.
 *	* If the index key is on the right, commute the clause to put it on the
 *	  left.  (Someday the executor might not need this, but for now it does.)
 *	* If the indexable operator is marked 'amopreqcheck' in pg_amop, then
 *	  the index is "lossy" for this operator: it may return more tuples than
 *	  actually satisfy the operator condition.	For each such operator, we
 *	  must add (the original form of) the indexqual clause to the "qpquals"
 *	  of the indexscan node, where the operator will be re-evaluated to
 *	  ensure it passes.
 *
 * This code used to be entirely bogus for multi-index scans.  Now it keeps
 * track of which index applies to each subgroup of index qual clauses...
 *
 * Both the input list and the output lists have the form of lists of sublists
 * of qual clauses --- the top-level list has one entry for each indexscan
 * to be performed.  The semantics are OR-of-ANDs.
 *
 * fixed_indexquals receives a modified copy of the indexqual list --- the
 * original is not changed.  Note also that the copy shares no substructure
 * with the original; this is needed in case there is a subplan in it (we need
 * two separate copies of the subplan tree, or things will go awry).
 *
 * recheck_indexquals similarly receives a full copy of whichever clauses
 * need rechecking.
 */
static void
fix_indxqual_references(List *indexquals, IndexPath *index_path,
					  List **fixed_indexquals, List **recheck_indexquals)
{
	FastList	fixed_quals;
	FastList	recheck_quals;
	Relids		baserelids = index_path->path.parent->relids;
	int			baserelid = index_path->path.parent->relid;
	List	   *ixinfo = index_path->indexinfo;
	List	   *i;

	FastListInit(&fixed_quals);
	FastListInit(&recheck_quals);
	foreach(i, indexquals)
	{
		List	   *indexqual = lfirst(i);
		IndexOptInfo *index = (IndexOptInfo *) lfirst(ixinfo);
		List	   *fixed_qual;
		List	   *recheck_qual;

		fix_indxqual_sublist(indexqual, baserelids, baserelid, index,
							 &fixed_qual, &recheck_qual);
		FastAppend(&fixed_quals, fixed_qual);
		if (recheck_qual != NIL)
			FastAppend(&recheck_quals, recheck_qual);

		ixinfo = lnext(ixinfo);
	}

	*fixed_indexquals = FastListValue(&fixed_quals);
	*recheck_indexquals = FastListValue(&recheck_quals);
}

/*
 * Fix the sublist of indexquals to be used in a particular scan.
 *
 * For each qual clause, commute if needed to put the indexkey operand on the
 * left, and then fix its varattno.  (We do not need to change the other side
 * of the clause.)	Also change the operator if necessary, and check for
 * lossy index behavior.
 *
 * Returns two lists: the list of fixed indexquals, and the list (usually
 * empty) of original clauses that must be rechecked as qpquals because
 * the index is lossy for this operator type.
 */
static void
fix_indxqual_sublist(List *indexqual,
					 Relids baserelids, int baserelid,
					 IndexOptInfo *index,
					 List **fixed_quals, List **recheck_quals)
{
	FastList	fixed_qual;
	FastList	recheck_qual;
	List	   *i;

	FastListInit(&fixed_qual);
	FastListInit(&recheck_qual);
	foreach(i, indexqual)
	{
		OpExpr	   *clause = (OpExpr *) lfirst(i);
		OpExpr	   *newclause;
		Relids		leftvarnos;
		Oid			opclass;

		if (!IsA(clause, OpExpr) ||length(clause->args) != 2)
			elog(ERROR, "indexqual clause is not binary opclause");

		/*
		 * Make a copy that will become the fixed clause.
		 *
		 * We used to try to do a shallow copy here, but that fails if there
		 * is a subplan in the arguments of the opclause.  So just do a
		 * full copy.
		 */
		newclause = (OpExpr *) copyObject((Node *) clause);

		/*
		 * Check to see if the indexkey is on the right; if so, commute
		 * the clause.	The indexkey should be the side that refers to
		 * (only) the base relation.
		 */
		leftvarnos = pull_varnos((Node *) lfirst(newclause->args));
		if (!bms_equal(leftvarnos, baserelids))
			CommuteClause(newclause);
		bms_free(leftvarnos);

		/*
		 * Now, determine which index attribute this is, change the
		 * indexkey operand as needed, and get the index opclass.
		 */
		lfirst(newclause->args) = fix_indxqual_operand(lfirst(newclause->args),
													   baserelid,
													   index,
													   &opclass);

		FastAppend(&fixed_qual, newclause);

		/*
		 * Finally, check to see if index is lossy for this operator. If
		 * so, add (a copy of) original form of clause to recheck list.
		 */
		if (op_requires_recheck(newclause->opno, opclass))
			FastAppend(&recheck_qual, copyObject((Node *) clause));
	}

	*fixed_quals = FastListValue(&fixed_qual);
	*recheck_quals = FastListValue(&recheck_qual);
}

static Node *
fix_indxqual_operand(Node *node, int baserelid, IndexOptInfo *index,
					 Oid *opclass)
{
	/*
	 * We represent index keys by Var nodes having the varno of the base
	 * table but varattno equal to the index's attribute number (index
	 * column position).  This is a bit hokey ... would be cleaner to use
	 * a special-purpose node type that could not be mistaken for a
	 * regular Var.  But it will do for now.
	 */
	Var		   *result;
	int			pos;
	List	   *indexprs;

	/*
	 * Remove any binary-compatible relabeling of the indexkey
	 */
	if (IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;

	if (IsA(node, Var) &&
		((Var *) node)->varno == baserelid)
	{
		/* Try to match against simple index columns */
		int			varatt = ((Var *) node)->varattno;

		if (varatt != 0)
		{
			for (pos = 0; pos < index->ncolumns; pos++)
			{
				if (index->indexkeys[pos] == varatt)
				{
					result = (Var *) copyObject(node);
					result->varattno = pos + 1;
					/* return the correct opclass, too */
					*opclass = index->classlist[pos];
					return (Node *) result;
				}
			}
		}
	}

	/* Try to match against index expressions */
	indexprs = index->indexprs;
	for (pos = 0; pos < index->ncolumns; pos++)
	{
		if (index->indexkeys[pos] == 0)
		{
			Node	   *indexkey;

			if (indexprs == NIL)
				elog(ERROR, "too few entries in indexprs list");
			indexkey = (Node *) lfirst(indexprs);
			if (indexkey && IsA(indexkey, RelabelType))
				indexkey = (Node *) ((RelabelType *) indexkey)->arg;
			if (equal(node, indexkey))
			{
				/* Found a match */
				result = makeVar(baserelid, pos + 1,
								 exprType(lfirst(indexprs)), -1,
								 0);
				/* return the correct opclass, too */
				*opclass = index->classlist[pos];
				return (Node *) result;
			}
			indexprs = lnext(indexprs);
		}
	}

	/* Ooops... */
	elog(ERROR, "node is not an index attribute");
	return NULL;				/* keep compiler quiet */
}

/*
 * get_switched_clauses
 *	  Given a list of merge or hash joinclauses (as RestrictInfo nodes),
 *	  extract the bare clauses, and rearrange the elements within the
 *	  clauses, if needed, so the outer join variable is on the left and
 *	  the inner is on the right.  The original data structure is not touched;
 *	  a modified list is returned.
 */
static List *
get_switched_clauses(List *clauses, Relids outerrelids)
{
	List	   *t_list = NIL;
	List	   *i;

	foreach(i, clauses)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(i);
		OpExpr	   *clause = (OpExpr *) restrictinfo->clause;

		Assert(is_opclause(clause));
		if (bms_is_subset(restrictinfo->right_relids, outerrelids))
		{
			/*
			 * Duplicate just enough of the structure to allow commuting
			 * the clause without changing the original list.  Could use
			 * copyObject, but a complete deep copy is overkill.
			 */
			OpExpr	   *temp = makeNode(OpExpr);

			temp->opno = clause->opno;
			temp->opfuncid = InvalidOid;
			temp->opresulttype = clause->opresulttype;
			temp->opretset = clause->opretset;
			temp->args = listCopy(clause->args);
			/* Commute it --- note this modifies the temp node in-place. */
			CommuteClause(temp);
			t_list = lappend(t_list, temp);
		}
		else
			t_list = lappend(t_list, clause);
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
 * selectivity, but unfortunately we have so little information about
 * execution cost of operators that it's really hard to do anything smart.
 * For now, we just move any quals that contain SubPlan references (but not
 * InitPlan references) to the end of the list.
 */
static List *
order_qual_clauses(Query *root, List *clauses)
{
	FastList	nosubplans;
	FastList	withsubplans;
	List	   *l;

	/* No need to work hard if the query is subselect-free */
	if (!root->hasSubLinks)
		return clauses;

	FastListInit(&nosubplans);
	FastListInit(&withsubplans);
	foreach(l, clauses)
	{
		Node	   *clause = lfirst(l);

		if (contain_subplans(clause))
			FastAppend(&withsubplans, clause);
		else
			FastAppend(&nosubplans, clause);
	}

	FastConcFast(&nosubplans, &withsubplans);
	return FastListValue(&nosubplans);
}

/*
 * Copy cost and size info from a Path node to the Plan node created from it.
 * The executor won't use this info, but it's needed by EXPLAIN.
 */
static void
copy_path_costsize(Plan *dest, Path *src)
{
	if (src)
	{
		dest->startup_cost = src->startup_cost;
		dest->total_cost = src->total_cost;
		dest->plan_rows = src->parent->rows;
		dest->plan_width = src->parent->width;
	}
	else
	{
		dest->startup_cost = 0;
		dest->total_cost = 0;
		dest->plan_rows = 0;
		dest->plan_width = 0;
	}
}

/*
 * Copy cost and size info from a lower plan node to an inserted node.
 * This is not critical, since the decisions have already been made,
 * but it helps produce more reasonable-looking EXPLAIN output.
 * (Some callers alter the info after copying it.)
 */
static void
copy_plan_costsize(Plan *dest, Plan *src)
{
	if (src)
	{
		dest->startup_cost = src->startup_cost;
		dest->total_cost = src->total_cost;
		dest->plan_rows = src->plan_rows;
		dest->plan_width = src->plan_width;
	}
	else
	{
		dest->startup_cost = 0;
		dest->total_cost = 0;
		dest->plan_rows = 0;
		dest->plan_width = 0;
	}
}


/*****************************************************************************
 *
 *	PLAN NODE BUILDING ROUTINES
 *
 * Some of these are exported because they are called to build plan nodes
 * in contexts where we're not deriving the plan node from a path node.
 *
 *****************************************************************************/

static SeqScan *
make_seqscan(List *qptlist,
			 List *qpqual,
			 Index scanrelid)
{
	SeqScan    *node = makeNode(SeqScan);
	Plan	   *plan = &node->plan;

	/* cost should be inserted by caller */
	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scanrelid = scanrelid;

	return node;
}

static IndexScan *
make_indexscan(List *qptlist,
			   List *qpqual,
			   Index scanrelid,
			   List *indxid,
			   List *indxqual,
			   List *indxqualorig,
			   ScanDirection indexscandir)
{
	IndexScan  *node = makeNode(IndexScan);
	Plan	   *plan = &node->scan.plan;

	/* cost should be inserted by caller */
	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->indxid = indxid;
	node->indxqual = indxqual;
	node->indxqualorig = indxqualorig;
	node->indxorderdir = indexscandir;

	return node;
}

static TidScan *
make_tidscan(List *qptlist,
			 List *qpqual,
			 Index scanrelid,
			 List *tideval)
{
	TidScan    *node = makeNode(TidScan);
	Plan	   *plan = &node->scan.plan;

	/* cost should be inserted by caller */
	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->tideval = tideval;

	return node;
}

SubqueryScan *
make_subqueryscan(List *qptlist,
				  List *qpqual,
				  Index scanrelid,
				  Plan *subplan)
{
	SubqueryScan *node = makeNode(SubqueryScan);
	Plan	   *plan = &node->scan.plan;

	/*
	 * Cost is figured here for the convenience of prepunion.c.  Note this
	 * is only correct for the case where qpqual is empty; otherwise
	 * caller should overwrite cost with a better estimate.
	 */
	copy_plan_costsize(plan, subplan);
	plan->total_cost += cpu_tuple_cost * subplan->plan_rows;

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
				  Index scanrelid)
{
	FunctionScan *node = makeNode(FunctionScan);
	Plan	   *plan = &node->scan.plan;

	/* cost should be inserted by caller */
	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;

	return node;
}

Append *
make_append(List *appendplans, bool isTarget, List *tlist)
{
	Append	   *node = makeNode(Append);
	Plan	   *plan = &node->plan;
	List	   *subnode;

	/*
	 * Compute cost as sum of subplan costs.  We charge nothing extra for
	 * the Append itself, which perhaps is too optimistic, but since it
	 * doesn't do any selection or projection, it is a pretty cheap node.
	 */
	plan->startup_cost = 0;
	plan->total_cost = 0;
	plan->plan_rows = 0;
	plan->plan_width = 0;
	foreach(subnode, appendplans)
	{
		Plan	   *subplan = (Plan *) lfirst(subnode);

		if (subnode == appendplans)		/* first node? */
			plan->startup_cost = subplan->startup_cost;
		plan->total_cost += subplan->total_cost;
		plan->plan_rows += subplan->plan_rows;
		if (plan->plan_width < subplan->plan_width)
			plan->plan_width = subplan->plan_width;
	}

	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->appendplans = appendplans;
	node->isTarget = isTarget;

	return node;
}

static NestLoop *
make_nestloop(List *tlist,
			  List *joinclauses,
			  List *otherclauses,
			  Plan *lefttree,
			  Plan *righttree,
			  JoinType jointype)
{
	NestLoop   *node = makeNode(NestLoop);
	Plan	   *plan = &node->join.plan;

	/* cost should be inserted by caller */
	plan->targetlist = tlist;
	plan->qual = otherclauses;
	plan->lefttree = lefttree;
	plan->righttree = righttree;
	node->join.jointype = jointype;
	node->join.joinqual = joinclauses;

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

	/* cost should be inserted by caller */
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
make_hash(List *tlist, List *hashkeys, Plan *lefttree)
{
	Hash	   *node = makeNode(Hash);
	Plan	   *plan = &node->plan;

	copy_plan_costsize(plan, lefttree);

	/*
	 * For plausibility, make startup & total costs equal total cost of
	 * input plan; this only affects EXPLAIN display not decisions.
	 */
	plan->startup_cost = plan->total_cost;
	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;
	node->hashkeys = hashkeys;

	return node;
}

static MergeJoin *
make_mergejoin(List *tlist,
			   List *joinclauses,
			   List *otherclauses,
			   List *mergeclauses,
			   Plan *lefttree,
			   Plan *righttree,
			   JoinType jointype)
{
	MergeJoin  *node = makeNode(MergeJoin);
	Plan	   *plan = &node->join.plan;

	/* cost should be inserted by caller */
	plan->targetlist = tlist;
	plan->qual = otherclauses;
	plan->lefttree = lefttree;
	plan->righttree = righttree;
	node->mergeclauses = mergeclauses;
	node->join.jointype = jointype;
	node->join.joinqual = joinclauses;

	return node;
}

/*
 * make_sort --- basic routine to build a Sort plan node
 *
 * Caller must have built the sortColIdx and sortOperators arrays already.
 */
static Sort *
make_sort(Query *root, List *tlist, Plan *lefttree, int numCols,
		  AttrNumber *sortColIdx, Oid *sortOperators)
{
	Sort	   *node = makeNode(Sort);
	Plan	   *plan = &node->plan;
	Path		sort_path;		/* dummy for result of cost_sort */

	copy_plan_costsize(plan, lefttree); /* only care about copying size */
	cost_sort(&sort_path, root, NIL,
			  lefttree->total_cost,
			  lefttree->plan_rows,
			  lefttree->plan_width);
	plan->startup_cost = sort_path.startup_cost;
	plan->total_cost = sort_path.total_cost;
	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;
	node->numCols = numCols;
	node->sortColIdx = sortColIdx;
	node->sortOperators = sortOperators;

	return node;
}

/*
 * add_sort_column --- utility subroutine for building sort info arrays
 *
 * We need this routine because the same column might be selected more than
 * once as a sort key column; if so, the extra mentions are redundant.
 *
 * Caller is assumed to have allocated the arrays large enough for the
 * max possible number of columns.	Return value is the new column count.
 */
static int
add_sort_column(AttrNumber colIdx, Oid sortOp,
				int numCols, AttrNumber *sortColIdx, Oid *sortOperators)
{
	int			i;

	for (i = 0; i < numCols; i++)
	{
		if (sortColIdx[i] == colIdx)
		{
			/* Already sorting by this col, so extra sort key is useless */
			return numCols;
		}
	}

	/* Add the column */
	sortColIdx[numCols] = colIdx;
	sortOperators[numCols] = sortOp;
	return numCols + 1;
}

/*
 * make_sort_from_pathkeys
 *	  Create sort plan to sort according to given pathkeys
 *
 *	  'lefttree' is the node which yields input tuples
 *	  'pathkeys' is the list of pathkeys by which the result is to be sorted
 *
 * We must convert the pathkey information into arrays of sort key column
 * numbers and sort operator OIDs.
 *
 * If the pathkeys include expressions that aren't simple Vars, we will
 * usually need to add resjunk items to the input plan's targetlist to
 * compute these expressions (since the Sort node itself won't do it).
 * If the input plan type isn't one that can do projections, this means
 * adding a Result node just to do the projection.
 */
static Sort *
make_sort_from_pathkeys(Query *root, Plan *lefttree, List *pathkeys)
{
	List	   *tlist = lefttree->targetlist;
	List	   *sort_tlist;
	List	   *i;
	int			numsortkeys;
	AttrNumber *sortColIdx;
	Oid		   *sortOperators;

	/* We will need at most length(pathkeys) sort columns; possibly less */
	numsortkeys = length(pathkeys);
	sortColIdx = (AttrNumber *) palloc(numsortkeys * sizeof(AttrNumber));
	sortOperators = (Oid *) palloc(numsortkeys * sizeof(Oid));

	numsortkeys = 0;

	foreach(i, pathkeys)
	{
		List	   *keysublist = (List *) lfirst(i);
		PathKeyItem *pathkey = NULL;
		Resdom	   *resdom = NULL;
		List	   *j;

		/*
		 * We can sort by any one of the sort key items listed in this
		 * sublist.  For now, we take the first one that corresponds to an
		 * available Var in the tlist.	If there isn't any, use the first
		 * one that is an expression in the input's vars.
		 *
		 * XXX if we have a choice, is there any way of figuring out which
		 * might be cheapest to execute?  (For example, int4lt is likely
		 * much cheaper to execute than numericlt, but both might appear
		 * in the same pathkey sublist...)	Not clear that we ever will
		 * have a choice in practice, so it may not matter.
		 */
		foreach(j, keysublist)
		{
			pathkey = lfirst(j);
			Assert(IsA(pathkey, PathKeyItem));
			resdom = tlist_member(pathkey->key, tlist);
			if (resdom)
				break;
		}
		if (!resdom)
		{
			/* No matching Var; look for a computable expression */
			foreach(j, keysublist)
			{
				List   *exprvars;
				List   *k;

				pathkey = lfirst(j);
				exprvars = pull_var_clause(pathkey->key, false);
				foreach(k, exprvars)
				{
					if (!tlist_member(lfirst(k), tlist))
						break;
				}
				freeList(exprvars);
				if (!k)
					break;		/* found usable expression */
			}
			if (!j)
				elog(ERROR, "could not find pathkey item to sort");

			/*
			 * Do we need to insert a Result node?
			 *
			 * Currently, the only non-projection-capable plan types we can
			 * see here are Append and Unique.
			 */
			if (IsA(lefttree, Append) || IsA(lefttree, Unique))
			{
				tlist = copyObject(tlist);
				lefttree = (Plan *) make_result(tlist, NULL, lefttree);
			}

			/*
			 * Add resjunk entry to input's tlist
			 */
			resdom = makeResdom(length(tlist) + 1,
								exprType(pathkey->key),
								exprTypmod(pathkey->key),
								NULL,
								true);
			tlist = lappend(tlist,
							makeTargetEntry(resdom,
											(Expr *) pathkey->key));
			lefttree->targetlist = tlist;		/* just in case NIL before */
		}

		/*
		 * The column might already be selected as a sort key, if the
		 * pathkeys contain duplicate entries.	(This can happen in
		 * scenarios where multiple mergejoinable clauses mention the same
		 * var, for example.)  So enter it only once in the sort arrays.
		 */
		numsortkeys = add_sort_column(resdom->resno, pathkey->sortop,
								 numsortkeys, sortColIdx, sortOperators);
	}

	Assert(numsortkeys > 0);

	/* Give Sort node its own copy of the tlist (still necessary?) */
	sort_tlist = copyObject(tlist);

	return make_sort(root, sort_tlist, lefttree, numsortkeys,
					 sortColIdx, sortOperators);
}

/*
 * make_sort_from_sortclauses
 *	  Create sort plan to sort according to given sortclauses
 *
 *	  'tlist' is the targetlist
 *	  'lefttree' is the node which yields input tuples
 *	  'sortcls' is a list of SortClauses
 */
Sort *
make_sort_from_sortclauses(Query *root, List *tlist,
						   Plan *lefttree, List *sortcls)
{
	List	   *sort_tlist;
	List	   *i;
	int			numsortkeys;
	AttrNumber *sortColIdx;
	Oid		   *sortOperators;

	/* We will need at most length(sortcls) sort columns; possibly less */
	numsortkeys = length(sortcls);
	sortColIdx = (AttrNumber *) palloc(numsortkeys * sizeof(AttrNumber));
	sortOperators = (Oid *) palloc(numsortkeys * sizeof(Oid));

	numsortkeys = 0;

	foreach(i, sortcls)
	{
		SortClause *sortcl = (SortClause *) lfirst(i);
		TargetEntry *tle = get_sortgroupclause_tle(sortcl, tlist);
		Resdom	   *resdom = tle->resdom;

		/*
		 * Check for the possibility of duplicate order-by clauses --- the
		 * parser should have removed 'em, but no point in sorting
		 * redundantly.
		 */
		numsortkeys = add_sort_column(resdom->resno, sortcl->sortop,
								 numsortkeys, sortColIdx, sortOperators);
	}

	Assert(numsortkeys > 0);

	/* Give Sort node its own copy of the tlist (still necessary?) */
	sort_tlist = copyObject(tlist);

	return make_sort(root, sort_tlist, lefttree, numsortkeys,
					 sortColIdx, sortOperators);
}

/*
 * make_sort_from_groupcols
 *	  Create sort plan to sort based on grouping columns
 *
 * 'groupcls' is the list of GroupClauses
 * 'grpColIdx' gives the column numbers to use
 *
 * This might look like it could be merged with make_sort_from_sortclauses,
 * but presently we *must* use the grpColIdx[] array to locate sort columns,
 * because the child plan's tlist is not marked with ressortgroupref info
 * appropriate to the grouping node.  So, only the sortop is used from the
 * GroupClause entries.
 */
Sort *
make_sort_from_groupcols(Query *root,
						 List *groupcls,
						 AttrNumber *grpColIdx,
						 Plan *lefttree)
{
	List	   *sub_tlist = lefttree->targetlist;
	List	   *sort_tlist;
	int			grpno = 0;
	List	   *i;
	int			numsortkeys;
	AttrNumber *sortColIdx;
	Oid		   *sortOperators;

	/* We will need at most length(groupcls) sort columns; possibly less */
	numsortkeys = length(groupcls);
	sortColIdx = (AttrNumber *) palloc(numsortkeys * sizeof(AttrNumber));
	sortOperators = (Oid *) palloc(numsortkeys * sizeof(Oid));

	numsortkeys = 0;

	foreach(i, groupcls)
	{
		GroupClause *grpcl = (GroupClause *) lfirst(i);
		TargetEntry *tle = get_tle_by_resno(sub_tlist, grpColIdx[grpno]);
		Resdom	   *resdom = tle->resdom;

		/*
		 * Check for the possibility of duplicate group-by clauses --- the
		 * parser should have removed 'em, but no point in sorting
		 * redundantly.
		 */
		numsortkeys = add_sort_column(resdom->resno, grpcl->sortop,
								 numsortkeys, sortColIdx, sortOperators);
		grpno++;
	}

	Assert(numsortkeys > 0);

	/* Give Sort node its own copy of the tlist (still necessary?) */
	sort_tlist = copyObject(sub_tlist);

	return make_sort(root, sort_tlist, lefttree, numsortkeys,
					 sortColIdx, sortOperators);
}

Material *
make_material(List *tlist, Plan *lefttree)
{
	Material   *node = makeNode(Material);
	Plan	   *plan = &node->plan;

	/* cost should be inserted by caller */
	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	return node;
}

/*
 * materialize_finished_plan: stick a Material node atop a completed plan
 *
 * There are a couple of places where we want to attach a Material node
 * after completion of subquery_planner().	This currently requires hackery.
 * Since subquery_planner has already run SS_finalize_plan on the subplan
 * tree, we have to kluge up parameter lists for the Material node.
 * Possibly this could be fixed by postponing SS_finalize_plan processing
 * until setrefs.c is run?
 */
Plan *
materialize_finished_plan(Plan *subplan)
{
	Plan	   *matplan;
	Path		matpath;		/* dummy for result of cost_material */

	matplan = (Plan *) make_material(subplan->targetlist, subplan);

	/* Set cost data */
	cost_material(&matpath,
				  subplan->total_cost,
				  subplan->plan_rows,
				  subplan->plan_width);
	matplan->startup_cost = matpath.startup_cost;
	matplan->total_cost = matpath.total_cost;
	matplan->plan_rows = subplan->plan_rows;
	matplan->plan_width = subplan->plan_width;

	/* parameter kluge --- see comments above */
	matplan->extParam = bms_copy(subplan->extParam);
	matplan->allParam = bms_copy(subplan->allParam);

	return matplan;
}

Agg *
make_agg(Query *root, List *tlist, List *qual,
		 AggStrategy aggstrategy,
		 int numGroupCols, AttrNumber *grpColIdx,
		 long numGroups, int numAggs,
		 Plan *lefttree)
{
	Agg		   *node = makeNode(Agg);
	Plan	   *plan = &node->plan;
	Path		agg_path;		/* dummy for result of cost_agg */
	QualCost	qual_cost;

	node->aggstrategy = aggstrategy;
	node->numCols = numGroupCols;
	node->grpColIdx = grpColIdx;
	node->numGroups = numGroups;

	copy_plan_costsize(plan, lefttree); /* only care about copying size */
	cost_agg(&agg_path, root,
			 aggstrategy, numAggs,
			 numGroupCols, numGroups,
			 lefttree->startup_cost,
			 lefttree->total_cost,
			 lefttree->plan_rows);
	plan->startup_cost = agg_path.startup_cost;
	plan->total_cost = agg_path.total_cost;

	/*
	 * We will produce a single output tuple if not grouping, and a tuple
	 * per group otherwise.
	 */
	if (aggstrategy == AGG_PLAIN)
		plan->plan_rows = 1;
	else
		plan->plan_rows = numGroups;

	/*
	 * We also need to account for the cost of evaluation of the qual (ie,
	 * the HAVING clause) and the tlist.  Note that cost_qual_eval doesn't
	 * charge anything for Aggref nodes; this is okay since they are
	 * really comparable to Vars.
	 *
	 * See notes in grouping_planner about why this routine and make_group
	 * are the only ones in this file that worry about tlist eval cost.
	 */
	if (qual)
	{
		cost_qual_eval(&qual_cost, qual);
		plan->startup_cost += qual_cost.startup;
		plan->total_cost += qual_cost.startup;
		plan->total_cost += qual_cost.per_tuple * plan->plan_rows;
	}
	cost_qual_eval(&qual_cost, tlist);
	plan->startup_cost += qual_cost.startup;
	plan->total_cost += qual_cost.startup;
	plan->total_cost += qual_cost.per_tuple * plan->plan_rows;

	plan->qual = qual;
	plan->targetlist = tlist;
	plan->lefttree = lefttree;
	plan->righttree = (Plan *) NULL;

	return node;
}

Group *
make_group(Query *root,
		   List *tlist,
		   int numGroupCols,
		   AttrNumber *grpColIdx,
		   double numGroups,
		   Plan *lefttree)
{
	Group	   *node = makeNode(Group);
	Plan	   *plan = &node->plan;
	Path		group_path;		/* dummy for result of cost_group */
	QualCost	qual_cost;

	node->numCols = numGroupCols;
	node->grpColIdx = grpColIdx;

	copy_plan_costsize(plan, lefttree); /* only care about copying size */
	cost_group(&group_path, root,
			   numGroupCols, numGroups,
			   lefttree->startup_cost,
			   lefttree->total_cost,
			   lefttree->plan_rows);
	plan->startup_cost = group_path.startup_cost;
	plan->total_cost = group_path.total_cost;

	/* One output tuple per estimated result group */
	plan->plan_rows = numGroups;

	/*
	 * We also need to account for the cost of evaluation of the tlist.
	 *
	 * XXX this double-counts the cost of evaluation of any expressions used
	 * for grouping, since in reality those will have been evaluated at a
	 * lower plan level and will only be copied by the Group node. Worth
	 * fixing?
	 *
	 * See notes in grouping_planner about why this routine and make_agg are
	 * the only ones in this file that worry about tlist eval cost.
	 */
	cost_qual_eval(&qual_cost, tlist);
	plan->startup_cost += qual_cost.startup;
	plan->total_cost += qual_cost.startup;
	plan->total_cost += qual_cost.per_tuple * plan->plan_rows;

	plan->qual = NIL;
	plan->targetlist = tlist;
	plan->lefttree = lefttree;
	plan->righttree = (Plan *) NULL;

	return node;
}

/*
 * distinctList is a list of SortClauses, identifying the targetlist items
 * that should be considered by the Unique filter.
 */
Unique *
make_unique(List *tlist, Plan *lefttree, List *distinctList)
{
	Unique	   *node = makeNode(Unique);
	Plan	   *plan = &node->plan;
	int			numCols = length(distinctList);
	int			keyno = 0;
	AttrNumber *uniqColIdx;
	List	   *slitem;

	copy_plan_costsize(plan, lefttree);

	/*
	 * Charge one cpu_operator_cost per comparison per input tuple. We
	 * assume all columns get compared at most of the tuples.  (XXX
	 * probably this is an overestimate.)
	 */
	plan->total_cost += cpu_operator_cost * plan->plan_rows * numCols;

	/*
	 * plan->plan_rows is left as a copy of the input subplan's plan_rows;
	 * ie, we assume the filter removes nothing.  The caller must alter
	 * this if he has a better idea.
	 */

	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	/*
	 * convert SortClause list into array of attr indexes, as wanted by
	 * exec
	 */
	Assert(numCols > 0);
	uniqColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numCols);

	foreach(slitem, distinctList)
	{
		SortClause *sortcl = (SortClause *) lfirst(slitem);
		TargetEntry *tle = get_sortgroupclause_tle(sortcl, tlist);

		uniqColIdx[keyno++] = tle->resdom->resno;
	}

	node->numCols = numCols;
	node->uniqColIdx = uniqColIdx;

	return node;
}

/*
 * distinctList is a list of SortClauses, identifying the targetlist items
 * that should be considered by the SetOp filter.
 */

SetOp *
make_setop(SetOpCmd cmd, List *tlist, Plan *lefttree,
		   List *distinctList, AttrNumber flagColIdx)
{
	SetOp	   *node = makeNode(SetOp);
	Plan	   *plan = &node->plan;
	int			numCols = length(distinctList);
	int			keyno = 0;
	AttrNumber *dupColIdx;
	List	   *slitem;

	copy_plan_costsize(plan, lefttree);

	/*
	 * Charge one cpu_operator_cost per comparison per input tuple. We
	 * assume all columns get compared at most of the tuples.
	 */
	plan->total_cost += cpu_operator_cost * plan->plan_rows * numCols;

	/*
	 * We make the unsupported assumption that there will be 10% as many
	 * tuples out as in.  Any way to do better?
	 */
	plan->plan_rows *= 0.1;
	if (plan->plan_rows < 1)
		plan->plan_rows = 1;

	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	/*
	 * convert SortClause list into array of attr indexes, as wanted by
	 * exec
	 */
	Assert(numCols > 0);
	dupColIdx = (AttrNumber *) palloc(sizeof(AttrNumber) * numCols);

	foreach(slitem, distinctList)
	{
		SortClause *sortcl = (SortClause *) lfirst(slitem);
		TargetEntry *tle = get_sortgroupclause_tle(sortcl, tlist);

		dupColIdx[keyno++] = tle->resdom->resno;
	}

	node->cmd = cmd;
	node->numCols = numCols;
	node->dupColIdx = dupColIdx;
	node->flagColIdx = flagColIdx;

	return node;
}

Limit *
make_limit(List *tlist, Plan *lefttree,
		   Node *limitOffset, Node *limitCount)
{
	Limit	   *node = makeNode(Limit);
	Plan	   *plan = &node->plan;

	copy_plan_costsize(plan, lefttree);

	/*
	 * If offset/count are constants, adjust the output rows count and
	 * costs accordingly.  This is only a cosmetic issue if we are at top
	 * level, but if we are building a subquery then it's important to
	 * report correct info to the outer planner.
	 */
	if (limitOffset && IsA(limitOffset, Const))
	{
		Const	   *limito = (Const *) limitOffset;
		int32		offset = DatumGetInt32(limito->constvalue);

		if (!limito->constisnull && offset > 0)
		{
			if (offset > plan->plan_rows)
				offset = (int32) plan->plan_rows;
			if (plan->plan_rows > 0)
				plan->startup_cost +=
					(plan->total_cost - plan->startup_cost)
					* ((double) offset) / plan->plan_rows;
			plan->plan_rows -= offset;
			if (plan->plan_rows < 1)
				plan->plan_rows = 1;
		}
	}
	if (limitCount && IsA(limitCount, Const))
	{
		Const	   *limitc = (Const *) limitCount;
		int32		count = DatumGetInt32(limitc->constvalue);

		if (!limitc->constisnull && count >= 0)
		{
			if (count > plan->plan_rows)
				count = (int32) plan->plan_rows;
			if (plan->plan_rows > 0)
				plan->total_cost = plan->startup_cost +
					(plan->total_cost - plan->startup_cost)
					* ((double) count) / plan->plan_rows;
			plan->plan_rows = count;
			if (plan->plan_rows < 1)
				plan->plan_rows = 1;
		}
	}

	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	node->limitOffset = limitOffset;
	node->limitCount = limitCount;

	return node;
}

Result *
make_result(List *tlist,
			Node *resconstantqual,
			Plan *subplan)
{
	Result	   *node = makeNode(Result);
	Plan	   *plan = &node->plan;

	if (subplan)
		copy_plan_costsize(plan, subplan);
	else
	{
		plan->startup_cost = 0;
		plan->total_cost = cpu_tuple_cost;
		plan->plan_rows = 1;	/* wrong if we have a set-valued function? */
		plan->plan_width = 0;	/* XXX try to be smarter? */
	}

	if (resconstantqual)
	{
		QualCost	qual_cost;

		cost_qual_eval(&qual_cost, (List *) resconstantqual);
		/* resconstantqual is evaluated once at startup */
		plan->startup_cost += qual_cost.startup + qual_cost.per_tuple;
		plan->total_cost += qual_cost.startup + qual_cost.per_tuple;
	}

	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = subplan;
	plan->righttree = NULL;
	node->resconstantqual = resconstantqual;

	return node;
}
