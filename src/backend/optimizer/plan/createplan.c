/*-------------------------------------------------------------------------
 *
 * createplan.c
 *	  Routines to create the desired plan for processing a query.
 *	  Planning is complete, we just need to convert the selected
 *	  Path into a Plan.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/plan/createplan.c,v 1.79 2000/01/15 02:59:30 petere Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>

#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/internal.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


static List *switch_outer(List *clauses);
static int set_tlist_sort_info(List *tlist, List *pathkeys);
static Scan *create_scan_node(Query *root, Path *best_path, List *tlist);
static Join *create_join_node(Query *root, JoinPath *best_path, List *tlist);
static SeqScan *create_seqscan_node(Path *best_path, List *tlist,
					List *scan_clauses);
static IndexScan *create_indexscan_node(Query *root, IndexPath *best_path,
										List *tlist, List *scan_clauses);
static TidScan *create_tidscan_node(TidPath *best_path, List *tlist,
					  List *scan_clauses); 
static NestLoop *create_nestloop_node(NestPath *best_path, List *tlist,
					 List *clauses, Plan *outer_node, List *outer_tlist,
					 Plan *inner_node, List *inner_tlist);
static MergeJoin *create_mergejoin_node(MergePath *best_path, List *tlist,
					  List *clauses, Plan *outer_node, List *outer_tlist,
					  Plan *inner_node, List *inner_tlist);
static HashJoin *create_hashjoin_node(HashPath *best_path, List *tlist,
					 List *clauses, Plan *outer_node, List *outer_tlist,
					 Plan *inner_node, List *inner_tlist);
static List *fix_indxqual_references(List *indexquals, IndexPath *index_path);
static List *fix_indxqual_sublist(List *indexqual, int baserelid, Oid relam,
								  Form_pg_index index);
static Node *fix_indxqual_operand(Node *node, int baserelid,
								  Form_pg_index index,
								  Oid *opclass);
static IndexScan *make_indexscan(List *qptlist, List *qpqual, Index scanrelid,
			   List *indxid, List *indxqual, List *indxqualorig);
static TidScan *make_tidscan(List *qptlist, List *qpqual, Index scanrelid,
                        List *tideval);
static NestLoop *make_nestloop(List *qptlist, List *qpqual, Plan *lefttree,
			  Plan *righttree);
static HashJoin *make_hashjoin(List *tlist, List *qpqual,
			  List *hashclauses, Plan *lefttree, Plan *righttree);
static Hash *make_hash(List *tlist, Var *hashkey, Plan *lefttree);
static MergeJoin *make_mergejoin(List *tlist, List *qpqual,
			   List *mergeclauses, Plan *righttree, Plan *lefttree);
static Material *make_material(List *tlist, Oid nonameid, Plan *lefttree,
			  int keycount);
static void copy_path_costsize(Plan *dest, Path *src);
static void copy_plan_costsize(Plan *dest, Plan *src);

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
 *	  Returns the access plan.
 */
Plan *
create_plan(Query *root, Path *best_path)
{
	List	   *tlist = best_path->parent->targetlist;
	Plan	   *plan_node = (Plan *) NULL;

	switch (best_path->pathtype)
	{
		case T_IndexScan:
		case T_SeqScan:
		case T_TidScan:
			plan_node = (Plan *) create_scan_node(root, best_path, tlist);
			break;
		case T_HashJoin:
		case T_MergeJoin:
		case T_NestLoop:
			plan_node = (Plan *) create_join_node(root,
												  (JoinPath *) best_path,
												  tlist);
			break;
		default:
			elog(ERROR, "create_plan: unknown pathtype %d",
				 best_path->pathtype);
			break;
	}

#ifdef NOT_USED					/* fix xfunc */
	/* sort clauses by cost/(1-selectivity) -- JMH 2/26/92 */
	if (XfuncMode != XFUNC_OFF)
	{
		set_qpqual((Plan) plan_node,
				   lisp_qsort(get_qpqual((Plan) plan_node),
							  xfunc_clause_compare));
		if (XfuncMode != XFUNC_NOR)
			/* sort the disjuncts within each clause by cost -- JMH 3/4/92 */
			xfunc_disjunct_sort(plan_node->qpqual);
	}
#endif

	return plan_node;
}

