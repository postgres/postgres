/*-------------------------------------------------------------------------
 *
 * indxpath.c
 *	  Routines to determine which indices are usable for scanning a
 *	  given relation, and create IndexPaths accordingly.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/path/indxpath.c,v 1.65 1999/07/25 23:07:24 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <math.h>

#include "postgres.h"

#include "access/heapam.h"
#include "access/nbtree.h"
#include "catalog/catname.h"
#include "catalog/pg_amop.h"
#include "executor/executor.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/keys.h"
#include "optimizer/ordering.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "optimizer/restrictinfo.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_oper.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"


static void match_index_orclauses(RelOptInfo *rel, RelOptInfo *index, int indexkey,
					  int xclass, List *restrictinfo_list);
static List *match_index_orclause(RelOptInfo *rel, RelOptInfo *index, int indexkey,
			 int xclass, List *or_clauses, List *other_matching_indices);
static List *group_clauses_by_indexkey(RelOptInfo *rel, RelOptInfo *index,
				  int *indexkeys, Oid *classes, List *restrictinfo_list);
static List *group_clauses_by_ikey_for_joins(RelOptInfo *rel, RelOptInfo *index,
								int *indexkeys, Oid *classes, List *join_cinfo_list, List *restr_cinfo_list);
static bool match_clause_to_indexkey(RelOptInfo *rel, RelOptInfo *index,
									 int indexkey, int xclass,
									 Expr *clause, bool join);
static bool pred_test(List *predicate_list, List *restrictinfo_list,
		  List *joininfo_list);
static bool one_pred_test(Expr *predicate, List *restrictinfo_list);
static bool one_pred_clause_expr_test(Expr *predicate, Node *clause);
static bool one_pred_clause_test(Expr *predicate, Node *clause);
static bool clause_pred_clause_test(Expr *predicate, Node *clause);
static void indexable_joinclauses(RelOptInfo *rel, RelOptInfo *index,
								  List *joininfo_list, List *restrictinfo_list,
								  List **clausegroups, List **outerrelids);
static List *index_innerjoin(Query *root, RelOptInfo *rel, RelOptInfo *index,
							 List *clausegroup_list, List *outerrelids_list);
static List *create_index_path_group(Query *root, RelOptInfo *rel, RelOptInfo *index,
						List *clausegroup_list, bool join);
static bool match_index_to_operand(int indexkey, Expr *operand,
								   RelOptInfo *rel, RelOptInfo *index);
static bool function_index_operand(Expr *funcOpnd, RelOptInfo *rel, RelOptInfo *index);


/*
 * create_index_paths()
 *	  Generate all interesting index paths for the given relation.
 *
 *	  To be considered for an index scan, an index must match one or more
 *	  restriction clauses or join clauses from the query's qual condition.
 *
 *	  Note: an index scan might also be used simply to order the result,
 *	  either for use in a mergejoin or to satisfy an ORDER BY request.
 *	  That possibility is handled elsewhere.
 *
 * 'rel' is the relation for which we want to generate index paths
 * 'indices' is a list of available indexes for 'rel'
 * 'restrictinfo_list' is a list of restrictinfo nodes for 'rel'
 * 'joininfo_list' is a list of joininfo nodes for 'rel'
 *
 * Returns a list of IndexPath access path descriptors.
 */
List *
create_index_paths(Query *root,
				   RelOptInfo *rel,
				   List *indices,
				   List *restrictinfo_list,
				   List *joininfo_list)
{
	List	   *retval = NIL;
	List	   *ilist;

	foreach(ilist, indices)
	{
		RelOptInfo *index = (RelOptInfo *) lfirst(ilist);
		List	   *scanclausegroups;
		List	   *joinclausegroups;
		List	   *joinouterrelids;

		/*
		 * If this is a partial index, we can only use it if it passes
		 * the predicate test.
		 */
		if (index->indpred != NIL)
			if (!pred_test(index->indpred, restrictinfo_list, joininfo_list))
				continue;

		/*
		 * 1. Try matching the index against subclauses of restriction 'or'
		 * clauses (ie, 'or' clauses that reference only this relation).
		 * The restrictinfo nodes for the 'or' clauses are marked with lists
		 * of the matching indices.  No paths are actually created now;
		 * that will be done in orindxpath.c after all indexes for the rel
		 * have been examined.  (We need to do it that way because we can
		 * potentially use a different index for each subclause of an 'or',
		 * so we can't build a path for an 'or' clause until all indexes have
		 * been matched against it.)
		 *
		 * We currently only look to match the first key of each index against
		 * 'or' subclauses.  There are cases where a later key of a multi-key
		 * index could be used (if other top-level clauses match earlier keys
		 * of the index), but our poor brains are hurting already...
		 *
		 * We don't even think about special handling of 'or' clauses that
		 * involve more than one relation, since they can't be processed by
		 * a single indexscan path anyway.  Currently, cnfify() is certain
		 * to have restructured any such toplevel 'or' clauses anyway.
		 */
		match_index_orclauses(rel,
							  index,
							  index->indexkeys[0],
							  index->classlist[0],
							  restrictinfo_list);

		/*
		 * 2. If the keys of this index match any of the available non-'or'
		 * restriction clauses, then create a path using those clauses
		 * as indexquals.
		 */
		scanclausegroups = group_clauses_by_indexkey(rel,
													 index,
													 index->indexkeys,
													 index->classlist,
													 restrictinfo_list);

		if (scanclausegroups != NIL)
			retval = nconc(retval,
						   create_index_path_group(root,
												   rel,
												   index,
												   scanclausegroups,
												   false));

		/*
		 * 3. If this index can be used with any join clause, then create
		 * pathnodes for each group of usable clauses.	An index can be
		 * used with a join clause if its ordering is useful for a
		 * mergejoin, or if the index can possibly be used for scanning
		 * the inner relation of a nestloop join.
		 */
		indexable_joinclauses(rel, index,
							  joininfo_list, restrictinfo_list,
							  &joinclausegroups,
							  &joinouterrelids);

		if (joinclausegroups != NIL)
		{
			retval = nconc(retval,
						   create_index_path_group(root,
												   rel,
												   index,
												   joinclausegroups,
												   true));
			rel->innerjoin = nconc(rel->innerjoin,
								   index_innerjoin(root, rel, index,
												   joinclausegroups,
												   joinouterrelids));
		}
	}

	return retval;
}


