/*-------------------------------------------------------------------------
 *
 * clausesel.c
 *	  Routines to compute clause selectivities
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/path/clausesel.c,v 1.90 2008/01/11 17:00:45 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_operator.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/plancat.h"
#include "parser/parsetree.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"


/*
 * Data structure for accumulating info about possible range-query
 * clause pairs in clauselist_selectivity.
 */
typedef struct RangeQueryClause
{
	struct RangeQueryClause *next;		/* next in linked list */
	Node	   *var;			/* The common variable of the clauses */
	bool		have_lobound;	/* found a low-bound clause yet? */
	bool		have_hibound;	/* found a high-bound clause yet? */
	Selectivity lobound;		/* Selectivity of a var > something clause */
	Selectivity hibound;		/* Selectivity of a var < something clause */
} RangeQueryClause;

static void addRangeClause(RangeQueryClause **rqlist, Node *clause,
			   bool varonleft, bool isLTsel, Selectivity s2);


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
 * Our basic approach is to take the product of the selectivities of the
 * subclauses.	However, that's only right if the subclauses have independent
 * probabilities, and in reality they are often NOT independent.  So,
 * we want to be smarter where we can.

 * Currently, the only extra smarts we have is to recognize "range queries",
 * such as "x > 34 AND x < 42".  Clauses are recognized as possible range
 * query components if they are restriction opclauses whose operators have
 * scalarltsel() or scalargtsel() as their restriction selectivity estimator.
 * We pair up clauses of this form that refer to the same variable.  An
 * unpairable clause of this kind is simply multiplied into the selectivity
 * product in the normal way.  But when we find a pair, we know that the
 * selectivities represent the relative positions of the low and high bounds
 * within the column's range, so instead of figuring the selectivity as
 * hisel * losel, we can figure it as hisel + losel - 1.  (To visualize this,
 * see that hisel is the fraction of the range below the high bound, while
 * losel is the fraction above the low bound; so hisel can be interpreted
 * directly as a 0..1 value but we need to convert losel to 1-losel before
 * interpreting it as a value.	Then the available range is 1-losel to hisel.
 * However, this calculation double-excludes nulls, so really we need
 * hisel + losel + null_frac - 1.)
 *
 * If either selectivity is exactly DEFAULT_INEQ_SEL, we forget this equation
 * and instead use DEFAULT_RANGE_INEQ_SEL.	The same applies if the equation
 * yields an impossible (negative) result.
 *
 * A free side-effect is that we can recognize redundant inequalities such
 * as "x < 4 AND x < 5"; only the tighter constraint will be counted.
 *
 * Of course this is all very dependent on the behavior of
 * scalarltsel/scalargtsel; perhaps some day we can generalize the approach.
 */
