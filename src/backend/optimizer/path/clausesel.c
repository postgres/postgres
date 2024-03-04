/*-------------------------------------------------------------------------
 *
 * clausesel.c
 *	  Routines to compute clause selectivities
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/path/clausesel.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/plancat.h"
#include "statistics/statistics.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"

/*
 * Data structure for accumulating info about possible range-query
 * clause pairs in clauselist_selectivity.
 */
typedef struct RangeQueryClause
{
	struct RangeQueryClause *next;	/* next in linked list */
	Node	   *var;			/* The common variable of the clauses */
	bool		have_lobound;	/* found a low-bound clause yet? */
	bool		have_hibound;	/* found a high-bound clause yet? */
	Selectivity lobound;		/* Selectivity of a var > something clause */
	Selectivity hibound;		/* Selectivity of a var < something clause */
} RangeQueryClause;

static void addRangeClause(RangeQueryClause **rqlist, Node *clause,
						   bool varonleft, bool isLTsel, Selectivity s2);
static RelOptInfo *find_single_rel_for_clauses(PlannerInfo *root,
											   List *clauses);
static Selectivity clauselist_selectivity_or(PlannerInfo *root,
											 List *clauses,
											 int varRelid,
											 JoinType jointype,
											 SpecialJoinInfo *sjinfo,
											 bool use_extended_stats);

/****************************************************************************
 *		ROUTINES TO COMPUTE SELECTIVITIES
 ****************************************************************************/

/*
 * clauselist_selectivity -
 *	  Compute the selectivity of an implicitly-ANDed list of boolean
 *	  expression clauses.  The list can be empty, in which case 1.0
 *	  must be returned.  List elements may be either RestrictInfos
 *	  or bare expression clauses --- the former is preferred since
 *	  it allows caching of results.
 *
 * See clause_selectivity() for the meaning of the additional parameters.
 *
 * The basic approach is to apply extended statistics first, on as many
 * clauses as possible, in order to capture cross-column dependencies etc.
 * The remaining clauses are then estimated by taking the product of their
 * selectivities, but that's only right if they have independent
 * probabilities, and in reality they are often NOT independent even if they
 * only refer to a single column.  So, we want to be smarter where we can.
 *
 * We also recognize "range queries", such as "x > 34 AND x < 42".  Clauses
 * are recognized as possible range query components if they are restriction
 * opclauses whose operators have scalarltsel or a related function as their
 * restriction selectivity estimator.  We pair up clauses of this form that
 * refer to the same variable.  An unpairable clause of this kind is simply
 * multiplied into the selectivity product in the normal way.  But when we
 * find a pair, we know that the selectivities represent the relative
 * positions of the low and high bounds within the column's range, so instead
 * of figuring the selectivity as hisel * losel, we can figure it as hisel +
 * losel - 1.  (To visualize this, see that hisel is the fraction of the range
 * below the high bound, while losel is the fraction above the low bound; so
 * hisel can be interpreted directly as a 0..1 value but we need to convert
 * losel to 1-losel before interpreting it as a value.  Then the available
 * range is 1-losel to hisel.  However, this calculation double-excludes
 * nulls, so really we need hisel + losel + null_frac - 1.)
 *
 * If either selectivity is exactly DEFAULT_INEQ_SEL, we forget this equation
 * and instead use DEFAULT_RANGE_INEQ_SEL.  The same applies if the equation
 * yields an impossible (negative) result.
 *
 * A free side-effect is that we can recognize redundant inequalities such
 * as "x < 4 AND x < 5"; only the tighter constraint will be counted.
 *
 * Of course this is all very dependent on the behavior of the inequality
 * selectivity functions; perhaps some day we can generalize the approach.
 */
Selectivity
clauselist_selectivity(PlannerInfo *root,
					   List *clauses,
					   int varRelid,
					   JoinType jointype,
					   SpecialJoinInfo *sjinfo)
{
	return clauselist_selectivity_ext(root, clauses, varRelid,
									  jointype, sjinfo, true);
}

/*
 * clauselist_selectivity_ext -
 *	  Extended version of clauselist_selectivity().  If "use_extended_stats"
 *	  is false, all extended statistics will be ignored, and only per-column
 *	  statistics will be used.
 */