/****************************************************************************
 *		----  ROUTINES TO PROCESS 'OR' CLAUSES  ----
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
 * 'indexkey' is the (single) key of the index that we will consider.
 * 'class' is the class of the operator corresponding to 'indexkey'.
 * 'restrictinfo_list' is the list of available restriction clauses.
 */
static void
match_index_orclauses(RelOptInfo *rel,
					  RelOptInfo *index,
					  int indexkey,
					  int xclass,
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
			restrictinfo->indexids =
				match_index_orclause(rel, index,
									 indexkey, xclass,
									 restrictinfo->clause->args,
									 restrictinfo->indexids);
		}
	}
}

/*
 * match_index_orclause
 *	  Attempts to match an index against the subclauses of an 'or' clause.
 *
 *	  A match means that:
 *	  (1) the operator within the subclause can be used with the
 *				index's specified operator class, and
 *	  (2) the variable on one side of the subclause matches the index key.
 *
 * 'or_clauses' is the list of subclauses within the 'or' clause
 * 'other_matching_indices' is the list of information on other indices
 *		that have already been matched to subclauses within this
 *		particular 'or' clause (i.e., a list previously generated by
 *		this routine), or NIL if this routine has not previously been
 *	    run for this 'or' clause.
 *
 * Returns a list of the form ((a b c) (d e f) nil (g h) ...) where
 * a,b,c are nodes of indices that match the first subclause in
 * 'or-clauses', d,e,f match the second subclause, no indices
 * match the third, g,h match the fourth, etc.
 */
static List *
match_index_orclause(RelOptInfo *rel,
					 RelOptInfo *index,
					 int indexkey,
					 int xclass,
					 List *or_clauses,
					 List *other_matching_indices)
{
	List	   *matching_indices;
	List	   *index_list;
	List	   *clist;

	/* first time through, we create list of same length as OR clause,
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

		if (match_clause_to_indexkey(rel, index, indexkey, xclass,
									 clause, false))
		{
			/* OK to add this index to sublist for this subclause */
			lfirst(matching_indices) = lcons(index,
											 lfirst(matching_indices));
		}

		matching_indices = lnext(matching_indices);
	}

	return index_list;
}

/****************************************************************************
 *				----  ROUTINES TO CHECK RESTRICTIONS  ----
 ****************************************************************************/


/*
 * DoneMatchingIndexKeys() - MACRO
 *
 * Determine whether we should continue matching index keys in a clause.
 * Depends on if there are more to match or if this is a functional index.
 * In the latter case we stop after the first match since the there can
 * be only key (i.e. the function's return value) and the attributes in
 * keys list represent the arguments to the function.  -mer 3 Oct. 1991
 */
#define DoneMatchingIndexKeys(indexkeys, index) \
		(indexkeys[0] == 0 || \
		 (index->indproc != InvalidOid))

/*
 * group_clauses_by_indexkey
 *	  Generates a list of restriction clauses that can be used with an index.
 *
 * 'rel' is the node of the relation itself.
 * 'index' is a index on 'rel'.
 * 'indexkeys' are the index keys to be matched.
 * 'classes' are the classes of the index operators on those keys.
 * 'restrictinfo_list' is the list of available restriction clauses for 'rel'.
 *
 * Returns NIL if no clauses can be used with this index.
 * Otherwise, a list containing a single sublist is returned (indicating
 * to create_index_path_group() that a single IndexPath should be created).
 * The sublist contains the RestrictInfo nodes for all clauses that can be
 * used with this index.
 *
 * The sublist is ordered by index key (but as far as I can tell, this is
 * an implementation artifact of this routine, and is not depended on by
 * any user of the returned list --- tgl 7/99).
 *
 * Note that in a multi-key index, we stop if we find a key that cannot be
 * used with any clause.  For example, given an index on (A,B,C), we might
 * return ((C1 C2 C3 C4)) if we find that clauses C1 and C2 use column A,
 * clauses C3 and C4 use column B, and no clauses use column C.  But if no
 * clauses match B we will return ((C1 C2)), whether or not there are
 * clauses matching column C, because the executor couldn't use them anyway.
 */
