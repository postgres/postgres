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
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/util/restrictinfo.c,v 1.19.4.1 2007/07/31 19:54:27 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "optimizer/clauses.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/var.h"


static bool join_clause_is_redundant(Query *root,
						 RestrictInfo *rinfo,
						 List *reference_list,
						 Relids outer_relids,
						 Relids inner_relids,
						 JoinType jointype);


/*
 * restriction_is_or_clause
 *
 * Returns t iff the restrictinfo node contains an 'or' clause.
 */
bool
restriction_is_or_clause(RestrictInfo *restrictinfo)
{
	if (restrictinfo != NULL &&
		or_clause((Node *) restrictinfo->clause))
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

		if (clause->ispusheddown)
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
							  Relids outer_relids,
							  Relids inner_relids,
							  JoinType jointype)
{
	List	   *result = NIL;
	List	   *item;

	foreach(item, restrictinfo_list)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(item);

		/* drop it if redundant with any prior clause */
		if (join_clause_is_redundant(root, rinfo, result,
									 outer_relids, inner_relids,
									 jointype))
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
								 Relids outer_relids,
								 Relids inner_relids,
								 JoinType jointype)
{
	List	   *result = NIL;
	List	   *item;

	foreach(item, restrictinfo_list)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(item);

		/* drop it if redundant with any reference clause */
		if (join_clause_is_redundant(root, rinfo, reference_list,
									 outer_relids, inner_relids,
									 jointype))
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
static bool
join_clause_is_redundant(Query *root,
						 RestrictInfo *rinfo,
						 List *reference_list,
						 Relids outer_relids,
						 Relids inner_relids,
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

		cache_mergeclause_pathkeys(root, rinfo);

		/* do the cheap tests first */
		foreach(refitem, reference_list)
		{
			RestrictInfo *refrinfo = (RestrictInfo *) lfirst(refitem);

			if (refrinfo->mergejoinoperator != InvalidOid &&
				rinfo->left_pathkey == refrinfo->left_pathkey &&
				rinfo->right_pathkey == refrinfo->right_pathkey &&
				(rinfo->ispusheddown == refrinfo->ispusheddown ||
				 !IS_OUTER_JOIN(jointype)))
			{
				redundant = true;
				break;
			}
		}

		if (redundant)
		{
			/*
			 * It looks redundant, now check for special cases.  This is
			 * ugly and slow because of the mistaken decision to not set
			 * left_relids/right_relids all the time, as 8.0 and up do.
			 * Not going to change that in 7.x though.
			 */
			Relids		left_relids = rinfo->left_relids;
			Relids		right_relids = rinfo->right_relids;

			if (left_relids == NULL)
				left_relids = pull_varnos(get_leftop(rinfo->clause));
			if (right_relids == NULL)
				right_relids = pull_varnos(get_rightop(rinfo->clause));

			if (bms_is_empty(left_relids) || bms_is_empty(right_relids))
				return false;	/* var = const, so not redundant */

			/* check for either side mentioning both rels */
			if (bms_overlap(left_relids, outer_relids) &&
				bms_overlap(left_relids, inner_relids))
				return false;	/* clause LHS uses both, so not redundant */
			if (bms_overlap(right_relids, outer_relids) &&
				bms_overlap(right_relids, inner_relids))
				return false;	/* clause RHS uses both, so not redundant */

			return true;		/* else it really is redundant */
		}
	}

	/* otherwise, not redundant */
	return false;
}
