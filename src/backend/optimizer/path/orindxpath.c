/*-------------------------------------------------------------------------
 *
 * orindxpath.c
 *	  Routines to find index paths that match a set of OR clauses
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/path/orindxpath.c,v 1.55 2004/01/04 00:07:32 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"


static IndexPath *best_or_subclause_indices(Query *root, RelOptInfo *rel,
											List *subclauses);
static bool best_or_subclause_index(Query *root,
						RelOptInfo *rel,
						Expr *subclause,
						IndexOptInfo **retIndexInfo,
						List **retIndexQual,
						Cost *retStartupCost,
						Cost *retTotalCost);


/*
 * create_or_index_paths
 *	  Creates multi-scan index paths for indices that match OR clauses.
 *
 * 'rel' is the relation entry for which the paths are to be created
 *
 * Returns nothing, but adds paths to rel->pathlist via add_path().
 *
 * Note: create_index_paths() must have been run already, since it does
 * the heavy lifting to determine whether partial indexes may be used.
 */
void
create_or_index_paths(Query *root, RelOptInfo *rel)
{
	List	   *i;

	/*
	 * Check each restriction clause to see if it is an OR clause, and if so,
	 * try to make a path using it.
	 */
	foreach(i, rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(i);

		if (restriction_is_or_clause(rinfo))
		{
			IndexPath  *pathnode;

			pathnode = best_or_subclause_indices(root,
												 rel,
							   ((BoolExpr *) rinfo->orclause)->args);

			if (pathnode)
				add_path(rel, (Path *) pathnode);
		}
	}

	/*
	 * Also consider join clauses that are ORs.  Although a join clause
	 * must reference other relations overall, an OR of ANDs clause might
	 * contain sub-clauses that reference just our relation and can be
	 * used to build a non-join indexscan.  For example consider
	 *		WHERE (a.x = 42 AND b.y = 43) OR (a.x = 44 AND b.z = 45);
	 * We could build an OR indexscan on a.x using those subclauses.
	 *
	 * XXX don't enable this code quite yet.  Although the plans it creates
	 * are correct, and possibly even useful, we are totally confused about
	 * the number of rows returned, leading to poor choices of join plans
	 * above the indexscan.  Need to restructure the way join sizes are
	 * calculated before this will really work.
	 */
#ifdef NOT_YET
	foreach(i, rel->joininfo)
	{
		JoinInfo   *joininfo = (JoinInfo *) lfirst(i);
		List	   *j;

		foreach(j, joininfo->jinfo_restrictinfo)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(j);

			if (restriction_is_or_clause(rinfo))
			{
				IndexPath  *pathnode;

				pathnode = best_or_subclause_indices(root,
													 rel,
										((BoolExpr *) rinfo->orclause)->args);

				if (pathnode)
					add_path(rel, (Path *) pathnode);
			}
		}
	}
#endif
}

/*
 * best_or_subclause_indices
 *	  Determine the best index to be used in conjunction with each subclause
 *	  of an OR clause, and build a Path for a multi-index scan.
 *
 * 'rel' is the node of the relation to be scanned
 * 'subclauses' are the subclauses of the OR clause (must be the modified
 *		form that includes sub-RestrictInfo clauses)
 *
 * Returns an IndexPath if successful, or NULL if it is not possible to
 * find an index for each OR subclause.
 *
 * NOTE: we choose each scan on the basis of its total cost, ignoring startup
 * cost.  This is reasonable as long as all index types have zero or small
 * startup cost, but we might have to work harder if any index types with
 * nontrivial startup cost are ever invented.
 *
 * This routine also creates the indexqual list that will be needed by
 * the executor.  The indexqual list has one entry for each scan of the base
 * rel, which is a sublist of indexqual conditions to apply in that scan.
 * The implicit semantics are AND across each sublist of quals, and OR across
 * the toplevel list (note that the executor takes care not to return any
 * single tuple more than once).
 */
