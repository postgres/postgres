/*-------------------------------------------------------------------------
 *
 * parse_agg.c
 *	  handle aggregates in parser
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_agg.c,v 1.29 1999/10/07 04:23:12 tgl Exp $
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

	/* This should only be called if we found aggregates, GROUP, or HAVING */
	Assert(pstate->p_hasAggs || qry->groupClause || qry->havingQual);

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
	 * The expression specified in the HAVING clause can only contain
	 * aggregates, group columns and functions thereof.  As with WHERE,
	 * we want to point the finger at HAVING before the target list.
	 */
	if (!exprIsAggOrGroupCol(qry->havingQual, groupClauses))
		elog(ERROR,
			 "Illegal use of aggregates or non-group column in HAVING clause");

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

	/* Release the list storage (but not the pointed-to expressions!) */
	freeList(groupClauses);
}


Aggref *
ParseAgg(ParseState *pstate, char *aggname, Oid basetype,
		 List *target, int precedence)
{
	HeapTuple	theAggTuple;
	Form_pg_aggregate aggform;
	Oid			fintype;
	Oid			xfn1;
	Oid			vartype;
	Aggref	   *aggref;
	bool		usenulls = false;

	theAggTuple = SearchSysCacheTuple(AGGNAME,
									  PointerGetDatum(aggname),
									  ObjectIdGetDatum(basetype),
									  0, 0);
	if (!HeapTupleIsValid(theAggTuple))
		elog(ERROR, "Aggregate %s does not exist", aggname);

	/*
	 * There used to be a really ugly hack for count(*) here.
	 *
	 * It's gone.  Now, the grammar transforms count(*) into count(1),
	 * which does the right thing.  (It didn't use to do the right thing,
	 * because the optimizer had the wrong ideas about semantics of queries
	 * without explicit variables.  Fixed as of Oct 1999 --- tgl.)
	 *
	 * Since "1" never evaluates as null, we currently have no need of
	 * the "usenulls" flag, but it should be kept around; in fact, we should
	 * extend the pg_aggregate table to let usenulls be specified as an
	 * attribute of user-defined aggregates.
	 */

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
