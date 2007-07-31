/*-------------------------------------------------------------------------
 *
 * restrictinfo.c
 *	  RestrictInfo node manipulation routines.
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/util/restrictinfo.c,v 1.49.2.1 2007/07/31 19:53:50 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/paths.h"
#include "optimizer/predtest.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/var.h"


static RestrictInfo *make_restrictinfo_internal(Expr *clause,
						   Expr *orclause,
						   bool is_pushed_down,
						   bool outerjoin_delayed,
						   bool pseudoconstant,
						   Relids required_relids);
static Expr *make_sub_restrictinfos(Expr *clause,
					   bool is_pushed_down,
					   bool outerjoin_delayed,
					   bool pseudoconstant,
					   Relids required_relids);
static RestrictInfo *join_clause_is_redundant(PlannerInfo *root,
						 RestrictInfo *rinfo,
						 List *reference_list,
						 Relids outer_relids,
						 Relids inner_relids,
						 bool isouterjoin);


/*
 * make_restrictinfo
 *
 * Build a RestrictInfo node containing the given subexpression.
 *
 * The is_pushed_down, outerjoin_delayed, and pseudoconstant flags for the
 * RestrictInfo must be supplied by the caller.  required_relids can be NULL,
 * in which case it defaults to the actual clause contents (i.e.,
 * clause_relids).
 *
 * We initialize fields that depend only on the given subexpression, leaving
 * others that depend on context (or may never be needed at all) to be filled
 * later.
 */
RestrictInfo *
make_restrictinfo(Expr *clause,
				  bool is_pushed_down,
				  bool outerjoin_delayed,
				  bool pseudoconstant,
				  Relids required_relids)
{
	/*
	 * If it's an OR clause, build a modified copy with RestrictInfos inserted
	 * above each subclause of the top-level AND/OR structure.
	 */
	if (or_clause((Node *) clause))
		return (RestrictInfo *) make_sub_restrictinfos(clause,
													   is_pushed_down,
													   outerjoin_delayed,
													   pseudoconstant,
													   required_relids);

	/* Shouldn't be an AND clause, else AND/OR flattening messed up */
	Assert(!and_clause((Node *) clause));

	return make_restrictinfo_internal(clause,
									  NULL,
									  is_pushed_down,
									  outerjoin_delayed,
									  pseudoconstant,
									  required_relids);
}

/*
 * make_restrictinfo_from_bitmapqual
 *
 * Given the bitmapqual Path structure for a bitmap indexscan, generate
 * RestrictInfo node(s) equivalent to the condition represented by the
 * indexclauses of the Path structure.
 *
 * The result is a List (effectively, implicit-AND representation) of
 * RestrictInfos.
 *
 * The caller must pass is_pushed_down, but we assume outerjoin_delayed
 * and pseudoconstant are false (no such qual should ever get into a
 * bitmapqual).
 *
 * If include_predicates is true, we add any partial index predicates to
 * the explicit index quals.  When this is not true, we return a condition
 * that might be weaker than the actual scan represents.
 *
 * To do this through the normal make_restrictinfo() API, callers would have
 * to strip off the RestrictInfo nodes present in the indexclauses lists, and
 * then make_restrictinfo() would have to build new ones.  It's better to have
 * a specialized routine to allow sharing of RestrictInfos.
 *
 * The qual manipulations here are much the same as in create_bitmap_subplan;
 * keep the two routines in sync!
 */