/*
 * create_scan_node
 *	 Create a scan path for the parent relation of 'best_path'.
 *
 *	 tlist is the targetlist for the base relation scanned by 'best_path'
 *
 *	 Returns the scan node.
 */
static Scan *
create_scan_node(Query *root, Path *best_path, List *tlist)
{
	Scan	   *node = NULL;
	List	   *scan_clauses;

	/*
	 * Extract the relevant restriction clauses from the parent relation;
	 * the executor must apply all these restrictions during the scan.
	 */
	scan_clauses = get_actual_clauses(best_path->parent->restrictinfo);

	switch (best_path->pathtype)
	{
		case T_SeqScan:
			node = (Scan *) create_seqscan_node(best_path, tlist, scan_clauses);
			break;

		case T_IndexScan:
			node = (Scan *) create_indexscan_node(root,
												  (IndexPath *) best_path,
												  tlist,
												  scan_clauses);
			break;

		case T_TidScan:
			node = (Scan *) create_tidscan_node((TidPath *) best_path,
												  tlist,
												  scan_clauses);
			break;

		default:
			elog(ERROR, "create_scan_node: unknown node type: %d",
				 best_path->pathtype);
			break;
	}

	return node;
}

/*
 * create_join_node
 *	  Create a join path for 'best_path' and(recursively) paths for its
 *	  inner and outer paths.
 *
 *	  'tlist' is the targetlist for the join relation corresponding to
 *		'best_path'
 *
 *	  Returns the join node.
 */
static Join *
create_join_node(Query *root, JoinPath *best_path, List *tlist)
{
	Plan	   *outer_node;
	List	   *outer_tlist;
	Plan	   *inner_node;
	List	   *inner_tlist;
	List	   *clauses;
	Join	   *retval = NULL;

	outer_node = create_plan(root, best_path->outerjoinpath);
	outer_tlist = outer_node->targetlist;

	inner_node = create_plan(root, best_path->innerjoinpath);
	inner_tlist = inner_node->targetlist;

	clauses = get_actual_clauses(best_path->path.parent->restrictinfo);

	switch (best_path->path.pathtype)
	{
		case T_MergeJoin:
			retval = (Join *) create_mergejoin_node((MergePath *) best_path,
													tlist,
													clauses,
													outer_node,
													outer_tlist,
													inner_node,
													inner_tlist);
			break;
		case T_HashJoin:
			retval = (Join *) create_hashjoin_node((HashPath *) best_path,
												   tlist,
												   clauses,
												   outer_node,
												   outer_tlist,
												   inner_node,
												   inner_tlist);
			break;
		case T_NestLoop:
			retval = (Join *) create_nestloop_node((NestPath *) best_path,
												   tlist,
												   clauses,
												   outer_node,
												   outer_tlist,
												   inner_node,
												   inner_tlist);
			break;
		default:
			elog(ERROR, "create_join_node: unknown node type: %d",
				 best_path->path.pathtype);
	}

#ifdef NOT_USED
	/*
	 * * Expensive function pullups may have pulled local predicates *
	 * into this path node.  Put them in the qpqual of the plan node. *
	 * JMH, 6/15/92
	 */
	if (get_loc_restrictinfo(best_path) != NIL)
		set_qpqual((Plan) retval,
				   nconc(get_qpqual((Plan) retval),
						 get_actual_clauses(get_loc_restrictinfo(best_path))));
#endif

	return retval;
}

/*****************************************************************************
 *
 *	BASE-RELATION SCAN METHODS
 *
 *****************************************************************************/


