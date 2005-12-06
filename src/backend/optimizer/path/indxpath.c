/*-------------------------------------------------------------------------
 *
 * indxpath.c
 *	  Routines to determine which indices are usable for scanning a
 *	  given relation, and create IndexPaths accordingly.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/path/indxpath.c,v 1.147.4.2 2005/12/06 16:59:22 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/nbtree.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/var.h"
#include "parser/parse_expr.h"
#include "rewrite/rewriteManip.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/lsyscache.h"
#include "utils/pg_locale.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"


/*
 * DoneMatchingIndexKeys() - MACRO
 */
#define DoneMatchingIndexKeys(classes)	(classes[0] == InvalidOid)

#define is_indexable_operator(clause,opclass,indexkey_on_left) \
	(indexable_operator(clause,opclass,indexkey_on_left) != InvalidOid)


static void match_index_orclauses(RelOptInfo *rel, IndexOptInfo *index,
					  List *restrictinfo_list);
static List *match_index_orclause(RelOptInfo *rel, IndexOptInfo *index,
					 List *or_clauses,
					 List *other_matching_indices);
static bool match_or_subclause_to_indexkey(RelOptInfo *rel,
							   IndexOptInfo *index,
							   Expr *clause);
static List *group_clauses_by_indexkey(RelOptInfo *rel, IndexOptInfo *index);
static List *group_clauses_by_indexkey_for_join(Query *root,
								   RelOptInfo *rel, IndexOptInfo *index,
								   Relids outer_relids,
								   JoinType jointype, bool isouterjoin);
static bool match_clause_to_indexcol(RelOptInfo *rel, IndexOptInfo *index,
						 int indexcol, Oid opclass, Expr *clause);
static bool match_join_clause_to_indexcol(RelOptInfo *rel, IndexOptInfo *index,
							  int indexcol, Oid opclass, Expr *clause);
static Oid indexable_operator(Expr *clause, Oid opclass,
				   bool indexkey_on_left);
static bool pred_test(List *predicate_list, List *restrictinfo_list,
		  List *joininfo_list);
static bool pred_test_restrict_list(Expr *predicate, List *restrictinfo_list);
static bool pred_test_recurse_clause(Expr *predicate, Node *clause);
static bool pred_test_recurse_pred(Expr *predicate, Node *clause);
static bool pred_test_simple_clause(Expr *predicate, Node *clause);
static Relids indexable_outerrelids(RelOptInfo *rel, IndexOptInfo *index);
static Path *make_innerjoin_index_path(Query *root,
						  RelOptInfo *rel, IndexOptInfo *index,
						  List *clausegroups);
static bool match_index_to_operand(Node *operand, int indexcol,
					   RelOptInfo *rel, IndexOptInfo *index);
static bool match_special_index_operator(Expr *clause, Oid opclass,
							 bool indexkey_on_left);
static List *expand_indexqual_condition(Expr *clause, Oid opclass);
static List *prefix_quals(Node *leftop, Oid opclass,
			 Const *prefix, Pattern_Prefix_Status pstatus);
static List *network_prefix_quals(Node *leftop, Oid expr_op, Oid opclass,
					 Datum rightop);
static Datum string_to_datum(const char *str, Oid datatype);
static Const *string_to_const(const char *str, Oid datatype);


/*
 * create_index_paths()
 *	  Generate all interesting index paths for the given relation.
 *	  Candidate paths are added to the rel's pathlist (using add_path).
 *
 * To be considered for an index scan, an index must match one or more
 * restriction clauses or join clauses from the query's qual condition,
 * or match the query's ORDER BY condition.
 *
 * There are two basic kinds of index scans.  A "plain" index scan uses
 * only restriction clauses (possibly none at all) in its indexqual,
 * so it can be applied in any context.  An "innerjoin" index scan uses
 * join clauses (plus restriction clauses, if available) in its indexqual.
 * Therefore it can only be used as the inner relation of a nestloop
 * join against an outer rel that includes all the other rels mentioned
 * in its join clauses.  In that context, values for the other rels'
 * attributes are available and fixed during any one scan of the indexpath.
 *
 * An IndexPath is generated and submitted to add_path() for each plain index
 * scan this routine deems potentially interesting for the current query.
 *
 * We also determine the set of other relids that participate in join
 * clauses that could be used with each index.	The actually best innerjoin
 * path will be generated for each outer relation later on, but knowing the
 * set of potential otherrels allows us to identify equivalent outer relations
 * and avoid repeated computation.
 *
 * 'rel' is the relation for which we want to generate index paths
 */
void
create_index_paths(Query *root, RelOptInfo *rel)
{
	List	   *restrictinfo_list = rel->baserestrictinfo;
	List	   *joininfo_list = rel->joininfo;
	Relids		all_join_outerrelids = NULL;
	List	   *ilist;

	foreach(ilist, rel->indexlist)
	{
		IndexOptInfo *index = (IndexOptInfo *) lfirst(ilist);
		List	   *restrictclauses;
		List	   *index_pathkeys;
		List	   *useful_pathkeys;
		bool		index_is_ordered;
		Relids		join_outerrelids;

		/*
		 * If this is a partial index, we can only use it if it passes the
		 * predicate test.
		 */
		if (index->indpred != NIL)
			if (!pred_test(index->indpred, restrictinfo_list, joininfo_list))
				continue;

		/*
		 * 1. Try matching the index against subclauses of restriction
		 * 'or' clauses (ie, 'or' clauses that reference only this
		 * relation). The restrictinfo nodes for the 'or' clauses are
		 * marked with lists of the matching indices.  No paths are
		 * actually created now; that will be done in orindxpath.c after
		 * all indexes for the rel have been examined.	(We need to do it
		 * that way because we can potentially use a different index for
		 * each subclause of an 'or', so we can't build a path for an 'or'
		 * clause until all indexes have been matched against it.)
		 *
		 * We don't even think about special handling of 'or' clauses that
		 * involve more than one relation (ie, are join clauses). Can we
		 * do anything useful with those?
		 */
		match_index_orclauses(rel, index, restrictinfo_list);

		/*
		 * 2. Match the index against non-'or' restriction clauses.
		 */
		restrictclauses = group_clauses_by_indexkey(rel, index);

		/*
		 * 3. Compute pathkeys describing index's ordering, if any, then
		 * see how many of them are actually useful for this query.
		 */
		index_is_ordered = OidIsValid(index->ordering[0]);
		if (index_is_ordered)
		{
			index_pathkeys = build_index_pathkeys(root, rel, index,
												  ForwardScanDirection);
			useful_pathkeys = truncate_useless_pathkeys(root, rel,
														index_pathkeys);
		}
		else
			useful_pathkeys = NIL;

		/*
		 * 4. Generate an indexscan path if there are relevant restriction
		 * clauses OR the index ordering is potentially useful for later
		 * merging or final output ordering.
		 *
		 * If there is a predicate, consider it anyway since the index
		 * predicate has already been found to match the query.  The
		 * selectivity of the predicate might alone make the index useful.
		 *
		 * Note: not all index AMs support scans with no restriction clauses.
		 * We assume here that the AM does so if and only if it supports
		 * ordered scans.  (It would probably be better if there were a
		 * specific flag for this in pg_am, but there's not.)
		 */
		if (restrictclauses != NIL ||
			useful_pathkeys != NIL ||
			(index->indpred != NIL && index_is_ordered))
			add_path(rel, (Path *)
					 create_index_path(root, rel, index,
									   restrictclauses,
									   useful_pathkeys,
									   index_is_ordered ?
									   ForwardScanDirection :
									   NoMovementScanDirection));

		/*
		 * 5. If the index is ordered, a backwards scan might be
		 * interesting. Currently this is only possible for a DESC query
		 * result ordering.
		 */
		if (index_is_ordered)
		{
			index_pathkeys = build_index_pathkeys(root, rel, index,
												  BackwardScanDirection);
			useful_pathkeys = truncate_useless_pathkeys(root, rel,
														index_pathkeys);
			if (useful_pathkeys != NIL)
				add_path(rel, (Path *)
						 create_index_path(root, rel, index,
										   restrictclauses,
										   useful_pathkeys,
										   BackwardScanDirection));
		}

		/*
		 * 6. Examine join clauses to see which ones are potentially
		 * usable with this index, and generate the set of all other
		 * relids that participate in such join clauses.  We'll use this
		 * set later to recognize outer rels that are equivalent for
		 * joining purposes. We compute both per-index and
		 * overall-for-relation sets.
		 */
		join_outerrelids = indexable_outerrelids(rel, index);
		index->outer_relids = join_outerrelids;
		all_join_outerrelids = bms_add_members(all_join_outerrelids,
											   join_outerrelids);
	}

	rel->index_outer_relids = all_join_outerrelids;
}


/****************************************************************************
 *		----  ROUTINES TO PROCESS 'OR' CLAUSES	----
 ****************************************************************************/


