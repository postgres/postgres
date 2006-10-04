/*-------------------------------------------------------------------------
 *
 * orindxpath.c
 *	  Routines to find index paths that match a set of OR clauses
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/path/orindxpath.c,v 1.81 2006/10/04 00:29:54 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "optimizer/cost.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"


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
 * formed.	(This would be true of a full transformation to CNF as well; the
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
 * when the joinrel is formed.	And it probably isn't right in cases where
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
create_or_index_quals(PlannerInfo *root, RelOptInfo *rel)
{
	BitmapOrPath *bestpath = NULL;
	RestrictInfo *bestrinfo = NULL;
	List	   *newrinfos;
	RestrictInfo *or_rinfo;
	Selectivity or_selec,
				orig_selec;
	ListCell   *i;

	/*
	 * Find potentially interesting OR joinclauses.  Note we must ignore any
	 * joinclauses that are marked outerjoin_delayed, because they cannot be
	 * pushed down to the per-relation level due to outer-join rules. (XXX in
	 * some cases it might be possible to allow this, but it would require
	 * substantially more bookkeeping about where the clause came from.)
	 */
	foreach(i, rel->joininfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(i);

		if (restriction_is_or_clause(rinfo) &&
			!rinfo->outerjoin_delayed)
		{
			/*
			 * Use the generate_bitmap_or_paths() machinery to estimate the
			 * value of each OR clause.  We can use regular restriction
			 * clauses along with the OR clause contents to generate
			 * indexquals.	We pass outer_rel = NULL so that sub-clauses that
			 * are actually joins will be ignored.
			 */
			List	   *orpaths;
			ListCell   *k;

			orpaths = generate_bitmap_or_paths(root, rel,
											   list_make1(rinfo),
											   rel->baserestrictinfo,
											   NULL);

			/* Locate the cheapest OR path */
			foreach(k, orpaths)
			{
				BitmapOrPath *path = (BitmapOrPath *) lfirst(k);

				Assert(IsA(path, BitmapOrPath));
				if (bestpath == NULL ||
					path->path.total_cost < bestpath->path.total_cost)
				{
					bestpath = path;
					bestrinfo = rinfo;
				}
			}
		}
	}

	/* Fail if no suitable clauses found */
	if (bestpath == NULL)
		return false;

	/*
	 * Convert the path's indexclauses structure to a RestrictInfo tree. We
	 * include any partial-index predicates so as to get a reasonable
	 * representation of what the path is actually scanning.
	 */
	newrinfos = make_restrictinfo_from_bitmapqual((Path *) bestpath,
												  true, true);

	/* It's possible we get back something other than a single OR clause */
	if (list_length(newrinfos) != 1)
		return false;
	or_rinfo = (RestrictInfo *) linitial(newrinfos);
	Assert(IsA(or_rinfo, RestrictInfo));
	if (!restriction_is_or_clause(or_rinfo))
		return false;

	/*
	 * OK, add it to the rel's restriction list.
	 */
	rel->baserestrictinfo = list_concat(rel->baserestrictinfo, newrinfos);

	/*
	 * Adjust the original OR clause's cached selectivity to compensate for
	 * the selectivity of the added (but redundant) lower-level qual. This
	 * should result in the join rel getting approximately the same rows
	 * estimate as it would have gotten without all these shenanigans. (XXX
	 * major hack alert ... this depends on the assumption that the
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