List *
make_restrictinfo_from_bitmapqual(Path *bitmapqual,
								  bool is_pushed_down,
								  bool include_predicates)
{
	List	   *result;
	ListCell   *l;

	if (IsA(bitmapqual, BitmapAndPath))
	{
		BitmapAndPath *apath = (BitmapAndPath *) bitmapqual;

		/*
		 * There may well be redundant quals among the subplans, since a
		 * top-level WHERE qual might have gotten used to form several
		 * different index quals.  We don't try exceedingly hard to eliminate
		 * redundancies, but we do eliminate obvious duplicates by using
		 * list_concat_unique.
		 */
		result = NIL;
		foreach(l, apath->bitmapquals)
		{
			List	   *sublist;

			sublist = make_restrictinfo_from_bitmapqual((Path *) lfirst(l),
														is_pushed_down,
														include_predicates);
			result = list_concat_unique(result, sublist);
		}
	}
	else if (IsA(bitmapqual, BitmapOrPath))
	{
		BitmapOrPath *opath = (BitmapOrPath *) bitmapqual;
		List	   *withris = NIL;
		List	   *withoutris = NIL;

		/*
		 * Here, we only detect qual-free subplans.  A qual-free subplan would
		 * cause us to generate "... OR true ..."  which we may as well reduce
		 * to just "true".	We do not try to eliminate redundant subclauses
		 * because (a) it's not as likely as in the AND case, and (b) we might
		 * well be working with hundreds or even thousands of OR conditions,
		 * perhaps from a long IN list.  The performance of list_append_unique
		 * would be unacceptable.
		 */
		foreach(l, opath->bitmapquals)
		{
			List	   *sublist;

			sublist = make_restrictinfo_from_bitmapqual((Path *) lfirst(l),
														is_pushed_down,
														include_predicates);
			if (sublist == NIL)
			{
				/*
				 * If we find a qual-less subscan, it represents a constant
				 * TRUE, and hence the OR result is also constant TRUE, so we
				 * can stop here.
				 */
				return NIL;
			}

			/*
			 * If the sublist contains multiple RestrictInfos, we create an
			 * AND subclause.  If there's just one, we have to check if it's
			 * an OR clause, and if so flatten it to preserve AND/OR flatness
			 * of our output.
			 *
			 * We construct lists with and without sub-RestrictInfos, so as
			 * not to have to regenerate duplicate RestrictInfos below.
			 */
			if (list_length(sublist) > 1)
			{
				withris = lappend(withris, make_andclause(sublist));
				sublist = get_actual_clauses(sublist);
				withoutris = lappend(withoutris, make_andclause(sublist));
			}
			else
			{
				RestrictInfo *subri = (RestrictInfo *) linitial(sublist);

				Assert(IsA(subri, RestrictInfo));
				if (restriction_is_or_clause(subri))
				{
					BoolExpr   *subor = (BoolExpr *) subri->orclause;

					Assert(or_clause((Node *) subor));
					withris = list_concat(withris,
										  list_copy(subor->args));
					subor = (BoolExpr *) subri->clause;
					Assert(or_clause((Node *) subor));
					withoutris = list_concat(withoutris,
											 list_copy(subor->args));
				}
				else
				{
					withris = lappend(withris, subri);
					withoutris = lappend(withoutris, subri->clause);
				}
			}
		}

		/*
		 * Avoid generating one-element ORs, which could happen due to
		 * redundancy elimination or ScalarArrayOpExpr quals.
		 */
		if (list_length(withris) <= 1)
			result = withris;
		else
		{
			/* Here's the magic part not available to outside callers */
			result =
				list_make1(make_restrictinfo_internal(make_orclause(withoutris),
													  make_orclause(withris),
													  is_pushed_down,
													  false,
													  false,
													  NULL));
		}
	}
	else if (IsA(bitmapqual, IndexPath))
	{
		IndexPath  *ipath = (IndexPath *) bitmapqual;

		result = list_copy(ipath->indexclauses);
		if (include_predicates && ipath->indexinfo->indpred != NIL)
		{
			foreach(l, ipath->indexinfo->indpred)
			{
				Expr	   *pred = (Expr *) lfirst(l);

				/*
				 * We know that the index predicate must have been implied by
				 * the query condition as a whole, but it may or may not be
				 * implied by the conditions that got pushed into the
				 * bitmapqual.	Avoid generating redundant conditions.
				 */
				if (!predicate_implied_by(list_make1(pred), result))
					result = lappend(result,
									 make_restrictinfo(pred,
													   is_pushed_down,
													   false,
													   false,
													   NULL));
			}
		}
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d", nodeTag(bitmapqual));
		result = NIL;			/* keep compiler quiet */
	}

	return result;
}

/*
 * make_restrictinfo_internal
 *
 * Common code for the main entry points and the recursive cases.
 */