/*
 * match_index_orclauses
 *	  Attempt to match an index against subclauses within 'or' clauses.
 *	  Each subclause that does match is marked with the index's node.
 *
 *	  Essentially, this adds 'index' to the list of subclause indices in
 *	  the RestrictInfo field of each of the 'or' clauses where it matches.
 *	  NOTE: we can use storage in the RestrictInfo for this purpose because
 *	  this processing is only done on single-relation restriction clauses.
 *	  Therefore, we will never have indexes for more than one relation
 *	  mentioned in the same RestrictInfo node's list.
 *
 * 'rel' is the node of the relation on which the index is defined.
 * 'index' is the index node.
 * 'restrictinfo_list' is the list of available restriction clauses.
 */
static void
match_index_orclauses(RelOptInfo *rel,
					  IndexOptInfo *index,
					  List *restrictinfo_list)
{
	List	   *i;

	foreach(i, restrictinfo_list)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(i);

		if (restriction_is_or_clause(restrictinfo))
		{
			/*
			 * Add this index to the subclause index list for each
			 * subclause that it matches.
			 */
			restrictinfo->subclauseindices =
				match_index_orclause(rel, index,
							   ((BoolExpr *) restrictinfo->clause)->args,
									 restrictinfo->subclauseindices);
		}
	}
}

/*
 * match_index_orclause
 *	  Attempts to match an index against the subclauses of an 'or' clause.
 *
 *	  A match means that:
 *	  (1) the operator within the subclause can be used with the
 *		  index's specified operator class, and
 *	  (2) one operand of the subclause matches the index key.
 *
 *	  If a subclause is an 'and' clause, then it matches if any of its
 *	  subclauses is an opclause that matches.
 *
 * 'or_clauses' is the list of subclauses within the 'or' clause
 * 'other_matching_indices' is the list of information on other indices
 *		that have already been matched to subclauses within this
 *		particular 'or' clause (i.e., a list previously generated by
 *		this routine), or NIL if this routine has not previously been
 *		run for this 'or' clause.
 *
 * Returns a list of the form ((a b c) (d e f) nil (g h) ...) where
 * a,b,c are nodes of indices that match the first subclause in
 * 'or-clauses', d,e,f match the second subclause, no indices
 * match the third, g,h match the fourth, etc.
 */
static List *
match_index_orclause(RelOptInfo *rel,
					 IndexOptInfo *index,
					 List *or_clauses,
					 List *other_matching_indices)
{
	List	   *matching_indices;
	List	   *index_list;
	List	   *clist;

	/*
	 * first time through, we create list of same length as OR clause,
	 * containing an empty sublist for each subclause.
	 */
	if (!other_matching_indices)
	{
		matching_indices = NIL;
		foreach(clist, or_clauses)
			matching_indices = lcons(NIL, matching_indices);
	}
	else
		matching_indices = other_matching_indices;

	index_list = matching_indices;

	foreach(clist, or_clauses)
	{
		Expr	   *clause = lfirst(clist);

		if (match_or_subclause_to_indexkey(rel, index, clause))
		{
			/* OK to add this index to sublist for this subclause */
			lfirst(matching_indices) = lcons(index,
											 lfirst(matching_indices));
		}

		matching_indices = lnext(matching_indices);
	}

	return index_list;
}

/*
 * See if a subclause of an OR clause matches an index.
 *
 * We accept the subclause if it is an operator clause that matches the
 * index, or if it is an AND clause any of whose members is an opclause
 * that matches the index.
 *
 * For multi-key indexes, we only look for matches to the first key;
 * without such a match the index is useless.  If the clause is an AND
 * then we may be able to extract additional subclauses to use with the
 * later indexkeys, but we need not worry about that until
 * extract_or_indexqual_conditions() is called (if it ever is).
 */
static bool
match_or_subclause_to_indexkey(RelOptInfo *rel,
							   IndexOptInfo *index,
							   Expr *clause)
{
	Oid			opclass = index->classlist[0];

	if (and_clause((Node *) clause))
	{
		List	   *item;

		foreach(item, ((BoolExpr *) clause)->args)
		{
			if (match_clause_to_indexcol(rel, index, 0, opclass,
										 lfirst(item)))
				return true;
		}
		return false;
	}
	else
		return match_clause_to_indexcol(rel, index, 0, opclass,
										clause);
}

/*----------
 * Given an OR subclause that has previously been determined to match
 * the specified index, extract a list of specific opclauses that can be
 * used as indexquals.
 *
 * In the simplest case this just means making a one-element list of the
 * given opclause.	However, if the OR subclause is an AND, we have to
 * scan it to find the opclause(s) that match the index.  (There should
 * be at least one, if match_or_subclause_to_indexkey succeeded, but there
 * could be more.)
 *
 * Also, we can look at other restriction clauses of the rel to discover
 * additional candidate indexquals: for example, consider
 *			... where (a = 11 or a = 12) and b = 42;
 * If we are dealing with an index on (a,b) then we can include the clause
 * b = 42 in the indexqual list generated for each of the OR subclauses.
 * Essentially, we are making an index-specific transformation from CNF to
 * DNF.  (NOTE: when we do this, we end up with a slightly inefficient plan
 * because create_indexscan_plan is not very bright about figuring out which
 * restriction clauses are implied by the generated indexqual condition.
 * Currently we'll end up rechecking both the OR clause and the transferred
 * restriction clause as qpquals.  FIXME someday.)
 *
 * Also, we apply expand_indexqual_condition() to convert any special
 * matching opclauses to indexable operators.
 *
 * The passed-in clause is not changed.
 *----------
 */
List *
extract_or_indexqual_conditions(RelOptInfo *rel,
								IndexOptInfo *index,
								Expr *orsubclause)
{
	FastList	quals;
	int			indexcol = 0;
	Oid		   *classes = index->classlist;

	FastListInit(&quals);

	/*
	 * Extract relevant indexclauses in indexkey order.  This is
	 * essentially just like group_clauses_by_indexkey() except that the
	 * input and output are lists of bare clauses, not of RestrictInfo
	 * nodes, and that we expand special operators immediately.
	 */
	do
	{
		Oid			curClass = classes[0];
		FastList	clausegroup;
		List	   *item;

		FastListInit(&clausegroup);
		if (and_clause((Node *) orsubclause))
		{
			foreach(item, ((BoolExpr *) orsubclause)->args)
			{
				Expr	   *subsubclause = (Expr *) lfirst(item);

				if (match_clause_to_indexcol(rel, index,
											 indexcol, curClass,
											 subsubclause))
					FastConc(&clausegroup,
							 expand_indexqual_condition(subsubclause,
														curClass));
			}
		}
		else if (match_clause_to_indexcol(rel, index,
										  indexcol, curClass,
										  orsubclause))
			FastConc(&clausegroup,
					 expand_indexqual_condition(orsubclause,
												curClass));

		/*
		 * If we found no clauses for this indexkey in the OR subclause
		 * itself, try looking in the rel's top-level restriction list.
		 */
		if (FastListValue(&clausegroup) == NIL)
		{
			foreach(item, rel->baserestrictinfo)
			{
				RestrictInfo *rinfo = (RestrictInfo *) lfirst(item);

				if (match_clause_to_indexcol(rel, index,
											 indexcol, curClass,
											 rinfo->clause))
					FastConc(&clausegroup,
							 expand_indexqual_condition(rinfo->clause,
														curClass));
			}
		}

		/*
		 * If still no clauses match this key, we're done; we don't want
		 * to look at keys to its right.
		 */
		if (FastListValue(&clausegroup) == NIL)
			break;

		FastConcFast(&quals, &clausegroup);

		indexcol++;
		classes++;

	} while (!DoneMatchingIndexKeys(classes));

	if (FastListValue(&quals) == NIL)
		elog(ERROR, "no matching OR clause");

	return FastListValue(&quals);
}


/****************************************************************************
 *				----  ROUTINES TO CHECK RESTRICTIONS  ----
 ****************************************************************************/


/*
 * group_clauses_by_indexkey
 *	  Find restriction clauses that can be used with an index.
 *
 * 'rel' is the node of the relation itself.
 * 'index' is a index on 'rel'.
 *
 * Returns a list of sublists of RestrictInfo nodes for clauses that can be
 * used with this index.  Each sublist contains clauses that can be used
 * with one index key (in no particular order); the top list is ordered by
 * index key.  (This is depended on by expand_indexqual_conditions().)
 *
 * Note that in a multi-key index, we stop if we find a key that cannot be
 * used with any clause.  For example, given an index on (A,B,C), we might
 * return ((C1 C2) (C3 C4)) if we find that clauses C1 and C2 use column A,
 * clauses C3 and C4 use column B, and no clauses use column C.  But if
 * no clauses match B we will return ((C1 C2)), whether or not there are
 * clauses matching column C, because the executor couldn't use them anyway.
 * Therefore, there are no empty sublists in the result.
 */
