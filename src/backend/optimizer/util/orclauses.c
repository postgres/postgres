/*-------------------------------------------------------------------------
 *
 * orclauses.c
 *	  Routines to extract restriction OR clauses from join OR clauses
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/util/orclauses.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/orclauses.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"


static bool is_safe_restriction_clause_for(RestrictInfo *rinfo, RelOptInfo *rel);
static Expr *extract_or_clause(RestrictInfo *or_rinfo, RelOptInfo *rel);
static void consider_new_or_clause(PlannerInfo *root, RelOptInfo *rel,
								   Expr *orclause, RestrictInfo *join_or_rinfo);


/*
 * extract_restriction_or_clauses
 *	  Examine join OR-of-AND clauses to see if any useful restriction OR
 *	  clauses can be extracted.  If so, add them to the query.
 *
 * Although a join clause must reference multiple relations overall,
 * an OR of ANDs clause might contain sub-clauses that reference just one
 * relation and can be used to build a restriction clause for that rel.
 * For example consider
 *		WHERE ((a.x = 42 AND b.y = 43) OR (a.x = 44 AND b.z = 45));
 * We can transform this into
 *		WHERE ((a.x = 42 AND b.y = 43) OR (a.x = 44 AND b.z = 45))
 *			AND (a.x = 42 OR a.x = 44)
 *			AND (b.y = 43 OR b.z = 45);
 * which allows the latter clauses to be applied during the scans of a and b,
 * perhaps as index qualifications, and in any case reducing the number of
 * rows arriving at the join.  In essence this is a partial transformation to
 * CNF (AND of ORs format).  It is not complete, however, because we do not
 * unravel the original OR --- doing so would usually bloat the qualification
 * expression to little gain.
 *
 * The added quals are partially redundant with the original OR, and therefore
 * would cause the size of the joinrel to be underestimated when it is finally
 * formed.  (This would be true of a full transformation to CNF as well; the
 * fault is not really in the transformation, but in clauselist_selectivity's
 * inability to recognize redundant conditions.)  We can compensate for this
 * redundancy by changing the cached selectivity of the original OR clause,
 * canceling out the (valid) reduction in the estimated sizes of the base
 * relations so that the estimated joinrel size remains the same.  This is
 * a MAJOR HACK: it depends on the fact that clause selectivities are cached
 * and on the fact that the same RestrictInfo node will appear in every
 * joininfo list that might be used when the joinrel is formed.
 * And it doesn't work in cases where the size estimation is nonlinear
 * (i.e., outer and IN joins).  But it beats not doing anything.
 *
 * We examine each base relation to see if join clauses associated with it
 * contain extractable restriction conditions.  If so, add those conditions
 * to the rel's baserestrictinfo and update the cached selectivities of the
 * join clauses.  Note that the same join clause will be examined afresh
 * from the point of view of each baserel that participates in it, so its
 * cached selectivity may get updated multiple times.
 */
void
extract_restriction_or_clauses(PlannerInfo *root)
{
	Index		rti;

	/* Examine each baserel for potential join OR clauses */
	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *rel = root->simple_rel_array[rti];
		ListCell   *lc;

		/* there may be empty slots corresponding to non-baserel RTEs */
		if (rel == NULL)
			continue;

		Assert(rel->relid == rti);	/* sanity check on array */

		/* ignore RTEs that are "other rels" */
		if (rel->reloptkind != RELOPT_BASEREL)
			continue;

		/*
		 * Find potentially interesting OR joinclauses.  We can use any
		 * joinclause that is considered safe to move to this rel by the
		 * parameterized-path machinery, even though what we are going to do
		 * with it is not exactly a parameterized path.
		 */
		foreach(lc, rel->joininfo)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

			if (restriction_is_or_clause(rinfo) &&
				join_clause_is_movable_to(rinfo, rel))
			{
				/* Try to extract a qual for this rel only */
				Expr	   *orclause = extract_or_clause(rinfo, rel);

				/*
				 * If successful, decide whether we want to use the clause,
				 * and insert it into the rel's restrictinfo list if so.
				 */
				if (orclause)
					consider_new_or_clause(root, rel, orclause, rinfo);
			}
		}
	}
}