static RestrictInfo *
make_restrictinfo_internal(Expr *clause,
						   Expr *orclause,
						   bool is_pushed_down,
						   bool outerjoin_delayed,
						   bool pseudoconstant,
						   Relids required_relids)
{
	RestrictInfo *restrictinfo = makeNode(RestrictInfo);

	restrictinfo->clause = clause;
	restrictinfo->orclause = orclause;
	restrictinfo->is_pushed_down = is_pushed_down;
	restrictinfo->outerjoin_delayed = outerjoin_delayed;
	restrictinfo->pseudoconstant = pseudoconstant;
	restrictinfo->can_join = false;		/* may get set below */

	/*
	 * If it's a binary opclause, set up left/right relids info. In any case
	 * set up the total clause relids info.
	 */
	if (is_opclause(clause) && list_length(((OpExpr *) clause)->args) == 2)
	{
		restrictinfo->left_relids = pull_varnos(get_leftop(clause));
		restrictinfo->right_relids = pull_varnos(get_rightop(clause));

		restrictinfo->clause_relids = bms_union(restrictinfo->left_relids,
												restrictinfo->right_relids);

		/*
		 * Does it look like a normal join clause, i.e., a binary operator
		 * relating expressions that come from distinct relations? If so we
		 * might be able to use it in a join algorithm.  Note that this is a
		 * purely syntactic test that is made regardless of context.
		 */
		if (!bms_is_empty(restrictinfo->left_relids) &&
			!bms_is_empty(restrictinfo->right_relids) &&
			!bms_overlap(restrictinfo->left_relids,
						 restrictinfo->right_relids))
		{
			restrictinfo->can_join = true;
			/* pseudoconstant should certainly not be true */
			Assert(!restrictinfo->pseudoconstant);
		}
	}
	else
	{
		/* Not a binary opclause, so mark left/right relid sets as empty */
		restrictinfo->left_relids = NULL;
		restrictinfo->right_relids = NULL;
		/* and get the total relid set the hard way */
		restrictinfo->clause_relids = pull_varnos((Node *) clause);
	}

	/* required_relids defaults to clause_relids */
	if (required_relids != NULL)
		restrictinfo->required_relids = required_relids;
	else
		restrictinfo->required_relids = restrictinfo->clause_relids;

	/*
	 * Fill in all the cacheable fields with "not yet set" markers. None of
	 * these will be computed until/unless needed.	Note in particular that we
	 * don't mark a binary opclause as mergejoinable or hashjoinable here;
	 * that happens only if it appears in the right context (top level of a
	 * joinclause list).
	 */
	restrictinfo->eval_cost.startup = -1;
	restrictinfo->this_selec = -1;

	restrictinfo->mergejoinoperator = InvalidOid;
	restrictinfo->left_sortop = InvalidOid;
	restrictinfo->right_sortop = InvalidOid;

	restrictinfo->left_pathkey = NIL;
	restrictinfo->right_pathkey = NIL;

	restrictinfo->left_mergescansel = -1;
	restrictinfo->right_mergescansel = -1;

	restrictinfo->hashjoinoperator = InvalidOid;

	restrictinfo->left_bucketsize = -1;
	restrictinfo->right_bucketsize = -1;

	return restrictinfo;
}

/*
 * Recursively insert sub-RestrictInfo nodes into a boolean expression.
 *
 * We put RestrictInfos above simple (non-AND/OR) clauses and above
 * sub-OR clauses, but not above sub-AND clauses, because there's no need.
 * This may seem odd but it is closely related to the fact that we use
 * implicit-AND lists at top level of RestrictInfo lists.  Only ORs and
 * simple clauses are valid RestrictInfos.
 *
 * The same is_pushed_down, outerjoin_delayed, and pseudoconstant flag
 * values can be applied to all RestrictInfo nodes in the result.
 *
 * The given required_relids are attached to our top-level output,
 * but any OR-clause constituents are allowed to default to just the
 * contained rels.
 */
static Expr *
make_sub_restrictinfos(Expr *clause,
					   bool is_pushed_down,
					   bool outerjoin_delayed,
					   bool pseudoconstant,
					   Relids required_relids)
{
	if (or_clause((Node *) clause))
	{
		List	   *orlist = NIL;
		ListCell   *temp;

		foreach(temp, ((BoolExpr *) clause)->args)
			orlist = lappend(orlist,
							 make_sub_restrictinfos(lfirst(temp),
													is_pushed_down,
													outerjoin_delayed,
													pseudoconstant,
													NULL));
		return (Expr *) make_restrictinfo_internal(clause,
												   make_orclause(orlist),
												   is_pushed_down,
												   outerjoin_delayed,
												   pseudoconstant,
												   required_relids);
	}
	else if (and_clause((Node *) clause))
	{
		List	   *andlist = NIL;
		ListCell   *temp;

		foreach(temp, ((BoolExpr *) clause)->args)
			andlist = lappend(andlist,
							  make_sub_restrictinfos(lfirst(temp),
													 is_pushed_down,
													 outerjoin_delayed,
													 pseudoconstant,
													 required_relids));
		return make_andclause(andlist);
	}
	else
		return (Expr *) make_restrictinfo_internal(clause,
												   NULL,
												   is_pushed_down,
												   outerjoin_delayed,
												   pseudoconstant,
												   required_relids);
}

/*
 * restriction_is_or_clause
 *
 * Returns t iff the restrictinfo node contains an 'or' clause.
 */
bool
restriction_is_or_clause(RestrictInfo *restrictinfo)
{
	if (restrictinfo->orclause != NULL)
		return true;
	else
		return false;
}