static List *
group_clauses_by_indexkey(RelOptInfo *rel, IndexOptInfo *index)
{
	FastList	clausegroup_list;
	List	   *restrictinfo_list = rel->baserestrictinfo;
	int			indexcol = 0;
	Oid		   *classes = index->classlist;

	if (restrictinfo_list == NIL)
		return NIL;

	FastListInit(&clausegroup_list);
	do
	{
		Oid			curClass = classes[0];
		FastList	clausegroup;
		List	   *i;

		FastListInit(&clausegroup);
		foreach(i, restrictinfo_list)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(i);

			if (match_clause_to_indexcol(rel,
										 index,
										 indexcol,
										 curClass,
										 rinfo->clause))
				FastAppend(&clausegroup, rinfo);
		}

		/*
		 * If no clauses match this key, we're done; we don't want to look
		 * at keys to its right.
		 */
		if (FastListValue(&clausegroup) == NIL)
			break;

		FastAppend(&clausegroup_list, FastListValue(&clausegroup));

		indexcol++;
		classes++;

	} while (!DoneMatchingIndexKeys(classes));

	return FastListValue(&clausegroup_list);
}

/*
 * group_clauses_by_indexkey_for_join
 *	  Generate a list of sublists of clauses that can be used with an index
 *	  to scan the inner side of a nestloop join.
 *
 * This is much like group_clauses_by_indexkey(), but we consider both
 * join and restriction clauses.  Any joinclause that uses only otherrels
 * in the specified outer_relids is fair game.	But there must be at least
 * one such joinclause in the final list, otherwise we return NIL indicating
 * that this index isn't interesting as an inner indexscan.  (A scan using
 * only restriction clauses shouldn't be created here, because a regular Path
 * will already have been generated for it.)
 */
static List *
group_clauses_by_indexkey_for_join(Query *root,
								   RelOptInfo *rel, IndexOptInfo *index,
								   Relids outer_relids,
								   JoinType jointype, bool isouterjoin)
{
	FastList	clausegroup_list;
	bool		jfound = false;
	int			indexcol = 0;
	Oid		   *classes = index->classlist;

	FastListInit(&clausegroup_list);
	do
	{
		Oid			curClass = classes[0];
		FastList	clausegroup;
		List	   *i;

		FastListInit(&clausegroup);

		/* Look for joinclauses that are usable with given outer_relids */
		foreach(i, rel->joininfo)
		{
			JoinInfo   *joininfo = (JoinInfo *) lfirst(i);
			List	   *j;

			if (!bms_is_subset(joininfo->unjoined_relids, outer_relids))
				continue;

			foreach(j, joininfo->jinfo_restrictinfo)
			{
				RestrictInfo *rinfo = (RestrictInfo *) lfirst(j);

				/* Can't use pushed-down clauses in outer join */
				if (isouterjoin && rinfo->ispusheddown)
					continue;

				if (match_join_clause_to_indexcol(rel,
												  index,
												  indexcol,
												  curClass,
												  rinfo->clause))
				{
					FastAppend(&clausegroup, rinfo);
					jfound = true;
				}
			}
		}

		/*
		 * If we found join clauses in more than one joininfo list, we may
		 * now have clauses that are known redundant.  Get rid of 'em.
		 * (There is no point in looking at restriction clauses, because
		 * remove_redundant_join_clauses will never think they are
		 * redundant, so we do this before adding restriction clauses to
		 * the clause group.)
		 */
		if (FastListValue(&clausegroup) != NIL)
		{
			List	   *nl;

			nl = remove_redundant_join_clauses(root,
											 FastListValue(&clausegroup),
											   jointype);
			FastListFromList(&clausegroup, nl);
		}

		/* We can also use plain restriction clauses for the rel */
		foreach(i, rel->baserestrictinfo)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(i);

			if (match_clause_to_indexcol(rel,
										 index,
										 indexcol,
										 curClass,
										 rinfo->clause))
				FastAppend(&clausegroup, rinfo);
		}

		/*
		 * If no clauses match this key, we're done; we don't want to look
		 * at keys to its right.
		 */
		if (FastListValue(&clausegroup) == NIL)
			break;

		FastAppend(&clausegroup_list, FastListValue(&clausegroup));

		indexcol++;
		classes++;

	} while (!DoneMatchingIndexKeys(classes));

	/* if no join clause was matched then forget it, per comments above */
	if (!jfound)
		return NIL;

	return FastListValue(&clausegroup_list);
}


/*
 * match_clause_to_indexcol()
 *	  Determines whether a restriction clause matches a column of an index.
 *
 *	  To match, the clause:
 *
 *	  (1)  must be in the form (indexkey op const) or (const op indexkey);
 *		   and
 *	  (2)  must contain an operator which is in the same class as the index
 *		   operator for this column, or is a "special" operator as recognized
 *		   by match_special_index_operator().
 *
 *	  Presently, the executor can only deal with indexquals that have the
 *	  indexkey on the left, so we can only use clauses that have the indexkey
 *	  on the right if we can commute the clause to put the key on the left.
 *	  We do not actually do the commuting here, but we check whether a
 *	  suitable commutator operator is available.
 *
 * 'rel' is the relation of interest.
 * 'index' is an index on 'rel'.
 * 'indexcol' is a column number of 'index' (counting from 0).
 * 'opclass' is the corresponding operator class.
 * 'clause' is the clause to be tested.
 *
 * Returns true if the clause can be used with this index key.
 *
 * NOTE:  returns false if clause is an OR or AND clause; it is the
 * responsibility of higher-level routines to cope with those.
 */
static bool
match_clause_to_indexcol(RelOptInfo *rel,
						 IndexOptInfo *index,
						 int indexcol,
						 Oid opclass,
						 Expr *clause)
{
	Node	   *leftop,
			   *rightop;

	/* Clause must be a binary opclause. */
	if (!is_opclause(clause))
		return false;
	leftop = get_leftop(clause);
	rightop = get_rightop(clause);
	if (!leftop || !rightop)
		return false;

	/*
	 * Check for clauses of the form: (indexkey operator constant) or
	 * (constant operator indexkey). Anything that is a "pseudo constant"
	 * expression will do.
	 */
	if (match_index_to_operand(leftop, indexcol, rel, index) &&
		is_pseudo_constant_clause(rightop))
	{
		if (is_indexable_operator(clause, opclass, true))
			return true;

		/*
		 * If we didn't find a member of the index's opclass, see whether
		 * it is a "special" indexable operator.
		 */
		if (match_special_index_operator(clause, opclass, true))
			return true;
		return false;
	}

	if (match_index_to_operand(rightop, indexcol, rel, index) &&
		is_pseudo_constant_clause(leftop))
	{
		if (is_indexable_operator(clause, opclass, false))
			return true;

		/*
		 * If we didn't find a member of the index's opclass, see whether
		 * it is a "special" indexable operator.
		 */
		if (match_special_index_operator(clause, opclass, false))
			return true;
		return false;
	}

	return false;
}

/*
 * match_join_clause_to_indexcol()
 *	  Determines whether a join clause matches a column of an index.
 *
 *	  To match, the clause:
 *
 *	  (1)  must be in the form (indexkey op others) or (others op indexkey),
 *		   where others is an expression involving only vars of the other
 *		   relation(s); and
 *	  (2)  must contain an operator which is in the same class as the index
 *		   operator for this column, or is a "special" operator as recognized
 *		   by match_special_index_operator().
 *
 *	  As above, we must be able to commute the clause to put the indexkey
 *	  on the left.
 *
 *	  Note that we already know that the clause as a whole uses vars from
 *	  the interesting set of relations.  But we need to defend against
 *	  expressions like (a.f1 OP (b.f2 OP a.f3)); that's not processable by
 *	  an indexscan nestloop join, whereas (a.f1 OP (b.f2 OP c.f3)) is.
 *
 * 'rel' is the relation of interest.
 * 'index' is an index on 'rel'.
 * 'indexcol' is a column number of 'index' (counting from 0).
 * 'opclass' is the corresponding operator class.
 * 'clause' is the clause to be tested.
 *
 * Returns true if the clause can be used with this index key.
 *
 * NOTE:  returns false if clause is an OR or AND clause; it is the
 * responsibility of higher-level routines to cope with those.
 */
static bool
match_join_clause_to_indexcol(RelOptInfo *rel,
							  IndexOptInfo *index,
							  int indexcol,
							  Oid opclass,
							  Expr *clause)
{
	Node	   *leftop,
			   *rightop;

	/* Clause must be a binary opclause. */
	if (!is_opclause(clause))
		return false;
	leftop = get_leftop(clause);
	rightop = get_rightop(clause);
	if (!leftop || !rightop)
		return false;

	/*
	 * Check for an indexqual that could be handled by a nestloop join. We
	 * need the index key to be compared against an expression that uses
	 * none of the indexed relation's vars and contains no volatile
	 * functions.
	 */
	if (match_index_to_operand(leftop, indexcol, rel, index))
	{
		Relids		othervarnos = pull_varnos(rightop);
		bool		isIndexable;

		isIndexable =
			!bms_overlap(rel->relids, othervarnos) &&
			!contain_volatile_functions(rightop) &&
			is_indexable_operator(clause, opclass, true);
		bms_free(othervarnos);
		return isIndexable;
	}

	if (match_index_to_operand(rightop, indexcol, rel, index))
	{
		Relids		othervarnos = pull_varnos(leftop);
		bool		isIndexable;

		isIndexable =
			!bms_overlap(rel->relids, othervarnos) &&
			!contain_volatile_functions(leftop) &&
			is_indexable_operator(clause, opclass, false);
		bms_free(othervarnos);
		return isIndexable;
	}

	return false;
}

