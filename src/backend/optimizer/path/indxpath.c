/*-------------------------------------------------------------------------
 *
 * indxpath.c--
 *	  Routines to determine which indices are usable for scanning a
 *	  given relation
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/path/indxpath.c,v 1.17 1998/07/18 04:22:31 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <math.h>

#include "postgres.h"

#include "access/attnum.h"
#include "access/heapam.h"
#include "access/nbtree.h"
#include "catalog/catname.h"
#include "catalog/pg_amop.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pg_list.h"
#include "nodes/relation.h"
#include "optimizer/clauses.h"
#include "optimizer/clauseinfo.h"
#include "optimizer/cost.h"
#include "optimizer/internal.h"
#include "optimizer/keys.h"
#include "optimizer/ordering.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "optimizer/pathnode.h"
#include "optimizer/xfunc.h"
#include "parser/parsetree.h"	/* for getrelid() */
#include "utils/lsyscache.h"


static void
match_index_orclauses(RelOptInfo *rel, RelOptInfo *index, int indexkey,
					  int xclass, List *clauseinfo_list);
static bool
match_index_to_operand(int indexkey, Expr *operand,
					   RelOptInfo *rel, RelOptInfo *index);
static List *
match_index_orclause(RelOptInfo *rel, RelOptInfo *index, int indexkey,
			 int xclass, List *or_clauses, List *other_matching_indices);
static List *
group_clauses_by_indexkey(RelOptInfo *rel, RelOptInfo *index,
					int *indexkeys, Oid *classes, List *clauseinfo_list);
static List *
group_clauses_by_ikey_for_joins(RelOptInfo *rel, RelOptInfo *index,
								int *indexkeys, Oid *classes, List *join_cinfo_list, List *restr_cinfo_list);
static CInfo *
match_clause_to_indexkey(RelOptInfo *rel, RelOptInfo *index, int indexkey,
						 int xclass, CInfo *clauseInfo, bool join);
static bool
pred_test(List *predicate_list, List *clauseinfo_list,
		  List *joininfo_list);
static bool one_pred_test(Expr *predicate, List *clauseinfo_list);
static bool one_pred_clause_expr_test(Expr *predicate, Node *clause);
static bool one_pred_clause_test(Expr *predicate, Node *clause);
static bool clause_pred_clause_test(Expr *predicate, Node *clause);
static List *
indexable_joinclauses(RelOptInfo *rel, RelOptInfo *index,
					  List *joininfo_list, List *clauseinfo_list);
static List *
index_innerjoin(Query *root, RelOptInfo *rel,
				List *clausegroup_list, RelOptInfo *index);
static List *
create_index_paths(Query *root, RelOptInfo *rel, RelOptInfo *index,
				   List *clausegroup_list, bool join);
static List *add_index_paths(List *indexpaths, List *new_indexpaths);
static bool function_index_operand(Expr *funcOpnd, RelOptInfo *rel, RelOptInfo *index);
static bool SingleAttributeIndex(RelOptInfo *index);

/* If Spyros can use a constant PRS2_BOOL_TYPEID, I can use this */
#define BOOL_TYPEID ((Oid) 16)

/*
 * find-index-paths--
 *	  Finds all possible index paths by determining which indices in the
 *	  list 'indices' are usable.
 *
 *	  To be usable, an index must match against either a set of
 *	  restriction clauses or join clauses.
 *
 *	  Note that the current implementation requires that there exist
 *	  matching clauses for every key in the index (i.e., no partial
 *	  matches are allowed).
 *
 *	  If an index can't be used with restriction clauses, but its keys
 *	  match those of the result sort order (according to information stored
 *	  within 'sortkeys'), then the index is also considered.
 *
 * 'rel' is the relation entry to which these index paths correspond
 * 'indices' is a list of possible index paths
 * 'clauseinfo-list' is a list of restriction clauseinfo nodes for 'rel'
 * 'joininfo-list' is a list of joininfo nodes for 'rel'
 * 'sortkeys' is a node describing the result sort order (from
 *		(find_sortkeys))
 *
 * Returns a list of index nodes.
 *
 */
