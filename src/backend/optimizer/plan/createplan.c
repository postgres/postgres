/*-------------------------------------------------------------------------
 *
 * createplan.c
 *	  Routines to create the desired plan for processing a query.
 *	  Planning is complete, we just need to convert the selected
 *	  Path into a Plan.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/plan/createplan.c,v 1.100 2000/11/12 00:36:58 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>

#include "postgres.h"

#include "catalog/pg_index.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "parser/parse_expr.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


static Scan *create_scan_plan(Query *root, Path *best_path);
static Join *create_join_plan(Query *root, JoinPath *best_path);
static Append *create_append_plan(Query *root, AppendPath *best_path);
static SeqScan *create_seqscan_plan(Path *best_path, List *tlist,
					List *scan_clauses);
static IndexScan *create_indexscan_plan(Query *root, IndexPath *best_path,
					  List *tlist, List *scan_clauses);
static TidScan *create_tidscan_plan(TidPath *best_path, List *tlist,
					List *scan_clauses);
static SubqueryScan *create_subqueryscan_plan(Path *best_path,
					  List *tlist, List *scan_clauses);
static NestLoop *create_nestloop_plan(NestPath *best_path, List *tlist,
									  List *joinclauses, List *otherclauses,
									  Plan *outer_plan, List *outer_tlist,
									  Plan *inner_plan, List *inner_tlist);
static MergeJoin *create_mergejoin_plan(MergePath *best_path, List *tlist,
										List *joinclauses, List *otherclauses,
										Plan *outer_plan, List *outer_tlist,
										Plan *inner_plan, List *inner_tlist);
static HashJoin *create_hashjoin_plan(HashPath *best_path, List *tlist,
									  List *joinclauses, List *otherclauses,
									  Plan *outer_plan, List *outer_tlist,
									  Plan *inner_plan, List *inner_tlist);
static List *fix_indxqual_references(List *indexquals, IndexPath *index_path);
static List *fix_indxqual_sublist(List *indexqual, int baserelid, Oid relam,
					 Form_pg_index index);
static Node *fix_indxqual_operand(Node *node, int baserelid,
					 Form_pg_index index,
					 Oid *opclass);
static List *switch_outer(List *clauses);
static void copy_path_costsize(Plan *dest, Path *src);
static void copy_plan_costsize(Plan *dest, Plan *src);
static SeqScan *make_seqscan(List *qptlist, List *qpqual, Index scanrelid);
static IndexScan *make_indexscan(List *qptlist, List *qpqual, Index scanrelid,
			   List *indxid, List *indxqual,
			   List *indxqualorig,
			   ScanDirection indexscandir);
static TidScan *make_tidscan(List *qptlist, List *qpqual, Index scanrelid,
			 List *tideval);
static NestLoop *make_nestloop(List *tlist,
							   List *joinclauses, List *otherclauses,
							   Plan *lefttree, Plan *righttree,
							   JoinType jointype);
static HashJoin *make_hashjoin(List *tlist,
							   List *joinclauses, List *otherclauses,
							   List *hashclauses,
							   Plan *lefttree, Plan *righttree,
							   JoinType jointype);
static Hash *make_hash(List *tlist, Node *hashkey, Plan *lefttree);
static MergeJoin *make_mergejoin(List *tlist,
								 List *joinclauses, List *otherclauses,
								 List *mergeclauses,
								 Plan *lefttree, Plan *righttree,
								 JoinType jointype);

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
		default:
			elog(ERROR, "create_plan: unknown pathtype %d",
				 best_path->pathtype);
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
	Scan	   *plan;
	List	   *tlist = best_path->parent->targetlist;
	List	   *scan_clauses;

	/*
	 * Extract the relevant restriction clauses from the parent relation;
	 * the executor must apply all these restrictions during the scan.
	 */
	scan_clauses = get_actual_clauses(best_path->parent->baserestrictinfo);

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

		default:
			elog(ERROR, "create_scan_plan: unknown node type: %d",
				 best_path->pathtype);
			plan = NULL;		/* keep compiler quiet */
			break;
	}

	return plan;
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
	List	   *join_tlist = best_path->path.parent->targetlist;
	Plan	   *outer_plan;
	List	   *outer_tlist;
	Plan	   *inner_plan;
	List	   *inner_tlist;
	List	   *joinclauses;
	List	   *otherclauses;
	Join	   *plan;

	outer_plan = create_plan(root, best_path->outerjoinpath);
	outer_tlist = outer_plan->targetlist;

	inner_plan = create_plan(root, best_path->innerjoinpath);
	inner_tlist = inner_plan->targetlist;

	if (IS_OUTER_JOIN(best_path->jointype))
	{
		get_actual_join_clauses(best_path->joinrestrictinfo,
								&joinclauses, &otherclauses);
	}
	else
	{
		/* We can treat all clauses alike for an inner join */
		joinclauses = get_actual_clauses(best_path->joinrestrictinfo);
		otherclauses = NIL;
	}

	switch (best_path->path.pathtype)
	{
		case T_MergeJoin:
			plan = (Join *) create_mergejoin_plan((MergePath *) best_path,
												  join_tlist,
												  joinclauses,
												  otherclauses,
												  outer_plan,
												  outer_tlist,
												  inner_plan,
												  inner_tlist);
			break;
		case T_HashJoin:
			plan = (Join *) create_hashjoin_plan((HashPath *) best_path,
												 join_tlist,
												 joinclauses,
												 otherclauses,
												 outer_plan,
												 outer_tlist,
												 inner_plan,
												 inner_tlist);
			break;
		case T_NestLoop:
			plan = (Join *) create_nestloop_plan((NestPath *) best_path,
												 join_tlist,
												 joinclauses,
												 otherclauses,
												 outer_plan,
												 outer_tlist,
												 inner_plan,
												 inner_tlist);
			break;
		default:
			elog(ERROR, "create_join_plan: unknown node type: %d",
				 best_path->path.pathtype);
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
	List	   *tlist = best_path->path.parent->targetlist;
	List	   *subplans = NIL;
	List	   *subpaths;

	foreach(subpaths, best_path->subpaths)
	{
		Path   *subpath = (Path *) lfirst(subpaths);
		
		subplans = lappend(subplans, create_plan(root, subpath));
	}

	plan = make_append(subplans, false, tlist);

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
	Index		scan_relid;

	/* there should be exactly one base rel involved... */
	Assert(length(best_path->parent->relids) == 1);
	Assert(! best_path->parent->issubquery);

	scan_relid = (Index) lfirsti(best_path->parent->relids);

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
 * and indexid lists must have the same length, ie, the number of scans
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
	Index		baserelid;
	List	   *qpqual;
	List	   *fixed_indxqual;
	List	   *ixid;
	IndexScan  *scan_plan;
	bool		lossy = false;

	/* there should be exactly one base rel involved... */
	Assert(length(best_path->path.parent->relids) == 1);
	Assert(! best_path->path.parent->issubquery);

	baserelid = lfirsti(best_path->path.parent->relids);

	/* check to see if any of the indices are lossy */
	foreach(ixid, best_path->indexid)
	{
		HeapTuple	indexTuple;
		Form_pg_index index;

		indexTuple = SearchSysCacheTuple(INDEXRELID,
										 ObjectIdGetDatum(lfirsti(ixid)),
										 0, 0, 0);
		if (!HeapTupleIsValid(indexTuple))
			elog(ERROR, "create_plan: index %u not found", lfirsti(ixid));
		index = (Form_pg_index) GETSTRUCT(indexTuple);
		if (index->indislossy)
		{
			lossy = true;
			break;
		}
	}

	/*
	 * The qpqual list must contain all restrictions not automatically
	 * handled by the index.  Note that for non-lossy indices, the
	 * predicates in the indxqual are checked fully by the index, while
	 * for lossy indices the indxqual predicates need to be double-checked
	 * after the index fetches the best-guess tuples.
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
		List	   *orclauses = NIL;
		List	   *orclause;
		Expr	   *indxqual_expr;

		foreach(orclause, indxqual)
			orclauses = lappend(orclauses,
								make_ands_explicit(lfirst(orclause)));
		indxqual_expr = make_orclause(orclauses);

		qpqual = set_difference(scan_clauses, makeList1(indxqual_expr));

		if (lossy)
			qpqual = lappend(qpqual, copyObject(indxqual_expr));
	}
	else if (indxqual != NIL)
	{

		/*
		 * Here, we can simply treat the first sublist as an independent
		 * set of qual expressions, since there is no top-level OR
		 * behavior.
		 */
		List	   *indxqual_list = lfirst(indxqual);

		qpqual = set_difference(scan_clauses, indxqual_list);

		if (lossy)
			qpqual = nconc(qpqual, (List *) copyObject(indxqual_list));
	}
	else
		qpqual = scan_clauses;

	/*
	 * The executor needs a copy with the indexkey on the left of each
	 * clause and with index attr numbers substituted for table ones.
	 */
	fixed_indxqual = fix_indxqual_references(indxqual, best_path);

	scan_plan = make_indexscan(tlist,
							   qpqual,
							   baserelid,
							   best_path->indexid,
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
	Index		scan_relid;

	/* there should be exactly one base rel involved... */
	Assert(length(best_path->path.parent->relids) == 1);
	Assert(! best_path->path.parent->issubquery);

	scan_relid = (Index) lfirsti(best_path->path.parent->relids);

	scan_plan = make_tidscan(tlist,
							 scan_clauses,
							 scan_relid,
							 best_path->tideval);

	if (best_path->unjoined_relids)
		scan_plan->needRescan = true;

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
	Index		scan_relid;

	/* there should be exactly one base rel involved... */
	Assert(length(best_path->parent->relids) == 1);
	/* and it must be a subquery */
	Assert(best_path->parent->issubquery);

	scan_relid = (Index) lfirsti(best_path->parent->relids);

	scan_plan = make_subqueryscan(tlist,
								  scan_clauses,
								  scan_relid,
								  best_path->parent->subplan);

	return scan_plan;
}

