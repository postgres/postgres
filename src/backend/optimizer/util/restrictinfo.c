/*-------------------------------------------------------------------------
 *
 * restrictinfo.c
 *	  RestrictInfo node manipulation routines.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/util/restrictinfo.c,v 1.24 2004/01/05 05:07:36 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "optimizer/clauses.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/var.h"


static Expr *make_sub_restrictinfos(Expr *clause, bool is_pushed_down,
									bool valid_everywhere);
static bool join_clause_is_redundant(Query *root,
						 RestrictInfo *rinfo,
						 List *reference_list,
						 JoinType jointype);


/*
 * make_restrictinfo
 *
 * Build a RestrictInfo node containing the given subexpression.
 *
 * The is_pushed_down and valid_everywhere flags must be supplied by the
 * caller.
 *
 * We initialize fields that depend only on the given subexpression, leaving
 * others that depend on context (or may never be needed at all) to be filled
 * later.
 */
RestrictInfo *
make_restrictinfo(Expr *clause, bool is_pushed_down, bool valid_everywhere)
{
	RestrictInfo *restrictinfo = makeNode(RestrictInfo);

	restrictinfo->clause = clause;
	restrictinfo->is_pushed_down = is_pushed_down;
	restrictinfo->valid_everywhere = valid_everywhere;
	restrictinfo->can_join = false;		/* may get set below */

	/*
	 * If it's a binary opclause, set up left/right relids info.
	 * In any case set up the total clause relids info.
	 */
	if (is_opclause(clause) && length(((OpExpr *) clause)->args) == 2)
	{
		restrictinfo->left_relids = pull_varnos(get_leftop(clause));
		restrictinfo->right_relids = pull_varnos(get_rightop(clause));

		restrictinfo->clause_relids = bms_union(restrictinfo->left_relids,
												restrictinfo->right_relids);

		/*
		 * Does it look like a normal join clause, i.e., a binary operator
		 * relating expressions that come from distinct relations? If so
		 * we might be able to use it in a join algorithm.  Note that this
		 * is a purely syntactic test that is made regardless of context.
		 */
		if (!bms_is_empty(restrictinfo->left_relids) &&
			!bms_is_empty(restrictinfo->right_relids) &&
			!bms_overlap(restrictinfo->left_relids,
						 restrictinfo->right_relids))
			restrictinfo->can_join = true;
	}
	else
	{
		/* Not a binary opclause, so mark left/right relid sets as empty */
		restrictinfo->left_relids = NULL;
		restrictinfo->right_relids = NULL;
		/* and get the total relid set the hard way */
		restrictinfo->clause_relids = pull_varnos((Node *) clause);
	}

	/*
	 * If it's an OR clause, set up a modified copy with RestrictInfos
	 * inserted above each subclause of the top-level AND/OR structure.
	 */
	if (or_clause((Node *) clause))
	{
		restrictinfo->orclause = make_sub_restrictinfos(clause,
														is_pushed_down,
														valid_everywhere);
	}
	else
	{
		/* Shouldn't be an AND clause, else flatten_andors messed up */
		Assert(!and_clause((Node *) clause));

		restrictinfo->orclause = NULL;
	}

	/*
	 * Fill in all the cacheable fields with "not yet set" markers.
	 * None of these will be computed until/unless needed.  Note in
	 * particular that we don't mark a binary opclause as mergejoinable
	 * or hashjoinable here; that happens only if it appears in the right
	 * context (top level of a joinclause list).
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
 */
static Expr *
make_sub_restrictinfos(Expr *clause, bool is_pushed_down,
					   bool valid_everywhere)
{
	if (or_clause((Node *) clause))
	{
		List	   *orlist = NIL;
		List	   *temp;

		foreach(temp, ((BoolExpr *) clause)->args)
			orlist = lappend(orlist,
							 make_sub_restrictinfos(lfirst(temp),
													is_pushed_down,
													valid_everywhere));
		return make_orclause(orlist);
	}
	else if (and_clause((Node *) clause))
	{
		List	   *andlist = NIL;
		List	   *temp;

		foreach(temp, ((BoolExpr *) clause)->args)
			andlist = lappend(andlist,
							  make_sub_restrictinfos(lfirst(temp),
													 is_pushed_down,
													 valid_everywhere));
		return make_andclause(andlist);
	}
	else
		return (Expr *) make_restrictinfo(clause,
										  is_pushed_down,
										  valid_everywhere);
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
 */
List *
get_actual_clauses(List *restrictinfo_list)
{
	List	   *result = NIL;
	List	   *temp;

	foreach(temp, restrictinfo_list)
	{
		RestrictInfo *clause = (RestrictInfo *) lfirst(temp);

		result = lappend(result, clause->clause);
	}
	return result;
}

/*
 * get_actual_join_clauses
 *
 * Extract clauses from 'restrictinfo_list', separating those that
 * syntactically match the join level from those that were pushed down.
 */
void
get_actual_join_clauses(List *restrictinfo_list,
						List **joinquals, List **otherquals)
{
	List	   *temp;

	*joinquals = NIL;
	*otherquals = NIL;

	foreach(temp, restrictinfo_list)
	{
		RestrictInfo *clause = (RestrictInfo *) lfirst(temp);

		if (clause->is_pushed_down)
			*otherquals = lappend(*otherquals, clause->clause);
		else
			*joinquals = lappend(*joinquals, clause->clause);
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
remove_redundant_join_clauses(Query *root, List *restrictinfo_list,
							  JoinType jointype)
{
	List	   *result = NIL;
	List	   *item;

	foreach(item, restrictinfo_list)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(item);

		/* drop it if redundant with any prior clause */
		if (join_clause_is_redundant(root, rinfo, result, jointype))
			continue;

		/* otherwise, add it to result list */
		result = lappend(result, rinfo);
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
select_nonredundant_join_clauses(Query *root,
								 List *restrictinfo_list,
								 List *reference_list,
								 JoinType jointype)
{
	List	   *result = NIL;
	List	   *item;

	foreach(item, restrictinfo_list)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(item);

		/* drop it if redundant with any reference clause */
		if (join_clause_is_redundant(root, rinfo, reference_list, jointype))
			continue;

		/* otherwise, add it to result list */
		result = lappend(result, rinfo);
	}

	return result;
}

/*
 * join_clause_is_redundant
 *		Returns true if rinfo is redundant with any clause in reference_list.
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
 * Weird special case: if we have two clauses that seem redundant
 * except one is pushed down into an outer join and the other isn't,
 * then they're not really redundant, because one constrains the
 * joined rows after addition of null fill rows, and the other doesn't.
 */
static bool
join_clause_is_redundant(Query *root,
						 RestrictInfo *rinfo,
						 List *reference_list,
						 JoinType jointype)
{
	/* always consider exact duplicates redundant */
	/* XXX would it be sufficient to use ptrMember here? */
	if (member(rinfo, reference_list))
		return true;

	/* check for redundant merge clauses */
	if (rinfo->mergejoinoperator != InvalidOid)
	{
		bool		redundant = false;
		List	   *refitem;

		/* do the cheap test first: is it a "var = const" clause? */
		if (bms_is_empty(rinfo->left_relids) ||
			bms_is_empty(rinfo->right_relids))
			return false;		/* var = const, so not redundant */

		cache_mergeclause_pathkeys(root, rinfo);

		foreach(refitem, reference_list)
		{
			RestrictInfo *refrinfo = (RestrictInfo *) lfirst(refitem);

			if (refrinfo->mergejoinoperator != InvalidOid &&
				rinfo->left_pathkey == refrinfo->left_pathkey &&
				rinfo->right_pathkey == refrinfo->right_pathkey &&
				(rinfo->is_pushed_down == refrinfo->is_pushed_down ||
				 !IS_OUTER_JOIN(jointype)))
			{
				redundant = true;
				break;
			}
		}

		if (redundant)
			return true;		/* var = var, so redundant */
	}

	/* otherwise, not redundant */
	return false;
}