List *
find_index_paths(Query *root,
				 RelOptInfo *rel,
				 List *indices,
				 List *clauseinfo_list,
				 List *joininfo_list)
{
	List	   *scanclausegroups = NIL;
	List	   *scanpaths = NIL;
	RelOptInfo		   *index = (RelOptInfo *) NULL;
	List	   *joinclausegroups = NIL;
	List	   *joinpaths = NIL;
	List	   *retval = NIL;

	if (indices == NIL)
		return (NULL);

	index = (RelOptInfo *) lfirst(indices);

	retval = find_index_paths(root,
							  rel,
							  lnext(indices),
							  clauseinfo_list,
							  joininfo_list);

	/* If this is a partial index, return if it fails the predicate test */
	if (index->indpred != NIL)
		if (!pred_test(index->indpred, clauseinfo_list, joininfo_list))
			return retval;

	/*
	 * 1. If this index has only one key, try matching it against
	 * subclauses of an 'or' clause.  The fields of the clauseinfo nodes
	 * are marked with lists of the matching indices no path are actually
	 * created.
	 *
	 * XXX NOTE:  Currently btrees dos not support indices with > 1 key, so
	 * the following test will always be true for now but we have decided
	 * not to support index-scans on disjunction . -- lp
	 */
	if (SingleAttributeIndex(index))
	{
		match_index_orclauses(rel,
							  index,
							  index->indexkeys[0],
							  index->classlist[0],
							  clauseinfo_list);
	}

	/*
	 * 2. If the keys of this index match any of the available restriction
	 * clauses, then create pathnodes corresponding to each group of
	 * usable clauses.
	 */
	scanclausegroups = group_clauses_by_indexkey(rel,
												 index,
												 index->indexkeys,
												 index->classlist,
												 clauseinfo_list);

	scanpaths = NIL;
	if (scanclausegroups != NIL)
		scanpaths = create_index_paths(root,
									   rel,
									   index,
									   scanclausegroups,
									   false);

	/*
	 * 3. If this index can be used with any join clause, then create
	 * pathnodes for each group of usable clauses.	An index can be used
	 * with a join clause if its ordering is useful for a mergejoin, or if
	 * the index can possibly be used for scanning the inner relation of a
	 * nestloop join.
	 */
	joinclausegroups = indexable_joinclauses(rel, index, joininfo_list, clauseinfo_list);
	joinpaths = NIL;

	if (joinclausegroups != NIL)
	{
		List	   *new_join_paths = create_index_paths(root, rel,
														index,
														joinclausegroups,
														true);
		List	   *innerjoin_paths = index_innerjoin(root, rel, joinclausegroups, index);

		rel->innerjoin = nconc(rel->innerjoin, innerjoin_paths);
		joinpaths = new_join_paths;
	}

	/*
	 * Some sanity checks to make sure that the indexpath is valid.
	 */
	if (joinpaths != NULL)
		retval = add_index_paths(joinpaths, retval);
	if (scanpaths != NULL)
		retval = add_index_paths(scanpaths, retval);

	return retval;

}


/****************************************************************************
 *		----  ROUTINES TO MATCH 'OR' CLAUSES  ----
 ****************************************************************************/


/*
 * match-index-orclauses--
 *	  Attempt to match an index against subclauses within 'or' clauses.
 *	  If the index does match, then the clause is marked with information
 *	  about the index.
 *
 *	  Essentially, this adds 'index' to the list of indices in the
 *	  ClauseInfo field of each of the clauses which it matches.
 *
 * 'rel' is the node of the relation on which the index is defined.
 * 'index' is the index node.
 * 'indexkey' is the (single) key of the index
 * 'class' is the class of the operator corresponding to 'indexkey'.
 * 'clauseinfo-list' is the list of available restriction clauses.
 *
 * Returns nothing.
 *
 */
