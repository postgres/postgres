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
 *	  $PostgreSQL: pgsql/src/backend/optimizer/path/orindxpath.c,v 1.58 2004/05/26 04:41:22 neilc Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"


static IndexPath *best_or_subclause_indexes(Query *root, RelOptInfo *rel,
											List *subclauses);
static bool best_or_subclause_index(Query *root,
						RelOptInfo *rel,
						Expr *subclause,
						IndexOptInfo **retIndexInfo,
						List **retIndexClauses,
						List **retIndexQuals,
						Cost *retStartupCost,
						Cost *retTotalCost);


/*----------
 * create_or_index_quals
 *	  Examine join OR-of-AND quals to see if any useful restriction OR
 *	  clauses can be extracted.  If so, add them to the query.
 *
 * Although a join clause must reference other relations overall,
 * an OR of ANDs clause might contain sub-clauses that reference just this
 * relation and can be used to build a restriction clause.
 * For example consider
 *		WHERE ((a.x = 42 AND b.y = 43) OR (a.x = 44 AND b.z = 45));
 * We can transform this into
 *		WHERE ((a.x = 42 AND b.y = 43) OR (a.x = 44 AND b.z = 45))
 *			AND (a.x = 42 OR a.x = 44)
 *			AND (b.y = 43 OR b.z = 45);
 * which opens the potential to build OR indexscans on a and b.  In essence
 * this is a partial transformation to CNF (AND of ORs format).  It is not
 * complete, however, because we do not unravel the original OR --- doing so
 * would usually bloat the qualification expression to little gain.
 *
 * The added quals are partially redundant with the original OR, and therefore
 * will cause the size of the joinrel to be underestimated when it is finally
 * formed.  (This would be true of a full transformation to CNF as well; the
 * fault is not really in the transformation, but in clauselist_selectivity's
 * inability to recognize redundant conditions.)  To minimize the collateral
 * damage, we want to minimize the number of quals added.  Therefore we do
 * not add every possible extracted restriction condition to the query.
 * Instead, we search for the single restriction condition that generates
 * the most useful (cheapest) OR indexscan, and add only that condition.
 * This is a pretty ad-hoc heuristic, but quite useful.
 *
 * We can then compensate for the redundancy of the added qual by poking
 * the recorded selectivity of the original OR clause, thereby ensuring
 * the added qual doesn't change the estimated size of the joinrel when
 * it is finally formed.  This is a MAJOR HACK: it depends on the fact
 * that clause selectivities are cached and on the fact that the same
 * RestrictInfo node will appear in every joininfo list that might be used
 * when the joinrel is formed.  And it probably isn't right in cases where
 * the size estimation is nonlinear (i.e., outer and IN joins).  But it
 * beats not doing anything.
 *
 * NOTE: one might think this messiness could be worked around by generating
 * the indexscan path with a small path->rows value, and not touching the
 * rel's baserestrictinfo or rel->rows.  However, that does not work.
 * The optimizer's fundamental design assumes that every general-purpose
 * Path for a given relation generates the same number of rows.  Without
 * this assumption we'd not be able to optimize solely on the cost of Paths,
 * but would have to take number of output rows into account as well.
 * (Perhaps someday that'd be worth doing, but it's a pretty big change...)
 *
 * 'rel' is the relation entry for which quals are to be created
 *
 * If successful, adds qual(s) to rel->baserestrictinfo and returns TRUE.
 * If no quals available, returns FALSE and doesn't change rel.
 *
 * Note: check_partial_indexes() must have been run previously.
 *----------
 */
bool
create_or_index_quals(Query *root, RelOptInfo *rel)
{
	IndexPath  *bestpath = NULL;
	RestrictInfo *bestrinfo = NULL;
	List	   *newrinfos;
	RestrictInfo *or_rinfo;
	Selectivity or_selec,
				orig_selec;
	ListCell   *i;

	/*
	 * We use the best_or_subclause_indexes() machinery to locate the
	 * best combination of restriction subclauses.  Note we must ignore
	 * any joinclauses that are not marked valid_everywhere, because they
	 * cannot be pushed down due to outer-join rules.
	 */
	foreach(i, rel->joininfo)
	{
		JoinInfo   *joininfo = (JoinInfo *) lfirst(i);
		ListCell   *j;

		foreach(j, joininfo->jinfo_restrictinfo)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(j);

			if (restriction_is_or_clause(rinfo) &&
				rinfo->valid_everywhere)
			{
				IndexPath  *pathnode;

				pathnode = best_or_subclause_indexes(root,
													 rel,
										((BoolExpr *) rinfo->orclause)->args);

				if (pathnode)
				{
					if (bestpath == NULL ||
						pathnode->path.total_cost < bestpath->path.total_cost)
					{
						bestpath = pathnode;
						bestrinfo = rinfo;
					}
				}
			}
		}
	}

	/* Fail if no suitable clauses found */
	if (bestpath == NULL)
		return false;

	/*
	 * Convert the indexclauses structure to a RestrictInfo tree,
	 * and add it to the rel's restriction list.
	 */
	newrinfos = make_restrictinfo_from_indexclauses(bestpath->indexclauses,
													true, true);
	Assert(length(newrinfos) == 1);
	or_rinfo = (RestrictInfo *) linitial(newrinfos);
	rel->baserestrictinfo = nconc(rel->baserestrictinfo, newrinfos);

	/*
	 * Adjust the original OR clause's cached selectivity to compensate
	 * for the selectivity of the added (but redundant) lower-level qual.
	 * This should result in the join rel getting approximately the same
	 * rows estimate as it would have gotten without all these shenanigans.
	 * (XXX major hack alert ... this depends on the assumption that the
	 * selectivity will stay cached ...)
	 */
	or_selec = clause_selectivity(root, (Node *) or_rinfo,
								  0, JOIN_INNER);
	if (or_selec > 0 && or_selec < 1)
	{
		orig_selec = clause_selectivity(root, (Node *) bestrinfo,
										0, JOIN_INNER);
		bestrinfo->this_selec = orig_selec / or_selec;
		/* clamp result to sane range */
		if (bestrinfo->this_selec > 1)
			bestrinfo->this_selec = 1;
	}

	/* Tell caller to recompute rel's rows estimate */
	return true;
}