/*
 * create_seqscan_node
 *	 Returns a seqscan node for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static SeqScan *
create_seqscan_node(Path *best_path, List *tlist, List *scan_clauses)
{
	SeqScan    *scan_node;
	Index		scan_relid;

	/* there should be exactly one base rel involved... */
	Assert(length(best_path->parent->relids) == 1);

	scan_relid = (Index) lfirsti(best_path->parent->relids);

	scan_node = make_seqscan(tlist,
							 scan_clauses,
							 scan_relid);

	copy_path_costsize(&scan_node->plan, best_path);

	return scan_node;
}

/*
 * create_indexscan_node
 *	  Returns a indexscan node for the base relation scanned by 'best_path'
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
create_indexscan_node(Query *root,
					  IndexPath *best_path,
					  List *tlist,
					  List *scan_clauses)
{
	List	   *indxqual = best_path->indexqual;
	List	   *qpqual;
	List	   *fixed_indxqual;
	List	   *ixid;
	IndexScan  *scan_node;
	bool		lossy = false;
	double		plan_rows;

	/* there should be exactly one base rel involved... */
	Assert(length(best_path->path.parent->relids) == 1);

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
	 * Since the indexquals were generated from the restriction clauses
	 * given by scan_clauses, there will normally be some duplications
	 * between the lists.  We get rid of the duplicates, then add back
	 * if lossy.
	 *
	 * If this indexscan is a nestloop-join inner indexscan (as indicated
	 * by having nonempty joinrelids), then it uses indexqual conditions
	 * that are not part of the relation's restriction clauses.  The rows
	 * estimate stored in the relation's RelOptInfo will be an overestimate
	 * because it did not take these extra conditions into account.  So,
	 * in this case we recompute the selectivity of the whole scan ---
	 * considering both indexqual and qpqual --- rather than using the
	 * RelOptInfo's rows value.  Since clausesel.c assumes it's working on
	 * minimized (no duplicates) expressions, we have to do that while we
	 * have the duplicate-free qpqual available.
	 */
	plan_rows = best_path->path.parent->rows; /* OK unless nestloop inner */

	if (length(indxqual) > 1)
	{
		/*
		 * Build an expression representation of the indexqual, expanding
		 * the implicit OR and AND semantics of the first- and second-level
		 * lists.
		 */
		List	   *orclauses = NIL;
		List	   *orclause;
		Expr	   *indxqual_expr;

		foreach(orclause, indxqual)
			orclauses = lappend(orclauses,
								make_ands_explicit(lfirst(orclause)));
		indxqual_expr = make_orclause(orclauses);

		qpqual = set_difference(scan_clauses,
								lcons(indxqual_expr, NIL));

		if (best_path->joinrelids)
		{
			/* recompute output row estimate using all available quals */
			plan_rows = best_path->path.parent->tuples *
				clauselist_selec(root, lcons(indxqual_expr, qpqual));
		}

		if (lossy)
			qpqual = lappend(qpqual, copyObject(indxqual_expr));
	}
	else if (indxqual != NIL)
	{
		/* Here, we can simply treat the first sublist as an independent
		 * set of qual expressions, since there is no top-level OR behavior.
		 */
		List	   *indxqual_list = lfirst(indxqual);

		qpqual = set_difference(scan_clauses, indxqual_list);

		if (best_path->joinrelids)
		{
			/* recompute output row estimate using all available quals */
			plan_rows = best_path->path.parent->tuples *
				clauselist_selec(root, nconc(listCopy(indxqual_list), qpqual));
		}

		if (lossy)
			qpqual = nconc(qpqual, (List *) copyObject(indxqual_list));
	}
	else
		qpqual = scan_clauses;

	/* The executor needs a copy with the indexkey on the left of each clause
	 * and with index attr numbers substituted for table ones.
	 */
	fixed_indxqual = fix_indxqual_references(indxqual, best_path);

	scan_node = make_indexscan(tlist,
							   qpqual,
							   lfirsti(best_path->path.parent->relids),
							   best_path->indexid,
							   fixed_indxqual,
							   indxqual);

	copy_path_costsize(&scan_node->scan.plan, &best_path->path);
	scan_node->scan.plan.plan_rows = plan_rows;

	return scan_node;
}