static void
match_index_orclauses(RelOptInfo *rel,
					  RelOptInfo *index,
					  int indexkey,
					  int xclass,
					  List *clauseinfo_list)
{
	CInfo	   *clauseinfo = (CInfo *) NULL;
	List	   *i = NIL;

	foreach(i, clauseinfo_list)
	{
		clauseinfo = (CInfo *) lfirst(i);
		if (valid_or_clause(clauseinfo))
		{

			/*
			 * Mark the 'or' clause with a list of indices which match
			 * each of its subclauses.	The list is generated by adding
			 * 'index' to the existing list where appropriate.
			 */
			clauseinfo->indexids =
				match_index_orclause(rel, index, indexkey,
									 xclass,
									 clauseinfo->clause->args,
									 clauseinfo->indexids);
		}
	}
}

/*
 * match_index_operand--
 *	  Generalize test for a match between an existing index's key
 *	  and the operand on the rhs of a restriction clause.  Now check
 *	  for functional indices as well.
 */
static bool
match_index_to_operand(int indexkey,
					   Expr *operand,
					   RelOptInfo *rel,
					   RelOptInfo *index)
{

	/*
	 * Normal index.
	 */
	if (index->indproc == InvalidOid)
		return match_indexkey_operand(indexkey, (Var *) operand, rel);

	/*
	 * functional index check
	 */
	return (function_index_operand(operand, rel, index));
}

/*
 * match-index-orclause--
 *	  Attempts to match an index against the subclauses of an 'or' clause.
 *
 *	  A match means that:
 *	  (1) the operator within the subclause can be used with one
 *				of the index's operator classes, and
 *	  (2) there is a usable key that matches the variable within a
 *				sargable clause.
 *
 * 'or-clauses' are the remaining subclauses within the 'or' clause
 * 'other-matching-indices' is the list of information on other indices
 *		that have already been matched to subclauses within this
 *		particular 'or' clause (i.e., a list previously generated by
 *		this routine)
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
	Node	   *clause = NULL;
	List	   *matched_indices = other_matching_indices;
	List	   *index_list = NIL;
	List	   *clist;
	List	   *ind;

	if (!matched_indices)
		matched_indices = lcons(NIL, NIL);

	for (clist = or_clauses, ind = matched_indices;
		 clist;
		 clist = lnext(clist), ind = lnext(ind))
	{
		clause = lfirst(clist);
		if (is_opclause(clause) &&
			op_class(((Oper *) ((Expr *) clause)->oper)->opno,
					 xclass, index->relam) &&
			match_index_to_operand(indexkey,
								   (Expr *) get_leftop((Expr *) clause),
								   rel,
								   index) &&
			IsA(get_rightop((Expr *) clause), Const))
		{

			matched_indices = lcons(index, matched_indices);
			index_list = lappend(index_list,
								 matched_indices);
		}
	}
	return (index_list);

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
 * group-clauses-by-indexkey--
 *	  Determines whether there are clauses which will match each and every
 *	  one of the remaining keys of an index.
 *
 * 'rel' is the node of the relation corresponding to the index.
 * 'indexkeys' are the remaining index keys to be matched.
 * 'classes' are the classes of the index operators on those keys.
 * 'clauses' is either:
 *		(1) the list of available restriction clauses on a single
 *				relation, or
 *		(2) a list of join clauses between 'rel' and a fixed set of
 *				relations,
 *		depending on the value of 'join'.
 *
 *		NOTE: it works now for restriction clauses only. - vadim 03/18/97
 *
 * Returns all possible groups of clauses that will match (given that
 * one or more clauses can match any of the remaining keys).
 * E.g., if you have clauses A, B, and C, ((A B) (A C)) might be
 * returned for an index with 2 keys.
 *
 */