/*****************************************************************************
 *
 *	JOIN METHODS
 *
 * A general note about join_references() processing in these routines:
 * once we have changed a Var node to refer to a subplan output rather than
 * the original relation, it is no longer equal() to an unmodified Var node
 * for the same var.  So, we cannot easily compare reference-adjusted qual
 * clauses to clauses that have not been adjusted.	Fortunately, that
 * doesn't seem to be necessary; all the decisions are made before we do
 * the reference adjustments.
 *
 * A cleaner solution would be to not call join_references() here at all,
 * but leave it for setrefs.c to do at the end of plan tree construction.
 * But that would make switch_outer() much more complicated, and some care
 * would be needed to get setrefs.c to do the right thing with nestloop
 * inner indexscan quals.  So, we do subplan reference adjustment here for
 * quals of join nodes (and *only* for quals of join nodes).
 *
 *****************************************************************************/

static NestLoop *
create_nestloop_plan(NestPath *best_path,
					 List *tlist,
					 List *joinclauses,
					 List *otherclauses,
					 Plan *outer_plan,
					 List *outer_tlist,
					 Plan *inner_plan,
					 List *inner_tlist)
{
	NestLoop   *join_plan;

	if (IsA(inner_plan, IndexScan))
	{

		/*
		 * An index is being used to reduce the number of tuples scanned
		 * in the inner relation.  If there are join clauses being used
		 * with the index, we must update their outer-rel var nodes to
		 * refer to the outer side of the join.
		 *
		 * We can also remove those join clauses from the list of clauses
		 * that have to be checked as qpquals at the join node, but only
		 * if there's just one indexscan in the inner path (otherwise,
		 * several different sets of clauses are being ORed together).
		 *
		 * Note: if the index is lossy, the same clauses may also be getting
		 * checked as qpquals in the indexscan.  We can still remove them
		 * from the nestloop's qpquals, but we gotta update the outer-rel
		 * vars in the indexscan's qpquals too.
		 *
		 * Note: we can safely do set_difference() against my clauses and
		 * join_references() because the innerscan is a primitive plan,
		 * and therefore has not itself done join_references renumbering
		 * of the vars in its quals.
		 */
		IndexScan  *innerscan = (IndexScan *) inner_plan;
		List	   *indxqualorig = innerscan->indxqualorig;

		/* No work needed if indxqual refers only to its own relation... */
		if (NumRelids((Node *) indxqualorig) > 1)
		{
			Index		innerrel = innerscan->scan.scanrelid;

			/*
			 * Remove redundant tests from my clauses, if possible. Note
			 * we must compare against indxqualorig not the "fixed"
			 * indxqual (which has index attnos instead of relation
			 * attnos, and may have been commuted as well).
			 */
			if (length(indxqualorig) == 1)		/* single indexscan? */
				joinclauses = set_difference(joinclauses,
											 lfirst(indxqualorig));

			/* only refs to outer vars get changed in the inner indexqual */
			innerscan->indxqualorig = join_references(indxqualorig,
													  outer_tlist,
													  NIL,
													  innerrel);
			innerscan->indxqual = join_references(innerscan->indxqual,
												  outer_tlist,
												  NIL,
												  innerrel);
			/* fix the inner qpqual too, if it has join clauses */
			if (NumRelids((Node *) inner_plan->qual) > 1)
				inner_plan->qual = join_references(inner_plan->qual,
												   outer_tlist,
												   NIL,
												   innerrel);
		}
	}
	else if (IsA(inner_plan, TidScan))
	{
		TidScan    *innerscan = (TidScan *) inner_plan;

		innerscan->tideval = join_references(innerscan->tideval,
											 outer_tlist,
											 inner_tlist,
											 innerscan->scan.scanrelid);
	}
	else if (IsA_Join(inner_plan))
	{

		/*
		 * Materialize the inner join for speed reasons.
		 *
		 * XXX It is probably *not* always fastest to materialize an inner
		 * join --- how can we estimate whether this is a good thing to
		 * do?
		 */
		inner_plan = (Plan *) make_material(inner_tlist,
											inner_plan);
	}

	/*
	 * Set quals to contain INNER/OUTER var references.
	 */
	joinclauses = join_references(joinclauses,
								  outer_tlist,
								  inner_tlist,
								  (Index) 0);
	otherclauses = join_references(otherclauses,
								   outer_tlist,
								   inner_tlist,
								   (Index) 0);

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
create_mergejoin_plan(MergePath *best_path,
					  List *tlist,
					  List *joinclauses,
					  List *otherclauses,
					  Plan *outer_plan,
					  List *outer_tlist,
					  Plan *inner_plan,
					  List *inner_tlist)
{
	List	   *mergeclauses;
	MergeJoin  *join_plan;

	mergeclauses = get_actual_clauses(best_path->path_mergeclauses);

	/*
	 * Remove the mergeclauses from the list of join qual clauses, leaving
	 * the list of quals that must be checked as qpquals. Set those
	 * clauses to contain INNER/OUTER var references.
	 */
	joinclauses = join_references(set_difference(joinclauses, mergeclauses),
								  outer_tlist,
								  inner_tlist,
								  (Index) 0);

	/*
	 * Fix the additional qpquals too.
	 */
	otherclauses = join_references(otherclauses,
								   outer_tlist,
								   inner_tlist,
								   (Index) 0);

	/*
	 * Now set the references in the mergeclauses and rearrange them so
	 * that the outer variable is always on the left.
	 */
	mergeclauses = switch_outer(join_references(mergeclauses,
												outer_tlist,
												inner_tlist,
												(Index) 0));

	/*
	 * Create explicit sort nodes for the outer and inner join paths if
	 * necessary.  The sort cost was already accounted for in the path.
	 */
	if (best_path->outersortkeys)
		outer_plan = (Plan *)
			make_sort_from_pathkeys(outer_tlist,
									outer_plan,
									best_path->outersortkeys);

	if (best_path->innersortkeys)
		inner_plan = (Plan *)
			make_sort_from_pathkeys(inner_tlist,
									inner_plan,
									best_path->innersortkeys);

	/*
	 * The executor requires the inner side of a mergejoin to support "mark"
	 * and "restore" operations.  Not all plan types do, so we must be careful
	 * not to generate an invalid plan.  If necessary, an invalid inner plan
	 * can be handled by inserting a Materialize node.
	 *
	 * Since the inner side must be ordered, and only Sorts and IndexScans can
	 * create order to begin with, you might think there's no problem --- but
	 * you'd be wrong.  Nestloop and merge joins can *preserve* the order of
	 * their inputs, so they can be selected as the input of a mergejoin,
	 * and that won't work in the present executor.
	 *
	 * Doing this here is a bit of a kluge since the cost of the Materialize
	 * wasn't taken into account in our earlier decisions.  But Materialize
	 * is hard to estimate a cost for, and the above consideration shows that
	 * this is a rare case anyway, so this seems an acceptable way to proceed.
	 *
	 * This check must agree with ExecMarkPos/ExecRestrPos in
	 * executor/execAmi.c!
	 */
	switch (nodeTag(inner_plan))
	{
		case T_SeqScan:
		case T_IndexScan:
		case T_Material:
		case T_Sort:
			/* OK, these inner plans support mark/restore */
			break;

		default:
			/* Ooops, need to materialize the inner plan */
			inner_plan = (Plan *) make_material(inner_tlist,
												inner_plan);
			break;
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
create_hashjoin_plan(HashPath *best_path,
					 List *tlist,
					 List *joinclauses,
					 List *otherclauses,
					 Plan *outer_plan,
					 List *outer_tlist,
					 Plan *inner_plan,
					 List *inner_tlist)
{
	List	   *hashclauses;
	HashJoin   *join_plan;
	Hash	   *hash_plan;
	Node	   *innerhashkey;

	/*
	 * NOTE: there will always be exactly one hashclause in the list
	 * best_path->path_hashclauses (cf. hash_inner_and_outer()). We
	 * represent it as a list anyway, for convenience with routines that
	 * want to work on lists of clauses.
	 */
	hashclauses = get_actual_clauses(best_path->path_hashclauses);

	/*
	 * Remove the hashclauses from the list of join qual clauses, leaving
	 * the list of quals that must be checked as qpquals. Set those
	 * clauses to contain INNER/OUTER var references.
	 */
	joinclauses = join_references(set_difference(joinclauses, hashclauses),
								  outer_tlist,
								  inner_tlist,
								  (Index) 0);

	/*
	 * Fix the additional qpquals too.
	 */
	otherclauses = join_references(otherclauses,
								   outer_tlist,
								   inner_tlist,
								   (Index) 0);

	/*
	 * Now set the references in the hashclauses and rearrange them so
	 * that the outer variable is always on the left.
	 */
	hashclauses = switch_outer(join_references(hashclauses,
											   outer_tlist,
											   inner_tlist,
											   (Index) 0));

	/* Now the righthand op of the sole hashclause is the inner hash key. */
	innerhashkey = (Node *) get_rightop(lfirst(hashclauses));

	/*
	 * Build the hash node and hash join node.
	 */
	hash_plan = make_hash(inner_tlist, innerhashkey, inner_plan);
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
 *	  machinery needs.
 *
 * We have three tasks here:
 *	* Index keys must be represented by Var nodes with varattno set to the
 *	  index's attribute number, not the attribute number in the original rel.
 *	* indxpath.c may have selected an index that is binary-compatible with
 *	  the actual expression operator, but not exactly the same datatype.
 *	  We must replace the expression's operator with the binary-compatible
 *	  equivalent operator that the index will recognize.
 *	* If the index key is on the right, commute the clause to put it on the
 *	  left.  (Someday the executor might not need this, but for now it does.)
 *
 * This code used to be entirely bogus for multi-index scans.  Now it keeps
 * track of which index applies to each subgroup of index qual clauses...
 *
 * Returns a modified copy of the indexqual list --- the original is not
 * changed.  Note also that the copy shares no substructure with the
 * original; this is needed in case there is a subplan in it (we need
 * two separate copies of the subplan tree, or things will go awry).
 */

static List *
fix_indxqual_references(List *indexquals, IndexPath *index_path)
{
	List	   *fixed_quals = NIL;
	int			baserelid = lfirsti(index_path->path.parent->relids);
	List	   *indexids = index_path->indexid;
	List	   *i;

	foreach(i, indexquals)
	{
		List	   *indexqual = lfirst(i);
		Oid			indexid = lfirsti(indexids);
		HeapTuple	indexTuple;
		Oid			relam;
		Form_pg_index index;

		/* Get the relam from the index's pg_class entry */
		indexTuple = SearchSysCacheTuple(RELOID,
										 ObjectIdGetDatum(indexid),
										 0, 0, 0);
		if (!HeapTupleIsValid(indexTuple))
			elog(ERROR, "fix_indxqual_references: index %u not found in pg_class",
				 indexid);
		relam = ((Form_pg_class) GETSTRUCT(indexTuple))->relam;

		/* Need the index's pg_index entry for other stuff */
		indexTuple = SearchSysCacheTuple(INDEXRELID,
										 ObjectIdGetDatum(indexid),
										 0, 0, 0);
		if (!HeapTupleIsValid(indexTuple))
			elog(ERROR, "fix_indxqual_references: index %u not found in pg_index",
				 indexid);
		index = (Form_pg_index) GETSTRUCT(indexTuple);

		fixed_quals = lappend(fixed_quals,
							  fix_indxqual_sublist(indexqual,
												   baserelid,
												   relam,
												   index));

		indexids = lnext(indexids);
	}
	return fixed_quals;
}

/*
 * Fix the sublist of indexquals to be used in a particular scan.
 *
 * For each qual clause, commute if needed to put the indexkey operand on the
 * left, and then fix its varattno.  (We do not need to change the other side
 * of the clause.)	Also change the operator if necessary.
 */
static List *
fix_indxqual_sublist(List *indexqual, int baserelid, Oid relam,
					 Form_pg_index index)
{
	List	   *fixed_qual = NIL;
	List	   *i;

	foreach(i, indexqual)
	{
		Expr	   *clause = (Expr *) lfirst(i);
		int			relid;
		AttrNumber	attno;
		Datum		constval;
		int			flag;
		Expr	   *newclause;
		Oid			opclass,
					newopno;

		if (!is_opclause((Node *) clause) ||
			length(clause->args) != 2)
			elog(ERROR, "fix_indxqual_sublist: indexqual clause is not binary opclause");

		/*
		 * Which side is the indexkey on?
		 *
		 * get_relattval sets flag&SEL_RIGHT if the indexkey is on the LEFT.
		 */
		get_relattval((Node *) clause, baserelid,
					  &relid, &attno, &constval, &flag);

		/*
		 * Make a copy that will become the fixed clause.
		 *
		 * We used to try to do a shallow copy here, but that fails if there
		 * is a subplan in the arguments of the opclause.  So just do a
		 * full copy.
		 */
		newclause = (Expr *) copyObject((Node *) clause);

		/* If the indexkey is on the right, commute the clause. */
		if ((flag & SEL_RIGHT) == 0)
			CommuteClause(newclause);

		/*
		 * Now, determine which index attribute this is, change the
		 * indexkey operand as needed, and get the index opclass.
		 */
		lfirst(newclause->args) = fix_indxqual_operand(lfirst(newclause->args),
													   baserelid,
													   index,
													   &opclass);

		/*
		 * Substitute the appropriate operator if the expression operator
		 * is merely binary-compatible with the index.	This shouldn't
		 * fail, since indxpath.c found it before...
		 */
		newopno = indexable_operator(newclause, opclass, relam, true);
		if (newopno == InvalidOid)
			elog(ERROR, "fix_indxqual_sublist: failed to find substitute op");
		((Oper *) newclause->oper)->opno = newopno;

		fixed_qual = lappend(fixed_qual, newclause);
	}
	return fixed_qual;
}

static Node *
fix_indxqual_operand(Node *node, int baserelid, Form_pg_index index,
					 Oid *opclass)
{
	/*
	 * Remove any binary-compatible relabeling of the indexkey
	 */
	if (IsA(node, RelabelType))
		node = ((RelabelType *) node)->arg;

	/*
	 * We represent index keys by Var nodes having the varno of the base
	 * table but varattno equal to the index's attribute number (index
	 * column position).  This is a bit hokey ... would be cleaner to use
	 * a special-purpose node type that could not be mistaken for a regular
	 * Var.  But it will do for now.
	 */
	if (IsA(node, Var))
	{
		/* If it's a var, find which index key position it occupies */
		if (((Var *) node)->varno == baserelid)
		{
			int			varatt = ((Var *) node)->varattno;
			int			pos;

			for (pos = 0; pos < INDEX_MAX_KEYS; pos++)
			{
				if (index->indkey[pos] == varatt)
				{
					Node	   *newnode = copyObject(node);

					((Var *) newnode)->varattno = pos + 1;
					/* return the correct opclass, too */
					*opclass = index->indclass[pos];
					return newnode;
				}
			}
		}

		/*
		 * Oops, this Var isn't the indexkey!
		 */
		elog(ERROR, "fix_indxqual_operand: var is not index attribute");
	}

	/*
	 * Else, it must be a func expression matching a functional index.
	 * Since we currently only support single-column functional indexes,
	 * the returned varattno must be 1.
	 */

	Assert(is_funcclause(node)); /* not a very thorough check, but easy */

	/* indclass[0] is the only class of a functional index */
	*opclass = index->indclass[0];

	return (Node *) makeVar(baserelid, 1, exprType(node), -1, 0);
}

/*
 * switch_outer
 *	  Given a list of merge or hash joinclauses, rearrange the elements within
 *	  the clauses so the outer join variable is on the left and the inner is
 *	  on the right.  The original list is not touched; a modified list
 *	  is returned.
 */
static List *
switch_outer(List *clauses)
{
	List	   *t_list = NIL;
	List	   *i;

	foreach(i, clauses)
	{
		Expr	   *clause = (Expr *) lfirst(i);
		Var		   *op;

		Assert(is_opclause((Node *) clause));
		op = get_rightop(clause);
		Assert(op && IsA(op, Var));
		if (var_is_outer(op))
		{

			/*
			 * Duplicate just enough of the structure to allow commuting
			 * the clause without changing the original list.  Could use
			 * copyObject, but a complete deep copy is overkill.
			 */
			Expr	   *temp;

			temp = make_clause(clause->opType, clause->oper,
							   listCopy(clause->args));
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
	plan->state = (EState *) NULL;
	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scanrelid = scanrelid;
	node->scanstate = (CommonScanState *) NULL;

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
	plan->state = (EState *) NULL;
	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->indxid = indxid;
	node->indxqual = indxqual;
	node->indxqualorig = indxqualorig;
	node->indxorderdir = indexscandir;
	node->scan.scanstate = (CommonScanState *) NULL;

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
	plan->state = (EState *) NULL;
	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->tideval = copyObject(tideval);		/* XXX do we really need a
												 * copy? */
	node->needRescan = false;
	node->scan.scanstate = (CommonScanState *) NULL;

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

	copy_plan_costsize(plan, subplan);
	plan->state = (EState *) NULL;
	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->subplan = subplan;
	node->scan.scanstate = (CommonScanState *) NULL;

	return node;
}

Append *
make_append(List *appendplans, bool isTarget, List *tlist)
{
	Append	   *node = makeNode(Append);
	Plan	   *plan = &node->plan;
	List	   *subnode;

	/* compute costs from subplan costs */
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
	plan->state = (EState *) NULL;
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
	plan->state = (EState *) NULL;
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
	plan->state = (EState *) NULL;
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
make_hash(List *tlist, Node *hashkey, Plan *lefttree)
{
	Hash	   *node = makeNode(Hash);
	Plan	   *plan = &node->plan;

	copy_plan_costsize(plan, lefttree);

	/*
	 * For plausibility, make startup & total costs equal total cost of
	 * input plan; this only affects EXPLAIN display not decisions.
	 */
	plan->startup_cost = plan->total_cost;
	plan->state = (EState *) NULL;
	plan->targetlist = tlist;
	plan->qual = NULL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;
	node->hashkey = hashkey;

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
	plan->state = (EState *) NULL;
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
 * To use make_sort directly, you must already have marked the tlist
 * with reskey and reskeyop information.  The keys had better be
 * non-redundant, too (ie, there had better be tlist items marked with
 * each key number from 1 to keycount), or the executor will get confused!
 */
Sort *
make_sort(List *tlist, Plan *lefttree, int keycount)
{
	Sort	   *node = makeNode(Sort);
	Plan	   *plan = &node->plan;
	Path		sort_path;		/* dummy for result of cost_sort */

	copy_plan_costsize(plan, lefttree); /* only care about copying size */
	cost_sort(&sort_path, NIL, lefttree->plan_rows, lefttree->plan_width);
	plan->startup_cost = sort_path.startup_cost + lefttree->total_cost;
	plan->total_cost = sort_path.total_cost + lefttree->total_cost;
	plan->state = (EState *) NULL;
	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;
	node->keycount = keycount;

	return node;
}

/*
 * make_sort_from_pathkeys
 *	  Create sort plan to sort according to given pathkeys
 *
 *	  'tlist' is the target list of the input plan
 *	  'lefttree' is the node which yields input tuples
 *	  'pathkeys' is the list of pathkeys by which the result is to be sorted
 *
 * We must convert the pathkey information into reskey and reskeyop fields
 * of resdom nodes in the sort plan's target list.
 */
Sort *
make_sort_from_pathkeys(List *tlist, Plan *lefttree, List *pathkeys)
{
	List	   *sort_tlist;
	List	   *i;
	int			numsortkeys = 0;

	/* Create a new target list for the sort, with sort keys set. */
	sort_tlist = new_unsorted_tlist(tlist);

	foreach(i, pathkeys)
	{
		List	   *keysublist = (List *) lfirst(i);
		PathKeyItem *pathkey = NULL;
		Resdom	   *resdom = NULL;
		List	   *j;

		/*
		 * We can sort by any one of the sort key items listed in this
		 * sublist.  For now, we take the first one that corresponds to an
		 * available Var in the sort_tlist.
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
			resdom = tlist_member(pathkey->key, sort_tlist);
			if (resdom)
				break;
		}
		if (!resdom)
			elog(ERROR, "make_sort_from_pathkeys: cannot find tlist item to sort");

		/*
		 * The resdom might be already marked as a sort key, if the
		 * pathkeys contain duplicate entries.	(This can happen in
		 * scenarios where multiple mergejoinable clauses mention the same
		 * var, for example.) In that case the current pathkey is
		 * essentially a no-op, because only one value can be seen within
		 * any subgroup where it would be consulted.  We can ignore it.
		 */
		if (resdom->reskey == 0)
		{
			/* OK, mark it as a sort key and set the sort operator regproc */
			resdom->reskey = ++numsortkeys;
			resdom->reskeyop = get_opcode(pathkey->sortop);
		}
	}

	Assert(numsortkeys > 0);

	return make_sort(sort_tlist, lefttree, numsortkeys);
}

Material *
make_material(List *tlist, Plan *lefttree)
{
	Material   *node = makeNode(Material);
	Plan	   *plan = &node->plan;

	copy_plan_costsize(plan, lefttree);

	/*
	 * For plausibility, make startup & total costs equal total cost of
	 * input plan; this only affects EXPLAIN display not decisions.
	 *
	 * XXX shouldn't we charge some additional cost for materialization?
	 */
	plan->startup_cost = plan->total_cost;
	plan->state = (EState *) NULL;
	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;

	return node;
}

Agg *
make_agg(List *tlist, List *qual, Plan *lefttree)
{
	Agg		   *node = makeNode(Agg);
	Plan	   *plan = &node->plan;

	copy_plan_costsize(plan, lefttree);

	/*
	 * Charge one cpu_operator_cost per aggregate function per input
	 * tuple.
	 */
	plan->total_cost += cpu_operator_cost * plan->plan_rows *
		(length(pull_agg_clause((Node *) tlist)) +
		 length(pull_agg_clause((Node *) qual)));

	/*
	 * We will produce a single output tuple if the input is not a Group,
	 * and a tuple per group otherwise.  For now, estimate the number of
	 * groups as 10% of the number of tuples --- bogus, but how to do
	 * better? (Note we assume the input Group node is in "tuplePerGroup"
	 * mode, so it didn't reduce its row count already.)
	 */
	if (IsA(lefttree, Group))
	{
		plan->plan_rows *= 0.1;
		if (plan->plan_rows < 1)
			plan->plan_rows = 1;
	}
	else
	{
		plan->plan_rows = 1;
		plan->startup_cost = plan->total_cost;
	}

	plan->state = (EState *) NULL;
	plan->qual = qual;
	plan->targetlist = tlist;
	plan->lefttree = lefttree;
	plan->righttree = (Plan *) NULL;

	return node;
}

Group *
make_group(List *tlist,
		   bool tuplePerGroup,
		   int ngrp,
		   AttrNumber *grpColIdx,
		   Plan *lefttree)
{
	Group	   *node = makeNode(Group);
	Plan	   *plan = &node->plan;

	copy_plan_costsize(plan, lefttree);

	/*
	 * Charge one cpu_operator_cost per comparison per input tuple. We
	 * assume all columns get compared at most of the tuples.
	 */
	plan->total_cost += cpu_operator_cost * plan->plan_rows * ngrp;

	/*
	 * If tuplePerGroup (which is named exactly backwards) is true, we
	 * will return all the input tuples, so the input node's row count is
	 * OK.	Otherwise, we'll return only one tuple from each group. For
	 * now, estimate the number of groups as 10% of the number of tuples
	 * --- bogus, but how to do better?
	 */
	if (!tuplePerGroup)
	{
		plan->plan_rows *= 0.1;
		if (plan->plan_rows < 1)
			plan->plan_rows = 1;
	}

	plan->state = (EState *) NULL;
	plan->qual = NULL;
	plan->targetlist = tlist;
	plan->lefttree = lefttree;
	plan->righttree = (Plan *) NULL;
	node->tuplePerGroup = tuplePerGroup;
	node->numCols = ngrp;
	node->grpColIdx = grpColIdx;

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
	 * assume all columns get compared at most of the tuples.
	 */
	plan->total_cost += cpu_operator_cost * plan->plan_rows * numCols;

	/*
	 * As for Group, we make the unsupported assumption that there will be
	 * 10% as many tuples out as in.
	 */
	plan->plan_rows *= 0.1;
	if (plan->plan_rows < 1)
		plan->plan_rows = 1;

	plan->state = (EState *) NULL;
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
	 * As for Group, we make the unsupported assumption that there will be
	 * 10% as many tuples out as in.
	 */
	plan->plan_rows *= 0.1;
	if (plan->plan_rows < 1)
		plan->plan_rows = 1;

	plan->state = (EState *) NULL;
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

	plan->state = (EState *) NULL;
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

#ifdef NOT_USED
	tlist = generate_fjoin(tlist);
#endif
	copy_plan_costsize(plan, subplan);
	plan->state = (EState *) NULL;
	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = subplan;
	plan->righttree = NULL;
	node->resconstantqual = resconstantqual;
	node->resstate = NULL;

	return node;
}

#ifdef NOT_USED
List *
generate_fjoin(List *tlist)
{
	List		tlistP;
	List		newTlist = NIL;
	List		fjoinList = NIL;
	int			nIters = 0;

	/*
	 * Break the target list into elements with Iter nodes, and those
	 * without them.
	 */
	foreach(tlistP, tlist)
	{
		List		tlistElem;

		tlistElem = lfirst(tlistP);
		if (IsA(lsecond(tlistElem), Iter))
		{
			nIters++;
			fjoinList = lappend(fjoinList, tlistElem);
		}
		else
			newTlist = lappend(newTlist, tlistElem);
	}

	/*
	 * if we have an Iter node then we need to flatten.
	 */
	if (nIters > 0)
	{
		List	   *inner;
		List	   *tempList;
		Fjoin	   *fjoinNode;
		DatumPtr	results = (DatumPtr) palloc(nIters * sizeof(Datum));
		BoolPtr		alwaysDone = (BoolPtr) palloc(nIters * sizeof(bool));

		inner = lfirst(fjoinList);
		fjoinList = lnext(fjoinList);
		fjoinNode = (Fjoin) MakeFjoin(false,
									  nIters,
									  inner,
									  results,
									  alwaysDone);
		tempList = lcons(fjoinNode, fjoinList);
		newTlist = lappend(newTlist, tempList);
	}
	return newTlist;
	return tlist;				/* do nothing for now - ay 10/94 */
}

#endif