/*
 * indexable_operator
 *	  Does a binary opclause contain an operator matching the index opclass?
 *
 * If the indexkey is on the right, what we actually want to know
 * is whether the operator has a commutator operator that matches
 * the index's opclass.
 *
 * Returns the OID of the matching operator, or InvalidOid if no match.
 * (Formerly, this routine might return a binary-compatible operator
 * rather than the original one, but that kluge is history.)
 */
static Oid
indexable_operator(Expr *clause, Oid opclass, bool indexkey_on_left)
{
	Oid			expr_op = ((OpExpr *) clause)->opno;
	Oid			commuted_op;

	/* Get the commuted operator if necessary */
	if (indexkey_on_left)
		commuted_op = expr_op;
	else
		commuted_op = get_commutator(expr_op);
	if (commuted_op == InvalidOid)
		return InvalidOid;

	/* OK if the (commuted) operator is a member of the index's opclass */
	if (op_in_opclass(commuted_op, opclass))
		return expr_op;

	return InvalidOid;
}

/****************************************************************************
 *				----  ROUTINES TO DO PARTIAL INDEX PREDICATE TESTS	----
 ****************************************************************************/

/*
 * pred_test
 *	  Does the "predicate inclusion test" for partial indexes.
 *
 *	  Recursively checks whether the clauses in restrictinfo_list imply
 *	  that the given predicate is true.
 *
 *	  This routine (together with the routines it calls) iterates over
 *	  ANDs in the predicate first, then reduces the qualification
 *	  clauses down to their constituent terms, and iterates over ORs
 *	  in the predicate last.  This order is important to make the test
 *	  succeed whenever possible (assuming the predicate has been converted
 *	  to CNF format). --Nels, Jan '93
 */
static bool
pred_test(List *predicate_list, List *restrictinfo_list, List *joininfo_list)
{
	List	   *pred;

	/*
	 * Note: if Postgres tried to optimize queries by forming equivalence
	 * classes over equi-joined attributes (i.e., if it recognized that a
	 * qualification such as "where a.b=c.d and a.b=5" could make use of
	 * an index on c.d), then we could use that equivalence class info
	 * here with joininfo_list to do more complete tests for the usability
	 * of a partial index.	For now, the test only uses restriction
	 * clauses (those in restrictinfo_list). --Nels, Dec '92
	 *
	 * XXX as of 7.1, equivalence class info *is* available.  Consider
	 * improving this code as foreseen by Nels.
	 */

	if (predicate_list == NIL)
		return true;			/* no predicate: the index is usable */
	if (restrictinfo_list == NIL)
		return false;			/* no restriction clauses: the test must
								 * fail */

	foreach(pred, predicate_list)
	{
		/*
		 * if any clause is not implied, the whole predicate is not
		 * implied.  Note we assume that any sub-ANDs have been flattened
		 * when the predicate was fed through canonicalize_qual().
		 */
		if (!pred_test_restrict_list(lfirst(pred), restrictinfo_list))
			return false;
	}
	return true;
}


/*
 * pred_test_restrict_list
 *	  Does the "predicate inclusion test" for one conjunct of a predicate
 *	  expression.
 */
static bool
pred_test_restrict_list(Expr *predicate, List *restrictinfo_list)
{
	List	   *item;

	foreach(item, restrictinfo_list)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(item);

		/* if any clause implies the predicate, return true */
		if (pred_test_recurse_clause(predicate,
									 (Node *) restrictinfo->clause))
			return true;
	}
	return false;
}


/*
 * pred_test_recurse_clause
 *	  Does the "predicate inclusion test" for a general restriction-clause
 *	  expression.  Here we recursively deal with the possibility that the
 *	  restriction clause is itself an AND or OR structure.
 */
static bool
pred_test_recurse_clause(Expr *predicate, Node *clause)
{
	List	   *items,
			   *item;

	Assert(clause != NULL);
	if (or_clause(clause))
	{
		items = ((BoolExpr *) clause)->args;
		foreach(item, items)
		{
			/* if any OR item doesn't imply the predicate, clause doesn't */
			if (!pred_test_recurse_clause(predicate, lfirst(item)))
				return false;
		}
		return true;
	}
	else if (and_clause(clause))
	{
		items = ((BoolExpr *) clause)->args;
		foreach(item, items)
		{
			/*
			 * if any AND item implies the predicate, the whole clause
			 * does
			 */
			if (pred_test_recurse_clause(predicate, lfirst(item)))
				return true;
		}
		return false;
	}
	else
		return pred_test_recurse_pred(predicate, clause);
}


/*
 * pred_test_recurse_pred
 *	  Does the "predicate inclusion test" for one conjunct of a predicate
 *	  expression for a simple restriction clause.  Here we recursively deal
 *	  with the possibility that the predicate conjunct is itself an AND or
 *	  OR structure.
 */
static bool
pred_test_recurse_pred(Expr *predicate, Node *clause)
{
	List	   *items,
			   *item;

	Assert(predicate != NULL);
	if (or_clause((Node *) predicate))
	{
		items = ((BoolExpr *) predicate)->args;
		foreach(item, items)
		{
			/* if any item is implied, the whole predicate is implied */
			if (pred_test_recurse_pred(lfirst(item), clause))
				return true;
		}
		return false;
	}
	else if (and_clause((Node *) predicate))
	{
		items = ((BoolExpr *) predicate)->args;
		foreach(item, items)
		{
			/*
			 * if any item is not implied, the whole predicate is not
			 * implied
			 */
			if (!pred_test_recurse_pred(lfirst(item), clause))
				return false;
		}
		return true;
	}
	else
		return pred_test_simple_clause(predicate, clause);
}


/*
 * Define an "operator implication table" for btree operators ("strategies").
 * The "strategy numbers" are:	(1) <	(2) <=	 (3) =	 (4) >=   (5) >
 *
 * The interpretation of:
 *
 *		test_op = BT_implic_table[given_op-1][target_op-1]
 *
 * where test_op, given_op and target_op are strategy numbers (from 1 to 5)
 * of btree operators, is as follows:
 *
 *	 If you know, for some ATTR, that "ATTR given_op CONST1" is true, and you
 *	 want to determine whether "ATTR target_op CONST2" must also be true, then
 *	 you can use "CONST1 test_op CONST2" as a test.  If this test returns true,
 *	 then the target expression must be true; if the test returns false, then
 *	 the target expression may be false.
 *
 * An entry where test_op==0 means the implication cannot be determined, i.e.,
 * this test should always be considered false.
 */

static const StrategyNumber
			BT_implic_table[BTMaxStrategyNumber][BTMaxStrategyNumber] = {
	{2, 2, 0, 0, 0},
	{1, 2, 0, 0, 0},
	{1, 2, 3, 4, 5},
	{0, 0, 0, 4, 5},
	{0, 0, 0, 4, 4}
};


/*
 * pred_test_simple_clause
 *	  Does the "predicate inclusion test" for a "simple clause" predicate
 *	  and a "simple clause" restriction.
 *
 *	  We have two strategies for determining whether one simple clause
 *	  implies another.	A simple and general way is to see if they are
 *	  equal(); this works for any kind of expression.  (Actually, there
 *	  is an implied assumption that the functions in the expression are
 *	  immutable, ie dependent only on their input arguments --- but this
 *	  was checked for the predicate by CheckPredicate().)
 *
 *	  Our other way works only for (binary boolean) operators that are
 *	  in some btree operator class.  We use the above operator implication
 *	  table to be able to derive implications between nonidentical clauses.
 *
 *	  Eventually, rtree operators could also be handled by defining an
 *	  appropriate "RT_implic_table" array.
 */