static List *
group_clauses_by_indexkey(RelOptInfo *rel,
						  RelOptInfo *index,
						  int *indexkeys,
						  Oid *classes,
						  List *clauseinfo_list)
{
	List	   *curCinfo = NIL;
	CInfo	   *matched_clause = (CInfo *) NULL;
	List	   *clausegroup = NIL;
	int			curIndxKey;
	Oid			curClass;

	if (clauseinfo_list == NIL)
		return NIL;

	while (!DoneMatchingIndexKeys(indexkeys, index))
	{
		List	   *tempgroup = NIL;

		curIndxKey = indexkeys[0];
		curClass = classes[0];

		foreach(curCinfo, clauseinfo_list)
		{
			CInfo	   *temp = (CInfo *) lfirst(curCinfo);

			matched_clause = match_clause_to_indexkey(rel,
													  index,
													  curIndxKey,
													  curClass,
													  temp,
													  false);
			if (!matched_clause)
				continue;

			tempgroup = lappend(tempgroup, matched_clause);
		}
		if (tempgroup == NIL)
			break;

		clausegroup = nconc(clausegroup, tempgroup);

		indexkeys++;
		classes++;

	}

	/* clausegroup holds all matched clauses ordered by indexkeys */

	if (clausegroup != NIL)
		return (lcons(clausegroup, NIL));
	return NIL;
}

/*
 * group-clauses-by-ikey-for-joins--
 *	  special edition of group-clauses-by-indexkey - will
 *	  match join & restriction clauses. See comment in indexable_joinclauses.
 *		- vadim 03/18/97
 *
 */
static List *
group_clauses_by_ikey_for_joins(RelOptInfo *rel,
								RelOptInfo *index,
								int *indexkeys,
								Oid *classes,
								List *join_cinfo_list,
								List *restr_cinfo_list)
{
	List	   *curCinfo = NIL;
	CInfo	   *matched_clause = (CInfo *) NULL;
	List	   *clausegroup = NIL;
	int			curIndxKey;
	Oid			curClass;
	bool		jfound = false;

	if (join_cinfo_list == NIL)
		return NIL;

	while (!DoneMatchingIndexKeys(indexkeys, index))
	{
		List	   *tempgroup = NIL;

		curIndxKey = indexkeys[0];
		curClass = classes[0];

		foreach(curCinfo, join_cinfo_list)
		{
			CInfo	   *temp = (CInfo *) lfirst(curCinfo);

			matched_clause = match_clause_to_indexkey(rel,
													  index,
													  curIndxKey,
													  curClass,
													  temp,
													  true);
			if (!matched_clause)
				continue;

			tempgroup = lappend(tempgroup, matched_clause);
			jfound = true;
		}
		foreach(curCinfo, restr_cinfo_list)
		{
			CInfo	   *temp = (CInfo *) lfirst(curCinfo);

			matched_clause = match_clause_to_indexkey(rel,
													  index,
													  curIndxKey,
													  curClass,
													  temp,
													  false);
			if (!matched_clause)
				continue;

			tempgroup = lappend(tempgroup, matched_clause);
		}
		if (tempgroup == NIL)
			break;

		clausegroup = nconc(clausegroup, tempgroup);

		indexkeys++;
		classes++;

	}

	/* clausegroup holds all matched clauses ordered by indexkeys */

	if (clausegroup != NIL)
	{

		/*
		 * if no one join clause was matched then there ain't clauses for
		 * joins at all.
		 */
		if (!jfound)
		{
			freeList(clausegroup);
			return NIL;
		}
		return (lcons(clausegroup, NIL));
	}
	return NIL;
}

/*
 * IndexScanableClause ()  MACRO
 *
 * Generalize condition on which we match a clause with an index.
 * Now we can match with functional indices.
 */
#define IndexScanableOperand(opnd, indkeys, rel, index) \
	((index->indproc == InvalidOid) ? \
		match_indexkey_operand(indkeys, opnd, rel) : \
		function_index_operand((Expr*)opnd,rel,index))

