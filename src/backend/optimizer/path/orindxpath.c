/*-------------------------------------------------------------------------
 *
 * orindxpath.c
 *	  Routines to find index paths that match a set of 'or' clauses
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/path/orindxpath.c,v 1.53 2003/08/04 02:40:00 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"


static void best_or_subclause_indices(Query *root, RelOptInfo *rel,
						  List *subclauses, List *indices,
						  IndexPath *pathnode);
static void best_or_subclause_index(Query *root, RelOptInfo *rel,
						Expr *subclause, List *indices,
						IndexOptInfo **retIndexInfo,
						List **retIndexQual,
						Cost *retStartupCost,
						Cost *retTotalCost);


/*
 * create_or_index_paths
 *	  Creates index paths for indices that match 'or' clauses.
 *	  create_index_paths() must already have been called.
 *
 * 'rel' is the relation entry for which the paths are to be created
 *
 * Returns nothing, but adds paths to rel->pathlist via add_path().
 */
void
create_or_index_paths(Query *root, RelOptInfo *rel)
{
	List	   *rlist;

	foreach(rlist, rel->baserestrictinfo)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(rlist);

		/*
		 * Check to see if this clause is an 'or' clause, and, if so,
		 * whether or not each of the subclauses within the 'or' clause
		 * has been matched by an index.  The information used was saved
		 * by create_index_paths().
		 */
		if (restriction_is_or_clause(restrictinfo) &&
			restrictinfo->subclauseindices)
		{
			bool		all_indexable = true;
			List	   *temp;

			foreach(temp, restrictinfo->subclauseindices)
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

				pathnode->path.pathtype = T_IndexScan;
				pathnode->path.parent = rel;

				/*
				 * This is an IndexScan, but the overall result will
				 * consist of tuples extracted in multiple passes (one for
				 * each subclause of the OR), so the result cannot be
				 * claimed to have any particular ordering.
				 */
				pathnode->path.pathkeys = NIL;

				/* It's not an innerjoin path. */
				pathnode->indexjoinclauses = NIL;

				/* We don't actually care what order the index scans in. */
				pathnode->indexscandir = NoMovementScanDirection;

				pathnode->rows = rel->rows;

				best_or_subclause_indices(root,
										  rel,
							   ((BoolExpr *) restrictinfo->clause)->args,
										  restrictinfo->subclauseindices,
										  pathnode);

				add_path(rel, (Path *) pathnode);
			}
		}
	}
}

/*
 * best_or_subclause_indices
 *	  Determines the best index to be used in conjunction with each subclause
 *	  of an 'or' clause and the cost of scanning a relation using these
 *	  indices.	The cost is the sum of the individual index costs, since
 *	  the executor will perform a scan for each subclause of the 'or'.
 *	  Returns a list of IndexOptInfo nodes, one per scan.
 *
 * This routine also creates the indexqual list that will be needed by
 * the executor.  The indexqual list has one entry for each scan of the base
 * rel, which is a sublist of indexqual conditions to apply in that scan.
 * The implicit semantics are AND across each sublist of quals, and OR across
 * the toplevel list (note that the executor takes care not to return any
 * single tuple more than once).
 *
 * 'rel' is the node of the relation on which the indexes are defined
 * 'subclauses' are the subclauses of the 'or' clause
 * 'indices' is a list of sublists of the IndexOptInfo nodes that matched
 *		each subclause of the 'or' clause
 * 'pathnode' is the IndexPath node being built.
 *
 * Results are returned by setting these fields of the passed pathnode:
 * 'indexinfo' gets a list of the index IndexOptInfo nodes, one per scan
 * 'indexqual' gets the constructed indexquals for the path (a list
 *		of sublists of clauses, one sublist per scan of the base rel)
 * 'startup_cost' and 'total_cost' get the complete path costs.
 *
 * 'startup_cost' is the startup cost for the first index scan only;
 * startup costs for later scans will be paid later on, so they just
 * get reflected in total_cost.
 *
 * NOTE: we choose each scan on the basis of its total cost, ignoring startup
 * cost.  This is reasonable as long as all index types have zero or small
 * startup cost, but we might have to work harder if any index types with
 * nontrivial startup cost are ever invented.
 */
static void
best_or_subclause_indices(Query *root,
						  RelOptInfo *rel,
						  List *subclauses,
						  List *indices,
						  IndexPath *pathnode)
{
	FastList	infos;
	FastList	quals;
	List	   *slist;

	FastListInit(&infos);
	FastListInit(&quals);
	pathnode->path.startup_cost = 0;
	pathnode->path.total_cost = 0;

	foreach(slist, subclauses)
	{
		Expr	   *subclause = lfirst(slist);
		IndexOptInfo *best_indexinfo;
		List	   *best_indexqual;
		Cost		best_startup_cost;
		Cost		best_total_cost;

		best_or_subclause_index(root, rel, subclause, lfirst(indices),
								&best_indexinfo, &best_indexqual,
								&best_startup_cost, &best_total_cost);

		Assert(best_indexinfo != NULL);

		FastAppend(&infos, best_indexinfo);
		FastAppend(&quals, best_indexqual);
		if (slist == subclauses)	/* first scan? */
			pathnode->path.startup_cost = best_startup_cost;
		pathnode->path.total_cost += best_total_cost;

		indices = lnext(indices);
	}

	pathnode->indexinfo = FastListValue(&infos);
	pathnode->indexqual = FastListValue(&quals);
}

/*
 * best_or_subclause_index
 *	  Determines which is the best index to be used with a subclause of an
 *	  'or' clause by estimating the cost of using each index and selecting
 *	  the least expensive (considering total cost only, for now).
 *
 * 'rel' is the node of the relation on which the index is defined
 * 'subclause' is the OR subclause being considered
 * 'indices' is a list of IndexOptInfo nodes that match the subclause
 * '*retIndexInfo' gets the IndexOptInfo of the best index
 * '*retIndexQual' gets a list of the indexqual conditions for the best index
 * '*retStartupCost' gets the startup cost of a scan with that index
 * '*retTotalCost' gets the total cost of a scan with that index
 */
static void
best_or_subclause_index(Query *root,
						RelOptInfo *rel,
						Expr *subclause,
						List *indices,
						IndexOptInfo **retIndexInfo,	/* return value */
						List **retIndexQual,	/* return value */
						Cost *retStartupCost,	/* return value */
						Cost *retTotalCost)		/* return value */
{
	bool		first_time = true;
	List	   *ilist;

	/* if we don't match anything, return zeros */
	*retIndexInfo = NULL;
	*retIndexQual = NIL;
	*retStartupCost = 0;
	*retTotalCost = 0;

	foreach(ilist, indices)
	{
		IndexOptInfo *index = (IndexOptInfo *) lfirst(ilist);
		List	   *indexqual;
		Path		subclause_path;

		Assert(IsA(index, IndexOptInfo));

		/* Convert this 'or' subclause to an indexqual list */
		indexqual = extract_or_indexqual_conditions(rel, index, subclause);

		cost_index(&subclause_path, root, rel, index, indexqual, false);

		if (first_time || subclause_path.total_cost < *retTotalCost)
		{
			*retIndexInfo = index;
			*retIndexQual = indexqual;
			*retStartupCost = subclause_path.startup_cost;
			*retTotalCost = subclause_path.total_cost;
			first_time = false;
		}
	}
}