static bool
pred_test_simple_clause(Expr *predicate, Node *clause)
{
	Var		   *pred_var,
			   *clause_var;
	Const	   *pred_const,
			   *clause_const;
	Oid			pred_op,
				clause_op,
				test_op;
	Oid			opclass_id = InvalidOid;
	bool		found = false;
	StrategyNumber pred_strategy = 0,
				clause_strategy = 0,
				test_strategy;
	Expr	   *test_expr;
	ExprState  *test_exprstate;
	Datum		test_result;
	bool		isNull;
	CatCList   *catlist;
	int			i;
	EState	   *estate;
	MemoryContext oldcontext;

	/* First try the equal() test */
	if (equal((Node *) predicate, clause))
		return true;

	/*
	 * Can't do anything more unless they are both binary opclauses with a
	 * Var on the left and a Const on the right.  (XXX someday try to
	 * commute Const/Var cases?)
	 */
	if (!is_opclause(predicate))
		return false;
	pred_var = (Var *) get_leftop(predicate);
	pred_const = (Const *) get_rightop(predicate);

	if (!is_opclause(clause))
		return false;
	clause_var = (Var *) get_leftop((Expr *) clause);
	clause_const = (Const *) get_rightop((Expr *) clause);

	if (!IsA(clause_var, Var) ||
		clause_const == NULL ||
		!IsA(clause_const, Const) ||
		!IsA(pred_var, Var) ||
		pred_const == NULL ||
		!IsA(pred_const, Const))
		return false;

	/*
	 * The implication can't be determined unless the predicate and the
	 * clause refer to the same attribute.
	 */
	if (clause_var->varno != pred_var->varno ||
		clause_var->varattno != pred_var->varattno)
		return false;

	/* Get the operators for the two clauses we're comparing */
	pred_op = ((OpExpr *) predicate)->opno;
	clause_op = ((OpExpr *) clause)->opno;

	/*
	 * 1. Find "btree" strategy numbers for the pred_op and clause_op.
	 *
	 * We must find a btree opclass that contains both operators, else the
	 * implication can't be determined.  If there are multiple such
	 * opclasses, assume we can use any one to determine the logical
	 * relationship of the two operators and the correct corresponding
	 * test operator.  This should work for any logically consistent
	 * opclasses.
	 */
	catlist = SearchSysCacheList(AMOPOPID, 1,
								 ObjectIdGetDatum(pred_op),
								 0, 0, 0);

	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	pred_tuple = &catlist->members[i]->tuple;
		Form_pg_amop pred_form = (Form_pg_amop) GETSTRUCT(pred_tuple);
		HeapTuple	clause_tuple;

		if (!opclass_is_btree(pred_form->amopclaid))
			continue;

		/* Get the predicate operator's btree strategy number */
		pred_strategy = (StrategyNumber) pred_form->amopstrategy;
		Assert(pred_strategy >= 1 && pred_strategy <= 5);

		/*
		 * Remember which operator class this strategy number came from
		 */
		opclass_id = pred_form->amopclaid;

		/*
		 * From the same opclass, find a strategy num for the clause_op,
		 * if possible
		 */
		clause_tuple = SearchSysCache(AMOPOPID,
									  ObjectIdGetDatum(clause_op),
									  ObjectIdGetDatum(opclass_id),
									  0, 0);
		if (HeapTupleIsValid(clause_tuple))
		{
			Form_pg_amop clause_form = (Form_pg_amop) GETSTRUCT(clause_tuple);

			/* Get the restriction clause operator's strategy number */
			clause_strategy = (StrategyNumber) clause_form->amopstrategy;
			Assert(clause_strategy >= 1 && clause_strategy <= 5);

			ReleaseSysCache(clause_tuple);
			found = true;
			break;
		}
	}

	ReleaseSysCacheList(catlist);

	if (!found)
	{
		/* couldn't find a btree opclass to interpret the operators */
		return false;
	}

	/*
	 * 2. Look up the "test" strategy number in the implication table
	 */
	test_strategy = BT_implic_table[clause_strategy - 1][pred_strategy - 1];
	if (test_strategy == 0)
		return false;			/* the implication cannot be determined */

	/*
	 * 3. From the same opclass, find the operator for the test strategy
	 */
	test_op = get_opclass_member(opclass_id, test_strategy);
	if (!OidIsValid(test_op))
	{
		/* This should not fail, else pg_amop entry is missing */
		elog(ERROR, "missing pg_amop entry for opclass %u strategy %d",
			 opclass_id, test_strategy);
	}

	/*
	 * 4. Evaluate the test.  For this we need an EState.
	 */
	estate = CreateExecutorState();

	/* We can use the estate's working context to avoid memory leaks. */
	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	/* Build expression tree */
	test_expr = make_opclause(test_op,
							  BOOLOID,
							  false,
							  (Expr *) clause_const,
							  (Expr *) pred_const);

	/* Prepare it for execution */
	test_exprstate = ExecPrepareExpr(test_expr, estate);

	/* And execute it. */
	test_result = ExecEvalExprSwitchContext(test_exprstate,
										  GetPerTupleExprContext(estate),
											&isNull, NULL);

	/* Get back to outer memory context */
	MemoryContextSwitchTo(oldcontext);

	/* Release all the junk we just created */
	FreeExecutorState(estate);

	if (isNull)
	{
		/* Treat a null result as false ... but it's a tad fishy ... */
		elog(DEBUG2, "null predicate test result");
		return false;
	}
	return DatumGetBool(test_result);
}


/****************************************************************************
 *				----  ROUTINES TO CHECK JOIN CLAUSES  ----
 ****************************************************************************/

/*
 * indexable_outerrelids
 *	  Finds all other relids that participate in any indexable join clause
 *	  for the specified index.	Returns a set of relids.
 *
 * 'rel' is the relation for which 'index' is defined
 */
static Relids
indexable_outerrelids(RelOptInfo *rel, IndexOptInfo *index)
{
	Relids		outer_relids = NULL;
	List	   *i;

	foreach(i, rel->joininfo)
	{
		JoinInfo   *joininfo = (JoinInfo *) lfirst(i);
		bool		match_found = false;
		List	   *j;

		/*
		 * Examine each joinclause in the JoinInfo node's list to see if
		 * it matches any key of the index.  If so, add the JoinInfo's
		 * otherrels to the result.  We can skip examining other
		 * joinclauses in the same list as soon as we find a match (since
		 * by definition they all have the same otherrels).
		 */
		foreach(j, joininfo->jinfo_restrictinfo)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(j);
			Expr	   *clause = rinfo->clause;
			int			indexcol = 0;
			Oid		   *classes = index->classlist;

			do
			{
				Oid			curClass = classes[0];

				if (match_join_clause_to_indexcol(rel,
												  index,
												  indexcol,
												  curClass,
												  clause))
				{
					match_found = true;
					break;
				}

				indexcol++;
				classes++;

			} while (!DoneMatchingIndexKeys(classes));

			if (match_found)
				break;
		}

		if (match_found)
		{
			outer_relids = bms_add_members(outer_relids,
										   joininfo->unjoined_relids);
		}
	}

	return outer_relids;
}

/*
 * best_inner_indexscan
 *	  Finds the best available inner indexscan for a nestloop join
 *	  with the given rel on the inside and the given outer_relids outside.
 *	  May return NULL if there are no possible inner indexscans.
 *
 * We ignore ordering considerations (since a nestloop's inner scan's order
 * is uninteresting).  Also, we consider only total cost when deciding which
 * of two possible paths is better --- this assumes that all indexpaths have
 * negligible startup cost.  (True today, but someday we might have to think
 * harder.)  Therefore, there is only one dimension of comparison and so it's
 * sufficient to return a single "best" path.
 */