static List *
group_clauses_by_indexkey(RelOptInfo *rel,
						  RelOptInfo *index,
						  int *indexkeys,
						  Oid *classes,
						  List *restrictinfo_list)
{
	List	   *clausegroup_list = NIL;

	if (restrictinfo_list == NIL || indexkeys[0] == 0)
		return NIL;

	do
	{
		int			curIndxKey = indexkeys[0];
		Oid			curClass = classes[0];
		List	   *clausegroup = NIL;
		List	   *curCinfo;

		foreach(curCinfo, restrictinfo_list)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(curCinfo);

			if (match_clause_to_indexkey(rel,
										 index,
										 curIndxKey,
										 curClass,
										 rinfo->clause,
										 false))
				clausegroup = lappend(clausegroup, rinfo);
		}

		/* If no clauses match this key, we're done; we don't want to
		 * look at keys to its right.
		 */
		if (clausegroup == NIL)
			break;

		clausegroup_list = nconc(clausegroup_list, clausegroup);

		indexkeys++;
		classes++;

	} while (!DoneMatchingIndexKeys(indexkeys, index));

	/* clausegroup_list holds all matched clauses ordered by indexkeys */
	if (clausegroup_list != NIL)
		return lcons(clausegroup_list, NIL);
	return NIL;
}

/*
 * group_clauses_by_ikey_for_joins
 *    Generates a list of join clauses that can be used with an index.
 *
 * This is much like group_clauses_by_indexkey(), but we consider both
 * join and restriction clauses.  For each indexkey in the index, we
 * accept both join and restriction clauses that match it (since both
 * will make useful indexquals if the index is being used to scan the
 * inner side of a join).  But there must be at least one matching
 * join clause, or we return NIL indicating that this index isn't useful
 * for joining.
 */
static List *
group_clauses_by_ikey_for_joins(RelOptInfo *rel,
								RelOptInfo *index,
								int *indexkeys,
								Oid *classes,
								List *join_cinfo_list,
								List *restr_cinfo_list)
{
	List	   *clausegroup_list = NIL;
	bool		jfound = false;

	if (join_cinfo_list == NIL || indexkeys[0] == 0)
		return NIL;

	do
	{
		int			curIndxKey = indexkeys[0];
		Oid			curClass = classes[0];
		List	   *clausegroup = NIL;
		List	   *curCinfo;

		foreach(curCinfo, join_cinfo_list)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(curCinfo);

			if (match_clause_to_indexkey(rel,
										 index,
										 curIndxKey,
										 curClass,
										 rinfo->clause,
										 true))
			{
				clausegroup = lappend(clausegroup, rinfo);
				jfound = true;
			}
		}
		foreach(curCinfo, restr_cinfo_list)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(curCinfo);

			if (match_clause_to_indexkey(rel,
										 index,
										 curIndxKey,
										 curClass,
										 rinfo->clause,
										 false))
				clausegroup = lappend(clausegroup, rinfo);
		}

		/* If no clauses match this key, we're done; we don't want to
		 * look at keys to its right.
		 */
		if (clausegroup == NIL)
			break;

		clausegroup_list = nconc(clausegroup_list, clausegroup);

		indexkeys++;
		classes++;

	} while (!DoneMatchingIndexKeys(indexkeys, index));

	/* clausegroup_list holds all matched clauses ordered by indexkeys */

	if (clausegroup_list != NIL)
	{
		/*
		 * if no join clause was matched then there ain't clauses for
		 * joins at all.
		 */
		if (!jfound)
		{
			freeList(clausegroup_list);
			return NIL;
		}
		return lcons(clausegroup_list, NIL);
	}
	return NIL;
}


/*
 * match_clause_to_indexkey()
 *    Determines whether a restriction or join clause matches
 *    a key of an index.
 *
 *	  To match, the clause must:
 *	  (1) be in the form (var op const) for a restriction clause,
 *		  or (var op var) for a join clause, where the var or one
 *		  of the vars matches the index key; and
 *	  (2) contain an operator which is in the same class as the index
 *		  operator for this key.
 *
 *	  In the restriction case, we can cope with (const op var) by commuting
 *	  the clause to (var op const), if there is a commutator operator.
 *	  XXX why do we bother to commute?  The executor doesn't care!!
 *
 *	  In the join case, later code will try to commute the clause if needed
 *	  to put the inner relation's var on the right.  We have no idea here
 *	  which relation might wind up on the inside, so we just accept
 *	  a match for either var.
 *	  XXX is this right?  We are making a list for this relation to
 *	  be an inner join relation, so if there is any commuting then
 *	  this rel must be on the right.  But again, it's not really clear
 *	  that we have to commute at all!
 *
 * 'rel' is the relation of interest.
 * 'index' is an index on 'rel'.
 * 'indexkey' is a key of 'index'.
 * 'xclass' is the corresponding operator class.
 * 'clause' is the clause to be tested.
 * 'join' is true if we are considering this clause for joins.
 *
 * Returns true if the clause can be used with this index key.
 *
 * NOTE:  returns false if clause is an or_clause; that's handled elsewhere.
 */