static IndexPath *
best_or_subclause_indices(Query *root,
						  RelOptInfo *rel,
						  List *subclauses)
{
	FastList	infos;
	FastList	quals;
	Cost		path_startup_cost;
	Cost		path_total_cost;
	List	   *slist;
	IndexPath  *pathnode;

	FastListInit(&infos);
	FastListInit(&quals);
	path_startup_cost = 0;
	path_total_cost = 0;

	/* Gather info for each OR subclause */
	foreach(slist, subclauses)
	{
		Expr	   *subclause = lfirst(slist);
		IndexOptInfo *best_indexinfo;
		List	   *best_indexqual;
		Cost		best_startup_cost;
		Cost		best_total_cost;

		if (!best_or_subclause_index(root, rel, subclause,
									 &best_indexinfo, &best_indexqual,
									 &best_startup_cost, &best_total_cost))
			return NULL;		/* failed to match this subclause */

		FastAppend(&infos, best_indexinfo);
		FastAppend(&quals, best_indexqual);
		/*
		 * Path startup_cost is the startup cost for the first index scan only;
		 * startup costs for later scans will be paid later on, so they just
		 * get reflected in total_cost.
		 *
		 * Total cost is sum of the per-scan costs.
		 */
		if (slist == subclauses)	/* first scan? */
			path_startup_cost = best_startup_cost;
		path_total_cost += best_total_cost;
	}

	/* We succeeded, so build an IndexPath node */
	pathnode = makeNode(IndexPath);

	pathnode->path.pathtype = T_IndexScan;
	pathnode->path.parent = rel;
	pathnode->path.startup_cost = path_startup_cost;
	pathnode->path.total_cost = path_total_cost;

	/*
	 * This is an IndexScan, but the overall result will consist of tuples
	 * extracted in multiple passes (one for each subclause of the OR),
	 * so the result cannot be claimed to have any particular ordering.
	 */
	pathnode->path.pathkeys = NIL;

	pathnode->indexinfo = FastListValue(&infos);
	pathnode->indexqual = FastListValue(&quals);

	/* It's not an innerjoin path. */
	pathnode->indexjoinclauses = NIL;

	/* We don't actually care what order the index scans in. */
	pathnode->indexscandir = NoMovementScanDirection;

	/* XXX this may be wrong when using join OR clauses... */
	pathnode->rows = rel->rows;

	return pathnode;
}

/*
 * best_or_subclause_index
 *	  Determines which is the best index to be used with a subclause of an
 *	  OR clause by estimating the cost of using each index and selecting
 *	  the least expensive (considering total cost only, for now).
 *
 * Returns FALSE if no index exists that can be used with this OR subclause;
 * in that case the output parameters are not set.
 *
 * 'rel' is the node of the relation to be scanned
 * 'subclause' is the OR subclause being considered
 *
 * '*retIndexInfo' gets the IndexOptInfo of the best index
 * '*retIndexQual' gets a list of the indexqual conditions for the best index
 * '*retStartupCost' gets the startup cost of a scan with that index
 * '*retTotalCost' gets the total cost of a scan with that index
 */
static bool
best_or_subclause_index(Query *root,
						RelOptInfo *rel,
						Expr *subclause,
						IndexOptInfo **retIndexInfo,	/* return value */
						List **retIndexQual,	/* return value */
						Cost *retStartupCost,	/* return value */
						Cost *retTotalCost)		/* return value */
{
	bool		found = false;
	List	   *ilist;

	foreach(ilist, rel->indexlist)
	{
		IndexOptInfo *index = (IndexOptInfo *) lfirst(ilist);
		List	   *qualrinfos;
		List	   *indexquals;
		Path		subclause_path;

		/* Ignore partial indexes that do not match the query */
		if (index->indpred != NIL && !index->predOK)
			continue;

		/* Collect index clauses usable with this index */
		qualrinfos = group_clauses_by_indexkey_for_or(rel, index, subclause);

		/* Ignore index if it doesn't match the subclause at all */
		if (qualrinfos == NIL)
			continue;

		/* Convert RestrictInfo nodes to indexquals the executor can handle */
		indexquals = expand_indexqual_conditions(index, qualrinfos);

		cost_index(&subclause_path, root, rel, index, indexquals, false);

		if (!found || subclause_path.total_cost < *retTotalCost)
		{
			*retIndexInfo = index;
			*retIndexQual = indexquals;
			*retStartupCost = subclause_path.startup_cost;
			*retTotalCost = subclause_path.total_cost;
			found = true;
		}
	}

	return found;
}
