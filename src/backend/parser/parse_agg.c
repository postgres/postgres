/*-------------------------------------------------------------------------
 *
 * parse_agg.c
 *	  handle aggregates in parser
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_agg.c,v 1.28 1999/08/21 03:48:55 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "catalog/pg_aggregate.h"
#include "optimizer/clauses.h"
#include "optimizer/tlist.h"
#include "parser/parse_agg.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

static bool contain_agg_clause(Node *clause);
static bool contain_agg_clause_walker(Node *node, void *context);
static bool exprIsAggOrGroupCol(Node *expr, List *groupClauses);
static bool exprIsAggOrGroupCol_walker(Node *node, List *groupClauses);

/*
 * contain_agg_clause
 *	  Recursively find aggref nodes within a clause.
 *
 *	  Returns true if any aggregate found.
 *
 * NOTE: we assume that the given clause has been transformed suitably for
 * parser output.  This means we can use the planner's expression_tree_walker.
 */
static bool
contain_agg_clause(Node *clause)
{
	return contain_agg_clause_walker(clause, NULL);
}

static bool
contain_agg_clause_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Aggref))
		return true;			/* abort the tree traversal and return true */
	return expression_tree_walker(node, contain_agg_clause_walker, context);
}

/*
 * exprIsAggOrGroupCol -
 *	  returns true if the expression does not contain non-group columns,
 *	  other than within the arguments of aggregate functions.
 *
 * NOTE: we assume that the given clause has been transformed suitably for
 * parser output.  This means we can use the planner's expression_tree_walker.
 *
 * NOTE: in the case of a SubLink, expression_tree_walker does not descend
 * into the subquery.  This means we will fail to detect ungrouped columns
 * that appear as outer-level variables within a subquery.  That case seems
 * unreasonably hard to handle here.  Instead, we expect the planner to check
 * for ungrouped columns after it's found all the outer-level references
 * inside the subquery and converted them into a list of parameters for the
 * subquery.
 */
static bool
exprIsAggOrGroupCol(Node *expr, List *groupClauses)
{
	/* My walker returns TRUE if it finds a subexpression that is NOT
	 * acceptable (since we can abort the recursion at that point).
	 * So, invert its result.
	 */
	return ! exprIsAggOrGroupCol_walker(expr, groupClauses);
}

static bool
exprIsAggOrGroupCol_walker(Node *node, List *groupClauses)
{
	List	   *gl;

	if (node == NULL)
		return false;
	if (IsA(node, Aggref))
		return false;			/* OK; do not examine argument of aggregate */
	if (IsA(node, Const) || IsA(node, Param))
		return false;			/* constants are always acceptable */
	/* Now check to see if expression as a whole matches any GROUP BY item.
	 * We need to do this at every recursion level so that we recognize
	 * GROUPed-BY expressions.
	 */
	foreach(gl, groupClauses)
	{
		if (equal(node, lfirst(gl)))
			return false;		/* acceptable, do not descend more */
	}
	/* If we have an ungrouped Var, we have a failure --- unless it is an
	 * outer-level Var.  In that case it's a constant as far as this query
	 * level is concerned, and we can accept it.  (If it's ungrouped as far
	 * as the upper query is concerned, that's someone else's problem...)
	 */
	if (IsA(node, Var))
	{
		if (((Var *) node)->varlevelsup == 0)
			return true;		/* found an ungrouped local variable */
		return false;			/* outer-level Var is acceptable */
	}
	/* Otherwise, recurse. */
	return expression_tree_walker(node, exprIsAggOrGroupCol_walker,
								  (void *) groupClauses);
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
	List	   *groupClauses = NIL;
	List	   *tl;

	/* This should only be called if we found aggregates or grouping */
	Assert(pstate->p_hasAggs || qry->groupClause);

	/*
	 * Aggregates must never appear in WHERE clauses. (Note this check
	 * should appear first to deliver an appropriate error message;
	 * otherwise we are likely to generate the generic "illegal use of
	 * aggregates in target list" message, which is outright misleading if
	 * the problem is in WHERE.)
	 */
	if (contain_agg_clause(qry->qual))
		elog(ERROR, "Aggregates not allowed in WHERE clause");

	/*
	 * No aggregates allowed in GROUP BY clauses, either.
	 *
	 * While we are at it, build a list of the acceptable GROUP BY expressions
	 * for use by exprIsAggOrGroupCol() (this avoids repeated scans of the
	 * targetlist within the recursive routines...)
	 */
	foreach(tl, qry->groupClause)
	{
		GroupClause *grpcl = lfirst(tl);
		Node		*expr;

		expr = get_sortgroupclause_expr(grpcl, qry->targetList);
		if (contain_agg_clause(expr))
			elog(ERROR, "Aggregates not allowed in GROUP BY clause");
		groupClauses = lcons(expr, groupClauses);
	}

	/*
	 * The target list can only contain aggregates, group columns and
	 * functions thereof.
	 */
	foreach(tl, qry->targetList)
	{
		TargetEntry *tle = lfirst(tl);

		if (!exprIsAggOrGroupCol(tle->expr, groupClauses))
			elog(ERROR,
				 "Illegal use of aggregates or non-group column in target list");
	}

	/*
	 * The expression specified in the HAVING clause has the same
	 * restriction as those in the target list.
	 */
	if (!exprIsAggOrGroupCol(qry->havingQual, groupClauses))
		elog(ERROR,
			 "Illegal use of aggregates or non-group column in HAVING clause");

	/* Release the list storage (but not the pointed-to expressions!) */
	freeList(groupClauses);
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
			&& (!IS_BINARY_COMPATIBLE(basetype, vartype)))
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
	aggref->usenulls = usenulls;

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