static TidScan *
make_tidscan(List *qptlist,
			List *qpqual,
			Index scanrelid,	
			List *tideval)
{
	TidScan	*node = makeNode(TidScan);
	Plan	*plan = &node->scan.plan;

	plan->cost = 0;
	plan->plan_rows = 0;
	plan->plan_width = 0;
	plan->state = (EState *) NULL;
	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->tideval = copyObject(tideval); /* XXX do we really need a copy? */
	node->needRescan = false;
	node->scan.scanstate = (CommonScanState *) NULL;

	return node;
}

/*
 * create_tidscan_node
 *	 Returns a tidscan node for the base relation scanned by 'best_path'
 *	 with restriction clauses 'scan_clauses' and targetlist 'tlist'.
 */
static TidScan *
create_tidscan_node(TidPath *best_path, List *tlist, List *scan_clauses)
{
	TidScan	*scan_node;
	Index	scan_relid;

	/* there should be exactly one base rel involved... */
	Assert(length(best_path->path.parent->relids) == 1);

	scan_relid = (Index) lfirsti(best_path->path.parent->relids);

	scan_node = make_tidscan(tlist,
							 scan_clauses,
							 scan_relid,
							 best_path->tideval);

	if (best_path->unjoined_relids)
		scan_node->needRescan = true; 

	copy_path_costsize(&scan_node->scan.plan, &best_path->path);

	return scan_node;
}

/*****************************************************************************
 *
 *	JOIN METHODS
 *
 * A general note about join_references() processing in these routines:
 * once we have changed a Var node to refer to a subplan output rather than
 * the original relation, it is no longer equal() to an unmodified Var node
 * for the same var.  So, we cannot easily compare reference-adjusted qual
 * clauses to clauses that have not been adjusted.  Fortunately, that
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
create_nestloop_node(NestPath *best_path,
					 List *tlist,
					 List *clauses,
					 Plan *outer_node,
					 List *outer_tlist,
					 Plan *inner_node,
					 List *inner_tlist)
{
	NestLoop   *join_node;

	if (IsA(inner_node, IndexScan))
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
		IndexScan  *innerscan = (IndexScan *) inner_node;
		List	   *indxqualorig = innerscan->indxqualorig;

		/* No work needed if indxqual refers only to its own relation... */
		if (NumRelids((Node *) indxqualorig) > 1)
		{
			Index		innerrel = innerscan->scan.scanrelid;

			/* Remove redundant tests from my clauses, if possible.
			 * Note we must compare against indxqualorig not the "fixed"
			 * indxqual (which has index attnos instead of relation attnos,
			 * and may have been commuted as well).
			 */
			if (length(indxqualorig) == 1) /* single indexscan? */
				clauses = set_difference(clauses, lfirst(indxqualorig));

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
			if (NumRelids((Node *) inner_node->qual) > 1)
				inner_node->qual = join_references(inner_node->qual,
												   outer_tlist,
												   NIL,
												   innerrel);
		}
	}
	else if (IsA(inner_node, TidScan))
	{
		List	*inner_tideval = ((TidScan *) inner_node)->tideval;
		TidScan	*innerscan = (TidScan *) inner_node; 
		((TidScan *) inner_node)->tideval = join_references(inner_tideval, outer_tlist, inner_tlist, innerscan->scan.scanrelid);
	} 
	else if (IsA_Join(inner_node))
	{
		/*
		 * Materialize the inner join for speed reasons.
		 *
		 * XXX It is probably *not* always fastest to materialize an inner
		 * join --- how can we estimate whether this is a good thing to do?
		 */
		inner_node = (Plan *) make_noname(inner_tlist,
										  NIL,
										  inner_node);
	}

	join_node = make_nestloop(tlist,
							  join_references(clauses,
											  outer_tlist,
											  inner_tlist,
											  (Index) 0),
							  outer_node,
							  inner_node);

	copy_path_costsize(&join_node->join, &best_path->path);

	return join_node;
}

