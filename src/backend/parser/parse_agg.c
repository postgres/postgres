/*-------------------------------------------------------------------------
 *
 * parse_agg.c
 *	  handle aggregates in parser
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_agg.c,v 1.20 1999/05/23 21:41:14 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "postgres.h"
#include "access/heapam.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_type.h"
#include "nodes/nodeFuncs.h"
#include "nodes/primnodes.h"
#include "nodes/relation.h"
#include "optimizer/clauses.h"
#include "parser/parse_agg.h"
#include "parser/parse_expr.h"
#include "parser/parse_node.h"
#include "parser/parse_target.h"
#include "parser/parse_coerce.h"
#include "utils/syscache.h"
#include "utils/lsyscache.h"

static bool contain_agg_clause(Node *clause);
static bool exprIsAggOrGroupCol(Node *expr, List *groupClause, List *tlist);
static bool tleIsAggOrGroupCol(TargetEntry *tle, List *groupClause, 
													List *tlist);

/*
 * contain_agg_clause
 *	  Recursively find aggref nodes from a clause.
 *
 *	  Returns true if any aggregate found.
 */
static bool
contain_agg_clause(Node *clause)
{
	if (clause == NULL)
		return FALSE;
	else if (IsA(clause, Aggref))
		return TRUE;
	else if (IsA(clause, Iter))
		return contain_agg_clause(((Iter *) clause)->iterexpr);
	else if (single_node(clause))
		return FALSE;
	else if (or_clause(clause) || and_clause(clause))
	{
		List	   *temp;

		foreach(temp, ((Expr *) clause)->args)
			if (contain_agg_clause(lfirst(temp)))
			return TRUE;
		return FALSE;
	}
	else if (is_funcclause(clause))
	{
		List	   *temp;

		foreach(temp, ((Expr *) clause)->args)
			if (contain_agg_clause(lfirst(temp)))
			return TRUE;
		return FALSE;
	}
	else if (IsA(clause, ArrayRef))
	{
		List	   *temp;

		foreach(temp, ((ArrayRef *) clause)->refupperindexpr)
			if (contain_agg_clause(lfirst(temp)))
			return TRUE;
		foreach(temp, ((ArrayRef *) clause)->reflowerindexpr)
			if (contain_agg_clause(lfirst(temp)))
			return TRUE;
		if (contain_agg_clause(((ArrayRef *) clause)->refexpr))
			return TRUE;
		if (contain_agg_clause(((ArrayRef *) clause)->refassgnexpr))
			return TRUE;
		return FALSE;
	}
	else if (not_clause(clause))
		return contain_agg_clause((Node *) get_notclausearg((Expr *) clause));
	else if (is_opclause(clause))
		return (contain_agg_clause((Node *) get_leftop((Expr *) clause)) ||
			  contain_agg_clause((Node *) get_rightop((Expr *) clause)));

	return FALSE;
}

/*
 * exprIsAggOrGroupCol -
 *	  returns true if the expression does not contain non-group columns.
 */
static bool
exprIsAggOrGroupCol(Node *expr, List *groupClause, List *tlist)
{
	List	   *gl;

	if (expr == NULL || IsA(expr, Const) ||
		IsA(expr, Param) || IsA(expr, Aggref) || 
		IsA(expr, SubLink))		/* can't handle currently !!! */
		return TRUE;

	foreach(gl, groupClause)
	{
		GroupClause *grpcl = lfirst(gl);

		if (equal(expr, get_groupclause_expr(grpcl, tlist)))
			return TRUE;
	}

	if (IsA(expr, Expr))
	{
		List	   *temp;

		foreach(temp, ((Expr *) expr)->args)
			if (!exprIsAggOrGroupCol(lfirst(temp), groupClause, tlist))
			return FALSE;
		return TRUE;
	}

	return FALSE;
}

/*
 * tleIsAggOrGroupCol -
 *	  returns true if the TargetEntry is Agg or GroupCol.
 */
static bool
tleIsAggOrGroupCol(TargetEntry *tle, List *groupClause, List *tlist)
{
	Node	   *expr = tle->expr;
	List	   *gl;

	if (expr == NULL || IsA(expr, Const) ||IsA(expr, Param))
		return TRUE;

	foreach(gl, groupClause)
	{
		GroupClause *grpcl = lfirst(gl);

		if (tle->resdom->resgroupref == grpcl->tleGroupref)
		{
			if (contain_agg_clause((Node *) expr))
				elog(ERROR, "Aggregates not allowed in GROUP BY clause");
			return TRUE;
		}
	}

	if (IsA(expr, Aggref))
		return TRUE;

	if (IsA(expr, Expr))
	{
		List	   *temp;

		foreach(temp, ((Expr *) expr)->args)
			if (!exprIsAggOrGroupCol(lfirst(temp), groupClause, tlist))
			return FALSE;
		return TRUE;
	}

	return FALSE;
}

/*
 * parseCheckAggregates
 *	Check for aggregates where they shouldn't be and improper grouping.
 *
 *	Ideally this should be done earlier, but it's difficult to distinguish
 *	aggregates from plain functions at the grammar level.  So instead we
 *	check here.  This function should be called after the target list and
 *	qualifications are finalized.
 */