Selectivity
clauselist_selectivity(PlannerInfo *root,
					   List *clauses,
					   int varRelid,
					   JoinType jointype)
{
	Selectivity s1 = 1.0;
	RangeQueryClause *rqlist = NULL;
	ListCell   *l;

	/*
	 * If there's exactly one clause, then no use in trying to match up
	 * pairs, so just go directly to clause_selectivity().
	 */
	if (list_length(clauses) == 1)
		return clause_selectivity(root, (Node *) linitial(clauses),
								  varRelid, jointype);

	/*
	 * Initial scan over clauses.  Anything that doesn't look like a potential
	 * rangequery clause gets multiplied into s1 and forgotten. Anything that
	 * does gets inserted into an rqlist entry.
	 */
	foreach(l, clauses)
	{
		Node	   *clause = (Node *) lfirst(l);
		RestrictInfo *rinfo;
		Selectivity s2;

		/* Always compute the selectivity using clause_selectivity */
		s2 = clause_selectivity(root, clause, varRelid, jointype);

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
				ok = (bms_membership(rinfo->clause_relids) == BMS_SINGLETON) &&
					(is_pseudo_constant_clause_relids(lsecond(expr->args),
													  rinfo->right_relids) ||
					 (varonleft = false,
					  is_pseudo_constant_clause_relids(linitial(expr->args),
													   rinfo->left_relids)));
			}
			else
			{
				ok = (NumRelids(clause) == 1) &&
					(is_pseudo_constant_clause(lsecond(expr->args)) ||
					 (varonleft = false,
					  is_pseudo_constant_clause(linitial(expr->args))));
			}

			if (ok)
			{
				/*
				 * If it's not a "<" or ">" operator, just merge the
				 * selectivity in generically.	But if it's the right oprrest,
				 * add the clause to rqlist for later processing.
				 */
				switch (get_oprrest(expr->opno))
				{
					case F_SCALARLTSEL:
						addRangeClause(&rqlist, clause,
									   varonleft, true, s2);
						break;
					case F_SCALARGTSEL:
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
				/* HACK: disable nulltestsel's special outer-join logic */
				s2 += nulltestsel(root, IS_NULL, rqlist->var,
								  varRelid, JOIN_INNER);

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
				 * x < y AND x < z.
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
				 * x > y AND x > z.
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
 * bms_is_subset_singleton
 *
 * Same result as bms_is_subset(s, bms_make_singleton(x)),
 * but a little faster and doesn't leak memory.
 *
 * Is this of use anywhere else?  If so move to bitmapset.c ...
 */
static bool
bms_is_subset_singleton(const Bitmapset *s, int x)
{
	switch (bms_membership(s))
	{
		case BMS_EMPTY_SET:
			return true;
		case BMS_SINGLETON:
			return bms_is_member(x, s);
		case BMS_MULTIPLE:
			return false;
	}
	/* can't get here... */
	return false;
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
 * When varRelid is 0, all variables are treated as variables.	This
 * is appropriate for ordinary join clauses and restriction clauses.
 *
 * jointype is the join type, if the clause is a join clause.  Pass JOIN_INNER
 * if the clause isn't a join clause or the context is uncertain.
 */
Selectivity
clause_selectivity(PlannerInfo *root,
				   Node *clause,
				   int varRelid,
				   JoinType jointype)
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
		 * return 1.0.	The only exception is that a constant FALSE may be
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
		 * the clause.	We can cache if varRelid is zero or the clause
		 * contains only vars of that relid --- otherwise varRelid will affect
		 * the result, so mustn't cache.  We also have to be careful about the
		 * jointype.  It's OK to cache when jointype is JOIN_INNER or one of
		 * the outer join types (any given outer-join clause should always be
		 * examined with the same jointype, so result won't change). It's not
		 * OK to cache when jointype is one of the special types associated
		 * with IN processing, because the same clause may be examined with
		 * different jointypes and the result should vary.
		 */
		if (varRelid == 0 ||
			bms_is_subset_singleton(rinfo->clause_relids, varRelid))
		{
			switch (jointype)
			{
				case JOIN_INNER:
				case JOIN_LEFT:
				case JOIN_FULL:
				case JOIN_RIGHT:
					/* Cacheable --- do we already have the result? */
					if (rinfo->this_selec >= 0)
						return rinfo->this_selec;
					cacheable = true;
					break;

				case JOIN_IN:
				case JOIN_REVERSE_IN:
				case JOIN_UNIQUE_OUTER:
				case JOIN_UNIQUE_INNER:
					/* unsafe to cache */
					break;
			}
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
			RangeTblEntry *rte = planner_rt_fetch(var->varno, root);

			if (rte->rtekind == RTE_SUBQUERY)
			{
				/*
				 * XXX not smart about subquery references... any way to do
				 * better than default?
				 */
			}
			else
			{
				/*
				 * A Var at the top of a clause must be a bool Var. This is
				 * equivalent to the clause reln.attribute = 't', so we
				 * compute the selectivity as if that is what we have.
				 */
				s1 = restriction_selectivity(root,
											 BooleanEqualOperator,
											 list_make2(var,
														makeBoolConst(true,
																	  false)),
											 varRelid);
			}
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
	else if (not_clause(clause))
	{
		/* inverse of the selectivity of the underlying clause */
		s1 = 1.0 - clause_selectivity(root,
								  (Node *) get_notclausearg((Expr *) clause),
									  varRelid,
									  jointype);
	}
	else if (and_clause(clause))
	{
		/* share code with clauselist_selectivity() */
		s1 = clauselist_selectivity(root,
									((BoolExpr *) clause)->args,
									varRelid,
									jointype);
	}
	else if (or_clause(clause))
	{
		/*
		 * Selectivities for an OR clause are computed as s1+s2 - s1*s2 to
		 * account for the probable overlap of selected tuple sets.
		 *
		 * XXX is this too conservative?
		 */
		ListCell   *arg;

		s1 = 0.0;
		foreach(arg, ((BoolExpr *) clause)->args)
		{
			Selectivity s2 = clause_selectivity(root,
												(Node *) lfirst(arg),
												varRelid,
												jointype);

			s1 = s1 + s2 - s1 * s2;
		}
	}
	else if (is_opclause(clause) || IsA(clause, DistinctExpr))
	{
		Oid			opno = ((OpExpr *) clause)->opno;
		bool		is_join_clause;

		if (varRelid != 0)
		{
			/*
			 * If we are considering a nestloop join then all clauses are
			 * restriction clauses, since we are only interested in the one
			 * relation.
			 */
			is_join_clause = false;
		}
		else
		{
			/*
			 * Otherwise, it's a join if there's more than one relation used.
			 * We can optimize this calculation if an rinfo was passed.
			 */
			if (rinfo)
				is_join_clause = (bms_membership(rinfo->clause_relids) ==
								  BMS_MULTIPLE);
			else
				is_join_clause = (NumRelids(clause) > 1);
		}

		if (is_join_clause)
		{
			/* Estimate selectivity for a join clause. */
			s1 = join_selectivity(root, opno,
								  ((OpExpr *) clause)->args,
								  jointype);
		}
		else
		{
			/* Estimate selectivity for a restriction clause. */
			s1 = restriction_selectivity(root, opno,
										 ((OpExpr *) clause)->args,
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
		/*
		 * This is not an operator, so we guess at the selectivity. THIS IS A
		 * HACK TO GET V4 OUT THE DOOR.  FUNCS SHOULD BE ABLE TO HAVE
		 * SELECTIVITIES THEMSELVES.	   -- JMH 7/9/92
		 */
		s1 = (Selectivity) 0.3333333;
	}
#ifdef NOT_USED
	else if (is_subplan(clause))
	{
		/*
		 * Just for the moment! FIX ME! - vadim 02/04/98
		 */
		s1 = (Selectivity) 0.5;
	}
#endif
	else if (IsA(clause, ScalarArrayOpExpr))
	{
		/* First, decide if it's a join clause, same as for OpExpr */
		bool		is_join_clause;

		if (varRelid != 0)
		{
			/*
			 * If we are considering a nestloop join then all clauses are
			 * restriction clauses, since we are only interested in the one
			 * relation.
			 */
			is_join_clause = false;
		}
		else
		{
			/*
			 * Otherwise, it's a join if there's more than one relation used.
			 * We can optimize this calculation if an rinfo was passed.
			 */
			if (rinfo)
				is_join_clause = (bms_membership(rinfo->clause_relids) ==
								  BMS_MULTIPLE);
			else
				is_join_clause = (NumRelids(clause) > 1);
		}

		/* Use node specific selectivity calculation function */
		s1 = scalararraysel(root,
							(ScalarArrayOpExpr *) clause,
							is_join_clause,
							varRelid,
							jointype);
	}
	else if (IsA(clause, RowCompareExpr))
	{
		/* Use node specific selectivity calculation function */
		s1 = rowcomparesel(root,
						   (RowCompareExpr *) clause,
						   varRelid,
						   jointype);
	}
	else if (IsA(clause, NullTest))
	{
		/* Use node specific selectivity calculation function */
		s1 = nulltestsel(root,
						 ((NullTest *) clause)->nulltesttype,
						 (Node *) ((NullTest *) clause)->arg,
						 varRelid,
						 jointype);
	}
	else if (IsA(clause, BooleanTest))
	{
		/* Use node specific selectivity calculation function */
		s1 = booltestsel(root,
						 ((BooleanTest *) clause)->booltesttype,
						 (Node *) ((BooleanTest *) clause)->arg,
						 varRelid,
						 jointype);
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
		s1 = clause_selectivity(root,
								(Node *) ((RelabelType *) clause)->arg,
								varRelid,
								jointype);
	}
	else if (IsA(clause, CoerceToDomain))
	{
		/* Not sure this case is needed, but it can't hurt */
		s1 = clause_selectivity(root,
								(Node *) ((CoerceToDomain *) clause)->arg,
								varRelid,
								jointype);
	}

	/* Cache the result if possible */
	if (cacheable)
		rinfo->this_selec = s1;

#ifdef SELECTIVITY_DEBUG
	elog(DEBUG4, "clause_selectivity: s1 %f", s1);
#endif   /* SELECTIVITY_DEBUG */

	return s1;
}