Selectivity
clauselist_selectivity_ext(PlannerInfo *root,
						   List *clauses,
						   int varRelid,
						   JoinType jointype,
						   SpecialJoinInfo *sjinfo,
						   bool use_extended_stats)
{
	Selectivity s1 = 1.0;
	RelOptInfo *rel;
	Bitmapset  *estimatedclauses = NULL;
	RangeQueryClause *rqlist = NULL;
	ListCell   *l;
	int			listidx;

	/*
	 * If there's exactly one clause, just go directly to
	 * clause_selectivity_ext(). None of what we might do below is relevant.
	 */
	if (list_length(clauses) == 1)
		return clause_selectivity_ext(root, (Node *) linitial(clauses),
									  varRelid, jointype, sjinfo,
									  use_extended_stats);

	/*
	 * Determine if these clauses reference a single relation.  If so, and if
	 * it has extended statistics, try to apply those.
	 */
	rel = find_single_rel_for_clauses(root, clauses);
	if (use_extended_stats && rel && rel->rtekind == RTE_RELATION && rel->statlist != NIL)
	{
		/*
		 * Estimate as many clauses as possible using extended statistics.
		 *
		 * 'estimatedclauses' is populated with the 0-based list position
		 * index of clauses estimated here, and that should be ignored below.
		 */
		s1 = statext_clauselist_selectivity(root, clauses, varRelid,
											jointype, sjinfo, rel,
											&estimatedclauses, false);
	}

	/*
	 * Apply normal selectivity estimates for remaining clauses. We'll be
	 * careful to skip any clauses which were already estimated above.
	 *
	 * Anything that doesn't look like a potential rangequery clause gets
	 * multiplied into s1 and forgotten. Anything that does gets inserted into
	 * an rqlist entry.
	 */
	listidx = -1;
	foreach(l, clauses)
	{
		Node	   *clause = (Node *) lfirst(l);
		RestrictInfo *rinfo;
		Selectivity s2;

		listidx++;

		/*
		 * Skip this clause if it's already been estimated by some other
		 * statistics above.
		 */
		if (bms_is_member(listidx, estimatedclauses))
			continue;

		/* Compute the selectivity of this clause in isolation */
		s2 = clause_selectivity_ext(root, clause, varRelid, jointype, sjinfo,
									use_extended_stats);

		/*
		 * Check for being passed a RestrictInfo.
		 *
		 * If it's a pseudoconstant RestrictInfo, then s2 is either 1.0 or
		 * 0.0; just use that rather than looking for range pairs.
		 */
		if (IsA(clause, RestrictInfo))
		{
			rinfo = (RestrictInfo *) clause;
			if (rinfo->pseudoconstant)
			{
				s1 = s1 * s2;
				continue;
			}
			clause = (Node *) rinfo->clause;
		}
		else
			rinfo = NULL;

		/*
		 * See if it looks like a restriction clause with a pseudoconstant on
		 * one side.  (Anything more complicated than that might not behave in
		 * the simple way we are expecting.)  Most of the tests here can be
		 * done more efficiently with rinfo than without.
		 */
		if (is_opclause(clause) && list_length(((OpExpr *) clause)->args) == 2)
		{
			OpExpr	   *expr = (OpExpr *) clause;
			bool		varonleft = true;
			bool		ok;

			if (rinfo)
			{
				ok = (rinfo->num_base_rels == 1) &&
					(is_pseudo_constant_clause_relids(lsecond(expr->args),
													  rinfo->right_relids) ||
					 (varonleft = false,
					  is_pseudo_constant_clause_relids(linitial(expr->args),
													   rinfo->left_relids)));
			}
			else
			{
				ok = (NumRelids(root, clause) == 1) &&
					(is_pseudo_constant_clause(lsecond(expr->args)) ||
					 (varonleft = false,
					  is_pseudo_constant_clause(linitial(expr->args))));
			}

			if (ok)
			{
				/*
				 * If it's not a "<"/"<="/">"/">=" operator, just merge the
				 * selectivity in generically.  But if it's the right oprrest,
				 * add the clause to rqlist for later processing.
				 */
				switch (get_oprrest(expr->opno))
				{
					case F_SCALARLTSEL:
					case F_SCALARLESEL:
						addRangeClause(&rqlist, clause,
									   varonleft, true, s2);
						break;
					case F_SCALARGTSEL:
					case F_SCALARGESEL:
						addRangeClause(&rqlist, clause,
									   varonleft, false, s2);
						break;
					default:
						/* Just merge the selectivity in generically */
						s1 = s1 * s2;
						break;
				}
				continue;		/* drop to loop bottom */
			}
		}

		/* Not the right form, so treat it generically. */
		s1 = s1 * s2;
	}

	/*
	 * Now scan the rangequery pair list.
	 */
	while (rqlist != NULL)
	{
		RangeQueryClause *rqnext;

		if (rqlist->have_lobound && rqlist->have_hibound)
		{
			/* Successfully matched a pair of range clauses */
			Selectivity s2;

			/*
			 * Exact equality to the default value probably means the
			 * selectivity function punted.  This is not airtight but should
			 * be good enough.
			 */
			if (rqlist->hibound == DEFAULT_INEQ_SEL ||
				rqlist->lobound == DEFAULT_INEQ_SEL)
			{
				s2 = DEFAULT_RANGE_INEQ_SEL;
			}
			else
			{
				s2 = rqlist->hibound + rqlist->lobound - 1.0;

				/* Adjust for double-exclusion of NULLs */
				s2 += nulltestsel(root, IS_NULL, rqlist->var,
								  varRelid, jointype, sjinfo);

				/*
				 * A zero or slightly negative s2 should be converted into a
				 * small positive value; we probably are dealing with a very
				 * tight range and got a bogus result due to roundoff errors.
				 * However, if s2 is very negative, then we probably have
				 * default selectivity estimates on one or both sides of the
				 * range that we failed to recognize above for some reason.
				 */
				if (s2 <= 0.0)
				{
					if (s2 < -0.01)
					{
						/*
						 * No data available --- use a default estimate that
						 * is small, but not real small.
						 */
						s2 = DEFAULT_RANGE_INEQ_SEL;
					}
					else
					{
						/*
						 * It's just roundoff error; use a small positive
						 * value
						 */
						s2 = 1.0e-10;
					}
				}
			}
			/* Merge in the selectivity of the pair of clauses */
			s1 *= s2;
		}
		else
		{
			/* Only found one of a pair, merge it in generically */
			if (rqlist->have_lobound)
				s1 *= rqlist->lobound;
			else
				s1 *= rqlist->hibound;
		}
		/* release storage and advance */
		rqnext = rqlist->next;
		pfree(rqlist);
		rqlist = rqnext;
	}

	return s1;
}

