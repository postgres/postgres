/*-------------------------------------------------------------------------
 *
 * indxpath.c
 *	  Routines to determine which indices are usable for scanning a
 *	  given relation, and create IndexPaths accordingly.
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/path/indxpath.c,v 1.173 2005/04/11 23:06:55 tgl Exp $
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
#include "catalog/pg_proc.h"
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

#define IsBooleanOpclass(opclass) \
	((opclass) == BOOL_BTREE_OPS_OID || (opclass) == BOOL_HASH_OPS_OID)


static List *group_clauses_by_indexkey_for_join(Query *root,
								   IndexOptInfo *index,
								   Relids outer_relids,
								   JoinType jointype, bool isouterjoin);
static bool match_clause_to_indexcol(IndexOptInfo *index,
						 int indexcol, Oid opclass,
						 RestrictInfo *rinfo);
static bool match_join_clause_to_indexcol(IndexOptInfo *index,
							  int indexcol, Oid opclass,
							  RestrictInfo *rinfo);
static Oid indexable_operator(Expr *clause, Oid opclass,
				   bool indexkey_on_left);
static bool pred_test_recurse(Node *clause, Node *predicate);
static bool pred_test_simple_clause(Expr *predicate, Node *clause);
static Relids indexable_outerrelids(IndexOptInfo *index);
static Path *make_innerjoin_index_path(Query *root, IndexOptInfo *index,
						  List *clausegroups);
static bool match_boolean_index_clause(Node *clause, int indexcol,
									   IndexOptInfo *index);
static bool match_special_index_operator(Expr *clause, Oid opclass,
							 bool indexkey_on_left);
static Expr *expand_boolean_index_clause(Node *clause, int indexcol,
										 IndexOptInfo *index);
static List *expand_indexqual_condition(RestrictInfo *rinfo, Oid opclass);
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
 *
 * Note: check_partial_indexes() must have been run previously.
 */
void
create_index_paths(Query *root, RelOptInfo *rel)
{
	Relids		all_join_outerrelids = NULL;
	ListCell   *ilist;

	foreach(ilist, rel->indexlist)
	{
		IndexOptInfo *index = (IndexOptInfo *) lfirst(ilist);
		List	   *restrictclauses;
		List	   *index_pathkeys;
		List	   *useful_pathkeys;
		bool		index_is_ordered;
		Relids		join_outerrelids;

		/* Ignore partial indexes that do not match the query */
		if (index->indpred != NIL && !index->predOK)
			continue;

		/*
		 * 1. Match the index against non-OR restriction clauses. (OR
		 * clauses will be considered later by orindxpath.c.)
		 */
		restrictclauses = group_clauses_by_indexkey(index);

		/*
		 * 2. Compute pathkeys describing index's ordering, if any, then
		 * see how many of them are actually useful for this query.
		 */
		index_pathkeys = build_index_pathkeys(root, index,
											  ForwardScanDirection);
		index_is_ordered = (index_pathkeys != NIL);
		useful_pathkeys = truncate_useless_pathkeys(root, rel,
													index_pathkeys);

		/*
		 * 3. Generate an indexscan path if there are relevant restriction
		 * clauses OR the index ordering is potentially useful for later
		 * merging or final output ordering.
		 *
		 * If there is a predicate, consider it anyway since the index
		 * predicate has already been found to match the query.  The
		 * selectivity of the predicate might alone make the index useful.
		 */
		if (restrictclauses != NIL ||
			useful_pathkeys != NIL ||
			index->indpred != NIL)
			add_path(rel, (Path *)
					 create_index_path(root, index,
									   restrictclauses,
									   useful_pathkeys,
									   index_is_ordered ?
									   ForwardScanDirection :
									   NoMovementScanDirection));

		/*
		 * 4. If the index is ordered, a backwards scan might be
		 * interesting. Currently this is only possible for a DESC query
		 * result ordering.
		 */
		if (index_is_ordered)
		{
			index_pathkeys = build_index_pathkeys(root, index,
												  BackwardScanDirection);
			useful_pathkeys = truncate_useless_pathkeys(root, rel,
														index_pathkeys);
			if (useful_pathkeys != NIL)
				add_path(rel, (Path *)
						 create_index_path(root, index,
										   restrictclauses,
										   useful_pathkeys,
										   BackwardScanDirection));
		}

		/*
		 * 5. Examine join clauses to see which ones are potentially
		 * usable with this index, and generate the set of all other
		 * relids that participate in such join clauses.  We'll use this
		 * set later to recognize outer rels that are equivalent for
		 * joining purposes. We compute both per-index and
		 * overall-for-relation sets.
		 */
		join_outerrelids = indexable_outerrelids(index);
		index->outer_relids = join_outerrelids;
		all_join_outerrelids = bms_add_members(all_join_outerrelids,
											   join_outerrelids);
	}

	rel->index_outer_relids = all_join_outerrelids;
}


/****************************************************************************
 *				----  ROUTINES TO CHECK RESTRICTIONS  ----
 ****************************************************************************/


