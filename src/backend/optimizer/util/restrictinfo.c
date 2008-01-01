/*-------------------------------------------------------------------------
 *
 * restrictinfo.c
 *	  RestrictInfo node manipulation routines.
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/util/restrictinfo.c,v 1.55 2008/01/01 19:45:50 momjian Exp $
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
static bool join_clause_is_redundant(PlannerInfo *root,
						 RestrictInfo *rinfo,
						 List *reference_list);


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
	restrictinfo->parent_ec = NULL;

	restrictinfo->eval_cost.startup = -1;
	restrictinfo->this_selec = -1;

	restrictinfo->mergeopfamilies = NIL;

	restrictinfo->left_ec = NULL;
	restrictinfo->right_ec = NULL;
	restrictinfo->left_em = NULL;
	restrictinfo->right_em = NULL;
	restrictinfo->scansel_cache = NIL;

	restrictinfo->outer_is_left = false;

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
 * select_nonredundant_join_clauses
 *
 * Given a list of RestrictInfo clauses that are to be applied in a join,
 * select the ones that are not redundant with any clause in the
 * reference_list.	This is used only for nestloop-with-inner-indexscan
 * joins: any clauses being checked by the index should be removed from
 * the qpquals list.
 *
 * "Redundant" means either equal() or derived from the same EquivalenceClass.
 * We have to check the latter because indxqual.c may select different derived
 * clauses than were selected by generate_join_implied_equalities().
 *
 * Note that we assume the given restrictinfo_list has already been checked
 * for local redundancies, so we don't check again.
 */
List *
select_nonredundant_join_clauses(PlannerInfo *root,
								 List *restrictinfo_list,
								 List *reference_list)
{
	List	   *result = NIL;
	ListCell   *item;

	foreach(item, restrictinfo_list)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(item);

		/* drop it if redundant with any reference clause */
		if (join_clause_is_redundant(root, rinfo, reference_list))
			continue;

		/* otherwise, add it to result list */
		result = lappend(result, rinfo);
	}

	return result;
}

/*
 * join_clause_is_redundant
 *		Test whether rinfo is redundant with any clause in reference_list.
 */
static bool
join_clause_is_redundant(PlannerInfo *root,
						 RestrictInfo *rinfo,
						 List *reference_list)
{
	ListCell   *refitem;

	foreach(refitem, reference_list)
	{
		RestrictInfo *refrinfo = (RestrictInfo *) lfirst(refitem);

		/* always consider exact duplicates redundant */
		if (equal(rinfo, refrinfo))
			return true;

		/* check if derived from same EquivalenceClass */
		if (rinfo->parent_ec != NULL &&
			rinfo->parent_ec == refrinfo->parent_ec)
			return true;
	}

	return false;
}