/*
 * clauselist_selectivity_or -
 *	  Compute the selectivity of an implicitly-ORed list of boolean
 *	  expression clauses.  The list can be empty, in which case 0.0
 *	  must be returned.  List elements may be either RestrictInfos
 *	  or bare expression clauses --- the former is preferred since
 *	  it allows caching of results.
 *
 * See clause_selectivity() for the meaning of the additional parameters.
 *
 * The basic approach is to apply extended statistics first, on as many
 * clauses as possible, in order to capture cross-column dependencies etc.
 * The remaining clauses are then estimated as if they were independent.
 */
static Selectivity
clauselist_selectivity_or(PlannerInfo *root,
						  List *clauses,
						  int varRelid,
						  JoinType jointype,
						  SpecialJoinInfo *sjinfo,
						  bool use_extended_stats)
{
	Selectivity s1 = 0.0;
	RelOptInfo *rel;
	Bitmapset  *estimatedclauses = NULL;
	ListCell   *lc;
	int			listidx;

	/*
	 * Determine if these clauses reference a single relation.  If so, and if
	 * it has extended statistics, try to apply those.
	 */
	rel = find_single_rel_for_clauses(root, clauses);
	if (use_extended_stats && rel && rel->rtekind == RTE_RELATION && rel->statlist != NIL)
	{
		/*
		 * Estimate as many clauses as possible using extended statistics.
		 *
		 * 'estimatedclauses' is populated with the 0-based list position
		 * index of clauses estimated here, and that should be ignored below.
		 */
		s1 = statext_clauselist_selectivity(root, clauses, varRelid,
											jointype, sjinfo, rel,
											&estimatedclauses, true);
	}

	/*
	 * Estimate the remaining clauses as if they were independent.
	 *
	 * Selectivities for an OR clause are computed as s1+s2 - s1*s2 to account
	 * for the probable overlap of selected tuple sets.
	 *
	 * XXX is this too conservative?
	 */
	listidx = -1;
	foreach(lc, clauses)
	{
		Selectivity s2;

		listidx++;

		/*
		 * Skip this clause if it's already been estimated by some other
		 * statistics above.
		 */
		if (bms_is_member(listidx, estimatedclauses))
			continue;

		s2 = clause_selectivity_ext(root, (Node *) lfirst(lc), varRelid,
									jointype, sjinfo, use_extended_stats);

		s1 = s1 + s2 - s1 * s2;
	}

	return s1;
}