/*
 * group_clauses_by_indexkey
 *	  Find restriction clauses that can be used with an index.
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
List *
group_clauses_by_indexkey(IndexOptInfo *index)
{
	List	   *clausegroup_list = NIL;
	List	   *restrictinfo_list = index->rel->baserestrictinfo;
	int			indexcol = 0;
	Oid		   *classes = index->classlist;

	if (restrictinfo_list == NIL)
		return NIL;

	do
	{
		Oid			curClass = classes[0];
		List	   *clausegroup = NIL;
		ListCell   *l;

		foreach(l, restrictinfo_list)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

			if (match_clause_to_indexcol(index,
										 indexcol,
										 curClass,
										 rinfo))
				clausegroup = lappend(clausegroup, rinfo);
		}

		/*
		 * If no clauses match this key, we're done; we don't want to look
		 * at keys to its right.
		 */
		if (clausegroup == NIL)
			break;

		clausegroup_list = lappend(clausegroup_list, clausegroup);

		indexcol++;
		classes++;

	} while (!DoneMatchingIndexKeys(classes));

	return clausegroup_list;
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
group_clauses_by_indexkey_for_join(Query *root, IndexOptInfo *index,
								   Relids outer_relids,
								   JoinType jointype, bool isouterjoin)
{
	List	   *clausegroup_list = NIL;
	bool		jfound = false;
	int			indexcol = 0;
	Oid		   *classes = index->classlist;

	do
	{
		Oid			curClass = classes[0];
		List	   *clausegroup = NIL;
		int			numsources;
		ListCell   *l;

		/*
		 * We can always use plain restriction clauses for the rel.  We
		 * scan these first because we want them first in the clausegroup
		 * list for the convenience of remove_redundant_join_clauses,
		 * which can never remove non-join clauses and hence won't be able
		 * to get rid of a non-join clause if it appears after a join
		 * clause it is redundant with.
		 */
		foreach(l, index->rel->baserestrictinfo)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

			/* Can't use pushed-down clauses in outer join */
			if (isouterjoin && rinfo->is_pushed_down)
				continue;

			if (match_clause_to_indexcol(index,
										 indexcol,
										 curClass,
										 rinfo))
				clausegroup = lappend(clausegroup, rinfo);
		}

		/* found anything in base restrict list? */
		numsources = (clausegroup != NIL) ? 1 : 0;

		/* Look for joinclauses that are usable with given outer_relids */
		foreach(l, index->rel->joininfo)
		{
			JoinInfo   *joininfo = (JoinInfo *) lfirst(l);
			bool		jfoundhere = false;
			ListCell   *j;

			if (!bms_is_subset(joininfo->unjoined_relids, outer_relids))
				continue;

			foreach(j, joininfo->jinfo_restrictinfo)
			{
				RestrictInfo *rinfo = (RestrictInfo *) lfirst(j);

				/* Can't use pushed-down clauses in outer join */
				if (isouterjoin && rinfo->is_pushed_down)
					continue;

				if (match_join_clause_to_indexcol(index,
												  indexcol,
												  curClass,
												  rinfo))
				{
					clausegroup = lappend(clausegroup, rinfo);
					if (!jfoundhere)
					{
						jfoundhere = true;
						jfound = true;
						numsources++;
					}
				}
			}
		}

		/*
		 * If we found clauses in more than one list, we may now have
		 * clauses that are known redundant.  Get rid of 'em.
		 */
		if (numsources > 1)
		{
			clausegroup = remove_redundant_join_clauses(root,
														clausegroup,
														jointype);
		}

		/*
		 * If no clauses match this key, we're done; we don't want to look
		 * at keys to its right.
		 */
		if (clausegroup == NIL)
			break;

		clausegroup_list = lappend(clausegroup_list, clausegroup);

		indexcol++;
		classes++;

	} while (!DoneMatchingIndexKeys(classes));

	/* if no join clause was matched then forget it, per comments above */
	if (!jfound)
		return NIL;

	return clausegroup_list;
}


/*
 * group_clauses_by_indexkey_for_or
 *	  Generate a list of sublists of clauses that can be used with an index
 *	  to find rows matching an OR subclause.
 *
 * This is essentially just like group_clauses_by_indexkey() except that
 * we can use the given clause (or any AND subclauses of it) as well as
 * top-level restriction clauses of the relation.  Furthermore, we demand
 * that at least one such use be made, otherwise we fail and return NIL.
 * (Any path we made without such a use would be redundant with non-OR
 * indexscans.	Compare also group_clauses_by_indexkey_for_join.)
 *
 * XXX When we generate an indexqual list that uses both the OR subclause
 * and top-level restriction clauses, we end up with a slightly inefficient
 * plan because create_indexscan_plan is not very bright about figuring out
 * which restriction clauses are implied by the generated indexqual condition.
 * Currently we'll end up rechecking both the OR clause and the top-level
 * restriction clause as qpquals.  FIXME someday.
 */
List *
group_clauses_by_indexkey_for_or(IndexOptInfo *index, Expr *orsubclause)
{
	List	   *clausegroup_list = NIL;
	bool		matched = false;
	int			indexcol = 0;
	Oid		   *classes = index->classlist;

	do
	{
		Oid			curClass = classes[0];
		List	   *clausegroup = NIL;
		ListCell   *item;

		/* Try to match the OR subclause to the index key */
		if (IsA(orsubclause, RestrictInfo))
		{
			if (match_clause_to_indexcol(index, indexcol, curClass,
										 (RestrictInfo *) orsubclause))
			{
				clausegroup = lappend(clausegroup, orsubclause);
				matched = true;
			}
		}
		else if (and_clause((Node *) orsubclause))
		{
			foreach(item, ((BoolExpr *) orsubclause)->args)
			{
				RestrictInfo *subsubclause = (RestrictInfo *) lfirst(item);

				if (IsA(subsubclause, RestrictInfo) &&
					match_clause_to_indexcol(index, indexcol, curClass,
											 subsubclause))
				{
					clausegroup = lappend(clausegroup, subsubclause);
					matched = true;
				}
			}
		}

		/*
		 * If we found no clauses for this indexkey in the OR subclause
		 * itself, try looking in the rel's top-level restriction list.
		 *
		 * XXX should we always search the top-level list?	Slower but could
		 * sometimes yield a better plan.
		 */
		if (clausegroup == NIL)
		{
			foreach(item, index->rel->baserestrictinfo)
			{
				RestrictInfo *rinfo = (RestrictInfo *) lfirst(item);

				if (match_clause_to_indexcol(index, indexcol, curClass,
											 rinfo))
					clausegroup = lappend(clausegroup, rinfo);
			}
		}

		/*
		 * If still no clauses match this key, we're done; we don't want
		 * to look at keys to its right.
		 */
		if (clausegroup == NIL)
			break;

		clausegroup_list = lappend(clausegroup_list, clausegroup);

		indexcol++;
		classes++;
	} while (!DoneMatchingIndexKeys(classes));

	/* if OR clause was not used then forget it, per comments above */
	if (!matched)
		return NIL;

	return clausegroup_list;
}


/*
 * match_clause_to_indexcol()
 *	  Determines whether a restriction clause matches a column of an index.
 *
 *	  To match a normal index, the clause:
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
 *	  For boolean indexes, it is also possible to match the clause directly
 *	  to the indexkey; or perhaps the clause is (NOT indexkey).
 *
 * 'index' is the index of interest.
 * 'indexcol' is a column number of 'index' (counting from 0).
 * 'opclass' is the corresponding operator class.
 * 'rinfo' is the clause to be tested (as a RestrictInfo node).
 *
 * Returns true if the clause can be used with this index key.
 *
 * NOTE:  returns false if clause is an OR or AND clause; it is the
 * responsibility of higher-level routines to cope with those.
 */