/*
 * There was
 *		equal_indexkey_var(indkeys,opnd) : \
 * above, and now
 *		match_indexkey_operand(indkeys, opnd, rel) : \
 * - vadim 01/22/97
 */

/*
 * match_clause_to-indexkey--
 *	  Finds the first of a relation's available restriction clauses that
 *	  matches a key of an index.
 *
 *	  To match, the clause must:
 *	  (1) be in the form (op var const) if the clause is a single-
 *				relation clause, and
 *	  (2) contain an operator which is in the same class as the index
 *				operator for this key.
 *
 *	  If the clause being matched is a join clause, then 'join' is t.
 *
 * Returns a single clauseinfo node corresponding to the matching
 * clause.
 *
 * NOTE:  returns nil if clause is an or_clause.
 *
 */
static CInfo *
match_clause_to_indexkey(RelOptInfo *rel,
						 RelOptInfo *index,
						 int indexkey,
						 int xclass,
						 CInfo *clauseInfo,
						 bool join)
{
	Expr	   *clause = clauseInfo->clause;
	Var		   *leftop,
			   *rightop;
	Oid			join_op = InvalidOid;
	Oid			restrict_op = InvalidOid;
	bool		isIndexable = false;

	if (or_clause((Node *) clause) ||
		not_clause((Node *) clause) || single_node((Node *) clause))
		return ((CInfo *) NULL);

	leftop = get_leftop(clause);
	rightop = get_rightop(clause);

	/*
	 * If this is not a join clause, check for clauses of the form:
	 * (operator var/func constant) and (operator constant var/func)
	 */
	if (!join)
	{

		/*
		 * Check for standard s-argable clause
		 */
		if ((rightop && IsA(rightop, Const)) ||
			(rightop && IsA(rightop, Param)))
		{
			restrict_op = ((Oper *) ((Expr *) clause)->oper)->opno;
			isIndexable =
				(op_class(restrict_op, xclass, index->relam) &&
				 IndexScanableOperand(leftop,
									  indexkey,
									  rel,
									  index));
		}

		/*
		 * Must try to commute the clause to standard s-arg format.
		 */
		else if ((leftop && IsA(leftop, Const)) ||
				 (leftop && IsA(leftop, Param)))
		{
			restrict_op =
				get_commutator(((Oper *) ((Expr *) clause)->oper)->opno);

			if ((restrict_op != InvalidOid) &&
				op_class(restrict_op, xclass, index->relam) &&
				IndexScanableOperand(rightop,
									 indexkey, rel, index))
			{
				isIndexable = true;

				/*
				 * In place list modification. (op const var/func) -> (op
				 * var/func const)
				 */
				CommuteClause((Node *) clause);
			}
		}
	}

	/*
	 * Check for an indexable scan on one of the join relations. clause is
	 * of the form (operator var/func var/func)
	 */
	else
	{
		if (rightop
		&& match_index_to_operand(indexkey, (Expr *) rightop, rel, index))
		{

			join_op = get_commutator(((Oper *) ((Expr *) clause)->oper)->opno);

		}
		else if (leftop
				 && match_index_to_operand(indexkey,
										   (Expr *) leftop, rel, index))
			join_op = ((Oper *) ((Expr *) clause)->oper)->opno;

		if (join_op && op_class(join_op, xclass, index->relam) &&
			join_clause_p((Node *) clause))
		{
			isIndexable = true;

			/*
			 * If we're using the operand's commutator we must commute the
			 * clause.
			 */
			if (join_op != ((Oper *) ((Expr *) clause)->oper)->opno)
				CommuteClause((Node *) clause);
		}
	}

	if (isIndexable)
		return (clauseInfo);

	return (NULL);
}

/****************************************************************************
 *				----  ROUTINES TO DO PARTIAL INDEX PREDICATE TESTS	----
 ****************************************************************************/