/*
 * addRangeClause --- add a new range clause for clauselist_selectivity
 *
 * Here is where we try to match up pairs of range-query clauses
 */
static void
addRangeClause(RangeQueryClause **rqlist, Node *clause,
			   bool varonleft, bool isLTsel, Selectivity s2)
{
	RangeQueryClause *rqelem;
	Node	   *var;
	bool		is_lobound;

	if (varonleft)
	{
		var = get_leftop((Expr *) clause);
		is_lobound = !isLTsel;	/* x < something is high bound */
	}
	else
	{
		var = get_rightop((Expr *) clause);
		is_lobound = isLTsel;	/* something < x is low bound */
	}

	for (rqelem = *rqlist; rqelem; rqelem = rqelem->next)
	{
		/*
		 * We use full equal() here because the "var" might be a function of
		 * one or more attributes of the same relation...
		 */
		if (!equal(var, rqelem->var))
			continue;
		/* Found the right group to put this clause in */
		if (is_lobound)
		{
			if (!rqelem->have_lobound)
			{
				rqelem->have_lobound = true;
				rqelem->lobound = s2;
			}
			else
			{

				/*------
				 * We have found two similar clauses, such as
				 * x < y AND x <= z.
				 * Keep only the more restrictive one.
				 *------
				 */
				if (rqelem->lobound > s2)
					rqelem->lobound = s2;
			}
		}
		else
		{
			if (!rqelem->have_hibound)
			{
				rqelem->have_hibound = true;
				rqelem->hibound = s2;
			}
			else
			{

				/*------
				 * We have found two similar clauses, such as
				 * x > y AND x >= z.
				 * Keep only the more restrictive one.
				 *------
				 */
				if (rqelem->hibound > s2)
					rqelem->hibound = s2;
			}
		}
		return;
	}

	/* No matching var found, so make a new clause-pair data structure */
	rqelem = (RangeQueryClause *) palloc(sizeof(RangeQueryClause));
	rqelem->var = var;
	if (is_lobound)
	{
		rqelem->have_lobound = true;
		rqelem->have_hibound = false;
		rqelem->lobound = s2;
	}
	else
	{
		rqelem->have_lobound = false;
		rqelem->have_hibound = true;
		rqelem->hibound = s2;
	}
	rqelem->next = *rqlist;
	*rqlist = rqelem;
}

/*
 * find_single_rel_for_clauses
 *		Examine each clause in 'clauses' and determine if all clauses
 *		reference only a single relation.  If so return that relation,
 *		otherwise return NULL.
 */
static RelOptInfo *
find_single_rel_for_clauses(PlannerInfo *root, List *clauses)
{
	int			lastrelid = 0;
	ListCell   *l;

	foreach(l, clauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);
		int			relid;

		/*
		 * If we have a list of bare clauses rather than RestrictInfos, we
		 * could pull out their relids the hard way with pull_varnos().
		 * However, currently the extended-stats machinery won't do anything
		 * with non-RestrictInfo clauses anyway, so there's no point in
		 * spending extra cycles; just fail if that's what we have.
		 *
		 * An exception to that rule is if we have a bare BoolExpr AND clause.
		 * We treat this as a special case because the restrictinfo machinery
		 * doesn't build RestrictInfos on top of AND clauses.
		 */
		if (is_andclause(rinfo))
		{
			RelOptInfo *rel;

			rel = find_single_rel_for_clauses(root,
											  ((BoolExpr *) rinfo)->args);

			if (rel == NULL)
				return NULL;
			if (lastrelid == 0)
				lastrelid = rel->relid;
			else if (rel->relid != lastrelid)
				return NULL;

			continue;
		}

		if (!IsA(rinfo, RestrictInfo))
			return NULL;

		if (bms_is_empty(rinfo->clause_relids))
			continue;			/* we can ignore variable-free clauses */
		if (!bms_get_singleton_member(rinfo->clause_relids, &relid))
			return NULL;		/* multiple relations in this clause */
		if (lastrelid == 0)
			lastrelid = relid;	/* first clause referencing a relation */
		else if (relid != lastrelid)
			return NULL;		/* relation not same as last one */
	}

	if (lastrelid != 0)
		return find_base_rel(root, lastrelid);

	return NULL;				/* no clauses */
}