/*
 * create_or_index_paths
 *	  Creates multi-scan index paths for indexes that match OR clauses.
 *
 * 'rel' is the relation entry for which the paths are to be created
 *
 * Returns nothing, but adds paths to rel->pathlist via add_path().
 *
 * Note: check_partial_indexes() must have been run previously.
 */
void
create_or_index_paths(Query *root, RelOptInfo *rel)
{
	ListCell   *l;

	/*
	 * Check each restriction clause to see if it is an OR clause, and if so,
	 * try to make a path using it.
	 */
	foreach(l, rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

		if (restriction_is_or_clause(rinfo))
		{
			IndexPath  *pathnode;

			pathnode = best_or_subclause_indexes(root,
												 rel,
							   ((BoolExpr *) rinfo->orclause)->args);

			if (pathnode)
				add_path(rel, (Path *) pathnode);
		}
	}
}

/*
 * best_or_subclause_indexes
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
best_or_subclause_indexes(Query *root,
						  RelOptInfo *rel,
						  List *subclauses)
{
	FastList	infos;
	FastList	clauses;
	FastList	quals;
	Cost		path_startup_cost;
	Cost		path_total_cost;
	ListCell   *slist;
	IndexPath  *pathnode;

	FastListInit(&infos);
	FastListInit(&clauses);
	FastListInit(&quals);
	path_startup_cost = 0;
	path_total_cost = 0;

	/* Gather info for each OR subclause */
	foreach(slist, subclauses)
	{
		Expr	   *subclause = lfirst(slist);
		IndexOptInfo *best_indexinfo;
		List	   *best_indexclauses;
		List	   *best_indexquals;
		Cost		best_startup_cost;
		Cost		best_total_cost;

		if (!best_or_subclause_index(root, rel, subclause,
									 &best_indexinfo,
									 &best_indexclauses, &best_indexquals,
									 &best_startup_cost, &best_total_cost))
			return NULL;		/* failed to match this subclause */

		FastAppend(&infos, best_indexinfo);
		FastAppend(&clauses, best_indexclauses);
		FastAppend(&quals, best_indexquals);
		/*
		 * Path startup_cost is the startup cost for the first index scan only;
		 * startup costs for later scans will be paid later on, so they just
		 * get reflected in total_cost.
		 *
		 * Total cost is sum of the per-scan costs.
		 */
		if (slist == list_head(subclauses))	/* first scan? */
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
	pathnode->indexclauses = FastListValue(&clauses);
	pathnode->indexquals = FastListValue(&quals);

	/* It's not an innerjoin path. */
	pathnode->isjoininner = false;

	/* We don't actually care what order the index scans in. */
	pathnode->indexscandir = NoMovementScanDirection;

	/*
	 * The number of rows is the same as the parent rel's estimate, since
	 * this isn't a join inner indexscan.
	 */
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
 * '*retIndexClauses' gets a list of the index clauses for the best index
 * '*retIndexQuals' gets a list of the expanded indexquals for the best index
 * '*retStartupCost' gets the startup cost of a scan with that index
 * '*retTotalCost' gets the total cost of a scan with that index
 */
static bool
best_or_subclause_index(Query *root,
						RelOptInfo *rel,
						Expr *subclause,
						IndexOptInfo **retIndexInfo,	/* return value */
						List **retIndexClauses,	/* return value */
						List **retIndexQuals,	/* return value */
						Cost *retStartupCost,	/* return value */
						Cost *retTotalCost)		/* return value */
{
	bool		found = false;
	ListCell   *ilist;

	foreach(ilist, rel->indexlist)
	{
		IndexOptInfo *index = (IndexOptInfo *) lfirst(ilist);
		List	   *indexclauses;
		List	   *indexquals;
		Path		subclause_path;

		/* Ignore partial indexes that do not match the query */
		if (index->indpred != NIL && !index->predOK)
			continue;

		/* Collect index clauses usable with this index */
		indexclauses = group_clauses_by_indexkey_for_or(rel, index, subclause);

		/* Ignore index if it doesn't match the subclause at all */
		if (indexclauses == NIL)
			continue;

		/* Convert clauses to indexquals the executor can handle */
		indexquals = expand_indexqual_conditions(index, indexclauses);

		cost_index(&subclause_path, root, rel, index, indexquals, false);

		if (!found || subclause_path.total_cost < *retTotalCost)
		{
			*retIndexInfo = index;
			*retIndexClauses = flatten_clausegroups_list(indexclauses);
			*retIndexQuals = indexquals;
			*retStartupCost = subclause_path.startup_cost;
			*retTotalCost = subclause_path.total_cost;
			found = true;
		}
	}

	return found;
}