static bool
match_clause_to_indexcol(IndexOptInfo *index,
						 int indexcol,
						 Oid opclass,
						 RestrictInfo *rinfo)
{
	Expr	   *clause = rinfo->clause;
	Node	   *leftop,
			   *rightop;

	/* First check for boolean-index cases. */
	if (IsBooleanOpclass(opclass))
	{
		if (match_boolean_index_clause((Node *) clause, indexcol, index))
			return true;
	}

	/* Else clause must be a binary opclause. */
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
	if (match_index_to_operand(leftop, indexcol, index) &&
		is_pseudo_constant_clause_relids(rightop, rinfo->right_relids))
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

	if (match_index_to_operand(rightop, indexcol, index) &&
		is_pseudo_constant_clause_relids(leftop, rinfo->left_relids))
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
 *	  The boolean-index cases don't apply.
 *
 *	  As above, we must be able to commute the clause to put the indexkey
 *	  on the left.
 *
 *	  Note that we already know that the clause as a whole uses vars from
 *	  the interesting set of relations.  But we need to defend against
 *	  expressions like (a.f1 OP (b.f2 OP a.f3)); that's not processable by
 *	  an indexscan nestloop join, whereas (a.f1 OP (b.f2 OP c.f3)) is.
 *
 * 'index' is the index of interest.
 * 'indexcol' is a column number of 'index' (counting from 0).
 * 'opclass' is the corresponding operator class.
 * 'rinfo' is the clause to be tested (as a RestrictInfo node).
 *
 * Returns true if the clause can be used with this index key.
 *
 * NOTE:  returns false if clause is an OR or AND clause; it is the
 * responsibility of higher-level routines to cope with those.
 */
static bool
match_join_clause_to_indexcol(IndexOptInfo *index,
							  int indexcol,
							  Oid opclass,
							  RestrictInfo *rinfo)
{
	Expr	   *clause = rinfo->clause;
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
	if (match_index_to_operand(leftop, indexcol, index))
	{
		Relids		othervarnos = rinfo->right_relids;
		bool		isIndexable;

		isIndexable =
			!bms_overlap(index->rel->relids, othervarnos) &&
			!contain_volatile_functions(rightop) &&
			is_indexable_operator(clause, opclass, true);
		return isIndexable;
	}

	if (match_index_to_operand(rightop, indexcol, index))
	{
		Relids		othervarnos = rinfo->left_relids;
		bool		isIndexable;

		isIndexable =
			!bms_overlap(index->rel->relids, othervarnos) &&
			!contain_volatile_functions(leftop) &&
			is_indexable_operator(clause, opclass, false);
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
 * check_partial_indexes
 *		Check each partial index of the relation, and mark it predOK or not
 *		depending on whether the predicate is satisfied for this query.
 */
void
check_partial_indexes(Query *root, RelOptInfo *rel)
{
	List	   *restrictinfo_list = rel->baserestrictinfo;
	ListCell   *ilist;

	foreach(ilist, rel->indexlist)
	{
		IndexOptInfo *index = (IndexOptInfo *) lfirst(ilist);

		/*
		 * If this is a partial index, we can only use it if it passes the
		 * predicate test.
		 */
		if (index->indpred == NIL)
			continue;			/* ignore non-partial indexes */

		index->predOK = pred_test(index->indpred, restrictinfo_list);
	}
}

/*
 * pred_test
 *	  Does the "predicate inclusion test" for partial indexes.
 *
 *	  Recursively checks whether the clauses in restrictinfo_list imply
 *	  that the given predicate is true.
 *
 *	  The top-level List structure of each list corresponds to an AND list.
 *	  We assume that eval_const_expressions() has been applied and so there
 *	  are no un-flattened ANDs or ORs (e.g., no AND immediately within an AND,
 *	  including AND just below the top-level List structure).
 *	  If this is not true we might fail to prove an implication that is
 *	  valid, but no worse consequences will ensue.
 */
bool
pred_test(List *predicate_list, List *restrictinfo_list)
{
	ListCell   *item;

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

	/*
	 * In all cases where the predicate is an AND-clause, pred_test_recurse()
	 * will prefer to iterate over the predicate's components.  So we can
	 * just do that to start with here, and eliminate the need for
	 * pred_test_recurse() to handle a bare List on the predicate side.
	 *
	 * Logic is: restriction must imply each of the AND'ed predicate items.
	 */
	foreach(item, predicate_list)
	{
		if (!pred_test_recurse((Node *) restrictinfo_list, lfirst(item)))
			return false;
	}
	return true;
}


/*----------
 * pred_test_recurse
 *	  Does the "predicate inclusion test" for non-NULL restriction and
 *	  predicate clauses.
 *
 * The logic followed here is ("=>" means "implies"):
 *	atom A => atom B iff:			pred_test_simple_clause says so
 *	atom A => AND-expr B iff:		A => each of B's components
 *	atom A => OR-expr B iff:		A => any of B's components
 *	AND-expr A => atom B iff:		any of A's components => B
 *	AND-expr A => AND-expr B iff:	A => each of B's components
 *	AND-expr A => OR-expr B iff:	A => any of B's components,
 *									*or* any of A's components => B
 *	OR-expr A => atom B iff:		each of A's components => B
 *	OR-expr A => AND-expr B iff:	A => each of B's components
 *	OR-expr A => OR-expr B iff:		each of A's components => any of B's
 *
 * An "atom" is anything other than an AND or OR node.  Notice that we don't
 * have any special logic to handle NOT nodes; these should have been pushed
 * down or eliminated where feasible by prepqual.c.
 *
 * We can't recursively expand either side first, but have to interleave
 * the expansions per the above rules, to be sure we handle all of these
 * examples:
 *		(x OR y) => (x OR y OR z)
 *		(x AND y AND z) => (x AND y)
 *		(x AND y) => ((x AND y) OR z)
 *		((x OR y) AND z) => (x OR y)
 * This is still not an exhaustive test, but it handles most normal cases
 * under the assumption that both inputs have been AND/OR flattened.
 *
 * A bare List node on the restriction side is interpreted as an AND clause,
 * in order to handle the top-level restriction List properly.  However we
 * need not consider a List on the predicate side since pred_test() already
 * expanded it.
 *
 * We have to be prepared to handle RestrictInfo nodes in the restrictinfo
 * tree, though not in the predicate tree.
 *----------
 */
static bool
pred_test_recurse(Node *clause, Node *predicate)
{
	ListCell   *item;

	Assert(clause != NULL);
	/* skip through RestrictInfo */
	if (IsA(clause, RestrictInfo))
	{
		clause = (Node *) ((RestrictInfo *) clause)->clause;
		Assert(clause != NULL);
		Assert(!IsA(clause, RestrictInfo));
	}
	Assert(predicate != NULL);

	/*
	 * Since a restriction List clause is handled the same as an AND clause,
	 * we can avoid duplicate code like this:
	 */
	if (and_clause(clause))
		clause = (Node *) ((BoolExpr *) clause)->args;

	if (IsA(clause, List))
	{
		if (and_clause(predicate))
		{
			/* AND-clause => AND-clause if A implies each of B's items */
			foreach(item, ((BoolExpr *) predicate)->args)
			{
				if (!pred_test_recurse(clause, lfirst(item)))
					return false;
			}
			return true;
		}
		else if (or_clause(predicate))
		{
			/* AND-clause => OR-clause if A implies any of B's items */
			/* Needed to handle (x AND y) => ((x AND y) OR z) */
			foreach(item, ((BoolExpr *) predicate)->args)
			{
				if (pred_test_recurse(clause, lfirst(item)))
					return true;
			}
			/* Also check if any of A's items implies B */
			/* Needed to handle ((x OR y) AND z) => (x OR y) */
			foreach(item, (List *) clause)
			{
				if (pred_test_recurse(lfirst(item), predicate))
					return true;
			}
			return false;
		}
		else
		{
			/* AND-clause => atom if any of A's items implies B */
			foreach(item, (List *) clause)
			{
				if (pred_test_recurse(lfirst(item), predicate))
					return true;
			}
			return false;
		}
	}
	else if (or_clause(clause))
	{
		if (or_clause(predicate))
		{
			/*
			 * OR-clause => OR-clause if each of A's items implies any of
			 * B's items.  Messy but can't do it any more simply.
			 */
			foreach(item, ((BoolExpr *) clause)->args)
			{
				Node	   *citem = lfirst(item);
				ListCell   *item2;

				foreach(item2, ((BoolExpr *) predicate)->args)
				{
					if (pred_test_recurse(citem, lfirst(item2)))
						break;
				}
				if (item2 == NULL)
					return false; /* doesn't imply any of B's */
			}
			return true;
		}
		else
		{
			/* OR-clause => AND-clause if each of A's items implies B */
			/* OR-clause => atom if each of A's items implies B */
			foreach(item, ((BoolExpr *) clause)->args)
			{
				if (!pred_test_recurse(lfirst(item), predicate))
					return false;
			}
			return true;
		}
	}
	else
	{
		if (and_clause(predicate))
		{
			/* atom => AND-clause if A implies each of B's items */
			foreach(item, ((BoolExpr *) predicate)->args)
			{
				if (!pred_test_recurse(clause, lfirst(item)))
					return false;
			}
			return true;
		}
		else if (or_clause(predicate))
		{
			/* atom => OR-clause if A implies any of B's items */
			foreach(item, ((BoolExpr *) predicate)->args)
			{
				if (pred_test_recurse(clause, lfirst(item)))
					return true;
			}
			return false;
		}
		else
		{
			/* atom => atom is the base case */
			return pred_test_simple_clause((Expr *) predicate, clause);
		}
	}
}


/*
 * Define an "operator implication table" for btree operators ("strategies").
 *
 * The strategy numbers defined by btree indexes (see access/skey.h) are:
 *		(1) <	(2) <=	 (3) =	 (4) >=   (5) >
 * and in addition we use (6) to represent <>.	<> is not a btree-indexable
 * operator, but we assume here that if the equality operator of a btree
 * opclass has a negator operator, the negator behaves as <> for the opclass.
 *
 * The interpretation of:
 *
 *		test_op = BT_implic_table[given_op-1][target_op-1]
 *
 * where test_op, given_op and target_op are strategy numbers (from 1 to 6)
 * of btree operators, is as follows:
 *
 *	 If you know, for some ATTR, that "ATTR given_op CONST1" is true, and you
 *	 want to determine whether "ATTR target_op CONST2" must also be true, then
 *	 you can use "CONST2 test_op CONST1" as a test.  If this test returns true,
 *	 then the target expression must be true; if the test returns false, then
 *	 the target expression may be false.
 *
 * An entry where test_op == 0 means the implication cannot be determined,
 * i.e., this test should always be considered false.
 */

#define BTLT BTLessStrategyNumber
#define BTLE BTLessEqualStrategyNumber
#define BTEQ BTEqualStrategyNumber
#define BTGE BTGreaterEqualStrategyNumber
#define BTGT BTGreaterStrategyNumber
#define BTNE 6

static const StrategyNumber
			BT_implic_table[6][6] = {
/*
 *			The target operator:
 *
 *	   LT	LE	   EQ	 GE    GT	 NE
 */
	{BTGE, BTGE, 0, 0, 0, BTGE},	/* LT */
	{BTGT, BTGE, 0, 0, 0, BTGT},	/* LE */
	{BTGT, BTGE, BTEQ, BTLE, BTLT, BTNE},		/* EQ */
	{0, 0, 0, BTLE, BTLT, BTLT},	/* GE */
	{0, 0, 0, BTLE, BTLE, BTLE},	/* GT */
	{0, 0, 0, 0, 0, BTEQ}		/* NE */
};


/*----------
 * pred_test_simple_clause
 *	  Does the "predicate inclusion test" for a "simple clause" predicate
 *	  and a "simple clause" restriction.
 *
 * We have three strategies for determining whether one simple clause
 * implies another:
 *
 * A simple and general way is to see if they are equal(); this works for any
 * kind of expression.	(Actually, there is an implied assumption that the
 * functions in the expression are immutable, ie dependent only on their input
 * arguments --- but this was checked for the predicate by CheckPredicate().)
 *
 * When the predicate is of the form "foo IS NOT NULL", we can conclude that
 * the predicate is implied if the clause is a strict operator or function
 * that has "foo" as an input.	In this case the clause must yield NULL when
 * "foo" is NULL, which we can take as equivalent to FALSE because we know
 * we are within an AND/OR subtree of a WHERE clause.  (Again, "foo" is
 * already known immutable, so the clause will certainly always fail.)
 *
 * Our other way works only for binary boolean opclauses of the form
 * "foo op constant", where "foo" is the same in both clauses.	The operators
 * and constants can be different but the operators must be in the same btree
 * operator class.	We use the above operator implication table to be able to
 * derive implications between nonidentical clauses.  (Note: "foo" is known
 * immutable, and constants are surely immutable, but we have to check that
 * the operators are too.  As of 8.0 it's possible for opclasses to contain
 * operators that are merely stable, and we dare not make deductions with
 * these.)
 *
 * Eventually, rtree operators could also be handled by defining an
 * appropriate "RT_implic_table" array.
 *----------
 */
static bool
pred_test_simple_clause(Expr *predicate, Node *clause)
{
	Node	   *leftop,
			   *rightop;
	Node	   *pred_var,
			   *clause_var;
	Const	   *pred_const,
			   *clause_const;
	bool		pred_var_on_left,
				clause_var_on_left,
				pred_op_negated;
	Oid			pred_op,
				clause_op,
				pred_op_negator,
				clause_op_negator,
				test_op = InvalidOid;
	Oid			opclass_id;
	bool		found = false;
	StrategyNumber pred_strategy,
				clause_strategy,
				test_strategy;
	Oid			clause_subtype;
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

	/* Next try the IS NOT NULL case */
	if (predicate && IsA(predicate, NullTest) &&
		((NullTest *) predicate)->nulltesttype == IS_NOT_NULL)
	{
		Expr	   *nonnullarg = ((NullTest *) predicate)->arg;

		if (is_opclause(clause) &&
			list_member(((OpExpr *) clause)->args, nonnullarg) &&
			op_strict(((OpExpr *) clause)->opno))
			return true;
		if (is_funcclause(clause) &&
			list_member(((FuncExpr *) clause)->args, nonnullarg) &&
			func_strict(((FuncExpr *) clause)->funcid))
			return true;
		return false;			/* we can't succeed below... */
	}

	/*
	 * Can't do anything more unless they are both binary opclauses with a
	 * Const on one side, and identical subexpressions on the other sides.
	 * Note we don't have to think about binary relabeling of the Const
	 * node, since that would have been folded right into the Const.
	 *
	 * If either Const is null, we also fail right away; this assumes that
	 * the test operator will always be strict.
	 */
	if (!is_opclause(predicate))
		return false;
	leftop = get_leftop(predicate);
	rightop = get_rightop(predicate);
	if (rightop == NULL)
		return false;			/* not a binary opclause */
	if (IsA(rightop, Const))
	{
		pred_var = leftop;
		pred_const = (Const *) rightop;
		pred_var_on_left = true;
	}
	else if (IsA(leftop, Const))
	{
		pred_var = rightop;
		pred_const = (Const *) leftop;
		pred_var_on_left = false;
	}
	else
		return false;			/* no Const to be found */
	if (pred_const->constisnull)
		return false;

	if (!is_opclause(clause))
		return false;
	leftop = get_leftop((Expr *) clause);
	rightop = get_rightop((Expr *) clause);
	if (rightop == NULL)
		return false;			/* not a binary opclause */
	if (IsA(rightop, Const))
	{
		clause_var = leftop;
		clause_const = (Const *) rightop;
		clause_var_on_left = true;
	}
	else if (IsA(leftop, Const))
	{
		clause_var = rightop;
		clause_const = (Const *) leftop;
		clause_var_on_left = false;
	}
	else
		return false;			/* no Const to be found */
	if (clause_const->constisnull)
		return false;

	/*
	 * Check for matching subexpressions on the non-Const sides.  We used
	 * to only allow a simple Var, but it's about as easy to allow any
	 * expression.	Remember we already know that the pred expression does
	 * not contain any non-immutable functions, so identical expressions
	 * should yield identical results.
	 */
	if (!equal(pred_var, clause_var))
		return false;

	/*
	 * Okay, get the operators in the two clauses we're comparing. Commute
	 * them if needed so that we can assume the variables are on the left.
	 */
	pred_op = ((OpExpr *) predicate)->opno;
	if (!pred_var_on_left)
	{
		pred_op = get_commutator(pred_op);
		if (!OidIsValid(pred_op))
			return false;
	}

	clause_op = ((OpExpr *) clause)->opno;
	if (!clause_var_on_left)
	{
		clause_op = get_commutator(clause_op);
		if (!OidIsValid(clause_op))
			return false;
	}

	/*
	 * Try to find a btree opclass containing the needed operators.
	 *
	 * We must find a btree opclass that contains both operators, else the
	 * implication can't be determined.  Also, the pred_op has to be of
	 * default subtype (implying left and right input datatypes are the
	 * same); otherwise it's unsafe to put the pred_const on the left side
	 * of the test.  Also, the opclass must contain a suitable test
	 * operator matching the clause_const's type (which we take to mean
	 * that it has the same subtype as the original clause_operator).
	 *
	 * If there are multiple matching opclasses, assume we can use any one to
	 * determine the logical relationship of the two operators and the
	 * correct corresponding test operator.  This should work for any
	 * logically consistent opclasses.
	 */
	catlist = SearchSysCacheList(AMOPOPID, 1,
								 ObjectIdGetDatum(pred_op),
								 0, 0, 0);

	/*
	 * If we couldn't find any opclass containing the pred_op, perhaps it
	 * is a <> operator.  See if it has a negator that is in an opclass.
	 */
	pred_op_negated = false;
	if (catlist->n_members == 0)
	{
		pred_op_negator = get_negator(pred_op);
		if (OidIsValid(pred_op_negator))
		{
			pred_op_negated = true;
			ReleaseSysCacheList(catlist);
			catlist = SearchSysCacheList(AMOPOPID, 1,
									   ObjectIdGetDatum(pred_op_negator),
										 0, 0, 0);
		}
	}

	/* Also may need the clause_op's negator */
	clause_op_negator = get_negator(clause_op);

	/* Now search the opclasses */
	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	pred_tuple = &catlist->members[i]->tuple;
		Form_pg_amop pred_form = (Form_pg_amop) GETSTRUCT(pred_tuple);
		HeapTuple	clause_tuple;

		opclass_id = pred_form->amopclaid;

		/* must be btree */
		if (!opclass_is_btree(opclass_id))
			continue;
		/* predicate operator must be default within this opclass */
		if (pred_form->amopsubtype != InvalidOid)
			continue;

		/* Get the predicate operator's btree strategy number */
		pred_strategy = (StrategyNumber) pred_form->amopstrategy;
		Assert(pred_strategy >= 1 && pred_strategy <= 5);

		if (pred_op_negated)
		{
			/* Only consider negators that are = */
			if (pred_strategy != BTEqualStrategyNumber)
				continue;
			pred_strategy = BTNE;
		}

		/*
		 * From the same opclass, find a strategy number for the
		 * clause_op, if possible
		 */
		clause_tuple = SearchSysCache(AMOPOPID,
									  ObjectIdGetDatum(clause_op),
									  ObjectIdGetDatum(opclass_id),
									  0, 0);
		if (HeapTupleIsValid(clause_tuple))
		{
			Form_pg_amop clause_form = (Form_pg_amop) GETSTRUCT(clause_tuple);

			/* Get the restriction clause operator's strategy/subtype */
			clause_strategy = (StrategyNumber) clause_form->amopstrategy;
			Assert(clause_strategy >= 1 && clause_strategy <= 5);
			clause_subtype = clause_form->amopsubtype;
			ReleaseSysCache(clause_tuple);
		}
		else if (OidIsValid(clause_op_negator))
		{
			clause_tuple = SearchSysCache(AMOPOPID,
									 ObjectIdGetDatum(clause_op_negator),
										  ObjectIdGetDatum(opclass_id),
										  0, 0);
			if (HeapTupleIsValid(clause_tuple))
			{
				Form_pg_amop clause_form = (Form_pg_amop) GETSTRUCT(clause_tuple);

				/* Get the restriction clause operator's strategy/subtype */
				clause_strategy = (StrategyNumber) clause_form->amopstrategy;
				Assert(clause_strategy >= 1 && clause_strategy <= 5);
				clause_subtype = clause_form->amopsubtype;
				ReleaseSysCache(clause_tuple);

				/* Only consider negators that are = */
				if (clause_strategy != BTEqualStrategyNumber)
					continue;
				clause_strategy = BTNE;
			}
			else
				continue;
		}
		else
			continue;

		/*
		 * Look up the "test" strategy number in the implication table
		 */
		test_strategy = BT_implic_table[clause_strategy - 1][pred_strategy - 1];
		if (test_strategy == 0)
		{
			/* Can't determine implication using this interpretation */
			continue;
		}

		/*
		 * See if opclass has an operator for the test strategy and the
		 * clause datatype.
		 */
		if (test_strategy == BTNE)
		{
			test_op = get_opclass_member(opclass_id, clause_subtype,
										 BTEqualStrategyNumber);
			if (OidIsValid(test_op))
				test_op = get_negator(test_op);
		}
		else
		{
			test_op = get_opclass_member(opclass_id, clause_subtype,
										 test_strategy);
		}
		if (OidIsValid(test_op))
		{
			/*
			 * Last check: test_op must be immutable.
			 *
			 * Note that we require only the test_op to be immutable, not the
			 * original clause_op.	(pred_op must be immutable, else it
			 * would not be allowed in an index predicate.)  Essentially
			 * we are assuming that the opclass is consistent even if it
			 * contains operators that are merely stable.
			 */
			if (op_volatile(test_op) == PROVOLATILE_IMMUTABLE)
			{
				found = true;
				break;
			}
		}
	}

	ReleaseSysCacheList(catlist);

	if (!found)
	{
		/* couldn't find a btree opclass to interpret the operators */
		return false;
	}

	/*
	 * Evaluate the test.  For this we need an EState.
	 */
	estate = CreateExecutorState();

	/* We can use the estate's working context to avoid memory leaks. */
	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	/* Build expression tree */
	test_expr = make_opclause(test_op,
							  BOOLOID,
							  false,
							  (Expr *) pred_const,
							  (Expr *) clause_const);

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
 */
static Relids
indexable_outerrelids(IndexOptInfo *index)
{
	Relids		outer_relids = NULL;
	ListCell   *l;

	foreach(l, index->rel->joininfo)
	{
		JoinInfo   *joininfo = (JoinInfo *) lfirst(l);
		bool		match_found = false;
		ListCell   *j;

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
			int			indexcol = 0;
			Oid		   *classes = index->classlist;

			do
			{
				Oid			curClass = classes[0];

				if (match_join_clause_to_indexcol(index,
												  indexcol,
												  curClass,
												  rinfo))
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
	ListCell   *ilist;
	ListCell   *jlist;
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

		if (jlist == NULL)		/* failed to find a match? */
		{
			List	   *clausegroups;

			/* find useful clauses for this index and outerjoin set */
			clausegroups = group_clauses_by_indexkey_for_join(root,
															  index,
													  index_outer_relids,
															  jointype,
															isouterjoin);
			if (clausegroups)
			{
				/* make the path */
				path = make_innerjoin_index_path(root, index, clausegroups);
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
 * 'index' is the index of interest
 * 'clausegroups' is a list of lists of RestrictInfos that can use 'index'
 */
static Path *
make_innerjoin_index_path(Query *root,
						  IndexOptInfo *index,
						  List *clausegroups)
{
	IndexPath  *pathnode = makeNode(IndexPath);
	RelOptInfo *rel = index->rel;
	List	   *indexquals,
			   *allclauses;

	/* XXX perhaps this code should be merged with create_index_path? */

	pathnode->path.pathtype = T_IndexScan;
	pathnode->path.parent = rel;

	/*
	 * There's no point in marking the path with any pathkeys, since it
	 * will only ever be used as the inner path of a nestloop, and so its
	 * ordering does not matter.
	 */
	pathnode->path.pathkeys = NIL;

	/* Convert clauses to indexquals the executor can handle */
	indexquals = expand_indexqual_conditions(index, clausegroups);

	/* Flatten the clausegroups list to produce indexclauses list */
	allclauses = flatten_clausegroups_list(clausegroups);

	/*
	 * Note that we are making a pathnode for a single-scan indexscan;
	 * therefore, indexinfo etc should be single-element lists.
	 */
	pathnode->indexinfo = list_make1(index);
	pathnode->indexclauses = list_make1(allclauses);
	pathnode->indexquals = list_make1(indexquals);

	pathnode->isjoininner = true;

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
	 * clauselist_selectivity into double-counting their selectivity.
	 * However, since RestrictInfo nodes aren't copied when linking them
	 * into different lists, it should be sufficient to use pointer
	 * comparison to remove duplicates.)
	 *
	 * Always assume the join type is JOIN_INNER; even if some of the join
	 * clauses come from other contexts, that's not our problem.
	 */
	allclauses = list_union_ptr(rel->baserestrictinfo, allclauses);
	pathnode->rows = rel->tuples *
		clauselist_selectivity(root,
							   allclauses,
							   rel->relid,		/* do not use 0! */
							   JOIN_INNER);
	/* Like costsize.c, force estimate to be at least one row */
	pathnode->rows = clamp_row_est(pathnode->rows);

	cost_index(&pathnode->path, root, index, indexquals, true);

	return (Path *) pathnode;
}

/*
 * flatten_clausegroups_list
 *	  Given a list of lists of RestrictInfos, flatten it to a list
 *	  of RestrictInfos.
 *
 * This is used to flatten out the result of group_clauses_by_indexkey()
 * or one of its sibling routines, to produce an indexclauses list.
 */
List *
flatten_clausegroups_list(List *clausegroups)
{
	List	   *allclauses = NIL;
	ListCell   *l;

	foreach(l, clausegroups)
		allclauses = list_concat(allclauses, list_copy((List *) lfirst(l)));
	return allclauses;
}

/*
 * make_expr_from_indexclauses()
 *	  Given an indexclauses structure, produce an ordinary boolean expression.
 *
 * This consists of stripping out the RestrictInfo nodes and inserting
 * explicit AND and OR nodes as needed.  There's not much to it, but
 * the functionality is needed in a few places, so centralize the logic.
 */
Expr *
make_expr_from_indexclauses(List *indexclauses)
{
	List	   *orclauses = NIL;
	ListCell   *orlist;

	/* There's no such thing as an indexpath with zero scans */
	Assert(indexclauses != NIL);

	foreach(orlist, indexclauses)
	{
		List	   *andlist = (List *) lfirst(orlist);

		/* Strip RestrictInfos */
		andlist = get_actual_clauses(andlist);
		/* Insert AND node if needed, and add to orclauses list */
		orclauses = lappend(orclauses, make_ands_explicit(andlist));
	}

	if (list_length(orclauses) > 1)
		return make_orclause(orclauses);
	else
		return (Expr *) linitial(orclauses);
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
 * index: the index of interest
 */
bool
match_index_to_operand(Node *operand,
					   int indexcol,
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
			index->rel->relid == ((Var *) operand)->varno &&
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
		ListCell   *indexpr_item;
		int			i;
		Node	   *indexkey;

		indexpr_item = list_head(index->indexprs);
		for (i = 0; i < indexcol; i++)
		{
			if (index->indexkeys[i] == 0)
			{
				if (indexpr_item == NULL)
					elog(ERROR, "wrong number of index expressions");
				indexpr_item = lnext(indexpr_item);
			}
		}
		if (indexpr_item == NULL)
			elog(ERROR, "wrong number of index expressions");
		indexkey = (Node *) lfirst(indexpr_item);

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
 * Another thing that we do with this machinery is to provide special
 * smarts for "boolean" indexes (that is, indexes on boolean columns
 * that support boolean equality).  We can transform a plain reference
 * to the indexkey into "indexkey = true", or "NOT indexkey" into
 * "indexkey = false", so as to make the expression indexable using the
 * regular index operators.  (As of Postgres 8.1, we must do this here
 * because constant simplification does the reverse transformation;
 * without this code there'd be no way to use such an index at all.)
 *
 * Three routines are provided here:
 *
 * match_special_index_operator() is just an auxiliary function for
 * match_clause_to_indexcol(); after the latter fails to recognize a
 * restriction opclause's operator as a member of an index's opclass,
 * it asks match_special_index_operator() whether the clause should be
 * considered an indexqual anyway.
 *
 * match_boolean_index_clause() similarly detects clauses that can be
 * converted into boolean equality operators.
 *
 * expand_indexqual_conditions() converts a list of lists of RestrictInfo
 * nodes (with implicit AND semantics across list elements) into
 * a list of clauses that the executor can actually handle.  For operators
 * that are members of the index's opclass this transformation is a no-op,
 * but clauses recognized by match_special_index_operator() or
 * match_boolean_index_clause() must be converted into one or more "regular"
 * indexqual conditions.
 *----------
 */

/*
 * match_boolean_index_clause
 *	  Recognize restriction clauses that can be matched to a boolean index.
 *
 * This should be called only when IsBooleanOpclass() recognizes the
 * index's operator class.  We check to see if the clause matches the
 * index's key.
 */
static bool
match_boolean_index_clause(Node *clause,
						   int indexcol,
						   IndexOptInfo *index)
{
	/* Direct match? */
	if (match_index_to_operand(clause, indexcol, index))
		return true;
	/* NOT clause? */
	if (not_clause(clause))
	{
		if (match_index_to_operand((Node *) get_notclausearg((Expr *) clause),
								   indexcol, index))
			return true;
	}
	/*
	 * Since we only consider clauses at top level of WHERE, we can convert
	 * indexkey IS TRUE and indexkey IS FALSE to index searches as well.
	 * The different meaning for NULL isn't important.
	 */
	else if (clause && IsA(clause, BooleanTest))
	{
		BooleanTest	   *btest = (BooleanTest *) clause;

		if (btest->booltesttype == IS_TRUE ||
			btest->booltesttype == IS_FALSE)
			if (match_index_to_operand((Node *) btest->arg,
									   indexcol, index))
				return true;
	}
	return false;
}

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
 *	  opclass) are passed through unchanged.  Boolean clauses and "special"
 *	  index operators are expanded into clauses that the indexscan machinery
 *	  will know what to do with.
 *
 * The input list is ordered by index key, and so the output list is too.
 * (The latter is not depended on by any part of the planner, so far as I can
 * tell; but some parts of the executor do assume that the indxqual list
 * ultimately delivered to the executor is so ordered.	One such place is
 * _bt_preprocess_keys() in the btree support.	Perhaps that ought to be fixed
 * someday --- tgl 7/00)
 */
List *
expand_indexqual_conditions(IndexOptInfo *index, List *clausegroups)
{
	List	   *resultquals = NIL;
	ListCell   *clausegroup_item;
	int			indexcol = 0;
	Oid		   *classes = index->classlist;

	if (clausegroups == NIL)
		return NIL;

	clausegroup_item = list_head(clausegroups);
	do
	{
		Oid			curClass = classes[0];
		ListCell   *l;

		foreach(l, (List *) lfirst(clausegroup_item))
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

			/* First check for boolean cases */
			if (IsBooleanOpclass(curClass))
			{
				Expr   *boolqual;

				boolqual = expand_boolean_index_clause((Node *) rinfo->clause,
													   indexcol,
													   index);
				if (boolqual)
				{
					resultquals = lappend(resultquals,
										  make_restrictinfo(boolqual,
															true, true));
					continue;
				}
			}

			resultquals = list_concat(resultquals,
									  expand_indexqual_condition(rinfo,
																 curClass));
		}

		clausegroup_item = lnext(clausegroup_item);

		indexcol++;
		classes++;
	} while (clausegroup_item != NULL && !DoneMatchingIndexKeys(classes));

	Assert(clausegroup_item == NULL);	/* else more groups than indexkeys */

	return resultquals;
}

/*
 * expand_boolean_index_clause
 *	  Convert a clause recognized by match_boolean_index_clause into
 *	  a boolean equality operator clause.
 *
 * Returns NULL if the clause isn't a boolean index qual.
 */
static Expr *
expand_boolean_index_clause(Node *clause,
							int indexcol,
							IndexOptInfo *index)
{
	/* Direct match? */
	if (match_index_to_operand(clause, indexcol, index))
	{
		/* convert to indexkey = TRUE */
		return make_opclause(BooleanEqualOperator, BOOLOID, false,
							 (Expr *) clause,
							 (Expr *) makeBoolConst(true, false));
	}
	/* NOT clause? */
	if (not_clause(clause))
	{
		Node   *arg = (Node *) get_notclausearg((Expr *) clause);

		/* It must have matched the indexkey */
		Assert(match_index_to_operand(arg, indexcol, index));
		/* convert to indexkey = FALSE */
		return make_opclause(BooleanEqualOperator, BOOLOID, false,
							 (Expr *) arg,
							 (Expr *) makeBoolConst(false, false));
	}
	if (clause && IsA(clause, BooleanTest))
	{
		BooleanTest	   *btest = (BooleanTest *) clause;
		Node   *arg = (Node *) btest->arg;

		/* It must have matched the indexkey */
		Assert(match_index_to_operand(arg, indexcol, index));
		if (btest->booltesttype == IS_TRUE)
		{
			/* convert to indexkey = TRUE */
			return make_opclause(BooleanEqualOperator, BOOLOID, false,
								 (Expr *) arg,
								 (Expr *) makeBoolConst(true, false));
		}
		if (btest->booltesttype == IS_FALSE)
		{
			/* convert to indexkey = FALSE */
			return make_opclause(BooleanEqualOperator, BOOLOID, false,
								 (Expr *) arg,
								 (Expr *) makeBoolConst(false, false));
		}
		/* Oops */
		Assert(false);
	}

	return NULL;
}

/*
 * expand_indexqual_condition --- expand a single indexqual condition
 *		(other than a boolean-qual case)
 *
 * The input is a single RestrictInfo, the output a list of RestrictInfos
 */
static List *
expand_indexqual_condition(RestrictInfo *rinfo, Oid opclass)
{
	Expr	   *clause = rinfo->clause;
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
			result = list_make1(rinfo);
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
		oproid = get_opclass_member(opclass, InvalidOid,
									BTEqualStrategyNumber);
		if (oproid == InvalidOid)
			elog(ERROR, "no = operator for opclass %u", opclass);
		expr = make_opclause(oproid, BOOLOID, false,
							 (Expr *) leftop, (Expr *) prefix_const);
		result = list_make1(make_restrictinfo(expr, true, true));
		return result;
	}

	/*
	 * Otherwise, we have a nonempty required prefix of the values.
	 *
	 * We can always say "x >= prefix".
	 */
	oproid = get_opclass_member(opclass, InvalidOid,
								BTGreaterEqualStrategyNumber);
	if (oproid == InvalidOid)
		elog(ERROR, "no >= operator for opclass %u", opclass);
	expr = make_opclause(oproid, BOOLOID, false,
						 (Expr *) leftop, (Expr *) prefix_const);
	result = list_make1(make_restrictinfo(expr, true, true));

	/*-------
	 * If we can create a string larger than the prefix, we can say
	 * "x < greaterstr".
	 *-------
	 */
	greaterstr = make_greater_string(prefix_const);
	if (greaterstr)
	{
		oproid = get_opclass_member(opclass, InvalidOid,
									BTLessStrategyNumber);
		if (oproid == InvalidOid)
			elog(ERROR, "no < operator for opclass %u", opclass);
		expr = make_opclause(oproid, BOOLOID, false,
							 (Expr *) leftop, (Expr *) greaterstr);
		result = lappend(result, make_restrictinfo(expr, true, true));
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
		opr1oid = get_opclass_member(opclass, InvalidOid,
									 BTGreaterEqualStrategyNumber);
		if (opr1oid == InvalidOid)
			elog(ERROR, "no >= operator for opclass %u", opclass);
	}
	else
	{
		opr1oid = get_opclass_member(opclass, InvalidOid,
									 BTGreaterStrategyNumber);
		if (opr1oid == InvalidOid)
			elog(ERROR, "no > operator for opclass %u", opclass);
	}

	opr1right = network_scan_first(rightop);

	expr = make_opclause(opr1oid, BOOLOID, false,
						 (Expr *) leftop,
						 (Expr *) makeConst(datatype, -1, opr1right,
											false, false));
	result = list_make1(make_restrictinfo(expr, true, true));

	/* create clause "key <= network_scan_last( rightop )" */

	opr2oid = get_opclass_member(opclass, InvalidOid,
								 BTLessEqualStrategyNumber);
	if (opr2oid == InvalidOid)
		elog(ERROR, "no <= operator for opclass %u", opclass);

	opr2right = network_scan_last(rightop);

	expr = make_opclause(opr2oid, BOOLOID, false,
						 (Expr *) leftop,
						 (Expr *) makeConst(datatype, -1, opr2right,
											false, false));
	result = lappend(result, make_restrictinfo(expr, true, true));

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