/*
 * treat_as_join_clause -
 *	  Decide whether an operator clause is to be handled by the
 *	  restriction or join estimator.  Subroutine for clause_selectivity().
 */
static inline bool
treat_as_join_clause(PlannerInfo *root, Node *clause, RestrictInfo *rinfo,
					 int varRelid, SpecialJoinInfo *sjinfo)
{
	if (varRelid != 0)
	{
		/*
		 * Caller is forcing restriction mode (eg, because we are examining an
		 * inner indexscan qual).
		 */
		return false;
	}
	else if (sjinfo == NULL)
	{
		/*
		 * It must be a restriction clause, since it's being evaluated at a
		 * scan node.
		 */
		return false;
	}
	else
	{
		/*
		 * Otherwise, it's a join if there's more than one base relation used.
		 * We can optimize this calculation if an rinfo was passed.
		 *
		 * XXX	Since we know the clause is being evaluated at a join, the
		 * only way it could be single-relation is if it was delayed by outer
		 * joins.  We intentionally count only baserels here, not OJs that
		 * might be present in rinfo->clause_relids, so that we direct such
		 * cases to the restriction qual estimators not join estimators.
		 * Eventually some notice should be taken of the possibility of
		 * injected nulls, but we'll likely want to do that in the restriction
		 * estimators rather than starting to treat such cases as join quals.
		 */
		if (rinfo)
			return (rinfo->num_base_rels > 1);
		else
			return (NumRelids(root, clause) > 1);
	}
}


/*
 * clause_selectivity -
 *	  Compute the selectivity of a general boolean expression clause.
 *
 * The clause can be either a RestrictInfo or a plain expression.  If it's
 * a RestrictInfo, we try to cache the selectivity for possible re-use,
 * so passing RestrictInfos is preferred.
 *
 * varRelid is either 0 or a rangetable index.
 *
 * When varRelid is not 0, only variables belonging to that relation are
 * considered in computing selectivity; other vars are treated as constants
 * of unknown values.  This is appropriate for estimating the selectivity of
 * a join clause that is being used as a restriction clause in a scan of a
 * nestloop join's inner relation --- varRelid should then be the ID of the
 * inner relation.
 *
 * When varRelid is 0, all variables are treated as variables.  This
 * is appropriate for ordinary join clauses and restriction clauses.
 *
 * jointype is the join type, if the clause is a join clause.  Pass JOIN_INNER
 * if the clause isn't a join clause.
 *
 * sjinfo is NULL for a non-join clause, otherwise it provides additional
 * context information about the join being performed.  There are some
 * special cases:
 *	1. For a special (not INNER) join, sjinfo is always a member of
 *	   root->join_info_list.
 *	2. For an INNER join, sjinfo is just a transient struct, and only the
 *	   relids and jointype fields in it can be trusted.
 * It is possible for jointype to be different from sjinfo->jointype.
 * This indicates we are considering a variant join: either with
 * the LHS and RHS switched, or with one input unique-ified.
 *
 * Note: when passing nonzero varRelid, it's normally appropriate to set
 * jointype == JOIN_INNER, sjinfo == NULL, even if the clause is really a
 * join clause; because we aren't treating it as a join clause.
 */
Selectivity
clause_selectivity(PlannerInfo *root,
				   Node *clause,
				   int varRelid,
				   JoinType jointype,
				   SpecialJoinInfo *sjinfo)
{
	return clause_selectivity_ext(root, clause, varRelid,
								  jointype, sjinfo, true);
}

/*
 * clause_selectivity_ext -
 *	  Extended version of clause_selectivity().  If "use_extended_stats" is
 *	  false, all extended statistics will be ignored, and only per-column
 *	  statistics will be used.
 */
