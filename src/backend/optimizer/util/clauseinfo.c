/*-------------------------------------------------------------------------
 *
 * restrictinfo.c--
 *	  RestrictInfo node manipulation routines.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/util/Attic/clauseinfo.c,v 1.11 1999/02/03 21:16:50 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/relation.h"
#include "nodes/nodeFuncs.h"

#include "optimizer/internal.h"
#include "optimizer/clauses.h"
#include "optimizer/restrictinfo.h"

/*
 * valid-or-clause--
 *
 * Returns t iff the restrictinfo node contains a 'normal' 'or' clause.
 *
 */
bool
valid_or_clause(RestrictInfo * restrictinfo)
{
	if (restrictinfo != NULL &&
		!single_node((Node *) restrictinfo->clause) &&
		!restrictinfo->notclause &&
		or_clause((Node *) restrictinfo->clause))
		return true;
	else
		return false;
}

/*
 * get-actual-clauses--
 *
 * Returns a list containing the clauses from 'restrictinfo-list'.
 *
 */
List *
get_actual_clauses(List *restrictinfo_list)
{
	List	   *temp = NIL;
	List	   *result = NIL;
	RestrictInfo *clause = (RestrictInfo *) NULL;

	foreach(temp, restrictinfo_list)
	{
		clause = (RestrictInfo *) lfirst(temp);
		result = lappend(result, clause->clause);
	}
	return result;
}

/*
 * XXX NOTE:
 *		The following routines must return their contents in the same order
 *		(e.g., the first clause's info should be first, and so on) or else
 *		get_index_sel() won't work.
 *
 */

/*
 * get_relattvals--
 *	  For each member of  a list of restrictinfo nodes to be used with an
 *	  index, create a vectori-long specifying:
 *				the attnos,
 *				the values of the clause constants, and
 *				flags indicating the type and location of the constant within
 *						each clause.
 *	  Each clause is of the form (op var some_type_of_constant), thus the
 *	  flag indicating whether the constant is on the left or right should
 *	  always be *SELEC-CONSTANT-RIGHT*.
 *
 * 'restrictinfo-list' is a list of restrictinfo nodes
 *
 * Returns a list of vectori-longs.
 *
 */
void
get_relattvals(List *restrictinfo_list,
			   List **attnos,
			   List **values,
			   List **flags)
{
	List	   *result1 = NIL;
	List	   *result2 = NIL;
	List	   *result3 = NIL;
	RestrictInfo *temp = (RestrictInfo *) NULL;
	List	   *i = NIL;

	foreach(i, restrictinfo_list)
	{
		int			dummy;
		AttrNumber	attno;
		Datum		constval;
		int			flag;

		temp = (RestrictInfo *) lfirst(i);
		get_relattval((Node *) temp->clause, &dummy, &attno, &constval, &flag);
		result1 = lappendi(result1, (int) attno);
		result2 = lappendi(result2, constval);
		result3 = lappendi(result3, flag);
	}

	*attnos = result1;
	*values = result2;
	*flags = result3;
	return;
}

/*
 * get_joinvars --
 *	  Given a list of join restrictinfo nodes to be used with the index
 *	  of an inner join relation, return three lists consisting of:
 *				the attributes corresponding to the inner join relation
 *				the value of the inner var clause (always "")
 *				whether the attribute appears on the left or right side of
 *						the operator.
 *
 * 'relid' is the inner join relation
 * 'restrictinfo-list' is a list of qualification clauses to be used with
 *		'rel'
 *
 */
void
get_joinvars(Oid relid,
			 List *restrictinfo_list,
			 List **attnos,
			 List **values,
			 List **flags)
{
	List	   *result1 = NIL;
	List	   *result2 = NIL;
	List	   *result3 = NIL;
	List	   *temp;

	foreach(temp, restrictinfo_list)
	{
		RestrictInfo *restrictinfo = lfirst(temp);
		Expr	   *clause = restrictinfo->clause;

		if (IsA(get_leftop(clause), Var) &&
			(relid == (get_leftop(clause))->varno))
		{
			result1 = lappendi(result1, (int4) (get_leftop(clause))->varattno);
			result2 = lappend(result2, "");
			result3 = lappendi(result3, _SELEC_CONSTANT_RIGHT_);
		}
		else
		{
			result1 = lappendi(result1, (int4) (get_rightop(clause))->varattno);
			result2 = lappend(result2, "");
			result3 = lappendi(result3, _SELEC_CONSTANT_LEFT_);
		}
	}
	*attnos = result1;
	*values = result2;
	*flags = result3;
	return;
}

/*
 * get_opnos--
 *	  Create and return a list containing the clause operators of each member
 *	  of a list of restrictinfo nodes to be used with an index.
 *
 */
List *
get_opnos(List *restrictinfo_list)
{
	RestrictInfo *temp = (RestrictInfo *) NULL;
	List	   *result = NIL;
	List	   *i = NIL;

	foreach(i, restrictinfo_list)
	{
		temp = (RestrictInfo *) lfirst(i);
		result = lappendi(result,
					 (((Oper *) temp->clause->oper)->opno));
	}
	return result;
}