Path *
best_inner_indexscan(Query *root, RelOptInfo *rel,
					 Relids outer_relids, JoinType jointype)
{
	Path	   *cheapest = NULL;
	bool		isouterjoin;
	List	   *ilist;
	List	   *jlist;
	InnerIndexscanInfo *info;
	MemoryContext oldcontext;

	/*
	 * Nestloop only supports inner, left, and IN joins.
	 */
	switch (jointype)
	{
		case JOIN_INNER:
		case JOIN_IN:
		case JOIN_UNIQUE_OUTER:
			isouterjoin = false;
			break;
		case JOIN_LEFT:
			isouterjoin = true;
			break;
		default:
			return NULL;
	}

	/*
	 * If there are no indexable joinclauses for this rel, exit quickly.
	 */
	if (bms_is_empty(rel->index_outer_relids))
		return NULL;

	/*
	 * Otherwise, we have to do path selection in the memory context of
	 * the given rel, so that any created path can be safely attached to
	 * the rel's cache of best inner paths.  (This is not currently an
	 * issue for normal planning, but it is an issue for GEQO planning.)
	 */
	oldcontext = MemoryContextSwitchTo(GetMemoryChunkContext(rel));

	/*
	 * Intersect the given outer_relids with index_outer_relids to find
	 * the set of outer relids actually relevant for this index. If there
	 * are none, again we can fail immediately.
	 */
	outer_relids = bms_intersect(rel->index_outer_relids, outer_relids);
	if (bms_is_empty(outer_relids))
	{
		bms_free(outer_relids);
		MemoryContextSwitchTo(oldcontext);
		return NULL;
	}

	/*
	 * Look to see if we already computed the result for this set of
	 * relevant outerrels.	(We include the isouterjoin status in the
	 * cache lookup key for safety.  In practice I suspect this is not
	 * necessary because it should always be the same for a given
	 * innerrel.)
	 */
	foreach(jlist, rel->index_inner_paths)
	{
		info = (InnerIndexscanInfo *) lfirst(jlist);
		if (bms_equal(info->other_relids, outer_relids) &&
			info->isouterjoin == isouterjoin)
		{
			bms_free(outer_relids);
			MemoryContextSwitchTo(oldcontext);
			return info->best_innerpath;
		}
	}

	/*
	 * For each index of the rel, find the best path; then choose the best
	 * overall.  We cache the per-index results as well as the overall
	 * result.	(This is useful because different indexes may have
	 * different relevant outerrel sets, so different overall outerrel
	 * sets might still map to the same computation for a given index.)
	 */
	foreach(ilist, rel->indexlist)
	{
		IndexOptInfo *index = (IndexOptInfo *) lfirst(ilist);
		Relids		index_outer_relids;
		Path	   *path = NULL;

		/* identify set of relevant outer relids for this index */
		index_outer_relids = bms_intersect(index->outer_relids, outer_relids);
		/* skip if none */
		if (bms_is_empty(index_outer_relids))
		{
			bms_free(index_outer_relids);
			continue;
		}

		/*
		 * Look to see if we already computed the result for this index.
		 */
		foreach(jlist, index->inner_paths)
		{
			info = (InnerIndexscanInfo *) lfirst(jlist);
			if (bms_equal(info->other_relids, index_outer_relids) &&
				info->isouterjoin == isouterjoin)
			{
				path = info->best_innerpath;
				bms_free(index_outer_relids);	/* not needed anymore */
				break;
			}
		}

		if (jlist == NIL)		/* failed to find a match? */
		{
			List	   *clausegroups;

			/* find useful clauses for this index and outerjoin set */
			clausegroups = group_clauses_by_indexkey_for_join(root,
															  rel,
															  index,
													  index_outer_relids,
															  jointype,
															isouterjoin);
			if (clausegroups)
			{
				/* make the path */
				path = make_innerjoin_index_path(root, rel, index,
												 clausegroups);
			}

			/* Cache the result --- whether positive or negative */
			info = makeNode(InnerIndexscanInfo);
			info->other_relids = index_outer_relids;
			info->isouterjoin = isouterjoin;
			info->best_innerpath = path;
			index->inner_paths = lcons(info, index->inner_paths);
		}

		if (path != NULL &&
			(cheapest == NULL ||
			 compare_path_costs(path, cheapest, TOTAL_COST) < 0))
			cheapest = path;
	}

	/* Cache the result --- whether positive or negative */
	info = makeNode(InnerIndexscanInfo);
	info->other_relids = outer_relids;
	info->isouterjoin = isouterjoin;
	info->best_innerpath = cheapest;
	rel->index_inner_paths = lcons(info, rel->index_inner_paths);

	MemoryContextSwitchTo(oldcontext);

	return cheapest;
}

/****************************************************************************
 *				----  PATH CREATION UTILITIES  ----
 ****************************************************************************/

/*
 * make_innerjoin_index_path
 *	  Create an index path node for a path to be used as an inner
 *	  relation in a nestloop join.
 *
 * 'rel' is the relation for which 'index' is defined
 * 'clausegroups' is a list of lists of RestrictInfos that can use 'index'
 */
static Path *
make_innerjoin_index_path(Query *root,
						  RelOptInfo *rel, IndexOptInfo *index,
						  List *clausegroups)
{
	IndexPath  *pathnode = makeNode(IndexPath);
	List	   *indexquals,
			   *allclauses,
			   *l;

	/* XXX perhaps this code should be merged with create_index_path? */

	pathnode->path.pathtype = T_IndexScan;
	pathnode->path.parent = rel;

	/*
	 * There's no point in marking the path with any pathkeys, since it
	 * will only ever be used as the inner path of a nestloop, and so its
	 * ordering does not matter.
	 */
	pathnode->path.pathkeys = NIL;

	/* Convert RestrictInfo nodes to indexquals the executor can handle */
	indexquals = expand_indexqual_conditions(index, clausegroups);

	/*
	 * Also make a flattened list of the RestrictInfo nodes; createplan.c
	 * will need this later.  We assume here that we can destructively
	 * modify the passed-in clausegroups list structure.
	 */
	allclauses = NIL;
	foreach(l, clausegroups)
	{
		/* nconc okay here since same clause couldn't be in two sublists */
		allclauses = nconc(allclauses, (List *) lfirst(l));
	}

	/*
	 * Note that we are making a pathnode for a single-scan indexscan;
	 * therefore, indexinfo and indexqual should be single-element lists.
	 */
	pathnode->indexinfo = makeList1(index);
	pathnode->indexqual = makeList1(indexquals);
	pathnode->indexjoinclauses = makeList1(allclauses);

	/* We don't actually care what order the index scans in ... */
	pathnode->indexscandir = NoMovementScanDirection;

	/*
	 * We must compute the estimated number of output rows for the
	 * indexscan.  This is less than rel->rows because of the additional
	 * selectivity of the join clauses.  Since clausegroups may contain
	 * both restriction and join clauses, we have to do a set union to get
	 * the full set of clauses that must be considered to compute the
	 * correct selectivity.  (Without the union operation, we might have
	 * some restriction clauses appearing twice, which'd mislead
	 * restrictlist_selectivity into double-counting their selectivity.
	 * However, since RestrictInfo nodes aren't copied when linking them
	 * into different lists, it should be sufficient to use pointer
	 * comparison to remove duplicates.)
	 *
	 * Always assume the join type is JOIN_INNER; even if some of the join
	 * clauses come from other contexts, that's not our problem.
	 */
	allclauses = set_ptrUnion(rel->baserestrictinfo, allclauses);
	pathnode->rows = rel->tuples *
		restrictlist_selectivity(root,
								 allclauses,
								 rel->relid,
								 JOIN_INNER);
	/* Like costsize.c, force estimate to be at least one row */
	if (pathnode->rows < 1.0)
		pathnode->rows = 1.0;

	cost_index(&pathnode->path, root, rel, index, indexquals, true);

	return (Path *) pathnode;
}

/****************************************************************************
 *				----  ROUTINES TO CHECK OPERANDS  ----
 ****************************************************************************/

/*
 * match_index_to_operand()
 *	  Generalized test for a match between an index's key
 *	  and the operand on one side of a restriction or join clause.
 *
 * operand: the nodetree to be compared to the index
 * indexcol: the column number of the index (counting from 0)
 * rel: the parent relation
 * index: the index of interest
 */
static bool
match_index_to_operand(Node *operand,
					   int indexcol,
					   RelOptInfo *rel,
					   IndexOptInfo *index)
{
	int			indkey;

	/*
	 * Ignore any RelabelType node above the operand.	This is needed to
	 * be able to apply indexscanning in binary-compatible-operator cases.
	 * Note: we can assume there is at most one RelabelType node;
	 * eval_const_expressions() will have simplified if more than one.
	 */
	if (operand && IsA(operand, RelabelType))
		operand = (Node *) ((RelabelType *) operand)->arg;

	indkey = index->indexkeys[indexcol];
	if (indkey != 0)
	{
		/*
		 * Simple index column; operand must be a matching Var.
		 */
		if (operand && IsA(operand, Var) &&
			rel->relid == ((Var *) operand)->varno &&
			indkey == ((Var *) operand)->varattno)
			return true;
	}
	else
	{
		/*
		 * Index expression; find the correct expression.  (This search
		 * could be avoided, at the cost of complicating all the callers
		 * of this routine; doesn't seem worth it.)
		 */
		List	   *indexprs;
		int			i;
		Node	   *indexkey;

		indexprs = index->indexprs;
		for (i = 0; i < indexcol; i++)
		{
			if (index->indexkeys[i] == 0)
			{
				if (indexprs == NIL)
					elog(ERROR, "wrong number of index expressions");
				indexprs = lnext(indexprs);
			}
		}
		if (indexprs == NIL)
			elog(ERROR, "wrong number of index expressions");
		indexkey = (Node *) lfirst(indexprs);

		/*
		 * Does it match the operand?  Again, strip any relabeling.
		 */
		if (indexkey && IsA(indexkey, RelabelType))
			indexkey = (Node *) ((RelabelType *) indexkey)->arg;

		if (equal(indexkey, operand))
			return true;
	}

	return false;
}

/****************************************************************************
 *			----  ROUTINES FOR "SPECIAL" INDEXABLE OPERATORS  ----
 ****************************************************************************/