/*
 * pred_test--
 *	  Does the "predicate inclusion test" for partial indexes.
 *
 *	  Recursively checks whether the clauses in clauseinfo_list imply
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
pred_test(List *predicate_list, List *clauseinfo_list, List *joininfo_list)
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
	 * clauses (those in clauseinfo_list). --Nels, Dec '92
	 */

	if (predicate_list == NULL)
		return true;			/* no predicate: the index is usable */
	if (clauseinfo_list == NULL)
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
				if (!one_pred_test(lfirst(item), clauseinfo_list))
					return false;
			}
		}
		else if (!one_pred_test(lfirst(pred), clauseinfo_list))
			return false;
	}
	return true;
}


/*
 * one_pred_test--
 *	  Does the "predicate inclusion test" for one conjunct of a predicate
 *	  expression.
 */
static bool
one_pred_test(Expr *predicate, List *clauseinfo_list)
{
	CInfo	   *clauseinfo;
	List	   *item;

	Assert(predicate != NULL);
	foreach(item, clauseinfo_list)
	{
		clauseinfo = (CInfo *) lfirst(item);
		/* if any clause implies the predicate, return true */
		if (one_pred_clause_expr_test(predicate, (Node *) clauseinfo->clause))
			return true;
	}
	return false;
}


/*
 * one_pred_clause_expr_test--
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
 * one_pred_clause_test--
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

StrategyNumber BT_implic_table[BTMaxStrategyNumber][BTMaxStrategyNumber] = {
	{2, 2, 0, 0, 0},
	{1, 2, 0, 0, 0},
	{1, 2, 3, 4, 5},
	{0, 0, 0, 4, 5},
	{0, 0, 0, 4, 4}
};


/*
 * clause_pred_clause_test--
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
	Form_pg_amop form;

	pred_var = (Var *) get_leftop(predicate);
	pred_const = (Const *) get_rightop(predicate);
	clause_var = (Var *) get_leftop((Expr *) clause);
	clause_const = (Const *) get_rightop((Expr *) clause);

	/* Check the basic form; for now, only allow the simplest case */
	if (!is_opclause(clause) ||
		!IsA(clause_var, Var) ||
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
	/* XXX - hardcoded amopid value 403 to find "btree" operator classes */
	ScanKeyEntryInitialize(&entry[0], 0,
						   Anum_pg_amop_amopid,
						   F_OIDEQ,
						   ObjectIdGetDatum(403));

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
	scan = heap_beginscan(relation, false, false, 2, entry);
	tuple = heap_getnext(scan, false, (Buffer *) NULL);
	if (!HeapTupleIsValid(tuple))
	{
		elog(DEBUG, "clause_pred_clause_test: unknown pred_op");
		return false;
	}
	form = (Form_pg_amop) GETSTRUCT(tuple);

	/* Get the predicate operator's strategy number (1 to 5) */
	pred_strategy = (StrategyNumber) form->amopstrategy;

	/* Remember which operator class this strategy number came from */
	opclass_id = form->amopclaid;

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

	scan = heap_beginscan(relation, false, false, 3, entry);
	tuple = heap_getnext(scan, false, (Buffer *) NULL);
	if (!HeapTupleIsValid(tuple))
	{
		elog(DEBUG, "clause_pred_clause_test: unknown clause_op");
		return false;
	}
	form = (Form_pg_amop) GETSTRUCT(tuple);

	/* Get the restriction clause operator's strategy number (1 to 5) */
	clause_strategy = (StrategyNumber) form->amopstrategy;
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

	scan = heap_beginscan(relation, false, false, 3, entry);
	tuple = heap_getnext(scan, false, (Buffer *) NULL);
	if (!HeapTupleIsValid(tuple))
	{
		elog(DEBUG, "clause_pred_clause_test: unknown test_op");
		return false;
	}
	form = (Form_pg_amop) GETSTRUCT(tuple);

	/* Get the test operator */
	test_op = form->amopopr;
	heap_endscan(scan);


	/*
	 * 5. Evaluate the test
	 */
	test_oper = makeOper(test_op,		/* opno */
						 InvalidOid,	/* opid */
						 BOOL_TYPEID,	/* opresulttype */
						 0,		/* opsize */
						 NULL); /* op_fcache */
	replace_opid(test_oper);

	test_expr = make_opclause(test_oper,
							  copyObject(clause_const),
							  copyObject(pred_const));

#ifndef OMIT_PARTIAL_INDEX
	test_result = ExecEvalExpr((Node *) test_expr, NULL, &isNull, NULL);
#endif							/* OMIT_PARTIAL_INDEX */
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
 * indexable-joinclauses--
 *	  Finds all groups of join clauses from among 'joininfo-list' that can
 *	  be used in conjunction with 'index'.
 *
 *	  The first clause in the group is marked as having the other relation
 *	  in the join clause as its outer join relation.
 *
 * Returns a list of these clause groups.
 *
 *	  Added: clauseinfo_list - list of restriction CInfos. It's to
 *		support multi-column indices in joins and for cases
 *		when a key is in both join & restriction clauses. - vadim 03/18/97
 *
 */
static List *
indexable_joinclauses(RelOptInfo *rel, RelOptInfo *index,
					  List *joininfo_list, List *clauseinfo_list)
{
	JInfo	   *joininfo = (JInfo *) NULL;
	List	   *cg_list = NIL;
	List	   *i = NIL;
	List	   *clausegroups = NIL;

	foreach(i, joininfo_list)
	{
		joininfo = (JInfo *) lfirst(i);

		if (joininfo->jinfoclauseinfo == NIL)
			continue;
		clausegroups =
			group_clauses_by_ikey_for_joins(rel,
											index,
											index->indexkeys,
											index->classlist,
											joininfo->jinfoclauseinfo,
											clauseinfo_list);

		if (clausegroups != NIL)
		{
			List	   *clauses = lfirst(clausegroups);

			((CInfo *) lfirst(clauses))->cinfojoinid =
				joininfo->otherrels;
		}
		cg_list = nconc(cg_list, clausegroups);
	}
	return (cg_list);
}

/****************************************************************************
 *				----  PATH CREATION UTILITIES  ----
 ****************************************************************************/

/*
 * extract_restrict_clauses -
 *	  the list of clause info contains join clauses and restriction clauses.
 *	  This routine returns the restriction clauses only.
 */
#ifdef NOT_USED
static List *
extract_restrict_clauses(List *clausegroup)
{
	List	   *restrict_cls = NIL;
	List	   *l;

	foreach(l, clausegroup)
	{
		CInfo	   *cinfo = lfirst(l);

		if (!join_clause_p((Node *) cinfo->clause))
			restrict_cls = lappend(restrict_cls, cinfo);
	}
	return restrict_cls;
}

#endif

/*
 * index-innerjoin--
 *	  Creates index path nodes corresponding to paths to be used as inner
 *	  relations in nestloop joins.
 *
 * 'clausegroup-list' is a list of list of clauseinfo nodes which can use
 * 'index' on their inner relation.
 *
 * Returns a list of index pathnodes.
 *
 */
static List *
index_innerjoin(Query *root, RelOptInfo *rel, List *clausegroup_list, RelOptInfo *index)
{
	List	   *clausegroup = NIL;
	List	   *cg_list = NIL;
	List	   *i = NIL;
	IndexPath  *pathnode = (IndexPath *) NULL;
	Cost		temp_selec;
	float		temp_pages;

	foreach(i, clausegroup_list)
	{
		List	   *attnos,
				   *values,
				   *flags;

		clausegroup = lfirst(i);
		pathnode = makeNode(IndexPath);

		get_joinvars(lfirsti(rel->relids), clausegroup,
					 &attnos, &values, &flags);
		index_selectivity(lfirsti(index->relids),
						  index->classlist,
						  get_opnos(clausegroup),
						  getrelid(lfirsti(rel->relids),
								   root->rtable),
						  attnos,
						  values,
						  flags,
						  length(clausegroup),
						  &temp_pages,
						  &temp_selec);
		pathnode->path.pathtype = T_IndexScan;
		pathnode->path.parent = rel;
		pathnode->indexid = index->relids;
		pathnode->indexkeys = index->indexkeys;
		pathnode->indexqual = clausegroup;

		pathnode->path.joinid = ((CInfo *) lfirst(clausegroup))->cinfojoinid;

		pathnode->path.path_cost =
			cost_index((Oid) lfirsti(index->relids),
					   (int) temp_pages,
					   temp_selec,
					   rel->pages,
					   rel->tuples,
					   index->pages,
					   index->tuples,
					   true);

		/*
		 * copy clauseinfo list into path for expensive function
		 * processing -- JMH, 7/7/92
		 */
		pathnode->path.locclauseinfo =
			set_difference(copyObject((Node *) rel->clauseinfo),
						   clausegroup);

#if 0							/* fix xfunc */
		/* add in cost for expensive functions!  -- JMH, 7/7/92 */
		if (XfuncMode != XFUNC_OFF)
		{
			((Path *) pathnode)->path_cost +=
				xfunc_get_path_cost((Path *) pathnode);
		}
#endif
		cg_list = lappend(cg_list, pathnode);
	}
	return (cg_list);
}

/*
 * create-index-paths--
 *	  Creates a list of index path nodes for each group of clauses
 *	  (restriction or join) that can be used in conjunction with an index.
 *
 * 'rel' is the relation for which 'index' is defined
 * 'clausegroup-list' is the list of clause groups (lists of clauseinfo
 *				nodes) grouped by mergesortorder
 * 'join' is a flag indicating whether or not the clauses are join
 *				clauses
 *
 * Returns a list of new index path nodes.
 *
 */
static List *
create_index_paths(Query *root,
				   RelOptInfo *rel,
				   RelOptInfo *index,
				   List *clausegroup_list,
				   bool join)
{
	List	   *clausegroup = NIL;
	List	   *ip_list = NIL;
	List	   *i = NIL;
	List	   *j = NIL;
	IndexPath  *temp_path;

	foreach(i, clausegroup_list)
	{
		CInfo	   *clauseinfo;
		List	   *temp_node = NIL;
		bool		temp = true;

		clausegroup = lfirst(i);

		foreach(j, clausegroup)
		{
			clauseinfo = (CInfo *) lfirst(j);
			if (!(join_clause_p((Node *) clauseinfo->clause) &&
				  equal_path_merge_ordering(index->ordering,
											clauseinfo->mergesortorder)))
				temp = false;
		}

		if (!join || temp)
		{						/* restriction, ordering scan */
			temp_path = create_index_path(root, rel, index, clausegroup, join);
			temp_node =
				lcons(temp_path, NIL);
			ip_list = nconc(ip_list, temp_node);
		}
	}
	return (ip_list);
}

static List *
add_index_paths(List *indexpaths, List *new_indexpaths)
{
	return append(indexpaths, new_indexpaths);
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
			return (false);

		if (((Var *) lfirst(arg))->varattno != indexKeys[i])
			return (false);

		i++;
	}

	return true;
}

static bool
SingleAttributeIndex(RelOptInfo *index)
{

	/*
	 * return false for now as I don't know if we support index scans on
	 * disjunction and the code doesn't work
	 */
	return (false);

#if 0

	/*
	 * Non-functional indices.
	 */
	if (index->indproc == InvalidOid)
		return (index->indexkeys[0] != 0 &&
				index->indexkeys[1] == 0);

	/*
	 * We have a functional index which is a single attr index
	 */
	return true;
#endif
}