static MergeJoin *
create_mergejoin_node(MergePath *best_path,
					  List *tlist,
					  List *clauses,
					  Plan *outer_node,
					  List *outer_tlist,
					  Plan *inner_node,
					  List *inner_tlist)
{
	List	   *qpqual,
			   *mergeclauses;
	MergeJoin  *join_node;

	/*
	 * Remove the mergeclauses from the list of join qual clauses,
	 * leaving the list of quals that must be checked as qpquals.
	 * Set those clauses to contain INNER/OUTER var references.
	 */
	qpqual = join_references(set_difference(clauses,
											best_path->path_mergeclauses),
							 outer_tlist,
							 inner_tlist,
							 (Index) 0);

	/*
	 * Now set the references in the mergeclauses and rearrange them so
	 * that the outer variable is always on the left.
	 */
	mergeclauses = switch_outer(join_references(best_path->path_mergeclauses,
												outer_tlist,
												inner_tlist,
												(Index) 0));

	/*
	 * Create explicit sort nodes for the outer and inner join paths if
	 * necessary.  The sort cost was already accounted for in the path.
	 */
	if (best_path->outersortkeys)
		outer_node = (Plan *) make_noname(outer_tlist,
										  best_path->outersortkeys,
										  outer_node);

	if (best_path->innersortkeys)
		inner_node = (Plan *) make_noname(inner_tlist,
										  best_path->innersortkeys,
										  inner_node);

	join_node = make_mergejoin(tlist,
							   qpqual,
							   mergeclauses,
							   inner_node,
							   outer_node);

	copy_path_costsize(&join_node->join, &best_path->jpath.path);

	return join_node;
}

static HashJoin *
create_hashjoin_node(HashPath *best_path,
					 List *tlist,
					 List *clauses,
					 Plan *outer_node,
					 List *outer_tlist,
					 Plan *inner_node,
					 List *inner_tlist)
{
	List	   *qpqual;
	List	   *hashclauses;
	HashJoin   *join_node;
	Hash	   *hash_node;
	Var		   *innerhashkey;

	/*
	 * NOTE: there will always be exactly one hashclause in the list
	 * best_path->path_hashclauses (cf. hash_inner_and_outer()).
	 * We represent it as a list anyway, for convenience with routines
	 * that want to work on lists of clauses.
	 */

	/*
	 * Remove the hashclauses from the list of join qual clauses,
	 * leaving the list of quals that must be checked as qpquals.
	 * Set those clauses to contain INNER/OUTER var references.
	 */
	qpqual = join_references(set_difference(clauses,
											best_path->path_hashclauses),
							 outer_tlist,
							 inner_tlist,
							 (Index) 0);

	/*
	 * Now set the references in the hashclauses and rearrange them so
	 * that the outer variable is always on the left.
	 */
	hashclauses = switch_outer(join_references(best_path->path_hashclauses,
											   outer_tlist,
											   inner_tlist,
											   (Index) 0));

	/* Now the righthand op of the sole hashclause is the inner hash key. */
	innerhashkey = get_rightop(lfirst(hashclauses));

	/*
	 * Build the hash node and hash join node.
	 */
	hash_node = make_hash(inner_tlist, innerhashkey, inner_node);
	join_node = make_hashjoin(tlist,
							  qpqual,
							  hashclauses,
							  outer_node,
							  (Plan *) hash_node);

	copy_path_costsize(&join_node->join, &best_path->jpath.path);

	return join_node;
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
 *	* Var nodes representing index keys must have varattno equal to the
 *	  index's attribute number, not the attribute number in the original rel.
 *	* indxpath.c may have selected an index that is binary-compatible with
 *	  the actual expression operator, but not the same; we must replace the
 *	  expression's operator with the binary-compatible equivalent operator
 *	  that the index will recognize.
 *	* If the index key is on the right, commute the clause to put it on the
 *	  left.  (Someday the executor might not need this, but for now it does.)
 *
 * This code used to be entirely bogus for multi-index scans.  Now it keeps
 * track of which index applies to each subgroup of index qual clauses...
 *
 * Returns a modified copy of the indexqual list --- the original is not
 * changed.
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
 * left, and then change its varno.  (We do not need to change the other side
 * of the clause.)  Also change the operator if necessary.
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

		/* Which side is the indexkey on?
		 *
		 * get_relattval sets flag&SEL_RIGHT if the indexkey is on the LEFT.
		 */
		get_relattval((Node *) clause, baserelid,
					  &relid, &attno, &constval, &flag);

		/* Copy enough structure to allow commuting and replacing an operand
		 * without changing original clause.
		 */
		newclause = make_clause(clause->opType, clause->oper,
								listCopy(clause->args));

		/* If the indexkey is on the right, commute the clause. */
		if ((flag & SEL_RIGHT) == 0)
			CommuteClause(newclause);

		/* Now, determine which index attribute this is,
		 * change the indexkey operand as needed,
		 * and get the index opclass.
		 */
		lfirst(newclause->args) = fix_indxqual_operand(lfirst(newclause->args),
													   baserelid,
													   index,
													   &opclass);

		/* Substitute the appropriate operator if the expression operator
		 * is merely binary-compatible with the index.  This shouldn't fail,
		 * since indxpath.c found it before...
		 */
		newopno = indexable_operator(newclause, opclass, relam, true);
		if (newopno == InvalidOid)
			elog(ERROR, "fix_indxqual_sublist: failed to find substitute op");
		if (newopno != ((Oper *) newclause->oper)->opno)
		{
			newclause->oper = (Node *) copyObject(newclause->oper);
			((Oper *) newclause->oper)->opno = newopno;
		}

		fixed_qual = lappend(fixed_qual, newclause);
	}
	return fixed_qual;
}