void
parseCheckAggregates(ParseState *pstate, Query *qry)
{
	List	   *tl;

	/* This should only be called if we found aggregates or grouping */
	Assert(pstate->p_hasAggs || qry->groupClause);

	/*
	 * Aggregates must never appear in WHERE clauses.
	 * (Note this check should appear first to deliver an appropriate
	 * error message; otherwise we are likely to generate the generic
	 * "illegal use of aggregates in target list" message, which is
	 * outright misleading if the problem is in WHERE.)
	 */
	if (contain_agg_clause(qry->qual))
		elog(ERROR, "Aggregates not allowed in WHERE clause");

	/*
	 * The target list can only contain aggregates, group columns and
	 * functions thereof.
	 */
	foreach(tl, qry->targetList)
	{
		TargetEntry *tle = lfirst(tl);

		if (!tleIsAggOrGroupCol(tle, qry->groupClause, qry->targetList))
			elog(ERROR,
				 "Illegal use of aggregates or non-group column in target list");
	}

	/*
	 * the expression specified in the HAVING clause has the same
	 * restriction as those in the target list.
	 */

	if (!exprIsAggOrGroupCol(qry->havingQual, qry->groupClause, qry->targetList))
		elog(ERROR,
			 "Illegal use of aggregates or non-group column in HAVING clause");
}


Aggref *
ParseAgg(ParseState *pstate, char *aggname, Oid basetype,
		 List *target, int precedence)
{
	Oid			fintype;
	Oid			vartype;
	Oid			xfn1;
	Form_pg_aggregate aggform;
	Aggref	   *aggref;
	HeapTuple	theAggTuple;
	bool		usenulls = false;

	theAggTuple = SearchSysCacheTuple(AGGNAME,
									  PointerGetDatum(aggname),
									  ObjectIdGetDatum(basetype),
									  0, 0);
	if (!HeapTupleIsValid(theAggTuple))
		elog(ERROR, "Aggregate %s does not exist", aggname);

	/*
	 * We do a major hack for count(*) here.
	 *
	 * Count(*) poses several problems.  First, we need a field that is
	 * guaranteed to be in the range table, and unique.  Using a constant
	 * causes the optimizer to properly remove the aggragate from any
	 * elements of the query. Using just 'oid', which can not be null, in
	 * the parser fails on:
	 *
	 * select count(*) from tab1, tab2	   -- oid is not unique select
	 * count(*) from viewtable		-- views don't have real oids
	 *
	 * So, for an aggregate with parameter '*', we use the first valid range
	 * table entry, and pick the first column from the table. We set a
	 * flag to count nulls, because we could have nulls in that column.
	 *
	 * It's an ugly job, but someone has to do it. bjm 1998/1/18
	 */

	if (nodeTag(lfirst(target)) == T_Const)
	{
		Const	   *con = (Const *) lfirst(target);

		if (con->consttype == UNKNOWNOID && VARSIZE(con->constvalue) == VARHDRSZ)
		{
			Attr	   *attr = makeNode(Attr);
			List	   *rtable,
					   *rlist;
			RangeTblEntry *first_valid_rte;

			Assert(lnext(target) == NULL);

			if (pstate->p_is_rule)
				rtable = lnext(lnext(pstate->p_rtable));
			else
				rtable = pstate->p_rtable;

			first_valid_rte = NULL;
			foreach(rlist, rtable)
			{
				RangeTblEntry *rte = lfirst(rlist);

				/* only entries on outer(non-function?) scope */
				if (!rte->inFromCl && rte != pstate->p_target_rangetblentry)
					continue;

				first_valid_rte = rte;
				break;
			}
			if (first_valid_rte == NULL)
				elog(ERROR, "Can't find column to do aggregate(*) on.");

			attr->relname = first_valid_rte->refname;
			attr->attrs = lcons(makeString(
						   get_attname(first_valid_rte->relid, 1)), NIL);

			lfirst(target) = transformExpr(pstate, (Node *) attr, precedence);
			usenulls = true;
		}
	}

	aggform = (Form_pg_aggregate) GETSTRUCT(theAggTuple);
	fintype = aggform->aggfinaltype;
	xfn1 = aggform->aggtransfn1;

	/* only aggregates with transfn1 need a base type */
	if (OidIsValid(xfn1))
	{
		basetype = aggform->aggbasetype;
		vartype = exprType(lfirst(target));
		if ((basetype != vartype)
			&& (! IS_BINARY_COMPATIBLE(basetype, vartype)))
		{
			Type		tp1,
						tp2;

			tp1 = typeidType(basetype);
			tp2 = typeidType(vartype);
			elog(ERROR, "Aggregate type mismatch"
						"\n\t%s() works on %s, not on %s",
						 aggname, typeTypeName(tp1), typeTypeName(tp2));
		}
	}

	aggref = makeNode(Aggref);
	aggref->aggname = pstrdup(aggname);
	aggref->basetype = aggform->aggbasetype;
	aggref->aggtype = fintype;

	aggref->target = lfirst(target);
	if (usenulls)
		aggref->usenulls = true;

	pstate->p_hasAggs = true;

	return aggref;
}

/*
 * Error message when aggregate lookup fails that gives details of the
 * basetype
 */
void
agg_error(char *caller, char *aggname, Oid basetypeID)
{

	/*
	 * basetypeID that is Invalid (zero) means aggregate over all types.
	 * (count)
	 */

	if (basetypeID == InvalidOid)
		elog(ERROR, "%s: aggregate '%s' for all types does not exist", caller, aggname);
	else
	{
		elog(ERROR, "%s: aggregate '%s' for '%s' does not exist", caller, aggname,
			 typeidTypeName(basetypeID));
	}
}
