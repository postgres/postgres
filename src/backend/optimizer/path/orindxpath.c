/*-------------------------------------------------------------------------
 *
 * orindxpath.c
 *	  Routines to find index paths that match a set of 'or' clauses
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/path/orindxpath.c,v 1.36 2000/02/05 18:26:09 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/internal.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"


static void best_or_subclause_indices(Query *root, RelOptInfo *rel,
									  List *subclauses, List *indices,
									  List **indexquals,
									  List **indexids,
									  Cost *cost);
static void best_or_subclause_index(Query *root, RelOptInfo *rel,
									Expr *subclause, List *indices,
									List **retIndexQual,
									Oid *retIndexid,
									Cost *retCost);


/*
 * create_or_index_paths
 *	  Creates index paths for indices that match 'or' clauses.
 *	  create_index_paths() must already have been called.
 *
 * 'rel' is the relation entry for which the paths are to be created
 * 'clauses' is the list of available restriction clause nodes
 *
 * Returns a list of index path nodes.
 *
 */
List *
create_or_index_paths(Query *root,
					  RelOptInfo *rel, List *clauses)
{
	List	   *path_list = NIL;
	List	   *clist;

	foreach(clist, clauses)
	{
		RestrictInfo *clausenode = (RestrictInfo *) lfirst(clist);

		/*
		 * Check to see if this clause is an 'or' clause, and, if so,
		 * whether or not each of the subclauses within the 'or' clause
		 * has been matched by an index.  The information used was
		 * saved by create_index_paths().
		 */
		if (restriction_is_or_clause(clausenode) &&
			clausenode->subclauseindices)
		{
			bool		all_indexable = true;
			List	   *temp;

			foreach(temp, clausenode->subclauseindices)
			{
				if (lfirst(temp) == NIL)
				{
					all_indexable = false;
					break;
				}
			}
			if (all_indexable)
			{
				/*
				 * OK, build an IndexPath for this OR clause, using the
				 * best available index for each subclause.
				 */
				IndexPath  *pathnode = makeNode(IndexPath);
				List	   *indexquals;
				List	   *indexids;
				Cost		cost;

				best_or_subclause_indices(root,
										  rel,
										  clausenode->clause->args,
										  clausenode->subclauseindices,
										  &indexquals,
										  &indexids,
										  &cost);

				pathnode->path.pathtype = T_IndexScan;
				pathnode->path.parent = rel;
				/*
				 * This is an IndexScan, but the overall result will consist
				 * of tuples extracted in multiple passes (one for each
				 * subclause of the OR), so the result cannot be claimed
				 * to have any particular ordering.
				 */
				pathnode->path.pathkeys = NIL;

				pathnode->indexid = indexids;
				pathnode->indexqual = indexquals;
				pathnode->joinrelids = NIL;	/* no join clauses here */
				pathnode->path.path_cost = cost;

				path_list = lappend(path_list, pathnode);
			}
		}
	}

	return path_list;
}

/*
 * best_or_subclause_indices
 *	  Determines the best index to be used in conjunction with each subclause
 *	  of an 'or' clause and the cost of scanning a relation using these
 *	  indices.	The cost is the sum of the individual index costs, since
 *	  the executor will perform a scan for each subclause of the 'or'.
 *
 * This routine also creates the indexquals and indexids lists that will
 * be needed by the executor.  The indexquals list has one entry for each
 * scan of the base rel, which is a sublist of indexqual conditions to
 * apply in that scan.  The implicit semantics are AND across each sublist
 * of quals, and OR across the toplevel list (note that the executor
 * takes care not to return any single tuple more than once).  The indexids
 * list gives the index to be used in each scan.
 *
 * 'rel' is the node of the relation on which the indexes are defined
 * 'subclauses' are the subclauses of the 'or' clause
 * 'indices' is a list of sublists of the IndexOptInfo nodes that matched
 *		each subclause of the 'or' clause
 * '*indexquals' gets the constructed indexquals for the path (a list
 *		of sublists of clauses, one sublist per scan of the base rel)
 * '*indexids' gets a list of the index OIDs for each scan of the rel
 * '*cost' gets the total cost of the path
 */
static void
best_or_subclause_indices(Query *root,
						  RelOptInfo *rel,
						  List *subclauses,
						  List *indices,
						  List **indexquals,	/* return value */
						  List **indexids,		/* return value */
						  Cost *cost)			/* return value */
{
	List	   *slist;

	*indexquals = NIL;
	*indexids = NIL;
	*cost = (Cost) 0.0;

	foreach(slist, subclauses)
	{
		Expr	   *subclause = lfirst(slist);
		List	   *best_indexqual;
		Oid			best_indexid;
		Cost		best_cost;

		best_or_subclause_index(root, rel, subclause, lfirst(indices),
								&best_indexqual, &best_indexid, &best_cost);

		Assert(best_indexid != InvalidOid);

		*indexquals = lappend(*indexquals, best_indexqual);
		*indexids = lappendi(*indexids, best_indexid);
		*cost += best_cost;

		indices = lnext(indices);
	}
}

/*
 * best_or_subclause_index
 *	  Determines which is the best index to be used with a subclause of
 *	  an 'or' clause by estimating the cost of using each index and selecting
 *	  the least expensive.
 *
 * 'rel' is the node of the relation on which the index is defined
 * 'subclause' is the OR subclause being considered
 * 'indices' is a list of IndexOptInfo nodes that match the subclause
 * '*retIndexQual' gets a list of the indexqual conditions for the best index
 * '*retIndexid' gets the OID of the best index
 * '*retCost' gets the cost of a scan with that index
 */
static void
best_or_subclause_index(Query *root,
						RelOptInfo *rel,
						Expr *subclause,
						List *indices,
						List **retIndexQual,	/* return value */
						Oid *retIndexid,		/* return value */
						Cost *retCost)			/* return value */
{
	bool		first_time = true;
	List	   *ilist;

	/* if we don't match anything, return zeros */
	*retIndexQual = NIL;
	*retIndexid = InvalidOid;
	*retCost = 0.0;

	foreach(ilist, indices)
	{
		IndexOptInfo *index = (IndexOptInfo *) lfirst(ilist);
		List	   *indexqual;
		Cost		subcost;

		Assert(IsA(index, IndexOptInfo));

		/* Convert this 'or' subclause to an indexqual list */
		indexqual = extract_or_indexqual_conditions(rel, index, subclause);

		subcost = cost_index(root, rel, index, indexqual,
							 false);

		if (first_time || subcost < *retCost)
		{
			*retIndexQual = indexqual;
			*retIndexid = index->indexoid;
			*retCost = subcost;
			first_time = false;
		}
	}
}