Selectivity
clause_selectivity_ext(PlannerInfo *root,
					   Node *clause,
					   int varRelid,
					   JoinType jointype,
					   SpecialJoinInfo *sjinfo,
					   bool use_extended_stats)
{
	Selectivity s1 = 0.5;		/* default for any unhandled clause type */
	RestrictInfo *rinfo = NULL;
	bool		cacheable = false;

	if (clause == NULL)			/* can this still happen? */
		return s1;

	if (IsA(clause, RestrictInfo))
	{
		rinfo = (RestrictInfo *) clause;

		/*
		 * If the clause is marked pseudoconstant, then it will be used as a
		 * gating qual and should not affect selectivity estimates; hence
		 * return 1.0.  The only exception is that a constant FALSE may be
		 * taken as having selectivity 0.0, since it will surely mean no rows
		 * out of the plan.  This case is simple enough that we need not
		 * bother caching the result.
		 */
		if (rinfo->pseudoconstant)
		{
			if (!IsA(rinfo->clause, Const))
				return (Selectivity) 1.0;
		}

		/*
		 * If possible, cache the result of the selectivity calculation for
		 * the clause.  We can cache if varRelid is zero or the clause
		 * contains only vars of that relid --- otherwise varRelid will affect
		 * the result, so mustn't cache.  Outer join quals might be examined
		 * with either their join's actual jointype or JOIN_INNER, so we need
		 * two cache variables to remember both cases.  Note: we assume the
		 * result won't change if we are switching the input relations or
		 * considering a unique-ified case, so we only need one cache variable
		 * for all non-JOIN_INNER cases.
		 */
		if (varRelid == 0 ||
			rinfo->num_base_rels == 0 ||
			(rinfo->num_base_rels == 1 &&
			 bms_is_member(varRelid, rinfo->clause_relids)))
		{
			/* Cacheable --- do we already have the result? */
			if (jointype == JOIN_INNER)
			{
				if (rinfo->norm_selec >= 0)
					return rinfo->norm_selec;
			}
			else
			{
				if (rinfo->outer_selec >= 0)
					return rinfo->outer_selec;
			}
			cacheable = true;
		}

		/*
		 * Proceed with examination of contained clause.  If the clause is an
		 * OR-clause, we want to look at the variant with sub-RestrictInfos,
		 * so that per-subclause selectivities can be cached.
		 */
		if (rinfo->orclause)
			clause = (Node *) rinfo->orclause;
		else
			clause = (Node *) rinfo->clause;
	}

	if (IsA(clause, Var))
	{
		Var		   *var = (Var *) clause;

		/*
		 * We probably shouldn't ever see an uplevel Var here, but if we do,
		 * return the default selectivity...
		 */
		if (var->varlevelsup == 0 &&
			(varRelid == 0 || varRelid == (int) var->varno))
		{
			/* Use the restriction selectivity function for a bool Var */
			s1 = boolvarsel(root, (Node *) var, varRelid);
		}
	}
	else if (IsA(clause, Const))
	{
		/* bool constant is pretty easy... */
		Const	   *con = (Const *) clause;

		s1 = con->constisnull ? 0.0 :
			DatumGetBool(con->constvalue) ? 1.0 : 0.0;
	}
	else if (IsA(clause, Param))
	{
		/* see if we can replace the Param */
		Node	   *subst = estimate_expression_value(root, clause);

		if (IsA(subst, Const))
		{
			/* bool constant is pretty easy... */
			Const	   *con = (Const *) subst;

			s1 = con->constisnull ? 0.0 :
				DatumGetBool(con->constvalue) ? 1.0 : 0.0;
		}
		else
		{
			/* XXX any way to do better than default? */
		}
	}
	else if (is_notclause(clause))
	{
		/* inverse of the selectivity of the underlying clause */
		s1 = 1.0 - clause_selectivity_ext(root,
										  (Node *) get_notclausearg((Expr *) clause),
										  varRelid,
										  jointype,
										  sjinfo,
										  use_extended_stats);
	}
	else if (is_andclause(clause))
	{
		/* share code with clauselist_selectivity() */
		s1 = clauselist_selectivity_ext(root,
										((BoolExpr *) clause)->args,
										varRelid,
										jointype,
										sjinfo,
										use_extended_stats);
	}
	else if (is_orclause(clause))
	{
		/*
		 * Almost the same thing as clauselist_selectivity, but with the
		 * clauses connected by OR.
		 */
		s1 = clauselist_selectivity_or(root,
									   ((BoolExpr *) clause)->args,
									   varRelid,
									   jointype,
									   sjinfo,
									   use_extended_stats);
	}
	else if (is_opclause(clause) || IsA(clause, DistinctExpr))
	{
		OpExpr	   *opclause = (OpExpr *) clause;
		Oid			opno = opclause->opno;

		if (treat_as_join_clause(root, clause, rinfo, varRelid, sjinfo))
		{
			/* Estimate selectivity for a join clause. */
			s1 = join_selectivity(root, opno,
								  opclause->args,
								  opclause->inputcollid,
								  jointype,
								  sjinfo);
		}
		else
		{
			/* Estimate selectivity for a restriction clause. */
			s1 = restriction_selectivity(root, opno,
										 opclause->args,
										 opclause->inputcollid,
										 varRelid);
		}

		/*
		 * DistinctExpr has the same representation as OpExpr, but the
		 * contained operator is "=" not "<>", so we must negate the result.
		 * This estimation method doesn't give the right behavior for nulls,
		 * but it's better than doing nothing.
		 */
		if (IsA(clause, DistinctExpr))
			s1 = 1.0 - s1;
	}
	else if (is_funcclause(clause))
	{
		FuncExpr   *funcclause = (FuncExpr *) clause;

		/* Try to get an estimate from the support function, if any */
		s1 = function_selectivity(root,
								  funcclause->funcid,
								  funcclause->args,
								  funcclause->inputcollid,
								  treat_as_join_clause(root, clause, rinfo,
													   varRelid, sjinfo),
								  varRelid,
								  jointype,
								  sjinfo);
	}
	else if (IsA(clause, ScalarArrayOpExpr))
	{
		/* Use node specific selectivity calculation function */
		s1 = scalararraysel(root,
							(ScalarArrayOpExpr *) clause,
							treat_as_join_clause(root, clause, rinfo,
												 varRelid, sjinfo),
							varRelid,
							jointype,
							sjinfo);
	}
	else if (IsA(clause, RowCompareExpr))
	{
		/* Use node specific selectivity calculation function */
		s1 = rowcomparesel(root,
						   (RowCompareExpr *) clause,
						   varRelid,
						   jointype,
						   sjinfo);
	}
	else if (IsA(clause, NullTest))
	{
		/* Use node specific selectivity calculation function */
		s1 = nulltestsel(root,
						 ((NullTest *) clause)->nulltesttype,
						 (Node *) ((NullTest *) clause)->arg,
						 varRelid,
						 jointype,
						 sjinfo);
	}
	else if (IsA(clause, BooleanTest))
	{
		/* Use node specific selectivity calculation function */
		s1 = booltestsel(root,
						 ((BooleanTest *) clause)->booltesttype,
						 (Node *) ((BooleanTest *) clause)->arg,
						 varRelid,
						 jointype,
						 sjinfo);
	}
	else if (IsA(clause, CurrentOfExpr))
	{
		/* CURRENT OF selects at most one row of its table */
		CurrentOfExpr *cexpr = (CurrentOfExpr *) clause;
		RelOptInfo *crel = find_base_rel(root, cexpr->cvarno);

		if (crel->tuples > 0)
			s1 = 1.0 / crel->tuples;
	}
	else if (IsA(clause, RelabelType))
	{
		/* Not sure this case is needed, but it can't hurt */
		s1 = clause_selectivity_ext(root,
									(Node *) ((RelabelType *) clause)->arg,
									varRelid,
									jointype,
									sjinfo,
									use_extended_stats);
	}
	else if (IsA(clause, CoerceToDomain))
	{
		/* Not sure this case is needed, but it can't hurt */
		s1 = clause_selectivity_ext(root,
									(Node *) ((CoerceToDomain *) clause)->arg,
									varRelid,
									jointype,
									sjinfo,
									use_extended_stats);
	}
	else
	{
		/*
		 * For anything else, see if we can consider it as a boolean variable.
		 * This only works if it's an immutable expression in Vars of a single
		 * relation; but there's no point in us checking that here because
		 * boolvarsel() will do it internally, and return a suitable default
		 * selectivity if not.
		 */
		s1 = boolvarsel(root, clause, varRelid);
	}

	/* Cache the result if possible */
	if (cacheable)
	{
		if (jointype == JOIN_INNER)
			rinfo->norm_selec = s1;
		else
			rinfo->outer_selec = s1;
	}

#ifdef SELECTIVITY_DEBUG
	elog(DEBUG4, "clause_selectivity: s1 %f", s1);
#endif							/* SELECTIVITY_DEBUG */

	return s1;
}