/*
 * Is the given primitive (non-OR) RestrictInfo safe to move to the rel?
 */
static bool
is_safe_restriction_clause_for(RestrictInfo *rinfo, RelOptInfo *rel)
{
	/*
	 * We want clauses that mention the rel, and only the rel.  So in
	 * particular pseudoconstant clauses can be rejected quickly.  Then check
	 * the clause's Var membership.
	 */
	if (rinfo->pseudoconstant)
		return false;
	if (!bms_equal(rinfo->clause_relids, rel->relids))
		return false;

	/* We don't want extra evaluations of any volatile functions */
	if (contain_volatile_functions((Node *) rinfo->clause))
		return false;

	return true;
}

/*
 * Try to extract a restriction clause mentioning only "rel" from the given
 * join OR-clause.
 *
 * We must be able to extract at least one qual for this rel from each of
 * the arms of the OR, else we can't use it.
 *
 * Returns an OR clause (not a RestrictInfo!) pertaining to rel, or NULL
 * if no OR clause could be extracted.
 */
static Expr *
extract_or_clause(RestrictInfo *or_rinfo, RelOptInfo *rel)
{
	List	   *clauselist = NIL;
	ListCell   *lc;

	/*
	 * Scan each arm of the input OR clause.  Notice we descend into
	 * or_rinfo->orclause, which has RestrictInfo nodes embedded below the
	 * toplevel OR/AND structure.  This is useful because we can use the info
	 * in those nodes to make is_safe_restriction_clause_for()'s checks
	 * cheaper.  We'll strip those nodes from the returned tree, though,
	 * meaning that fresh ones will be built if the clause is accepted as a
	 * restriction clause.  This might seem wasteful --- couldn't we re-use
	 * the existing RestrictInfos?	But that'd require assuming that
	 * selectivity and other cached data is computed exactly the same way for
	 * a restriction clause as for a join clause, which seems undesirable.
	 */
	Assert(is_orclause(or_rinfo->orclause));
	foreach(lc, ((BoolExpr *) or_rinfo->orclause)->args)
	{
		Node	   *orarg = (Node *) lfirst(lc);
		List	   *subclauses = NIL;
		Node	   *subclause;

		/* OR arguments should be ANDs or sub-RestrictInfos */
		if (is_andclause(orarg))
		{
			List	   *andargs = ((BoolExpr *) orarg)->args;
			ListCell   *lc2;

			foreach(lc2, andargs)
			{
				RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc2);

				if (restriction_is_or_clause(rinfo))
				{
					/*
					 * Recurse to deal with nested OR.  Note we *must* recurse
					 * here, this isn't just overly-tense optimization: we
					 * have to descend far enough to find and strip all
					 * RestrictInfos in the expression.
					 */
					Expr	   *suborclause;

					suborclause = extract_or_clause(rinfo, rel);
					if (suborclause)
						subclauses = lappend(subclauses, suborclause);
				}
				else if (is_safe_restriction_clause_for(rinfo, rel))
					subclauses = lappend(subclauses, rinfo->clause);
			}
		}
		else
		{
			RestrictInfo *rinfo = castNode(RestrictInfo, orarg);

			Assert(!restriction_is_or_clause(rinfo));
			if (is_safe_restriction_clause_for(rinfo, rel))
				subclauses = lappend(subclauses, rinfo->clause);
		}

		/*
		 * If nothing could be extracted from this arm, we can't do anything
		 * with this OR clause.
		 */
		if (subclauses == NIL)
			return NULL;

		/*
		 * OK, add subclause(s) to the result OR.  If we found more than one,
		 * we need an AND node.  But if we found only one, and it is itself an
		 * OR node, add its subclauses to the result instead; this is needed
		 * to preserve AND/OR flatness (ie, no OR directly underneath OR).
		 */
		subclause = (Node *) make_ands_explicit(subclauses);
		if (is_orclause(subclause))
			clauselist = list_concat(clauselist,
									 ((BoolExpr *) subclause)->args);
		else
			clauselist = lappend(clauselist, subclause);
	}

	/*
	 * If we got a restriction clause from every arm, wrap them up in an OR
	 * node.  (In theory the OR node might be unnecessary, if there was only
	 * one arm --- but then the input OR node was also redundant.)
	 */
	if (clauselist != NIL)
		return make_orclause(clauselist);
	return NULL;
}