static bool
match_clause_to_indexkey(RelOptInfo *rel,
						 RelOptInfo *index,
						 int indexkey,
						 int xclass,
						 Expr *clause,
						 bool join)
{
	bool		isIndexable = false;
	Var		   *leftop,
			   *rightop;

	if (! is_opclause((Node *) clause))
		return false;
	leftop = get_leftop(clause);
	rightop = get_rightop(clause);
	if (! leftop || ! rightop)
		return false;

	if (!join)
	{
		/*
		 * Not considering joins, so check for clauses of the form:
		 * (var/func operator constant) and (constant operator var/func)
		 */
		Oid			restrict_op = InvalidOid;

		/*
		 * Check for standard s-argable clause
		 */
		if (IsA(rightop, Const) || IsA(rightop, Param))
		{
			restrict_op = ((Oper *) ((Expr *) clause)->oper)->opno;

			isIndexable = (op_class(restrict_op, xclass, index->relam) &&
						   match_index_to_operand(indexkey,
												  (Expr *) leftop,
												  rel,
												  index));

#ifndef IGNORE_BINARY_COMPATIBLE_INDICES

			/*
			 * Didn't find an index? Then maybe we can find another
			 * binary-compatible index instead... thomas 1998-08-14
			 */
			if (!isIndexable)
			{
				Oid			ltype = exprType((Node *) leftop);
				Oid			rtype = exprType((Node *) rightop);

				/*
				 * make sure we have two different binary-compatible
				 * types...
				 */
				if ((ltype != rtype)
					&& IS_BINARY_COMPATIBLE(ltype, rtype))
				{
					char	   *opname;
					Operator	newop;

					opname = get_opname(restrict_op);
					if (opname != NULL)
						newop = oper(opname, ltype, ltype, TRUE);
					else
						newop = NULL;

					/* actually have a different operator to try? */
					if (HeapTupleIsValid(newop) &&
						(oprid(newop) != restrict_op))
					{
						restrict_op = oprid(newop);

						isIndexable = (op_class(restrict_op, xclass, index->relam) &&
									   match_index_to_operand(indexkey,
															  (Expr *) leftop,
															  rel,
															  index));

						if (isIndexable)
							((Oper *) ((Expr *) clause)->oper)->opno = restrict_op;
					}
				}
			}
#endif
		}

		/*
		 * Must try to commute the clause to standard s-arg format.
		 */
		else if (IsA(leftop, Const) || IsA(leftop, Param))
		{
			restrict_op = get_commutator(((Oper *) ((Expr *) clause)->oper)->opno);

			isIndexable = ((restrict_op != InvalidOid) &&
						   op_class(restrict_op, xclass, index->relam) &&
						   match_index_to_operand(indexkey,
												  (Expr *) rightop,
												  rel,
												  index));

#ifndef IGNORE_BINARY_COMPATIBLE_INDICES
			if (!isIndexable)
			{
				Oid			ltype;
				Oid			rtype;

				ltype = exprType((Node *) leftop);
				rtype = exprType((Node *) rightop);

				if ((ltype != rtype)
					&& IS_BINARY_COMPATIBLE(ltype, rtype))
				{
					char	   *opname;
					Operator	newop;

					restrict_op = ((Oper *) ((Expr *) clause)->oper)->opno;

					opname = get_opname(restrict_op);
					if (opname != NULL)
						newop = oper(opname, rtype, rtype, TRUE);
					else
						newop = NULL;

					if (HeapTupleIsValid(newop) && (oprid(newop) != restrict_op))
					{
						restrict_op = get_commutator(oprid(newop));

						isIndexable = ((restrict_op != InvalidOid) &&
						   op_class(restrict_op, xclass, index->relam) &&
									   match_index_to_operand(indexkey,
															  (Expr *) rightop,
															  rel,
															  index));

						if (isIndexable)
							((Oper *) ((Expr *) clause)->oper)->opno = oprid(newop);
					}
				}
			}
#endif

			if (isIndexable)
			{

				/*
				 * In place list modification. (op const var/func) -> (op
				 * var/func const)
				 */
				CommuteClause((Node *) clause);
			}
		}
	}
	else
	{
		/*
		 * Check for an indexable scan on one of the join relations.
		 * clause is of the form (operator var/func var/func)
		 *  XXX this does not seem right.  Should check other side
		 * looks like var/func? do we really want to only consider
		 * this rel on lefthand side??
		 */
		Oid			join_op = InvalidOid;

		if (match_index_to_operand(indexkey, (Expr *) leftop,
								   rel, index))
			join_op = ((Oper *) ((Expr *) clause)->oper)->opno;
		else if (match_index_to_operand(indexkey, (Expr *) rightop,
										rel, index))
			join_op = get_commutator(((Oper *) ((Expr *) clause)->oper)->opno);

		if (join_op && op_class(join_op, xclass, index->relam) &&
			is_joinable((Node *) clause))
			isIndexable = true;
	}

	return isIndexable;
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
 *	  succeed whenever possible (assuming the predicate has been
 *	  successfully cnfify()-ed). --Nels, Jan '93
 */
static bool
pred_test(List *predicate_list, List *restrictinfo_list, List *joininfo_list)
{
	List	   *pred,
			   *items,
			   *item;

	/*
	 * Note: if Postgres tried to optimize queries by forming equivalence
	 * classes over equi-joined attributes (i.e., if it recognized that a
	 * qualification such as "where a.b=c.d and a.b=5" could make use of
	 * an index on c.d), then we could use that equivalence class info
	 * here with joininfo_list to do more complete tests for the usability
	 * of a partial index.	For now, the test only uses restriction
	 * clauses (those in restrictinfo_list). --Nels, Dec '92
	 */

	if (predicate_list == NULL)
		return true;			/* no predicate: the index is usable */
	if (restrictinfo_list == NULL)
		return false;			/* no restriction clauses: the test must
								 * fail */

	foreach(pred, predicate_list)
	{

		/*
		 * if any clause is not implied, the whole predicate is not
		 * implied
		 */
		if (and_clause(lfirst(pred)))
		{
			items = ((Expr *) lfirst(pred))->args;
			foreach(item, items)
			{
				if (!one_pred_test(lfirst(item), restrictinfo_list))
					return false;
			}
		}
		else if (!one_pred_test(lfirst(pred), restrictinfo_list))
			return false;
	}
	return true;
}


/*
 * one_pred_test
 *	  Does the "predicate inclusion test" for one conjunct of a predicate
 *	  expression.
 */
static bool
one_pred_test(Expr *predicate, List *restrictinfo_list)
{
	RestrictInfo *restrictinfo;
	List	   *item;

	Assert(predicate != NULL);
	foreach(item, restrictinfo_list)
	{
		restrictinfo = (RestrictInfo *) lfirst(item);
		/* if any clause implies the predicate, return true */
		if (one_pred_clause_expr_test(predicate, (Node *) restrictinfo->clause))
			return true;
	}
	return false;
}


/*
 * one_pred_clause_expr_test
 *	  Does the "predicate inclusion test" for a general restriction-clause
 *	  expression.
 */
static bool
one_pred_clause_expr_test(Expr *predicate, Node *clause)
{
	List	   *items,
			   *item;

	if (is_opclause(clause))
		return one_pred_clause_test(predicate, clause);
	else if (or_clause(clause))
	{
		items = ((Expr *) clause)->args;
		foreach(item, items)
		{
			/* if any OR item doesn't imply the predicate, clause doesn't */
			if (!one_pred_clause_expr_test(predicate, lfirst(item)))
				return false;
		}
		return true;
	}
	else if (and_clause(clause))
	{
		items = ((Expr *) clause)->args;
		foreach(item, items)
		{

			/*
			 * if any AND item implies the predicate, the whole clause
			 * does
			 */
			if (one_pred_clause_expr_test(predicate, lfirst(item)))
				return true;
		}
		return false;
	}
	else
	{
		/* unknown clause type never implies the predicate */
		return false;
	}
}


/*
 * one_pred_clause_test
 *	  Does the "predicate inclusion test" for one conjunct of a predicate
 *	  expression for a simple restriction clause.
 */
static bool
one_pred_clause_test(Expr *predicate, Node *clause)
{
	List	   *items,
			   *item;

	if (is_opclause((Node *) predicate))
		return clause_pred_clause_test(predicate, clause);
	else if (or_clause((Node *) predicate))
	{
		items = predicate->args;
		foreach(item, items)
		{
			/* if any item is implied, the whole predicate is implied */
			if (one_pred_clause_test(lfirst(item), clause))
				return true;
		}
		return false;
	}
	else if (and_clause((Node *) predicate))
	{
		items = predicate->args;
		foreach(item, items)
		{

			/*
			 * if any item is not implied, the whole predicate is not
			 * implied
			 */
			if (!one_pred_clause_test(lfirst(item), clause))
				return false;
		}
		return true;
	}
	else
	{
		elog(DEBUG, "Unsupported predicate type, index will not be used");
		return false;
	}
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

static StrategyNumber
BT_implic_table[BTMaxStrategyNumber][BTMaxStrategyNumber] = {
	{2, 2, 0, 0, 0},
	{1, 2, 0, 0, 0},
	{1, 2, 3, 4, 5},
	{0, 0, 0, 4, 5},
	{0, 0, 0, 4, 4}
};


/*
 * clause_pred_clause_test
 *	  Use operator class info to check whether clause implies predicate.
 *
 *	  Does the "predicate inclusion test" for a "simple clause" predicate
 *	  for a single "simple clause" restriction.  Currently, this only handles
 *	  (binary boolean) operators that are in some btree operator class.
 *	  Eventually, rtree operators could also be handled by defining an
 *	  appropriate "RT_implic_table" array.
 */
static bool
clause_pred_clause_test(Expr *predicate, Node *clause)
{
	Var		   *pred_var,
			   *clause_var;
	Const	   *pred_const,
			   *clause_const;
	Oid			pred_op,
				clause_op,
				test_op;
	Oid			opclass_id;
	StrategyNumber pred_strategy,
				clause_strategy,
				test_strategy;
	Oper	   *test_oper;
	Expr	   *test_expr;
	bool		test_result,
				isNull;
	Relation	relation;
	HeapScanDesc scan;
	HeapTuple	tuple;
	ScanKeyData entry[3];
	Form_pg_amop aform;

	pred_var = (Var *) get_leftop(predicate);
	pred_const = (Const *) get_rightop(predicate);
	clause_var = (Var *) get_leftop((Expr *) clause);
	clause_const = (Const *) get_rightop((Expr *) clause);

	/* Check the basic form; for now, only allow the simplest case */
	if (!is_opclause(clause) ||
		!IsA(clause_var, Var) ||
		clause_const == NULL ||
		!IsA(clause_const, Const) ||
		!IsA(predicate->oper, Oper) ||
		!IsA(pred_var, Var) ||
		!IsA(pred_const, Const))
		return false;

	/*
	 * The implication can't be determined unless the predicate and the
	 * clause refer to the same attribute.
	 */
	if (clause_var->varattno != pred_var->varattno)
		return false;

	/* Get the operators for the two clauses we're comparing */
	pred_op = ((Oper *) ((Expr *) predicate)->oper)->opno;
	clause_op = ((Oper *) ((Expr *) clause)->oper)->opno;


	/*
	 * 1. Find a "btree" strategy number for the pred_op
	 */
	ScanKeyEntryInitialize(&entry[0], 0,
						   Anum_pg_amop_amopid,
						   F_OIDEQ,
						   ObjectIdGetDatum(BTREE_AM_OID));

	ScanKeyEntryInitialize(&entry[1], 0,
						   Anum_pg_amop_amopopr,
						   F_OIDEQ,
						   ObjectIdGetDatum(pred_op));

	relation = heap_openr(AccessMethodOperatorRelationName);

	/*
	 * The following assumes that any given operator will only be in a
	 * single btree operator class.  This is true at least for all the
	 * pre-defined operator classes.  If it isn't true, then whichever
	 * operator class happens to be returned first for the given operator
	 * will be used to find the associated strategy numbers for the test.
	 * --Nels, Jan '93
	 */
	scan = heap_beginscan(relation, false, SnapshotNow, 2, entry);
	tuple = heap_getnext(scan, 0);
	if (!HeapTupleIsValid(tuple))
	{
		elog(DEBUG, "clause_pred_clause_test: unknown pred_op");
		return false;
	}
	aform = (Form_pg_amop) GETSTRUCT(tuple);

	/* Get the predicate operator's strategy number (1 to 5) */
	pred_strategy = (StrategyNumber) aform->amopstrategy;

	/* Remember which operator class this strategy number came from */
	opclass_id = aform->amopclaid;

	heap_endscan(scan);


	/*
	 * 2. From the same opclass, find a strategy num for the clause_op
	 */
	ScanKeyEntryInitialize(&entry[1], 0,
						   Anum_pg_amop_amopclaid,
						   F_OIDEQ,
						   ObjectIdGetDatum(opclass_id));

	ScanKeyEntryInitialize(&entry[2], 0,
						   Anum_pg_amop_amopopr,
						   F_OIDEQ,
						   ObjectIdGetDatum(clause_op));

	scan = heap_beginscan(relation, false, SnapshotNow, 3, entry);
	tuple = heap_getnext(scan, 0);
	if (!HeapTupleIsValid(tuple))
	{
		elog(DEBUG, "clause_pred_clause_test: unknown clause_op");
		return false;
	}
	aform = (Form_pg_amop) GETSTRUCT(tuple);

	/* Get the restriction clause operator's strategy number (1 to 5) */
	clause_strategy = (StrategyNumber) aform->amopstrategy;
	heap_endscan(scan);


	/*
	 * 3. Look up the "test" strategy number in the implication table
	 */

	test_strategy = BT_implic_table[clause_strategy - 1][pred_strategy - 1];
	if (test_strategy == 0)
		return false;			/* the implication cannot be determined */


	/*
	 * 4. From the same opclass, find the operator for the test strategy
	 */

	ScanKeyEntryInitialize(&entry[2], 0,
						   Anum_pg_amop_amopstrategy,
						   F_INT2EQ,
						   Int16GetDatum(test_strategy));

	scan = heap_beginscan(relation, false, SnapshotNow, 3, entry);
	tuple = heap_getnext(scan, 0);
	if (!HeapTupleIsValid(tuple))
	{
		elog(DEBUG, "clause_pred_clause_test: unknown test_op");
		return false;
	}
	aform = (Form_pg_amop) GETSTRUCT(tuple);

	/* Get the test operator */
	test_op = aform->amopopr;
	heap_endscan(scan);


	/*
	 * 5. Evaluate the test
	 */
	test_oper = makeOper(test_op,		/* opno */
						 InvalidOid,	/* opid */
						 BOOLOID,		/* opresulttype */
						 0,		/* opsize */
						 NULL); /* op_fcache */
	replace_opid(test_oper);

	test_expr = make_opclause(test_oper,
							  copyObject(clause_const),
							  copyObject(pred_const));

#ifndef OMIT_PARTIAL_INDEX
	test_result = ExecEvalExpr((Node *) test_expr, NULL, &isNull, NULL);
#endif	 /* OMIT_PARTIAL_INDEX */
	if (isNull)
	{
		elog(DEBUG, "clause_pred_clause_test: null test result");
		return false;
	}
	return test_result;
}


/****************************************************************************
 *				----  ROUTINES TO CHECK JOIN CLAUSES  ----
 ****************************************************************************/

/*
 * indexable_joinclauses
 *	  Finds all groups of join clauses from among 'joininfo_list' that can
 *	  be used in conjunction with 'index'.
 *
 *	  Each clause group comes from a single joininfo node plus the current
 *	  rel's restrictinfo list.  Therefore, every clause in the group references
 *	  the current rel plus the same set of other rels (except for the restrict
 *	  clauses, which only reference the current rel).  Therefore, this set
 *    of clauses could be used as an indexqual if the relation is scanned
 *	  as the inner side of a nestloop join when the outer side contains
 *	  (at least) all those "other rels".
 *
 *	  XXX Actually, given that we are considering a join that requires an
 *	  outer rel set (A,B,C), we should use all qual clauses that reference
 *	  any subset of these rels, not just the full set or none.  This is
 *	  doable with a doubly nested loop over joininfo_list; is it worth it?
 *
 * Returns two parallel lists of the same length: the clause groups,
 * and the required outer rel set for each one.
 *
 * 'rel' is the relation for which 'index' is defined
 * 'joininfo_list' is the list of JoinInfo nodes for 'rel'
 * 'restrictinfo_list' is the list of restriction clauses for 'rel'
 * '*clausegroups' receives a list of clause sublists
 * '*outerrelids' receives a list of relid lists
 */
static void
indexable_joinclauses(RelOptInfo *rel, RelOptInfo *index,
					  List *joininfo_list, List *restrictinfo_list,
					  List **clausegroups, List **outerrelids)
{
	List	   *cg_list = NIL;
	List	   *relid_list = NIL;
	List	   *i;

	foreach(i, joininfo_list)
	{
		JoinInfo   *joininfo = (JoinInfo *) lfirst(i);
		List	   *clausegroups;

		if (joininfo->jinfo_restrictinfo == NIL)
			continue;
		clausegroups = group_clauses_by_ikey_for_joins(rel,
													   index,
													   index->indexkeys,
													   index->classlist,
											joininfo->jinfo_restrictinfo,
													   restrictinfo_list);

		/*----------
		 * This code knows that group_clauses_by_ikey_for_joins() returns
		 * either NIL or a list containing a single sublist of clauses.  
		 * The line
		 *		cg_list = nconc(cg_list, clausegroups);
		 * is better read as
		 *		cg_list = lappend(cg_list, lfirst(clausegroups));
		 * That is, we are appending the only sublist returned by
		 * group_clauses_by_ikey_for_joins() to the list of clause sublists
		 * that this routine will return.  By using nconc() we recycle
		 * a cons cell that would be wasted ... whoever wrote this code
		 * was too clever by half...
		 *----------
		 */
		if (clausegroups != NIL)
		{
			cg_list = nconc(cg_list, clausegroups);
			relid_list = lappend(relid_list, joininfo->unjoined_relids);
		}
	}

	/* Make sure above clever code didn't screw up */
	Assert(length(cg_list) == length(relid_list));

	*clausegroups = cg_list;
	*outerrelids = relid_list;
}

/****************************************************************************
 *				----  PATH CREATION UTILITIES  ----
 ****************************************************************************/

/*
 * index_innerjoin
 *	  Creates index path nodes corresponding to paths to be used as inner
 *	  relations in nestloop joins.
 *
 * 'rel' is the relation for which 'index' is defined
 * 'clausegroup_list' is a list of lists of restrictinfo nodes which can use
 * 'index' on their inner relation.
 * 'outerrelids_list' is a list of the required outer rels for each group
 * of join clauses.
 *
 * Returns a list of index pathnodes.
 */
static List *
index_innerjoin(Query *root, RelOptInfo *rel, RelOptInfo *index,
				List *clausegroup_list, List *outerrelids_list)
{
	List	   *path_list = NIL;
	List	   *i;

	foreach(i, clausegroup_list)
	{
		List	   *clausegroup = lfirst(i);
		IndexPath  *pathnode = makeNode(IndexPath);
		List	   *indexquals;
		float		npages;
		float		selec;

		indexquals = get_actual_clauses(clausegroup);

		index_selectivity(root,
						  lfirsti(rel->relids),
						  lfirsti(index->relids),
						  indexquals,
						  &npages,
						  &selec);

		/* XXX this code ought to be merged with create_index_path */

		pathnode->path.pathtype = T_IndexScan;
		pathnode->path.parent = rel;
		pathnode->path.pathorder = makeNode(PathOrder);
		pathnode->path.pathorder->ordtype = SORTOP_ORDER;
		pathnode->path.pathorder->ord.sortop = index->ordering;
		pathnode->path.pathkeys = NIL;

		/* Note that we are making a pathnode for a single-scan indexscan;
		 * therefore, both indexid and indexqual should be single-element
		 * lists.
		 */
		pathnode->indexid = index->relids;
		pathnode->indexkeys = index->indexkeys;
		pathnode->indexqual = lcons(indexquals, NIL);

		/* joinid saves the rels needed on the outer side of the join */
		pathnode->path.joinid = lfirst(outerrelids_list);

		pathnode->path.path_cost = cost_index((Oid) lfirsti(index->relids),
											  (int) npages,
											  selec,
											  rel->pages,
											  rel->tuples,
											  index->pages,
											  index->tuples,
											  true);

		/*
		 * copy restrictinfo list into path for expensive function
		 * processing -- JMH, 7/7/92
		 */
		pathnode->path.loc_restrictinfo = set_difference(copyObject((Node *) rel->restrictinfo),
														 clausegroup);

#ifdef NOT_USED					/* fix xfunc */
		/* add in cost for expensive functions!  -- JMH, 7/7/92 */
		if (XfuncMode != XFUNC_OFF)
			((Path *) pathnode)->path_cost += xfunc_get_path_cost((Path *) pathnode);
#endif
		path_list = lappend(path_list, pathnode);
		outerrelids_list = lnext(outerrelids_list);
	}
	return path_list;
}

/*
 * create_index_path_group
 *	  Creates a list of index path nodes for each group of clauses
 *	  (restriction or join) that can be used in conjunction with an index.
 *
 * 'rel' is the relation for which 'index' is defined
 * 'clausegroup_list' is the list of clause groups (lists of restrictinfo
 *				nodes) grouped by mergejoinorder
 * 'join' is a flag indicating whether or not the clauses are join
 *				clauses
 *
 * Returns a list of new index path nodes.
 *
 */
static List *
create_index_path_group(Query *root,
						RelOptInfo *rel,
						RelOptInfo *index,
						List *clausegroup_list,
						bool join)
{
	List	   *path_list = NIL;
	List	   *i;

	foreach(i, clausegroup_list)
	{
		List	   *clausegroup = lfirst(i);
		bool		usable = true;

		if (join)
		{
			List	   *j;

			foreach(j, clausegroup)
			{
				RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(j);
				if (!(is_joinable((Node *) restrictinfo->clause) &&
					  equal_path_merge_ordering(index->ordering,
												restrictinfo->mergejoinorder)))
				{
					usable = false;
					break;
				}
			}
		}

		if (usable)
		{
			path_list = lappend(path_list,
								create_index_path(root, rel, index,
												  clausegroup, join));
		}
	}
	return path_list;
}

/****************************************************************************
 *				----  ROUTINES TO CHECK OPERANDS  ----
 ****************************************************************************/

/*
 * match_index_to_operand()
 *	  Generalized test for a match between an index's key
 *	  and the operand on one side of a restriction or join clause.
 *    Now check for functional indices as well.
 */
static bool
match_index_to_operand(int indexkey,
					   Expr *operand,
					   RelOptInfo *rel,
					   RelOptInfo *index)
{
	if (index->indproc == InvalidOid)
	{
		/*
		 * Normal index.
		 */
		return match_indexkey_operand(indexkey, (Var *) operand, rel);
	}

	/*
	 * functional index check
	 */
	return function_index_operand(operand, rel, index);
}

static bool
function_index_operand(Expr *funcOpnd, RelOptInfo *rel, RelOptInfo *index)
{
	Oid			heapRelid = (Oid) lfirsti(rel->relids);
	Func	   *function;
	List	   *funcargs;
	int		   *indexKeys = index->indexkeys;
	List	   *arg;
	int			i;

	/*
	 * sanity check, make sure we know what we're dealing with here.
	 */
	if (funcOpnd == NULL ||
		nodeTag(funcOpnd) != T_Expr || funcOpnd->opType != FUNC_EXPR ||
		funcOpnd->oper == NULL || indexKeys == NULL)
		return false;

	function = (Func *) funcOpnd->oper;
	funcargs = funcOpnd->args;

	if (function->funcid != index->indproc)
		return false;

	/*
	 * Check that the arguments correspond to the same arguments used to
	 * create the functional index.  To do this we must check that 1.
	 * refer to the right relatiion. 2. the args have the right attr.
	 * numbers in the right order.
	 *
	 * Check all args refer to the correct relation (i.e. the one with the
	 * functional index defined on it (rel).  To do this we can simply
	 * compare range table entry numbers, they must be the same.
	 */
	foreach(arg, funcargs)
	{
		if (heapRelid != ((Var *) lfirst(arg))->varno)
			return false;
	}

	/*
	 * check attr numbers and order.
	 */
	i = 0;
	foreach(arg, funcargs)
	{
		if (indexKeys[i] == 0)
			return false;

		if (((Var *) lfirst(arg))->varattno != indexKeys[i])
			return false;

		i++;
	}

	return true;
}