/*----------
 * These routines handle special optimization of operators that can be
 * used with index scans even though they are not known to the executor's
 * indexscan machinery.  The key idea is that these operators allow us
 * to derive approximate indexscan qual clauses, such that any tuples
 * that pass the operator clause itself must also satisfy the simpler
 * indexscan condition(s).	Then we can use the indexscan machinery
 * to avoid scanning as much of the table as we'd otherwise have to,
 * while applying the original operator as a qpqual condition to ensure
 * we deliver only the tuples we want.	(In essence, we're using a regular
 * index as if it were a lossy index.)
 *
 * An example of what we're doing is
 *			textfield LIKE 'abc%'
 * from which we can generate the indexscanable conditions
 *			textfield >= 'abc' AND textfield < 'abd'
 * which allow efficient scanning of an index on textfield.
 * (In reality, character set and collation issues make the transformation
 * from LIKE to indexscan limits rather harder than one might think ...
 * but that's the basic idea.)
 *
 * Two routines are provided here, match_special_index_operator() and
 * expand_indexqual_conditions().  match_special_index_operator() is
 * just an auxiliary function for match_clause_to_indexcol(); after
 * the latter fails to recognize a restriction opclause's operator
 * as a member of an index's opclass, it asks match_special_index_operator()
 * whether the clause should be considered an indexqual anyway.
 * expand_indexqual_conditions() converts a list of lists of RestrictInfo
 * nodes (with implicit AND semantics across list elements) into
 * a list of clauses that the executor can actually handle.  For operators
 * that are members of the index's opclass this transformation is a no-op,
 * but operators recognized by match_special_index_operator() must be
 * converted into one or more "regular" indexqual conditions.
 *----------
 */

/*
 * match_special_index_operator
 *	  Recognize restriction clauses that can be used to generate
 *	  additional indexscanable qualifications.
 *
 * The given clause is already known to be a binary opclause having
 * the form (indexkey OP pseudoconst) or (pseudoconst OP indexkey),
 * but the OP proved not to be one of the index's opclass operators.
 * Return 'true' if we can do something with it anyway.
 */
static bool
match_special_index_operator(Expr *clause, Oid opclass,
							 bool indexkey_on_left)
{
	bool		isIndexable = false;
	Node	   *rightop;
	Oid			expr_op;
	Const	   *patt;
	Const	   *prefix = NULL;
	Const	   *rest = NULL;

	/*
	 * Currently, all known special operators require the indexkey on the
	 * left, but this test could be pushed into the switch statement if
	 * some are added that do not...
	 */
	if (!indexkey_on_left)
		return false;

	/* we know these will succeed */
	rightop = get_rightop(clause);
	expr_op = ((OpExpr *) clause)->opno;

	/* again, required for all current special ops: */
	if (!IsA(rightop, Const) ||
		((Const *) rightop)->constisnull)
		return false;
	patt = (Const *) rightop;

	switch (expr_op)
	{
		case OID_TEXT_LIKE_OP:
		case OID_BPCHAR_LIKE_OP:
		case OID_NAME_LIKE_OP:
			/* the right-hand const is type text for all of these */
			isIndexable = pattern_fixed_prefix(patt, Pattern_Type_Like,
								  &prefix, &rest) != Pattern_Prefix_None;
			break;

		case OID_BYTEA_LIKE_OP:
			isIndexable = pattern_fixed_prefix(patt, Pattern_Type_Like,
								  &prefix, &rest) != Pattern_Prefix_None;
			break;

		case OID_TEXT_ICLIKE_OP:
		case OID_BPCHAR_ICLIKE_OP:
		case OID_NAME_ICLIKE_OP:
			/* the right-hand const is type text for all of these */
			isIndexable = pattern_fixed_prefix(patt, Pattern_Type_Like_IC,
								  &prefix, &rest) != Pattern_Prefix_None;
			break;

		case OID_TEXT_REGEXEQ_OP:
		case OID_BPCHAR_REGEXEQ_OP:
		case OID_NAME_REGEXEQ_OP:
			/* the right-hand const is type text for all of these */
			isIndexable = pattern_fixed_prefix(patt, Pattern_Type_Regex,
								  &prefix, &rest) != Pattern_Prefix_None;
			break;

		case OID_TEXT_ICREGEXEQ_OP:
		case OID_BPCHAR_ICREGEXEQ_OP:
		case OID_NAME_ICREGEXEQ_OP:
			/* the right-hand const is type text for all of these */
			isIndexable = pattern_fixed_prefix(patt, Pattern_Type_Regex_IC,
								  &prefix, &rest) != Pattern_Prefix_None;
			break;

		case OID_INET_SUB_OP:
		case OID_INET_SUBEQ_OP:
		case OID_CIDR_SUB_OP:
		case OID_CIDR_SUBEQ_OP:
			isIndexable = true;
			break;
	}

	if (prefix)
	{
		pfree(DatumGetPointer(prefix->constvalue));
		pfree(prefix);
	}

	/* done if the expression doesn't look indexable */
	if (!isIndexable)
		return false;

	/*
	 * Must also check that index's opclass supports the operators we will
	 * want to apply.  (A hash index, for example, will not support ">=".)
	 * Currently, only btree supports the operators we need.
	 *
	 * We insist on the opclass being the specific one we expect, else we'd
	 * do the wrong thing if someone were to make a reverse-sort opclass
	 * with the same operators.
	 */
	switch (expr_op)
	{
		case OID_TEXT_LIKE_OP:
		case OID_TEXT_ICLIKE_OP:
		case OID_TEXT_REGEXEQ_OP:
		case OID_TEXT_ICREGEXEQ_OP:
			/* text operators will be used for varchar inputs, too */
			isIndexable =
				(opclass == TEXT_PATTERN_BTREE_OPS_OID) ||
				(opclass == TEXT_BTREE_OPS_OID && lc_collate_is_c()) ||
				(opclass == VARCHAR_PATTERN_BTREE_OPS_OID) ||
				(opclass == VARCHAR_BTREE_OPS_OID && lc_collate_is_c());
			break;

		case OID_BPCHAR_LIKE_OP:
		case OID_BPCHAR_ICLIKE_OP:
		case OID_BPCHAR_REGEXEQ_OP:
		case OID_BPCHAR_ICREGEXEQ_OP:
			isIndexable =
				(opclass == BPCHAR_PATTERN_BTREE_OPS_OID) ||
				(opclass == BPCHAR_BTREE_OPS_OID && lc_collate_is_c());
			break;

		case OID_NAME_LIKE_OP:
		case OID_NAME_ICLIKE_OP:
		case OID_NAME_REGEXEQ_OP:
		case OID_NAME_ICREGEXEQ_OP:
			isIndexable =
				(opclass == NAME_PATTERN_BTREE_OPS_OID) ||
				(opclass == NAME_BTREE_OPS_OID && lc_collate_is_c());
			break;

		case OID_BYTEA_LIKE_OP:
			isIndexable = (opclass == BYTEA_BTREE_OPS_OID);
			break;

		case OID_INET_SUB_OP:
		case OID_INET_SUBEQ_OP:
			isIndexable = (opclass == INET_BTREE_OPS_OID);
			break;

		case OID_CIDR_SUB_OP:
		case OID_CIDR_SUBEQ_OP:
			isIndexable = (opclass == CIDR_BTREE_OPS_OID);
			break;
	}

	return isIndexable;
}

/*
 * expand_indexqual_conditions
 *	  Given a list of sublists of RestrictInfo nodes, produce a flat list
 *	  of index qual clauses.  Standard qual clauses (those in the index's
 *	  opclass) are passed through unchanged.  "Special" index operators
 *	  are expanded into clauses that the indexscan machinery will know
 *	  what to do with.
 *
 * The input list is ordered by index key, and so the output list is too.
 * (The latter is not depended on by any part of the planner, so far as I can
 * tell; but some parts of the executor do assume that the indxqual list
 * ultimately delivered to the executor is so ordered.	One such place is
 * _bt_orderkeys() in the btree support.  Perhaps that ought to be fixed
 * someday --- tgl 7/00)
 */
List *
expand_indexqual_conditions(IndexOptInfo *index, List *clausegroups)
{
	FastList	resultquals;
	Oid		   *classes = index->classlist;

	if (clausegroups == NIL)
		return NIL;

	FastListInit(&resultquals);
	do
	{
		Oid			curClass = classes[0];
		List	   *i;

		foreach(i, (List *) lfirst(clausegroups))
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(i);

			FastConc(&resultquals,
					 expand_indexqual_condition(rinfo->clause,
												curClass));
		}

		clausegroups = lnext(clausegroups);

		classes++;

	} while (clausegroups != NIL && !DoneMatchingIndexKeys(classes));

	Assert(clausegroups == NIL);	/* else more groups than indexkeys... */

	return FastListValue(&resultquals);
}

/*
 * expand_indexqual_condition --- expand a single indexqual condition
 */