/*
 * get_actual_clauses
 *
 * Returns a list containing the bare clauses from 'restrictinfo_list'.
 *
 * This is only to be used in cases where none of the RestrictInfos can
 * be pseudoconstant clauses (for instance, it's OK on indexqual lists).
 */
List *
get_actual_clauses(List *restrictinfo_list)
{
	List	   *result = NIL;
	ListCell   *l;

	foreach(l, restrictinfo_list)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

		Assert(IsA(rinfo, RestrictInfo));

		Assert(!rinfo->pseudoconstant);

		result = lappend(result, rinfo->clause);
	}
	return result;
}

/*
 * extract_actual_clauses
 *
 * Extract bare clauses from 'restrictinfo_list', returning either the
 * regular ones or the pseudoconstant ones per 'pseudoconstant'.
 */
List *
extract_actual_clauses(List *restrictinfo_list,
					   bool pseudoconstant)
{
	List	   *result = NIL;
	ListCell   *l;

	foreach(l, restrictinfo_list)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

		Assert(IsA(rinfo, RestrictInfo));

		if (rinfo->pseudoconstant == pseudoconstant)
			result = lappend(result, rinfo->clause);
	}
	return result;
}

/*
 * extract_actual_join_clauses
 *
 * Extract bare clauses from 'restrictinfo_list', separating those that
 * syntactically match the join level from those that were pushed down.
 * Pseudoconstant clauses are excluded from the results.
 *
 * This is only used at outer joins, since for plain joins we don't care
 * about pushed-down-ness.
 */
void
extract_actual_join_clauses(List *restrictinfo_list,
							List **joinquals,
							List **otherquals)
{
	ListCell   *l;

	*joinquals = NIL;
	*otherquals = NIL;

	foreach(l, restrictinfo_list)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

		Assert(IsA(rinfo, RestrictInfo));

		if (rinfo->is_pushed_down)
		{
			if (!rinfo->pseudoconstant)
				*otherquals = lappend(*otherquals, rinfo->clause);
		}
		else
		{
			/* joinquals shouldn't have been marked pseudoconstant */
			Assert(!rinfo->pseudoconstant);
			*joinquals = lappend(*joinquals, rinfo->clause);
		}
	}
}

/*
 * remove_redundant_join_clauses
 *
 * Given a list of RestrictInfo clauses that are to be applied in a join,
 * remove any duplicate or redundant clauses.
 *
 * We must eliminate duplicates when forming the restrictlist for a joinrel,
 * since we will see many of the same clauses arriving from both input
 * relations. Also, if a clause is a mergejoinable clause, it's possible that
 * it is redundant with previous clauses (see optimizer/README for
 * discussion). We detect that case and omit the redundant clause from the
 * result list.
 *
 * The result is a fresh List, but it points to the same member nodes
 * as were in the input.
 */
List *
remove_redundant_join_clauses(PlannerInfo *root, List *restrictinfo_list,
							  Relids outer_relids,
							  Relids inner_relids,
							  bool isouterjoin)
{
	List	   *result = NIL;
	ListCell   *item;
	QualCost	cost;

	/*
	 * If there are any redundant clauses, we want to eliminate the ones that
	 * are more expensive in favor of the ones that are less so. Run
	 * cost_qual_eval() to ensure the eval_cost fields are set up.
	 */
	cost_qual_eval(&cost, restrictinfo_list);

	/*
	 * We don't have enough knowledge yet to be able to estimate the number of
	 * times a clause might be evaluated, so it's hard to weight the startup
	 * and per-tuple costs appropriately.  For now just weight 'em the same.
	 */
#define CLAUSECOST(r)  ((r)->eval_cost.startup + (r)->eval_cost.per_tuple)

	foreach(item, restrictinfo_list)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(item);
		RestrictInfo *prevrinfo;

		/* is it redundant with any prior clause? */
		prevrinfo = join_clause_is_redundant(root, rinfo, result,
											 outer_relids, inner_relids,
											 isouterjoin);
		if (prevrinfo == NULL)
		{
			/* no, so add it to result list */
			result = lappend(result, rinfo);
		}
		else if (CLAUSECOST(rinfo) < CLAUSECOST(prevrinfo))
		{
			/* keep this one, drop the previous one */
			result = list_delete_ptr(result, prevrinfo);
			result = lappend(result, rinfo);
		}
		/* else, drop this one */
	}

	return result;
}