static Node *
fix_indxqual_operand(Node *node, int baserelid, Form_pg_index index,
					 Oid *opclass)
{
	if (IsA(node, Var))
	{
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
	 * Else, it must be a func expression representing a functional index.
	 *
	 * Currently, there is no need for us to do anything here for
	 * functional indexes.  If nodeIndexscan.c sees a func clause as the left
	 * or right-hand toplevel operand of an indexqual, it assumes that that is
	 * a reference to the functional index's value and makes the appropriate
	 * substitution.  (It would be cleaner to make the substitution here, I
	 * think --- suspect this issue if a join clause involving a function call
	 * misbehaves...)
	 */

	/* indclass[0] is the only class of a functional index */
	*opclass = index->indclass[0];

	/* return the unmodified node */
	return node;
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
 * set_tlist_sort_info
 *	  Sets the reskey and reskeyop fields of resdom nodes in a target list
 *	  for a sort node.
 *
 * 'tlist' is the target list (which is modified in-place).
 *			tlist's reskey fields must be clear to start with.
 * 'pathkeys' is the desired pathkeys for the sort.  NIL means no sort.
 *
 * Returns the number of sort keys assigned (which might be less than
 * length(pathkeys)!)
 */
static int
set_tlist_sort_info(List *tlist, List *pathkeys)
{
	int			keysassigned = 0;
	List	   *i;

	foreach(i, pathkeys)
	{
		List		   *keysublist = (List *) lfirst(i);
		PathKeyItem	   *pathkey = NULL;
		Resdom		   *resdom = NULL;
		List		   *j;

		/*
		 * We can sort by any one of the sort key items listed in this
		 * sublist.  For now, we take the first one that corresponds to
		 * an available Var in the tlist.
		 *
		 * XXX if we have a choice, is there any way of figuring out which
		 * might be cheapest to execute?  (For example, int4lt is likely
		 * much cheaper to execute than numericlt, but both might appear in
		 * the same pathkey sublist...)  Not clear that we ever will have
		 * a choice in practice, so it may not matter.
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
			elog(ERROR, "set_tlist_sort_info: cannot find tlist item to sort");

		/*
		 * The resdom might be already marked as a sort key, if the pathkeys
		 * contain duplicate entries.  (This can happen in scenarios where
		 * multiple mergejoinable clauses mention the same var, for example.)
		 * In that case the current pathkey is essentially a no-op, because
		 * only one value can be seen within any subgroup where it would be
		 * consulted.  We can ignore it.
		 */
		if (resdom->reskey == 0)
		{
			/* OK, mark it as a sort key and set the sort operator regproc */
			resdom->reskey = ++keysassigned;
			resdom->reskeyop = get_opcode(pathkey->sortop);
		}
	}

	return keysassigned;
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
		dest->cost = src->path_cost;
		dest->plan_rows = src->parent->rows;
		dest->plan_width = src->parent->width;
	}
	else
	{
		dest->cost = 0;
		dest->plan_rows = 0;
		dest->plan_width = 0;
	}
}

/*
 * Copy cost and size info from a lower plan node to an inserted node.
 * This is not critical, since the decisions have already been made,
 * but it helps produce more reasonable-looking EXPLAIN output.
 */
static void
copy_plan_costsize(Plan *dest, Plan *src)
{
	if (src)
	{
		dest->cost = src->cost;
		dest->plan_rows = src->plan_rows;
		dest->plan_width = src->plan_width;
	}
	else
	{
		dest->cost = 0;
		dest->plan_rows = 0;
		dest->plan_width = 0;
	}
}


/*****************************************************************************
 *
 *
 *****************************************************************************/

/*
 * make_noname
 *	  Create plan node to sort or materialize relations into noname.
 *
 *	  'tlist' is the target list of the scan to be sorted or materialized
 *	  'pathkeys' is the list of pathkeys by which the result is to be sorted
 *			(NIL implies no sort needed, just materialize it)
 *	  'subplan' is the node which yields input tuples
 */
Noname *
make_noname(List *tlist,
			List *pathkeys,
			Plan *subplan)
{
	List	   *noname_tlist;
	int			numsortkeys;
	Plan	   *retval;

	/* Create a new target list for the noname, with sort keys set. */
	noname_tlist = new_unsorted_tlist(tlist);
	numsortkeys = set_tlist_sort_info(noname_tlist, pathkeys);

	if (numsortkeys > 0)
	{
		/* need to sort */
		retval = (Plan *) make_sort(noname_tlist,
									_NONAME_RELATION_ID_,
									subplan,
									numsortkeys);
	}
	else
	{
		/* no sort */
		retval = (Plan *) make_material(noname_tlist,
										_NONAME_RELATION_ID_,
										subplan,
										0);
	}

	return (Noname *) retval;
}


SeqScan    *
make_seqscan(List *qptlist,
			 List *qpqual,
			 Index scanrelid)
{
	SeqScan    *node = makeNode(SeqScan);
	Plan	   *plan = &node->plan;

	copy_plan_costsize(plan, NULL);
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
			   List *indxqualorig)
{
	IndexScan  *node = makeNode(IndexScan);
	Plan	   *plan = &node->scan.plan;

	copy_plan_costsize(plan, NULL);
	plan->state = (EState *) NULL;
	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	node->scan.scanrelid = scanrelid;
	node->indxid = indxid;
	node->indxqual = indxqual;
	node->indxqualorig = indxqualorig;
	node->indxorderdir = NoMovementScanDirection;
	node->scan.scanstate = (CommonScanState *) NULL;

	return node;
}


static NestLoop *
make_nestloop(List *qptlist,
			  List *qpqual,
			  Plan *lefttree,
			  Plan *righttree)
{
	NestLoop   *node = makeNode(NestLoop);
	Plan	   *plan = &node->join;

	/*
	 * this cost estimate is entirely bogus... hopefully it will be
	 * overwritten by caller.
	 */
	plan->cost = (lefttree ? lefttree->cost : 0) +
		(righttree ? righttree->cost : 0);
	plan->state = (EState *) NULL;
	plan->targetlist = qptlist;
	plan->qual = qpqual;
	plan->lefttree = lefttree;
	plan->righttree = righttree;
	node->nlstate = (NestLoopState *) NULL;

	return node;
}

static HashJoin *
make_hashjoin(List *tlist,
			  List *qpqual,
			  List *hashclauses,
			  Plan *lefttree,
			  Plan *righttree)
{
	HashJoin   *node = makeNode(HashJoin);
	Plan	   *plan = &node->join;

	/*
	 * this cost estimate is entirely bogus... hopefully it will be
	 * overwritten by caller.
	 */
	plan->cost = (lefttree ? lefttree->cost : 0) +
		(righttree ? righttree->cost : 0);
	plan->state = (EState *) NULL;
	plan->targetlist = tlist;
	plan->qual = qpqual;
	plan->lefttree = lefttree;
	plan->righttree = righttree;
	node->hashclauses = hashclauses;
	node->hashdone = false;

	return node;
}

static Hash *
make_hash(List *tlist, Var *hashkey, Plan *lefttree)
{
	Hash	   *node = makeNode(Hash);
	Plan	   *plan = &node->plan;

	copy_plan_costsize(plan, lefttree);
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
			   List *qpqual,
			   List *mergeclauses,
			   Plan *righttree,
			   Plan *lefttree)
{
	MergeJoin  *node = makeNode(MergeJoin);
	Plan	   *plan = &node->join;

	/*
	 * this cost estimate is entirely bogus... hopefully it will be
	 * overwritten by caller.
	 */
	plan->cost = (lefttree ? lefttree->cost : 0) +
		(righttree ? righttree->cost : 0);
	plan->state = (EState *) NULL;
	plan->targetlist = tlist;
	plan->qual = qpqual;
	plan->lefttree = lefttree;
	plan->righttree = righttree;
	node->mergeclauses = mergeclauses;

	return node;
}

Sort *
make_sort(List *tlist, Oid nonameid, Plan *lefttree, int keycount)
{
	Sort	   *node = makeNode(Sort);
	Plan	   *plan = &node->plan;

	copy_plan_costsize(plan, lefttree);
	plan->cost += cost_sort(NIL, plan->plan_rows, plan->plan_width);
	plan->state = (EState *) NULL;
	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;
	node->nonameid = nonameid;
	node->keycount = keycount;

	return node;
}

static Material *
make_material(List *tlist,
			  Oid nonameid,
			  Plan *lefttree,
			  int keycount)
{
	Material   *node = makeNode(Material);
	Plan	   *plan = &node->plan;

	copy_plan_costsize(plan, lefttree);
	plan->state = (EState *) NULL;
	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;
	node->nonameid = nonameid;
	node->keycount = keycount;

	return node;
}

Agg *
make_agg(List *tlist, Plan *lefttree)
{
	Agg		   *node = makeNode(Agg);

	copy_plan_costsize(&node->plan, lefttree);
	node->plan.state = (EState *) NULL;
	node->plan.qual = NULL;
	node->plan.targetlist = tlist;
	node->plan.lefttree = lefttree;
	node->plan.righttree = (Plan *) NULL;

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

	copy_plan_costsize(&node->plan, lefttree);
	node->plan.state = (EState *) NULL;
	node->plan.qual = NULL;
	node->plan.targetlist = tlist;
	node->plan.lefttree = lefttree;
	node->plan.righttree = (Plan *) NULL;
	node->tuplePerGroup = tuplePerGroup;
	node->numCols = ngrp;
	node->grpColIdx = grpColIdx;

	return node;
}

/*
 * The uniqueAttr argument must be a null-terminated string,
 * either the name of the attribute to select unique on
 * or "*"
 */

Unique *
make_unique(List *tlist, Plan *lefttree, char *uniqueAttr)
{
	Unique	   *node = makeNode(Unique);
	Plan	   *plan = &node->plan;

	copy_plan_costsize(plan, lefttree);
	plan->state = (EState *) NULL;
	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = lefttree;
	plan->righttree = NULL;
	node->nonameid = _NONAME_RELATION_ID_;
	node->keycount = 0;
	if (strcmp(uniqueAttr, "*") == 0)
		node->uniqueAttr = NULL;
	else
		node->uniqueAttr = pstrdup(uniqueAttr);
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