static List *
expand_indexqual_condition(Expr *clause, Oid opclass)
{
	/* we know these will succeed */
	Node	   *leftop = get_leftop(clause);
	Node	   *rightop = get_rightop(clause);
	Oid			expr_op = ((OpExpr *) clause)->opno;
	Const	   *patt = (Const *) rightop;
	Const	   *prefix = NULL;
	Const	   *rest = NULL;
	Pattern_Prefix_Status pstatus;
	List	   *result;

	switch (expr_op)
	{
			/*
			 * LIKE and regex operators are not members of any index
			 * opclass, so if we find one in an indexqual list we can
			 * assume that it was accepted by
			 * match_special_index_operator().
			 */
		case OID_TEXT_LIKE_OP:
		case OID_BPCHAR_LIKE_OP:
		case OID_NAME_LIKE_OP:
		case OID_BYTEA_LIKE_OP:
			pstatus = pattern_fixed_prefix(patt, Pattern_Type_Like,
										   &prefix, &rest);
			result = prefix_quals(leftop, opclass, prefix, pstatus);
			break;

		case OID_TEXT_ICLIKE_OP:
		case OID_BPCHAR_ICLIKE_OP:
		case OID_NAME_ICLIKE_OP:
			/* the right-hand const is type text for all of these */
			pstatus = pattern_fixed_prefix(patt, Pattern_Type_Like_IC,
										   &prefix, &rest);
			result = prefix_quals(leftop, opclass, prefix, pstatus);
			break;

		case OID_TEXT_REGEXEQ_OP:
		case OID_BPCHAR_REGEXEQ_OP:
		case OID_NAME_REGEXEQ_OP:
			/* the right-hand const is type text for all of these */
			pstatus = pattern_fixed_prefix(patt, Pattern_Type_Regex,
										   &prefix, &rest);
			result = prefix_quals(leftop, opclass, prefix, pstatus);
			break;

		case OID_TEXT_ICREGEXEQ_OP:
		case OID_BPCHAR_ICREGEXEQ_OP:
		case OID_NAME_ICREGEXEQ_OP:
			/* the right-hand const is type text for all of these */
			pstatus = pattern_fixed_prefix(patt, Pattern_Type_Regex_IC,
										   &prefix, &rest);
			result = prefix_quals(leftop, opclass, prefix, pstatus);
			break;

		case OID_INET_SUB_OP:
		case OID_INET_SUBEQ_OP:
		case OID_CIDR_SUB_OP:
		case OID_CIDR_SUBEQ_OP:
			result = network_prefix_quals(leftop, expr_op, opclass,
										  patt->constvalue);
			break;

		default:
			result = makeList1(clause);
			break;
	}

	return result;
}

/*
 * Given a fixed prefix that all the "leftop" values must have,
 * generate suitable indexqual condition(s).  opclass is the index
 * operator class; we use it to deduce the appropriate comparison
 * operators and operand datatypes.
 */
static List *
prefix_quals(Node *leftop, Oid opclass,
			 Const *prefix_const, Pattern_Prefix_Status pstatus)
{
	List	   *result;
	Oid			datatype;
	Oid			oproid;
	Expr	   *expr;
	Const	   *greaterstr;

	Assert(pstatus != Pattern_Prefix_None);

	switch (opclass)
	{
		case TEXT_BTREE_OPS_OID:
		case TEXT_PATTERN_BTREE_OPS_OID:
			datatype = TEXTOID;
			break;

		case VARCHAR_BTREE_OPS_OID:
		case VARCHAR_PATTERN_BTREE_OPS_OID:
			datatype = VARCHAROID;
			break;

		case BPCHAR_BTREE_OPS_OID:
		case BPCHAR_PATTERN_BTREE_OPS_OID:
			datatype = BPCHAROID;
			break;

		case NAME_BTREE_OPS_OID:
		case NAME_PATTERN_BTREE_OPS_OID:
			datatype = NAMEOID;
			break;

		case BYTEA_BTREE_OPS_OID:
			datatype = BYTEAOID;
			break;

		default:
			/* shouldn't get here */
			elog(ERROR, "unexpected opclass: %u", opclass);
			return NIL;
	}

	/*
	 * If necessary, coerce the prefix constant to the right type. The
	 * given prefix constant is either text or bytea type.
	 */
	if (prefix_const->consttype != datatype)
	{
		char	   *prefix;

		switch (prefix_const->consttype)
		{
			case TEXTOID:
				prefix = DatumGetCString(DirectFunctionCall1(textout,
											  prefix_const->constvalue));
				break;
			case BYTEAOID:
				prefix = DatumGetCString(DirectFunctionCall1(byteaout,
											  prefix_const->constvalue));
				break;
			default:
				elog(ERROR, "unexpected const type: %u",
					 prefix_const->consttype);
				return NIL;
		}
		prefix_const = string_to_const(prefix, datatype);
		pfree(prefix);
	}

	/*
	 * If we found an exact-match pattern, generate an "=" indexqual.
	 */
	if (pstatus == Pattern_Prefix_Exact)
	{
		oproid = get_opclass_member(opclass, BTEqualStrategyNumber);
		if (oproid == InvalidOid)
			elog(ERROR, "no = operator for opclass %u", opclass);
		expr = make_opclause(oproid, BOOLOID, false,
							 (Expr *) leftop, (Expr *) prefix_const);
		result = makeList1(expr);
		return result;
	}

	/*
	 * Otherwise, we have a nonempty required prefix of the values.
	 *
	 * We can always say "x >= prefix".
	 */
	oproid = get_opclass_member(opclass, BTGreaterEqualStrategyNumber);
	if (oproid == InvalidOid)
		elog(ERROR, "no >= operator for opclass %u", opclass);
	expr = make_opclause(oproid, BOOLOID, false,
						 (Expr *) leftop, (Expr *) prefix_const);
	result = makeList1(expr);

	/*-------
	 * If we can create a string larger than the prefix, we can say
	 * "x < greaterstr".
	 *-------
	 */
	greaterstr = make_greater_string(prefix_const);
	if (greaterstr)
	{
		oproid = get_opclass_member(opclass, BTLessStrategyNumber);
		if (oproid == InvalidOid)
			elog(ERROR, "no < operator for opclass %u", opclass);
		expr = make_opclause(oproid, BOOLOID, false,
							 (Expr *) leftop, (Expr *) greaterstr);
		result = lappend(result, expr);
	}

	return result;
}

/*
 * Given a leftop and a rightop, and a inet-class sup/sub operator,
 * generate suitable indexqual condition(s).  expr_op is the original
 * operator, and opclass is the index opclass.
 */
static List *
network_prefix_quals(Node *leftop, Oid expr_op, Oid opclass, Datum rightop)
{
	bool		is_eq;
	Oid			datatype;
	Oid			opr1oid;
	Oid			opr2oid;
	Datum		opr1right;
	Datum		opr2right;
	List	   *result;
	Expr	   *expr;

	switch (expr_op)
	{
		case OID_INET_SUB_OP:
			datatype = INETOID;
			is_eq = false;
			break;
		case OID_INET_SUBEQ_OP:
			datatype = INETOID;
			is_eq = true;
			break;
		case OID_CIDR_SUB_OP:
			datatype = CIDROID;
			is_eq = false;
			break;
		case OID_CIDR_SUBEQ_OP:
			datatype = CIDROID;
			is_eq = true;
			break;
		default:
			elog(ERROR, "unexpected operator: %u", expr_op);
			return NIL;
	}

	/*
	 * create clause "key >= network_scan_first( rightop )", or ">" if the
	 * operator disallows equality.
	 */
	if (is_eq)
	{
		opr1oid = get_opclass_member(opclass, BTGreaterEqualStrategyNumber);
		if (opr1oid == InvalidOid)
			elog(ERROR, "no >= operator for opclass %u", opclass);
	}
	else
	{
		opr1oid = get_opclass_member(opclass, BTGreaterStrategyNumber);
		if (opr1oid == InvalidOid)
			elog(ERROR, "no > operator for opclass %u", opclass);
	}

	opr1right = network_scan_first(rightop);

	expr = make_opclause(opr1oid, BOOLOID, false,
						 (Expr *) leftop,
						 (Expr *) makeConst(datatype, -1, opr1right,
											false, false));
	result = makeList1(expr);

	/* create clause "key <= network_scan_last( rightop )" */

	opr2oid = get_opclass_member(opclass, BTLessEqualStrategyNumber);
	if (opr2oid == InvalidOid)
		elog(ERROR, "no <= operator for opclass %u", opclass);

	opr2right = network_scan_last(rightop);

	expr = make_opclause(opr2oid, BOOLOID, false,
						 (Expr *) leftop,
						 (Expr *) makeConst(datatype, -1, opr2right,
											false, false));
	result = lappend(result, expr);

	return result;
}

/*
 * Handy subroutines for match_special_index_operator() and friends.
 */

/*
 * Generate a Datum of the appropriate type from a C string.
 * Note that all of the supported types are pass-by-ref, so the
 * returned value should be pfree'd if no longer needed.
 */
static Datum
string_to_datum(const char *str, Oid datatype)
{
	/*
	 * We cheat a little by assuming that textin() will do for bpchar and
	 * varchar constants too...
	 */
	if (datatype == NAMEOID)
		return DirectFunctionCall1(namein, CStringGetDatum(str));
	else if (datatype == BYTEAOID)
		return DirectFunctionCall1(byteain, CStringGetDatum(str));
	else
		return DirectFunctionCall1(textin, CStringGetDatum(str));
}

/*
 * Generate a Const node of the appropriate type from a C string.
 */
static Const *
string_to_const(const char *str, Oid datatype)
{
	Datum		conval = string_to_datum(str, datatype);

	return makeConst(datatype, ((datatype == NAMEOID) ? NAMEDATALEN : -1),
					 conval, false, false);
}