/*
 * select_nonredundant_join_clauses
 *
 * Given a list of RestrictInfo clauses that are to be applied in a join,
 * select the ones that are not redundant with any clause in the
 * reference_list.
 *
 * This is similar to remove_redundant_join_clauses, but we are looking for
 * redundancies with a separate list of clauses (i.e., clauses that have
 * already been applied below the join itself).
 *
 * Note that we assume the given restrictinfo_list has already been checked
 * for local redundancies, so we don't check again.
 */
List *
select_nonredundant_join_clauses(PlannerInfo *root,
								 List *restrictinfo_list,
								 List *reference_list,
								 Relids outer_relids,
								 Relids inner_relids,
								 bool isouterjoin)
{
	List	   *result = NIL;
	ListCell   *item;

	foreach(item, restrictinfo_list)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(item);

		/* drop it if redundant with any reference clause */
		if (join_clause_is_redundant(root, rinfo, reference_list,
									 outer_relids, inner_relids,
									 isouterjoin) != NULL)
			continue;

		/* otherwise, add it to result list */
		result = lappend(result, rinfo);
	}

	return result;
}

/*
 * join_clause_is_redundant
 *		If rinfo is redundant with any clause in reference_list,
 *		return one such clause; otherwise return NULL.
 *
 * This is the guts of both remove_redundant_join_clauses and
 * select_nonredundant_join_clauses.  See the docs above for motivation.
 *
 * We can detect redundant mergejoinable clauses very cheaply by using their
 * left and right pathkeys, which uniquely identify the sets of equijoined
 * variables in question.  All the members of a pathkey set that are in the
 * left relation have already been forced to be equal; likewise for those in
 * the right relation.	So, we need to have only one clause that checks
 * equality between any set member on the left and any member on the right;
 * by transitivity, all the rest are then equal.
 *
 * However, clauses that are of the form "var expr = const expr" cannot be
 * eliminated as redundant.  This is because when there are const expressions
 * in a pathkey set, generate_implied_equalities() suppresses "var = var"
 * clauses in favor of "var = const" clauses.  We cannot afford to drop any
 * of the latter, even though they might seem redundant by the pathkey
 * membership test.
 *
 * Also, we cannot eliminate clauses wherein one side mentions vars from
 * both relations, as in "WHERE t1.f1 = t2.f1 AND t1.f1 = t1.f2 - t2.f2".
 * In this example, "t1.f2 - t2.f2" could not have been computed at all
 * before forming the join of t1 and t2, so it certainly wasn't constrained
 * earlier.
 *
 * Weird special case: if we have two clauses that seem redundant
 * except one is pushed down into an outer join and the other isn't,
 * then they're not really redundant, because one constrains the
 * joined rows after addition of null fill rows, and the other doesn't.
 */
static RestrictInfo *
join_clause_is_redundant(PlannerInfo *root,
						 RestrictInfo *rinfo,
						 List *reference_list,
						 Relids outer_relids,
						 Relids inner_relids,
						 bool isouterjoin)
{
	ListCell   *refitem;

	/* always consider exact duplicates redundant */
	foreach(refitem, reference_list)
	{
		RestrictInfo *refrinfo = (RestrictInfo *) lfirst(refitem);

		if (equal(rinfo, refrinfo))
			return refrinfo;
	}

	/* check for redundant merge clauses */
	if (rinfo->mergejoinoperator != InvalidOid)
	{
		/* do the cheap test first: is it a "var = const" clause? */
		if (bms_is_empty(rinfo->left_relids) ||
			bms_is_empty(rinfo->right_relids))
			return NULL;		/* var = const, so not redundant */

		/* check for either side mentioning both rels */
		if (bms_overlap(rinfo->left_relids, outer_relids) &&
			bms_overlap(rinfo->left_relids, inner_relids))
			return NULL;		/* clause LHS uses both, so not redundant */
		if (bms_overlap(rinfo->right_relids, outer_relids) &&
			bms_overlap(rinfo->right_relids, inner_relids))
			return NULL;		/* clause RHS uses both, so not redundant */

		cache_mergeclause_pathkeys(root, rinfo);

		foreach(refitem, reference_list)
		{
			RestrictInfo *refrinfo = (RestrictInfo *) lfirst(refitem);

			if (refrinfo->mergejoinoperator != InvalidOid)
			{
				cache_mergeclause_pathkeys(root, refrinfo);

				if (rinfo->left_pathkey == refrinfo->left_pathkey &&
					rinfo->right_pathkey == refrinfo->right_pathkey &&
					(rinfo->is_pushed_down == refrinfo->is_pushed_down ||
					 !isouterjoin))
				{
					/* Yup, it's redundant */
					return refrinfo;
				}
			}
		}
	}

	/* otherwise, not redundant */
	return NULL;
}