/*
 * Consider whether a successfully-extracted restriction OR clause is
 * actually worth using.  If so, add it to the planner's data structures,
 * and adjust the original join clause (join_or_rinfo) to compensate.
 */
static void
consider_new_or_clause(PlannerInfo *root, RelOptInfo *rel,
					   Expr *orclause, RestrictInfo *join_or_rinfo)
{
	RestrictInfo *or_rinfo;
	Selectivity or_selec,
				orig_selec;

	/*
	 * Build a RestrictInfo from the new OR clause.  We can assume it's valid
	 * as a base restriction clause.
	 */
	or_rinfo = make_restrictinfo(root,
								 orclause,
								 true,
								 false,
								 false,
								 false,
								 join_or_rinfo->security_level,
								 NULL,
								 NULL,
								 NULL);

	/*
	 * Estimate its selectivity.  (We could have done this earlier, but doing
	 * it on the RestrictInfo representation allows the result to get cached,
	 * saving work later.)
	 */
	or_selec = clause_selectivity(root, (Node *) or_rinfo,
								  0, JOIN_INNER, NULL);

	/*
	 * The clause is only worth adding to the query if it rejects a useful
	 * fraction of the base relation's rows; otherwise, it's just going to
	 * cause duplicate computation (since we will still have to check the
	 * original OR clause when the join is formed).  Somewhat arbitrarily, we
	 * set the selectivity threshold at 0.9.
	 */
	if (or_selec > 0.9)
		return;					/* forget it */

	/*
	 * OK, add it to the rel's restriction-clause list.
	 */
	rel->baserestrictinfo = lappend(rel->baserestrictinfo, or_rinfo);
	rel->baserestrict_min_security = Min(rel->baserestrict_min_security,
										 or_rinfo->security_level);

	/*
	 * Adjust the original join OR clause's cached selectivity to compensate
	 * for the selectivity of the added (but redundant) lower-level qual. This
	 * should result in the join rel getting approximately the same rows
	 * estimate as it would have gotten without all these shenanigans.
	 *
	 * XXX major hack alert: this depends on the assumption that the
	 * selectivity will stay cached.
	 *
	 * XXX another major hack: we adjust only norm_selec, the cached
	 * selectivity for JOIN_INNER semantics, even though the join clause
	 * might've been an outer-join clause.  This is partly because we can't
	 * easily identify the relevant SpecialJoinInfo here, and partly because
	 * the linearity assumption we're making would fail anyway.  (If it is an
	 * outer-join clause, "rel" must be on the nullable side, else we'd not
	 * have gotten here.  So the computation of the join size is going to be
	 * quite nonlinear with respect to the size of "rel", so it's not clear
	 * how we ought to adjust outer_selec even if we could compute its
	 * original value correctly.)
	 */
	if (or_selec > 0)
	{
		SpecialJoinInfo sjinfo;

		/*
		 * Make up a SpecialJoinInfo for JOIN_INNER semantics.  (Compare
		 * approx_tuple_count() in costsize.c.)
		 */
		init_dummy_sjinfo(&sjinfo,
						  bms_difference(join_or_rinfo->clause_relids,
										 rel->relids),
						  rel->relids);

		/* Compute inner-join size */
		orig_selec = clause_selectivity(root, (Node *) join_or_rinfo,
										0, JOIN_INNER, &sjinfo);

		/* And hack cached selectivity so join size remains the same */
		join_or_rinfo->norm_selec = orig_selec / or_selec;
		/* ensure result stays in sane range */
		if (join_or_rinfo->norm_selec > 1)
			join_or_rinfo->norm_selec = 1;
		/* as explained above, we don't touch outer_selec */
	}
}
