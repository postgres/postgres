/*-------------------------------------------------------------------------
 *
 * analyze.c
 *	  transform the raw parse tree into a query tree
 *
 * For optimizable statements, we are careful to obtain a suitable lock on
 * each referenced table, and other modules of the backend preserve or
 * re-obtain these locks before depending on the results.  It is therefore
 * okay to do significant semantic analysis of these statements.  For
 * utility commands, no locks are obtained here (and if they were, we could
 * not be sure we'd still have them at execution).  Hence the general rule
 * for utility commands is to just dump them into a Query node untransformed.
 * DECLARE CURSOR and EXPLAIN are exceptions because they contain
 * optimizable statements.
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	$PostgreSQL: pgsql/src/backend/parser/analyze.c,v 1.402 2010/02/26 02:00:49 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/sysattr.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/var.h"
#include "parser/analyze.h"
#include "parser/parse_agg.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "parser/parse_cte.h"
#include "parser/parse_oper.h"
#include "parser/parse_param.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/rel.h"


static Query *transformDeleteStmt(ParseState *pstate, DeleteStmt *stmt);
static Query *transformInsertStmt(ParseState *pstate, InsertStmt *stmt);
static List *transformInsertRow(ParseState *pstate, List *exprlist,
				   List *stmtcols, List *icolumns, List *attrnos);
static Query *transformSelectStmt(ParseState *pstate, SelectStmt *stmt);
static Query *transformValuesClause(ParseState *pstate, SelectStmt *stmt);
static Query *transformSetOperationStmt(ParseState *pstate, SelectStmt *stmt);
static Node *transformSetOperationTree(ParseState *pstate, SelectStmt *stmt,
						  bool isTopLevel, List **colInfo);
static void determineRecursiveColTypes(ParseState *pstate,
						   Node *larg, List *lcolinfo);
static void applyColumnNames(List *dst, List *src);
static Query *transformUpdateStmt(ParseState *pstate, UpdateStmt *stmt);
static List *transformReturningList(ParseState *pstate, List *returningList);
static Query *transformDeclareCursorStmt(ParseState *pstate,
						   DeclareCursorStmt *stmt);
static Query *transformExplainStmt(ParseState *pstate,
					 ExplainStmt *stmt);
static void transformLockingClause(ParseState *pstate, Query *qry,
					   LockingClause *lc, bool pushedDown);


/*
 * parse_analyze
 *		Analyze a raw parse tree and transform it to Query form.
 *
 * Optionally, information about $n parameter types can be supplied.
 * References to $n indexes not defined by paramTypes[] are disallowed.
 *
 * The result is a Query node.  Optimizable statements require considerable
 * transformation, while utility-type statements are simply hung off
 * a dummy CMD_UTILITY Query node.
 */
Query *
parse_analyze(Node *parseTree, const char *sourceText,
			  Oid *paramTypes, int numParams)
{
	ParseState *pstate = make_parsestate(NULL);
	Query	   *query;

	Assert(sourceText != NULL); /* required as of 8.4 */

	pstate->p_sourcetext = sourceText;

	if (numParams > 0)
		parse_fixed_parameters(pstate, paramTypes, numParams);

	query = transformStmt(pstate, parseTree);

	free_parsestate(pstate);

	return query;
}

/*
 * parse_analyze_varparams
 *
 * This variant is used when it's okay to deduce information about $n
 * symbol datatypes from context.  The passed-in paramTypes[] array can
 * be modified or enlarged (via repalloc).
 */
Query *
parse_analyze_varparams(Node *parseTree, const char *sourceText,
						Oid **paramTypes, int *numParams)
{
	ParseState *pstate = make_parsestate(NULL);
	Query	   *query;

	Assert(sourceText != NULL); /* required as of 8.4 */

	pstate->p_sourcetext = sourceText;

	parse_variable_parameters(pstate, paramTypes, numParams);

	query = transformStmt(pstate, parseTree);

	/* make sure all is well with parameter types */
	check_variable_parameters(pstate, query);

	free_parsestate(pstate);

	return query;
}

/*
 * parse_sub_analyze
 *		Entry point for recursively analyzing a sub-statement.
 */
Query *
parse_sub_analyze(Node *parseTree, ParseState *parentParseState,
				  CommonTableExpr *parentCTE,
				  bool locked_from_parent)
{
	ParseState *pstate = make_parsestate(parentParseState);
	Query	   *query;

	pstate->p_parent_cte = parentCTE;
	pstate->p_locked_from_parent = locked_from_parent;

	query = transformStmt(pstate, parseTree);

	free_parsestate(pstate);

	return query;
}

/*
 * transformStmt -
 *	  transform a Parse tree into a Query tree.
 */
Query *
transformStmt(ParseState *pstate, Node *parseTree)
{
	Query	   *result;

	switch (nodeTag(parseTree))
	{
			/*
			 * Optimizable statements
			 */
		case T_InsertStmt:
			result = transformInsertStmt(pstate, (InsertStmt *) parseTree);
			break;

		case T_DeleteStmt:
			result = transformDeleteStmt(pstate, (DeleteStmt *) parseTree);
			break;

		case T_UpdateStmt:
			result = transformUpdateStmt(pstate, (UpdateStmt *) parseTree);
			break;

		case T_SelectStmt:
			{
				SelectStmt *n = (SelectStmt *) parseTree;

				if (n->valuesLists)
					result = transformValuesClause(pstate, n);
				else if (n->op == SETOP_NONE)
					result = transformSelectStmt(pstate, n);
				else
					result = transformSetOperationStmt(pstate, n);
			}
			break;

			/*
			 * Special cases
			 */
		case T_DeclareCursorStmt:
			result = transformDeclareCursorStmt(pstate,
											(DeclareCursorStmt *) parseTree);
			break;

		case T_ExplainStmt:
			result = transformExplainStmt(pstate,
										  (ExplainStmt *) parseTree);
			break;

		default:

			/*
			 * other statements don't require any transformation; just return
			 * the original parsetree with a Query node plastered on top.
			 */
			result = makeNode(Query);
			result->commandType = CMD_UTILITY;
			result->utilityStmt = (Node *) parseTree;
			break;
	}

	/* Mark as original query until we learn differently */
	result->querySource = QSRC_ORIGINAL;
	result->canSetTag = true;

	return result;
}

/*
 * analyze_requires_snapshot
 *		Returns true if a snapshot must be set before doing parse analysis
 *		on the given raw parse tree.
 *
 * Classification here should match transformStmt(); but we also have to
 * allow a NULL input (for Parse/Bind of an empty query string).
 */
bool
analyze_requires_snapshot(Node *parseTree)
{
	bool		result;

	if (parseTree == NULL)
		return false;

	switch (nodeTag(parseTree))
	{
			/*
			 * Optimizable statements
			 */
		case T_InsertStmt:
		case T_DeleteStmt:
		case T_UpdateStmt:
		case T_SelectStmt:
			result = true;
			break;

			/*
			 * Special cases
			 */
		case T_DeclareCursorStmt:
			/* yes, because it's analyzed just like SELECT */
			result = true;
			break;

		case T_ExplainStmt:
			/* yes, because we must analyze the contained statement */
			result = true;
			break;

		default:
			/* other utility statements don't have any real parse analysis */
			result = false;
			break;
	}

	return result;
}

/*
 * transformDeleteStmt -
 *	  transforms a Delete Statement
 */
static Query *
transformDeleteStmt(ParseState *pstate, DeleteStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	Node	   *qual;

	qry->commandType = CMD_DELETE;

	/* set up range table with just the result rel */
	qry->resultRelation = setTargetTable(pstate, stmt->relation,
								  interpretInhOption(stmt->relation->inhOpt),
										 true,
										 ACL_DELETE);

	qry->distinctClause = NIL;

	/*
	 * The USING clause is non-standard SQL syntax, and is equivalent in
	 * functionality to the FROM list that can be specified for UPDATE. The
	 * USING keyword is used rather than FROM because FROM is already a
	 * keyword in the DELETE syntax.
	 */
	transformFromClause(pstate, stmt->usingClause);

	qual = transformWhereClause(pstate, stmt->whereClause, "WHERE");

	qry->returningList = transformReturningList(pstate, stmt->returningList);

	/* done building the range table and jointree */
	qry->rtable = pstate->p_rtable;
	qry->jointree = makeFromExpr(pstate->p_joinlist, qual);

	qry->hasSubLinks = pstate->p_hasSubLinks;
	qry->hasAggs = pstate->p_hasAggs;
	if (pstate->p_hasAggs)
		parseCheckAggregates(pstate, qry);
	qry->hasWindowFuncs = pstate->p_hasWindowFuncs;
	if (pstate->p_hasWindowFuncs)
		parseCheckWindowFuncs(pstate, qry);

	return qry;
}

/*
 * transformInsertStmt -
 *	  transform an Insert Statement
 */
static Query *
transformInsertStmt(ParseState *pstate, InsertStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	SelectStmt *selectStmt = (SelectStmt *) stmt->selectStmt;
	List	   *exprList = NIL;
	bool		isGeneralSelect;
	List	   *sub_rtable;
	List	   *sub_relnamespace;
	List	   *sub_varnamespace;
	List	   *icolumns;
	List	   *attrnos;
	RangeTblEntry *rte;
	RangeTblRef *rtr;
	ListCell   *icols;
	ListCell   *attnos;
	ListCell   *lc;

	qry->commandType = CMD_INSERT;
	pstate->p_is_insert = true;

	/*
	 * We have three cases to deal with: DEFAULT VALUES (selectStmt == NULL),
	 * VALUES list, or general SELECT input.  We special-case VALUES, both for
	 * efficiency and so we can handle DEFAULT specifications.
	 *
	 * The grammar allows attaching ORDER BY, LIMIT, FOR UPDATE, or WITH to a
	 * VALUES clause.  If we have any of those, treat it as a general SELECT;
	 * so it will work, but you can't use DEFAULT items together with those.
	 */
	isGeneralSelect = (selectStmt && (selectStmt->valuesLists == NIL ||
									  selectStmt->sortClause != NIL ||
									  selectStmt->limitOffset != NULL ||
									  selectStmt->limitCount != NULL ||
									  selectStmt->lockingClause != NIL ||
									  selectStmt->withClause != NULL));

	/*
	 * If a non-nil rangetable/namespace was passed in, and we are doing
	 * INSERT/SELECT, arrange to pass the rangetable/namespace down to the
	 * SELECT.  This can only happen if we are inside a CREATE RULE, and in
	 * that case we want the rule's OLD and NEW rtable entries to appear as
	 * part of the SELECT's rtable, not as outer references for it.  (Kluge!)
	 * The SELECT's joinlist is not affected however.  We must do this before
	 * adding the target table to the INSERT's rtable.
	 */
	if (isGeneralSelect)
	{
		sub_rtable = pstate->p_rtable;
		pstate->p_rtable = NIL;
		sub_relnamespace = pstate->p_relnamespace;
		pstate->p_relnamespace = NIL;
		sub_varnamespace = pstate->p_varnamespace;
		pstate->p_varnamespace = NIL;
		/* There can't be any outer WITH to worry about */
		Assert(pstate->p_ctenamespace == NIL);
	}
	else
	{
		sub_rtable = NIL;		/* not used, but keep compiler quiet */
		sub_relnamespace = NIL;
		sub_varnamespace = NIL;
	}

	/*
	 * Must get write lock on INSERT target table before scanning SELECT, else
	 * we will grab the wrong kind of initial lock if the target table is also
	 * mentioned in the SELECT part.  Note that the target table is not added
	 * to the joinlist or namespace.
	 */
	qry->resultRelation = setTargetTable(pstate, stmt->relation,
										 false, false, ACL_INSERT);

	/* Validate stmt->cols list, or build default list if no list given */
	icolumns = checkInsertTargets(pstate, stmt->cols, &attrnos);
	Assert(list_length(icolumns) == list_length(attrnos));

	/*
	 * Determine which variant of INSERT we have.
	 */
	if (selectStmt == NULL)
	{
		/*
		 * We have INSERT ... DEFAULT VALUES.  We can handle this case by
		 * emitting an empty targetlist --- all columns will be defaulted when
		 * the planner expands the targetlist.
		 */
		exprList = NIL;
	}
	else if (isGeneralSelect)
	{
		/*
		 * We make the sub-pstate a child of the outer pstate so that it can
		 * see any Param definitions supplied from above.  Since the outer
		 * pstate's rtable and namespace are presently empty, there are no
		 * side-effects of exposing names the sub-SELECT shouldn't be able to
		 * see.
		 */
		ParseState *sub_pstate = make_parsestate(pstate);
		Query	   *selectQuery;

		/*
		 * Process the source SELECT.
		 *
		 * It is important that this be handled just like a standalone SELECT;
		 * otherwise the behavior of SELECT within INSERT might be different
		 * from a stand-alone SELECT. (Indeed, Postgres up through 6.5 had
		 * bugs of just that nature...)
		 */
		sub_pstate->p_rtable = sub_rtable;
		sub_pstate->p_joinexprs = NIL;	/* sub_rtable has no joins */
		sub_pstate->p_relnamespace = sub_relnamespace;
		sub_pstate->p_varnamespace = sub_varnamespace;

		selectQuery = transformStmt(sub_pstate, stmt->selectStmt);

		free_parsestate(sub_pstate);

		/* The grammar should have produced a SELECT, but it might have INTO */
		if (!IsA(selectQuery, Query) ||
			selectQuery->commandType != CMD_SELECT ||
			selectQuery->utilityStmt != NULL)
			elog(ERROR, "unexpected non-SELECT command in INSERT ... SELECT");
		if (selectQuery->intoClause)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("INSERT ... SELECT cannot specify INTO"),
					 parser_errposition(pstate,
						   exprLocation((Node *) selectQuery->intoClause))));

		/*
		 * Make the source be a subquery in the INSERT's rangetable, and add
		 * it to the INSERT's joinlist.
		 */
		rte = addRangeTableEntryForSubquery(pstate,
											selectQuery,
											makeAlias("*SELECT*", NIL),
											false);
		rtr = makeNode(RangeTblRef);
		/* assume new rte is at end */
		rtr->rtindex = list_length(pstate->p_rtable);
		Assert(rte == rt_fetch(rtr->rtindex, pstate->p_rtable));
		pstate->p_joinlist = lappend(pstate->p_joinlist, rtr);

		/*----------
		 * Generate an expression list for the INSERT that selects all the
		 * non-resjunk columns from the subquery.  (INSERT's tlist must be
		 * separate from the subquery's tlist because we may add columns,
		 * insert datatype coercions, etc.)
		 *
		 * HACK: unknown-type constants and params in the SELECT's targetlist
		 * are copied up as-is rather than being referenced as subquery
		 * outputs.  This is to ensure that when we try to coerce them to
		 * the target column's datatype, the right things happen (see
		 * special cases in coerce_type).  Otherwise, this fails:
		 *		INSERT INTO foo SELECT 'bar', ... FROM baz
		 *----------
		 */
		exprList = NIL;
		foreach(lc, selectQuery->targetList)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(lc);
			Expr	   *expr;

			if (tle->resjunk)
				continue;
			if (tle->expr &&
				(IsA(tle->expr, Const) ||IsA(tle->expr, Param)) &&
				exprType((Node *) tle->expr) == UNKNOWNOID)
				expr = tle->expr;
			else
			{
				Var		   *var = makeVar(rtr->rtindex,
										  tle->resno,
										  exprType((Node *) tle->expr),
										  exprTypmod((Node *) tle->expr),
										  0);

				var->location = exprLocation((Node *) tle->expr);
				expr = (Expr *) var;
			}
			exprList = lappend(exprList, expr);
		}

		/* Prepare row for assignment to target table */
		exprList = transformInsertRow(pstate, exprList,
									  stmt->cols,
									  icolumns, attrnos);
	}
	else if (list_length(selectStmt->valuesLists) > 1)
	{
		/*
		 * Process INSERT ... VALUES with multiple VALUES sublists. We
		 * generate a VALUES RTE holding the transformed expression lists, and
		 * build up a targetlist containing Vars that reference the VALUES
		 * RTE.
		 */
		List	   *exprsLists = NIL;
		int			sublist_length = -1;

		/* process the WITH clause */
		if (selectStmt->withClause)
		{
			qry->hasRecursive = selectStmt->withClause->recursive;
			qry->cteList = transformWithClause(pstate, selectStmt->withClause);
		}

		foreach(lc, selectStmt->valuesLists)
		{
			List	   *sublist = (List *) lfirst(lc);

			/* Do basic expression transformation (same as a ROW() expr) */
			sublist = transformExpressionList(pstate, sublist);

			/*
			 * All the sublists must be the same length, *after*
			 * transformation (which might expand '*' into multiple items).
			 * The VALUES RTE can't handle anything different.
			 */
			if (sublist_length < 0)
			{
				/* Remember post-transformation length of first sublist */
				sublist_length = list_length(sublist);
			}
			else if (sublist_length != list_length(sublist))
			{
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("VALUES lists must all be the same length"),
						 parser_errposition(pstate,
											exprLocation((Node *) sublist))));
			}

			/* Prepare row for assignment to target table */
			sublist = transformInsertRow(pstate, sublist,
										 stmt->cols,
										 icolumns, attrnos);

			exprsLists = lappend(exprsLists, sublist);
		}

		/*
		 * There mustn't have been any table references in the expressions,
		 * else strange things would happen, like Cartesian products of those
		 * tables with the VALUES list ...
		 */
		if (pstate->p_joinlist != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("VALUES must not contain table references"),
					 parser_errposition(pstate,
							  locate_var_of_level((Node *) exprsLists, 0))));

		/*
		 * Another thing we can't currently support is NEW/OLD references in
		 * rules --- seems we'd need something like SQL99's LATERAL construct
		 * to ensure that the values would be available while evaluating the
		 * VALUES RTE.  This is a shame.  FIXME
		 */
		if (list_length(pstate->p_rtable) != 1 &&
			contain_vars_of_level((Node *) exprsLists, 0))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("VALUES must not contain OLD or NEW references"),
					 errhint("Use SELECT ... UNION ALL ... instead."),
					 parser_errposition(pstate,
							  locate_var_of_level((Node *) exprsLists, 0))));

		/*
		 * Generate the VALUES RTE
		 */
		rte = addRangeTableEntryForValues(pstate, exprsLists, NULL, true);
		rtr = makeNode(RangeTblRef);
		/* assume new rte is at end */
		rtr->rtindex = list_length(pstate->p_rtable);
		Assert(rte == rt_fetch(rtr->rtindex, pstate->p_rtable));
		pstate->p_joinlist = lappend(pstate->p_joinlist, rtr);

		/*
		 * Generate list of Vars referencing the RTE
		 */
		expandRTE(rte, rtr->rtindex, 0, -1, false, NULL, &exprList);
	}
	else
	{
		/*----------
		 * Process INSERT ... VALUES with a single VALUES sublist.
		 * We treat this separately for efficiency and for historical
		 * compatibility --- specifically, allowing table references,
		 * such as
		 *			INSERT INTO foo VALUES(bar.*)
		 *
		 * The sublist is just computed directly as the Query's targetlist,
		 * with no VALUES RTE.  So it works just like SELECT without FROM.
		 *----------
		 */
		List	   *valuesLists = selectStmt->valuesLists;

		Assert(list_length(valuesLists) == 1);

		/* process the WITH clause */
		if (selectStmt->withClause)
		{
			qry->hasRecursive = selectStmt->withClause->recursive;
			qry->cteList = transformWithClause(pstate, selectStmt->withClause);
		}

		/* Do basic expression transformation (same as a ROW() expr) */
		exprList = transformExpressionList(pstate,
										   (List *) linitial(valuesLists));

		/* Prepare row for assignment to target table */
		exprList = transformInsertRow(pstate, exprList,
									  stmt->cols,
									  icolumns, attrnos);
	}

	/*
	 * Generate query's target list using the computed list of expressions.
	 * Also, mark all the target columns as needing insert permissions.
	 */
	rte = pstate->p_target_rangetblentry;
	qry->targetList = NIL;
	icols = list_head(icolumns);
	attnos = list_head(attrnos);
	foreach(lc, exprList)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		ResTarget  *col;
		AttrNumber	attr_num;
		TargetEntry *tle;

		col = (ResTarget *) lfirst(icols);
		Assert(IsA(col, ResTarget));
		attr_num = (AttrNumber) lfirst_int(attnos);

		tle = makeTargetEntry(expr,
							  attr_num,
							  col->name,
							  false);
		qry->targetList = lappend(qry->targetList, tle);

		rte->modifiedCols = bms_add_member(rte->modifiedCols,
							  attr_num - FirstLowInvalidHeapAttributeNumber);

		icols = lnext(icols);
		attnos = lnext(attnos);
	}

	/*
	 * If we have a RETURNING clause, we need to add the target relation to
	 * the query namespace before processing it, so that Var references in
	 * RETURNING will work.  Also, remove any namespace entries added in a
	 * sub-SELECT or VALUES list.
	 */
	if (stmt->returningList)
	{
		pstate->p_relnamespace = NIL;
		pstate->p_varnamespace = NIL;
		addRTEtoQuery(pstate, pstate->p_target_rangetblentry,
					  false, true, true);
		qry->returningList = transformReturningList(pstate,
													stmt->returningList);
	}

	/* done building the range table and jointree */
	qry->rtable = pstate->p_rtable;
	qry->jointree = makeFromExpr(pstate->p_joinlist, NULL);

	qry->hasSubLinks = pstate->p_hasSubLinks;
	/* aggregates not allowed (but subselects are okay) */
	if (pstate->p_hasAggs)
		ereport(ERROR,
				(errcode(ERRCODE_GROUPING_ERROR),
				 errmsg("cannot use aggregate function in VALUES"),
				 parser_errposition(pstate,
									locate_agg_of_level((Node *) qry, 0))));
	if (pstate->p_hasWindowFuncs)
		ereport(ERROR,
				(errcode(ERRCODE_WINDOWING_ERROR),
				 errmsg("cannot use window function in VALUES"),
				 parser_errposition(pstate,
									locate_windowfunc((Node *) qry))));

	return qry;
}

/*
 * Prepare an INSERT row for assignment to the target table.
 *
 * The row might be either a VALUES row, or variables referencing a
 * sub-SELECT output.
 */
static List *
transformInsertRow(ParseState *pstate, List *exprlist,
				   List *stmtcols, List *icolumns, List *attrnos)
{
	List	   *result;
	ListCell   *lc;
	ListCell   *icols;
	ListCell   *attnos;

	/*
	 * Check length of expr list.  It must not have more expressions than
	 * there are target columns.  We allow fewer, but only if no explicit
	 * columns list was given (the remaining columns are implicitly
	 * defaulted).  Note we must check this *after* transformation because
	 * that could expand '*' into multiple items.
	 */
	if (list_length(exprlist) > list_length(icolumns))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("INSERT has more expressions than target columns"),
				 parser_errposition(pstate,
									exprLocation(list_nth(exprlist,
												  list_length(icolumns))))));
	if (stmtcols != NIL &&
		list_length(exprlist) < list_length(icolumns))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("INSERT has more target columns than expressions"),
				 parser_errposition(pstate,
									exprLocation(list_nth(icolumns,
												  list_length(exprlist))))));

	/*
	 * Prepare columns for assignment to target table.
	 */
	result = NIL;
	icols = list_head(icolumns);
	attnos = list_head(attrnos);
	foreach(lc, exprlist)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		ResTarget  *col;

		col = (ResTarget *) lfirst(icols);
		Assert(IsA(col, ResTarget));

		expr = transformAssignedExpr(pstate, expr,
									 col->name,
									 lfirst_int(attnos),
									 col->indirection,
									 col->location);

		result = lappend(result, expr);

		icols = lnext(icols);
		attnos = lnext(attnos);
	}

	return result;
}


/*
 * transformSelectStmt -
 *	  transforms a Select Statement
 *
 * Note: this covers only cases with no set operations and no VALUES lists;
 * see below for the other cases.
 */
static Query *
transformSelectStmt(ParseState *pstate, SelectStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	Node	   *qual;
	ListCell   *l;

	qry->commandType = CMD_SELECT;

	/* process the WITH clause independently of all else */
	if (stmt->withClause)
	{
		qry->hasRecursive = stmt->withClause->recursive;
		qry->cteList = transformWithClause(pstate, stmt->withClause);
	}

	/* make FOR UPDATE/FOR SHARE info available to addRangeTableEntry */
	pstate->p_locking_clause = stmt->lockingClause;

	/* make WINDOW info available for window functions, too */
	pstate->p_windowdefs = stmt->windowClause;

	/* process the FROM clause */
	transformFromClause(pstate, stmt->fromClause);

	/* transform targetlist */
	qry->targetList = transformTargetList(pstate, stmt->targetList);

	/* mark column origins */
	markTargetListOrigins(pstate, qry->targetList);

	/* transform WHERE */
	qual = transformWhereClause(pstate, stmt->whereClause, "WHERE");

	/*
	 * Initial processing of HAVING clause is just like WHERE clause.
	 */
	qry->havingQual = transformWhereClause(pstate, stmt->havingClause,
										   "HAVING");

	/*
	 * Transform sorting/grouping stuff.  Do ORDER BY first because both
	 * transformGroupClause and transformDistinctClause need the results. Note
	 * that these functions can also change the targetList, so it's passed to
	 * them by reference.
	 */
	qry->sortClause = transformSortClause(pstate,
										  stmt->sortClause,
										  &qry->targetList,
										  true /* fix unknowns */ ,
										  false /* allow SQL92 rules */ );

	qry->groupClause = transformGroupClause(pstate,
											stmt->groupClause,
											&qry->targetList,
											qry->sortClause,
											false /* allow SQL92 rules */ );

	if (stmt->distinctClause == NIL)
	{
		qry->distinctClause = NIL;
		qry->hasDistinctOn = false;
	}
	else if (linitial(stmt->distinctClause) == NULL)
	{
		/* We had SELECT DISTINCT */
		qry->distinctClause = transformDistinctClause(pstate,
													  &qry->targetList,
													  qry->sortClause,
													  false);
		qry->hasDistinctOn = false;
	}
	else
	{
		/* We had SELECT DISTINCT ON */
		qry->distinctClause = transformDistinctOnClause(pstate,
														stmt->distinctClause,
														&qry->targetList,
														qry->sortClause);
		qry->hasDistinctOn = true;
	}

	/* transform LIMIT */
	qry->limitOffset = transformLimitClause(pstate, stmt->limitOffset,
											"OFFSET");
	qry->limitCount = transformLimitClause(pstate, stmt->limitCount,
										   "LIMIT");

	/* transform window clauses after we have seen all window functions */
	qry->windowClause = transformWindowDefinitions(pstate,
												   pstate->p_windowdefs,
												   &qry->targetList);

	/* handle any SELECT INTO/CREATE TABLE AS spec */
	if (stmt->intoClause)
	{
		qry->intoClause = stmt->intoClause;
		if (stmt->intoClause->colNames)
			applyColumnNames(qry->targetList, stmt->intoClause->colNames);
	}

	qry->rtable = pstate->p_rtable;
	qry->jointree = makeFromExpr(pstate->p_joinlist, qual);

	qry->hasSubLinks = pstate->p_hasSubLinks;
	qry->hasAggs = pstate->p_hasAggs;
	if (pstate->p_hasAggs || qry->groupClause || qry->havingQual)
		parseCheckAggregates(pstate, qry);
	qry->hasWindowFuncs = pstate->p_hasWindowFuncs;
	if (pstate->p_hasWindowFuncs)
		parseCheckWindowFuncs(pstate, qry);

	foreach(l, stmt->lockingClause)
	{
		transformLockingClause(pstate, qry,
							   (LockingClause *) lfirst(l), false);
	}

	return qry;
}

/*
 * transformValuesClause -
 *	  transforms a VALUES clause that's being used as a standalone SELECT
 *
 * We build a Query containing a VALUES RTE, rather as if one had written
 *			SELECT * FROM (VALUES ...) AS "*VALUES*"
 */
static Query *
transformValuesClause(ParseState *pstate, SelectStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	List	   *exprsLists = NIL;
	List	  **colexprs = NULL;
	Oid		   *coltypes = NULL;
	int			sublist_length = -1;
	List	   *newExprsLists;
	RangeTblEntry *rte;
	RangeTblRef *rtr;
	ListCell   *lc;
	ListCell   *lc2;
	int			i;

	qry->commandType = CMD_SELECT;

	/* Most SELECT stuff doesn't apply in a VALUES clause */
	Assert(stmt->distinctClause == NIL);
	Assert(stmt->targetList == NIL);
	Assert(stmt->fromClause == NIL);
	Assert(stmt->whereClause == NULL);
	Assert(stmt->groupClause == NIL);
	Assert(stmt->havingClause == NULL);
	Assert(stmt->windowClause == NIL);
	Assert(stmt->op == SETOP_NONE);

	/* process the WITH clause independently of all else */
	if (stmt->withClause)
	{
		qry->hasRecursive = stmt->withClause->recursive;
		qry->cteList = transformWithClause(pstate, stmt->withClause);
	}

	/*
	 * For each row of VALUES, transform the raw expressions and gather type
	 * information.  This is also a handy place to reject DEFAULT nodes, which
	 * the grammar allows for simplicity.
	 */
	foreach(lc, stmt->valuesLists)
	{
		List	   *sublist = (List *) lfirst(lc);

		/* Do basic expression transformation (same as a ROW() expr) */
		sublist = transformExpressionList(pstate, sublist);

		/*
		 * All the sublists must be the same length, *after* transformation
		 * (which might expand '*' into multiple items).  The VALUES RTE can't
		 * handle anything different.
		 */
		if (sublist_length < 0)
		{
			/* Remember post-transformation length of first sublist */
			sublist_length = list_length(sublist);
			/* and allocate arrays for per-column info */
			colexprs = (List **) palloc0(sublist_length * sizeof(List *));
			coltypes = (Oid *) palloc0(sublist_length * sizeof(Oid));
		}
		else if (sublist_length != list_length(sublist))
		{
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("VALUES lists must all be the same length"),
					 parser_errposition(pstate,
										exprLocation((Node *) sublist))));
		}

		exprsLists = lappend(exprsLists, sublist);

		/* Check for DEFAULT and build per-column expression lists */
		i = 0;
		foreach(lc2, sublist)
		{
			Node	   *col = (Node *) lfirst(lc2);

			if (IsA(col, SetToDefault))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("DEFAULT can only appear in a VALUES list within INSERT"),
						 parser_errposition(pstate, exprLocation(col))));
			colexprs[i] = lappend(colexprs[i], col);
			i++;
		}
	}

	/*
	 * Now resolve the common types of the columns, and coerce everything to
	 * those types.
	 */
	for (i = 0; i < sublist_length; i++)
	{
		coltypes[i] = select_common_type(pstate, colexprs[i], "VALUES", NULL);
	}

	newExprsLists = NIL;
	foreach(lc, exprsLists)
	{
		List	   *sublist = (List *) lfirst(lc);
		List	   *newsublist = NIL;

		i = 0;
		foreach(lc2, sublist)
		{
			Node	   *col = (Node *) lfirst(lc2);

			col = coerce_to_common_type(pstate, col, coltypes[i], "VALUES");
			newsublist = lappend(newsublist, col);
			i++;
		}

		newExprsLists = lappend(newExprsLists, newsublist);
	}

	/*
	 * Generate the VALUES RTE
	 */
	rte = addRangeTableEntryForValues(pstate, newExprsLists, NULL, true);
	rtr = makeNode(RangeTblRef);
	/* assume new rte is at end */
	rtr->rtindex = list_length(pstate->p_rtable);
	Assert(rte == rt_fetch(rtr->rtindex, pstate->p_rtable));
	pstate->p_joinlist = lappend(pstate->p_joinlist, rtr);
	pstate->p_relnamespace = lappend(pstate->p_relnamespace, rte);
	pstate->p_varnamespace = lappend(pstate->p_varnamespace, rte);

	/*
	 * Generate a targetlist as though expanding "*"
	 */
	Assert(pstate->p_next_resno == 1);
	qry->targetList = expandRelAttrs(pstate, rte, rtr->rtindex, 0, -1);

	/*
	 * The grammar allows attaching ORDER BY, LIMIT, and FOR UPDATE to a
	 * VALUES, so cope.
	 */
	qry->sortClause = transformSortClause(pstate,
										  stmt->sortClause,
										  &qry->targetList,
										  true /* fix unknowns */ ,
										  false /* allow SQL92 rules */ );

	qry->limitOffset = transformLimitClause(pstate, stmt->limitOffset,
											"OFFSET");
	qry->limitCount = transformLimitClause(pstate, stmt->limitCount,
										   "LIMIT");

	if (stmt->lockingClause)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("SELECT FOR UPDATE/SHARE cannot be applied to VALUES")));

	/* handle any CREATE TABLE AS spec */
	if (stmt->intoClause)
	{
		qry->intoClause = stmt->intoClause;
		if (stmt->intoClause->colNames)
			applyColumnNames(qry->targetList, stmt->intoClause->colNames);
	}

	/*
	 * There mustn't have been any table references in the expressions, else
	 * strange things would happen, like Cartesian products of those tables
	 * with the VALUES list.  We have to check this after parsing ORDER BY et
	 * al since those could insert more junk.
	 */
	if (list_length(pstate->p_joinlist) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("VALUES must not contain table references"),
				 parser_errposition(pstate,
						   locate_var_of_level((Node *) newExprsLists, 0))));

	/*
	 * Another thing we can't currently support is NEW/OLD references in rules
	 * --- seems we'd need something like SQL99's LATERAL construct to ensure
	 * that the values would be available while evaluating the VALUES RTE.
	 * This is a shame.  FIXME
	 */
	if (list_length(pstate->p_rtable) != 1 &&
		contain_vars_of_level((Node *) newExprsLists, 0))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("VALUES must not contain OLD or NEW references"),
				 errhint("Use SELECT ... UNION ALL ... instead."),
				 parser_errposition(pstate,
						   locate_var_of_level((Node *) newExprsLists, 0))));

	qry->rtable = pstate->p_rtable;
	qry->jointree = makeFromExpr(pstate->p_joinlist, NULL);

	qry->hasSubLinks = pstate->p_hasSubLinks;
	/* aggregates not allowed (but subselects are okay) */
	if (pstate->p_hasAggs)
		ereport(ERROR,
				(errcode(ERRCODE_GROUPING_ERROR),
				 errmsg("cannot use aggregate function in VALUES"),
				 parser_errposition(pstate,
						   locate_agg_of_level((Node *) newExprsLists, 0))));
	if (pstate->p_hasWindowFuncs)
		ereport(ERROR,
				(errcode(ERRCODE_WINDOWING_ERROR),
				 errmsg("cannot use window function in VALUES"),
				 parser_errposition(pstate,
								locate_windowfunc((Node *) newExprsLists))));

	return qry;
}

/*
 * transformSetOperationStmt -
 *	  transforms a set-operations tree
 *
 * A set-operation tree is just a SELECT, but with UNION/INTERSECT/EXCEPT
 * structure to it.  We must transform each leaf SELECT and build up a top-
 * level Query that contains the leaf SELECTs as subqueries in its rangetable.
 * The tree of set operations is converted into the setOperations field of
 * the top-level Query.
 */
static Query *
transformSetOperationStmt(ParseState *pstate, SelectStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	SelectStmt *leftmostSelect;
	int			leftmostRTI;
	Query	   *leftmostQuery;
	SetOperationStmt *sostmt;
	List	   *socolinfo;
	List	   *intoColNames = NIL;
	List	   *sortClause;
	Node	   *limitOffset;
	Node	   *limitCount;
	List	   *lockingClause;
	WithClause *withClause;
	Node	   *node;
	ListCell   *left_tlist,
			   *lct,
			   *lcm,
			   *l;
	List	   *targetvars,
			   *targetnames,
			   *sv_relnamespace,
			   *sv_varnamespace;
	int			sv_rtable_length;
	RangeTblEntry *jrte;
	int			tllen;

	qry->commandType = CMD_SELECT;

	/*
	 * Find leftmost leaf SelectStmt; extract the one-time-only items from it
	 * and from the top-level node.
	 */
	leftmostSelect = stmt->larg;
	while (leftmostSelect && leftmostSelect->op != SETOP_NONE)
		leftmostSelect = leftmostSelect->larg;
	Assert(leftmostSelect && IsA(leftmostSelect, SelectStmt) &&
		   leftmostSelect->larg == NULL);
	if (leftmostSelect->intoClause)
	{
		qry->intoClause = leftmostSelect->intoClause;
		intoColNames = leftmostSelect->intoClause->colNames;
	}

	/* clear this to prevent complaints in transformSetOperationTree() */
	leftmostSelect->intoClause = NULL;

	/*
	 * These are not one-time, exactly, but we want to process them here and
	 * not let transformSetOperationTree() see them --- else it'll just
	 * recurse right back here!
	 */
	sortClause = stmt->sortClause;
	limitOffset = stmt->limitOffset;
	limitCount = stmt->limitCount;
	lockingClause = stmt->lockingClause;
	withClause = stmt->withClause;

	stmt->sortClause = NIL;
	stmt->limitOffset = NULL;
	stmt->limitCount = NULL;
	stmt->lockingClause = NIL;
	stmt->withClause = NULL;

	/* We don't support FOR UPDATE/SHARE with set ops at the moment. */
	if (lockingClause)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("SELECT FOR UPDATE/SHARE is not allowed with UNION/INTERSECT/EXCEPT")));

	/* Process the WITH clause independently of all else */
	if (withClause)
	{
		qry->hasRecursive = withClause->recursive;
		qry->cteList = transformWithClause(pstate, withClause);
	}

	/*
	 * Recursively transform the components of the tree.
	 */
	sostmt = (SetOperationStmt *) transformSetOperationTree(pstate, stmt,
															true,
															&socolinfo);
	Assert(sostmt && IsA(sostmt, SetOperationStmt));
	qry->setOperations = (Node *) sostmt;

	/*
	 * Re-find leftmost SELECT (now it's a sub-query in rangetable)
	 */
	node = sostmt->larg;
	while (node && IsA(node, SetOperationStmt))
		node = ((SetOperationStmt *) node)->larg;
	Assert(node && IsA(node, RangeTblRef));
	leftmostRTI = ((RangeTblRef *) node)->rtindex;
	leftmostQuery = rt_fetch(leftmostRTI, pstate->p_rtable)->subquery;
	Assert(leftmostQuery != NULL);

	/*
	 * Generate dummy targetlist for outer query using column names of
	 * leftmost select and common datatypes of topmost set operation. Also
	 * make lists of the dummy vars and their names for use in parsing ORDER
	 * BY.
	 *
	 * Note: we use leftmostRTI as the varno of the dummy variables. It
	 * shouldn't matter too much which RT index they have, as long as they
	 * have one that corresponds to a real RT entry; else funny things may
	 * happen when the tree is mashed by rule rewriting.
	 */
	qry->targetList = NIL;
	targetvars = NIL;
	targetnames = NIL;
	left_tlist = list_head(leftmostQuery->targetList);

	forboth(lct, sostmt->colTypes, lcm, sostmt->colTypmods)
	{
		Oid			colType = lfirst_oid(lct);
		int32		colTypmod = lfirst_int(lcm);
		TargetEntry *lefttle = (TargetEntry *) lfirst(left_tlist);
		char	   *colName;
		TargetEntry *tle;
		Var		   *var;

		Assert(!lefttle->resjunk);
		colName = pstrdup(lefttle->resname);
		var = makeVar(leftmostRTI,
					  lefttle->resno,
					  colType,
					  colTypmod,
					  0);
		var->location = exprLocation((Node *) lefttle->expr);
		tle = makeTargetEntry((Expr *) var,
							  (AttrNumber) pstate->p_next_resno++,
							  colName,
							  false);
		qry->targetList = lappend(qry->targetList, tle);
		targetvars = lappend(targetvars, var);
		targetnames = lappend(targetnames, makeString(colName));
		left_tlist = lnext(left_tlist);
	}

	/*
	 * As a first step towards supporting sort clauses that are expressions
	 * using the output columns, generate a varnamespace entry that makes the
	 * output columns visible.  A Join RTE node is handy for this, since we
	 * can easily control the Vars generated upon matches.
	 *
	 * Note: we don't yet do anything useful with such cases, but at least
	 * "ORDER BY upper(foo)" will draw the right error message rather than
	 * "foo not found".
	 */
	sv_rtable_length = list_length(pstate->p_rtable);

	jrte = addRangeTableEntryForJoin(pstate,
									 targetnames,
									 JOIN_INNER,
									 targetvars,
									 NULL,
									 false);

	sv_relnamespace = pstate->p_relnamespace;
	pstate->p_relnamespace = NIL;		/* no qualified names allowed */

	sv_varnamespace = pstate->p_varnamespace;
	pstate->p_varnamespace = list_make1(jrte);

	/*
	 * For now, we don't support resjunk sort clauses on the output of a
	 * setOperation tree --- you can only use the SQL92-spec options of
	 * selecting an output column by name or number.  Enforce by checking that
	 * transformSortClause doesn't add any items to tlist.
	 */
	tllen = list_length(qry->targetList);

	qry->sortClause = transformSortClause(pstate,
										  sortClause,
										  &qry->targetList,
										  false /* no unknowns expected */ ,
										  false /* allow SQL92 rules */ );

	pstate->p_rtable = list_truncate(pstate->p_rtable, sv_rtable_length);
	pstate->p_relnamespace = sv_relnamespace;
	pstate->p_varnamespace = sv_varnamespace;

	if (tllen != list_length(qry->targetList))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("invalid UNION/INTERSECT/EXCEPT ORDER BY clause"),
				 errdetail("Only result column names can be used, not expressions or functions."),
				 errhint("Add the expression/function to every SELECT, or move the UNION into a FROM clause."),
				 parser_errposition(pstate,
						   exprLocation(list_nth(qry->targetList, tllen)))));

	qry->limitOffset = transformLimitClause(pstate, limitOffset,
											"OFFSET");
	qry->limitCount = transformLimitClause(pstate, limitCount,
										   "LIMIT");

	/*
	 * Handle SELECT INTO/CREATE TABLE AS.
	 *
	 * Any column names from CREATE TABLE AS need to be attached to both the
	 * top level and the leftmost subquery.  We do not do this earlier because
	 * we do *not* want sortClause processing to be affected.
	 */
	if (intoColNames)
	{
		applyColumnNames(qry->targetList, intoColNames);
		applyColumnNames(leftmostQuery->targetList, intoColNames);
	}

	qry->rtable = pstate->p_rtable;
	qry->jointree = makeFromExpr(pstate->p_joinlist, NULL);

	qry->hasSubLinks = pstate->p_hasSubLinks;
	qry->hasAggs = pstate->p_hasAggs;
	if (pstate->p_hasAggs || qry->groupClause || qry->havingQual)
		parseCheckAggregates(pstate, qry);
	qry->hasWindowFuncs = pstate->p_hasWindowFuncs;
	if (pstate->p_hasWindowFuncs)
		parseCheckWindowFuncs(pstate, qry);

	foreach(l, lockingClause)
	{
		transformLockingClause(pstate, qry,
							   (LockingClause *) lfirst(l), false);
	}

	return qry;
}

/*
 * transformSetOperationTree
 *		Recursively transform leaves and internal nodes of a set-op tree
 *
 * In addition to returning the transformed node, we return a list of
 * expression nodes showing the type, typmod, and location (for error messages)
 * of each output column of the set-op node.  This is used only during the
 * internal recursion of this function.  At the upper levels we use
 * SetToDefault nodes for this purpose, since they carry exactly the fields
 * needed, but any other expression node type would do as well.
 */
static Node *
transformSetOperationTree(ParseState *pstate, SelectStmt *stmt,
						  bool isTopLevel, List **colInfo)
{
	bool		isLeaf;

	Assert(stmt && IsA(stmt, SelectStmt));

	/* Guard against stack overflow due to overly complex set-expressions */
	check_stack_depth();

	/*
	 * Validity-check both leaf and internal SELECTs for disallowed ops.
	 */
	if (stmt->intoClause)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("INTO is only allowed on first SELECT of UNION/INTERSECT/EXCEPT"),
				 parser_errposition(pstate,
								  exprLocation((Node *) stmt->intoClause))));

	/* We don't support FOR UPDATE/SHARE with set ops at the moment. */
	if (stmt->lockingClause)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("SELECT FOR UPDATE/SHARE is not allowed with UNION/INTERSECT/EXCEPT")));

	/*
	 * If an internal node of a set-op tree has ORDER BY, LIMIT, FOR UPDATE,
	 * or WITH clauses attached, we need to treat it like a leaf node to
	 * generate an independent sub-Query tree.  Otherwise, it can be
	 * represented by a SetOperationStmt node underneath the parent Query.
	 */
	if (stmt->op == SETOP_NONE)
	{
		Assert(stmt->larg == NULL && stmt->rarg == NULL);
		isLeaf = true;
	}
	else
	{
		Assert(stmt->larg != NULL && stmt->rarg != NULL);
		if (stmt->sortClause || stmt->limitOffset || stmt->limitCount ||
			stmt->lockingClause || stmt->withClause)
			isLeaf = true;
		else
			isLeaf = false;
	}

	if (isLeaf)
	{
		/* Process leaf SELECT */
		Query	   *selectQuery;
		char		selectName[32];
		RangeTblEntry *rte;
		RangeTblRef *rtr;
		ListCell   *tl;

		/*
		 * Transform SelectStmt into a Query.
		 *
		 * Note: previously transformed sub-queries don't affect the parsing
		 * of this sub-query, because they are not in the toplevel pstate's
		 * namespace list.
		 */
		selectQuery = parse_sub_analyze((Node *) stmt, pstate, NULL, false);

		/*
		 * Check for bogus references to Vars on the current query level (but
		 * upper-level references are okay). Normally this can't happen
		 * because the namespace will be empty, but it could happen if we are
		 * inside a rule.
		 */
		if (pstate->p_relnamespace || pstate->p_varnamespace)
		{
			if (contain_vars_of_level((Node *) selectQuery, 1))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
						 errmsg("UNION/INTERSECT/EXCEPT member statement cannot refer to other relations of same query level"),
						 parser_errposition(pstate,
							 locate_var_of_level((Node *) selectQuery, 1))));
		}

		/*
		 * Extract a list of the result expressions for upper-level checking.
		 */
		*colInfo = NIL;
		foreach(tl, selectQuery->targetList)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(tl);

			if (!tle->resjunk)
				*colInfo = lappend(*colInfo, tle->expr);
		}

		/*
		 * Make the leaf query be a subquery in the top-level rangetable.
		 */
		snprintf(selectName, sizeof(selectName), "*SELECT* %d",
				 list_length(pstate->p_rtable) + 1);
		rte = addRangeTableEntryForSubquery(pstate,
											selectQuery,
											makeAlias(selectName, NIL),
											false);

		/*
		 * Return a RangeTblRef to replace the SelectStmt in the set-op tree.
		 */
		rtr = makeNode(RangeTblRef);
		/* assume new rte is at end */
		rtr->rtindex = list_length(pstate->p_rtable);
		Assert(rte == rt_fetch(rtr->rtindex, pstate->p_rtable));
		return (Node *) rtr;
	}
	else
	{
		/* Process an internal node (set operation node) */
		SetOperationStmt *op = makeNode(SetOperationStmt);
		List	   *lcolinfo;
		List	   *rcolinfo;
		ListCell   *lci;
		ListCell   *rci;
		const char *context;

		context = (stmt->op == SETOP_UNION ? "UNION" :
				   (stmt->op == SETOP_INTERSECT ? "INTERSECT" :
					"EXCEPT"));

		op->op = stmt->op;
		op->all = stmt->all;

		/*
		 * Recursively transform the left child node.
		 */
		op->larg = transformSetOperationTree(pstate, stmt->larg,
											 false,
											 &lcolinfo);

		/*
		 * If we are processing a recursive union query, now is the time to
		 * examine the non-recursive term's output columns and mark the
		 * containing CTE as having those result columns.  We should do this
		 * only at the topmost setop of the CTE, of course.
		 */
		if (isTopLevel &&
			pstate->p_parent_cte &&
			pstate->p_parent_cte->cterecursive)
			determineRecursiveColTypes(pstate, op->larg, lcolinfo);

		/*
		 * Recursively transform the right child node.
		 */
		op->rarg = transformSetOperationTree(pstate, stmt->rarg,
											 false,
											 &rcolinfo);

		/*
		 * Verify that the two children have the same number of non-junk
		 * columns, and determine the types of the merged output columns.
		 */
		if (list_length(lcolinfo) != list_length(rcolinfo))
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("each %s query must have the same number of columns",
						context),
					 parser_errposition(pstate,
										exprLocation((Node *) rcolinfo))));

		*colInfo = NIL;
		op->colTypes = NIL;
		op->colTypmods = NIL;
		op->groupClauses = NIL;
		forboth(lci, lcolinfo, rci, rcolinfo)
		{
			Node	   *lcolnode = (Node *) lfirst(lci);
			Node	   *rcolnode = (Node *) lfirst(rci);
			Oid			lcoltype = exprType(lcolnode);
			Oid			rcoltype = exprType(rcolnode);
			int32		lcoltypmod = exprTypmod(lcolnode);
			int32		rcoltypmod = exprTypmod(rcolnode);
			Node	   *bestexpr;
			SetToDefault *rescolnode;
			Oid			rescoltype;
			int32		rescoltypmod;

			/* select common type, same as CASE et al */
			rescoltype = select_common_type(pstate,
											list_make2(lcolnode, rcolnode),
											context,
											&bestexpr);
			/* if same type and same typmod, use typmod; else default */
			if (lcoltype == rcoltype && lcoltypmod == rcoltypmod)
				rescoltypmod = lcoltypmod;
			else
				rescoltypmod = -1;

			/*
			 * Verify the coercions are actually possible.  If not, we'd fail
			 * later anyway, but we want to fail now while we have sufficient
			 * context to produce an error cursor position.
			 *
			 * The if-tests might look wrong, but they are correct: we should
			 * verify if the input is non-UNKNOWN *or* if it is an UNKNOWN
			 * Const (to verify the literal is valid for the target data type)
			 * or Param (to possibly resolve the Param's type).  We should do
			 * nothing if the input is say an UNKNOWN Var, which can happen in
			 * some cases.  The planner is sometimes able to fold the Var to a
			 * constant before it has to coerce the type, so failing now would
			 * just break cases that might work.
			 */
			if (lcoltype != UNKNOWNOID ||
				IsA(lcolnode, Const) ||IsA(lcolnode, Param))
				(void) coerce_to_common_type(pstate, lcolnode,
											 rescoltype, context);
			if (rcoltype != UNKNOWNOID ||
				IsA(rcolnode, Const) ||IsA(rcolnode, Param))
				(void) coerce_to_common_type(pstate, rcolnode,
											 rescoltype, context);

			/* emit results */
			rescolnode = makeNode(SetToDefault);
			rescolnode->typeId = rescoltype;
			rescolnode->typeMod = rescoltypmod;
			rescolnode->location = exprLocation(bestexpr);
			*colInfo = lappend(*colInfo, rescolnode);

			op->colTypes = lappend_oid(op->colTypes, rescoltype);
			op->colTypmods = lappend_int(op->colTypmods, rescoltypmod);

			/*
			 * For all cases except UNION ALL, identify the grouping operators
			 * (and, if available, sorting operators) that will be used to
			 * eliminate duplicates.
			 */
			if (op->op != SETOP_UNION || !op->all)
			{
				SortGroupClause *grpcl = makeNode(SortGroupClause);
				Oid			sortop;
				Oid			eqop;
				ParseCallbackState pcbstate;

				setup_parser_errposition_callback(&pcbstate, pstate,
												  rescolnode->location);

				/* determine the eqop and optional sortop */
				get_sort_group_operators(rescoltype,
										 false, true, false,
										 &sortop, &eqop, NULL);

				cancel_parser_errposition_callback(&pcbstate);

				/* we don't have a tlist yet, so can't assign sortgrouprefs */
				grpcl->tleSortGroupRef = 0;
				grpcl->eqop = eqop;
				grpcl->sortop = sortop;
				grpcl->nulls_first = false;		/* OK with or without sortop */

				op->groupClauses = lappend(op->groupClauses, grpcl);
			}
		}

		return (Node *) op;
	}
}

/*
 * Process the outputs of the non-recursive term of a recursive union
 * to set up the parent CTE's columns
 */
static void
determineRecursiveColTypes(ParseState *pstate, Node *larg, List *lcolinfo)
{
	Node	   *node;
	int			leftmostRTI;
	Query	   *leftmostQuery;
	List	   *targetList;
	ListCell   *left_tlist;
	ListCell   *lci;
	int			next_resno;

	/*
	 * Find leftmost leaf SELECT
	 */
	node = larg;
	while (node && IsA(node, SetOperationStmt))
		node = ((SetOperationStmt *) node)->larg;
	Assert(node && IsA(node, RangeTblRef));
	leftmostRTI = ((RangeTblRef *) node)->rtindex;
	leftmostQuery = rt_fetch(leftmostRTI, pstate->p_rtable)->subquery;
	Assert(leftmostQuery != NULL);

	/*
	 * Generate dummy targetlist using column names of leftmost select and
	 * dummy result expressions of the non-recursive term.
	 */
	targetList = NIL;
	left_tlist = list_head(leftmostQuery->targetList);
	next_resno = 1;

	foreach(lci, lcolinfo)
	{
		Expr	   *lcolexpr = (Expr *) lfirst(lci);
		TargetEntry *lefttle = (TargetEntry *) lfirst(left_tlist);
		char	   *colName;
		TargetEntry *tle;

		Assert(!lefttle->resjunk);
		colName = pstrdup(lefttle->resname);
		tle = makeTargetEntry(lcolexpr,
							  next_resno++,
							  colName,
							  false);
		targetList = lappend(targetList, tle);
		left_tlist = lnext(left_tlist);
	}

	/* Now build CTE's output column info using dummy targetlist */
	analyzeCTETargetList(pstate, pstate->p_parent_cte, targetList);
}

/*
 * Attach column names from a ColumnDef list to a TargetEntry list
 * (for CREATE TABLE AS)
 */
static void
applyColumnNames(List *dst, List *src)
{
	ListCell   *dst_item;
	ListCell   *src_item;

	src_item = list_head(src);

	foreach(dst_item, dst)
	{
		TargetEntry *d = (TargetEntry *) lfirst(dst_item);
		ColumnDef  *s;

		/* junk targets don't count */
		if (d->resjunk)
			continue;

		/* fewer ColumnDefs than target entries is OK */
		if (src_item == NULL)
			break;

		s = (ColumnDef *) lfirst(src_item);
		src_item = lnext(src_item);

		d->resname = pstrdup(s->colname);
	}

	/* more ColumnDefs than target entries is not OK */
	if (src_item != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("CREATE TABLE AS specifies too many column names")));
}


/*
 * transformUpdateStmt -
 *	  transforms an update statement
 */
static Query *
transformUpdateStmt(ParseState *pstate, UpdateStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	RangeTblEntry *target_rte;
	Node	   *qual;
	ListCell   *origTargetList;
	ListCell   *tl;

	qry->commandType = CMD_UPDATE;
	pstate->p_is_update = true;

	qry->resultRelation = setTargetTable(pstate, stmt->relation,
								  interpretInhOption(stmt->relation->inhOpt),
										 true,
										 ACL_UPDATE);

	/*
	 * the FROM clause is non-standard SQL syntax. We used to be able to do
	 * this with REPLACE in POSTQUEL so we keep the feature.
	 */
	transformFromClause(pstate, stmt->fromClause);

	qry->targetList = transformTargetList(pstate, stmt->targetList);

	qual = transformWhereClause(pstate, stmt->whereClause, "WHERE");

	qry->returningList = transformReturningList(pstate, stmt->returningList);

	qry->rtable = pstate->p_rtable;
	qry->jointree = makeFromExpr(pstate->p_joinlist, qual);

	qry->hasSubLinks = pstate->p_hasSubLinks;

	/*
	 * Top-level aggregates are simply disallowed in UPDATE, per spec. (From
	 * an implementation point of view, this is forced because the implicit
	 * ctid reference would otherwise be an ungrouped variable.)
	 */
	if (pstate->p_hasAggs)
		ereport(ERROR,
				(errcode(ERRCODE_GROUPING_ERROR),
				 errmsg("cannot use aggregate function in UPDATE"),
				 parser_errposition(pstate,
									locate_agg_of_level((Node *) qry, 0))));
	if (pstate->p_hasWindowFuncs)
		ereport(ERROR,
				(errcode(ERRCODE_WINDOWING_ERROR),
				 errmsg("cannot use window function in UPDATE"),
				 parser_errposition(pstate,
									locate_windowfunc((Node *) qry))));

	/*
	 * Now we are done with SELECT-like processing, and can get on with
	 * transforming the target list to match the UPDATE target columns.
	 */

	/* Prepare to assign non-conflicting resnos to resjunk attributes */
	if (pstate->p_next_resno <= pstate->p_target_relation->rd_rel->relnatts)
		pstate->p_next_resno = pstate->p_target_relation->rd_rel->relnatts + 1;

	/* Prepare non-junk columns for assignment to target table */
	target_rte = pstate->p_target_rangetblentry;
	origTargetList = list_head(stmt->targetList);

	foreach(tl, qry->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(tl);
		ResTarget  *origTarget;
		int			attrno;

		if (tle->resjunk)
		{
			/*
			 * Resjunk nodes need no additional processing, but be sure they
			 * have resnos that do not match any target columns; else rewriter
			 * or planner might get confused.  They don't need a resname
			 * either.
			 */
			tle->resno = (AttrNumber) pstate->p_next_resno++;
			tle->resname = NULL;
			continue;
		}
		if (origTargetList == NULL)
			elog(ERROR, "UPDATE target count mismatch --- internal error");
		origTarget = (ResTarget *) lfirst(origTargetList);
		Assert(IsA(origTarget, ResTarget));

		attrno = attnameAttNum(pstate->p_target_relation,
							   origTarget->name, true);
		if (attrno == InvalidAttrNumber)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" of relation \"%s\" does not exist",
							origTarget->name,
						 RelationGetRelationName(pstate->p_target_relation)),
					 parser_errposition(pstate, origTarget->location)));

		updateTargetListEntry(pstate, tle, origTarget->name,
							  attrno,
							  origTarget->indirection,
							  origTarget->location);

		/* Mark the target column as requiring update permissions */
		target_rte->modifiedCols = bms_add_member(target_rte->modifiedCols,
								attrno - FirstLowInvalidHeapAttributeNumber);

		origTargetList = lnext(origTargetList);
	}
	if (origTargetList != NULL)
		elog(ERROR, "UPDATE target count mismatch --- internal error");

	return qry;
}

/*
 * transformReturningList -
 *	handle a RETURNING clause in INSERT/UPDATE/DELETE
 */
static List *
transformReturningList(ParseState *pstate, List *returningList)
{
	List	   *rlist;
	int			save_next_resno;
	bool		save_hasAggs;
	bool		save_hasWindowFuncs;
	int			length_rtable;

	if (returningList == NIL)
		return NIL;				/* nothing to do */

	/*
	 * We need to assign resnos starting at one in the RETURNING list. Save
	 * and restore the main tlist's value of p_next_resno, just in case
	 * someone looks at it later (probably won't happen).
	 */
	save_next_resno = pstate->p_next_resno;
	pstate->p_next_resno = 1;

	/* save other state so that we can detect disallowed stuff */
	save_hasAggs = pstate->p_hasAggs;
	pstate->p_hasAggs = false;
	save_hasWindowFuncs = pstate->p_hasWindowFuncs;
	pstate->p_hasWindowFuncs = false;
	length_rtable = list_length(pstate->p_rtable);

	/* transform RETURNING identically to a SELECT targetlist */
	rlist = transformTargetList(pstate, returningList);

	/* check for disallowed stuff */

	/* aggregates not allowed (but subselects are okay) */
	if (pstate->p_hasAggs)
		ereport(ERROR,
				(errcode(ERRCODE_GROUPING_ERROR),
				 errmsg("cannot use aggregate function in RETURNING"),
				 parser_errposition(pstate,
									locate_agg_of_level((Node *) rlist, 0))));
	if (pstate->p_hasWindowFuncs)
		ereport(ERROR,
				(errcode(ERRCODE_WINDOWING_ERROR),
				 errmsg("cannot use window function in RETURNING"),
				 parser_errposition(pstate,
									locate_windowfunc((Node *) rlist))));

	/* no new relation references please */
	if (list_length(pstate->p_rtable) != length_rtable)
	{
		int			vlocation = -1;
		int			relid;

		/* try to locate such a reference to point to */
		for (relid = length_rtable + 1; relid <= list_length(pstate->p_rtable); relid++)
		{
			vlocation = locate_var_of_relation((Node *) rlist, relid, 0);
			if (vlocation >= 0)
				break;
		}
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			errmsg("RETURNING cannot contain references to other relations"),
				 parser_errposition(pstate, vlocation)));
	}

	/* mark column origins */
	markTargetListOrigins(pstate, rlist);

	/* restore state */
	pstate->p_next_resno = save_next_resno;
	pstate->p_hasAggs = save_hasAggs;
	pstate->p_hasWindowFuncs = save_hasWindowFuncs;

	return rlist;
}


/*
 * transformDeclareCursorStmt -
 *	transform a DECLARE CURSOR Statement
 *
 * DECLARE CURSOR is a hybrid case: it's an optimizable statement (in fact not
 * significantly different from a SELECT) as far as parsing/rewriting/planning
 * are concerned, but it's not passed to the executor and so in that sense is
 * a utility statement.  We transform it into a Query exactly as if it were
 * a SELECT, then stick the original DeclareCursorStmt into the utilityStmt
 * field to carry the cursor name and options.
 */
static Query *
transformDeclareCursorStmt(ParseState *pstate, DeclareCursorStmt *stmt)
{
	Query	   *result;

	/*
	 * Don't allow both SCROLL and NO SCROLL to be specified
	 */
	if ((stmt->options & CURSOR_OPT_SCROLL) &&
		(stmt->options & CURSOR_OPT_NO_SCROLL))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_DEFINITION),
				 errmsg("cannot specify both SCROLL and NO SCROLL")));

	result = transformStmt(pstate, stmt->query);

	/* Grammar should not have allowed anything but SELECT */
	if (!IsA(result, Query) ||
		result->commandType != CMD_SELECT ||
		result->utilityStmt != NULL)
		elog(ERROR, "unexpected non-SELECT command in DECLARE CURSOR");

	/* But we must explicitly disallow DECLARE CURSOR ... SELECT INTO */
	if (result->intoClause)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_DEFINITION),
				 errmsg("DECLARE CURSOR cannot specify INTO"),
				 parser_errposition(pstate,
								exprLocation((Node *) result->intoClause))));

	/* FOR UPDATE and WITH HOLD are not compatible */
	if (result->rowMarks != NIL && (stmt->options & CURSOR_OPT_HOLD))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("DECLARE CURSOR WITH HOLD ... FOR UPDATE/SHARE is not supported"),
				 errdetail("Holdable cursors must be READ ONLY.")));

	/* FOR UPDATE and SCROLL are not compatible */
	if (result->rowMarks != NIL && (stmt->options & CURSOR_OPT_SCROLL))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		errmsg("DECLARE SCROLL CURSOR ... FOR UPDATE/SHARE is not supported"),
				 errdetail("Scrollable cursors must be READ ONLY.")));

	/* FOR UPDATE and INSENSITIVE are not compatible */
	if (result->rowMarks != NIL && (stmt->options & CURSOR_OPT_INSENSITIVE))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("DECLARE INSENSITIVE CURSOR ... FOR UPDATE/SHARE is not supported"),
				 errdetail("Insensitive cursors must be READ ONLY.")));

	/* We won't need the raw querytree any more */
	stmt->query = NULL;

	result->utilityStmt = (Node *) stmt;

	return result;
}


/*
 * transformExplainStmt -
 *	transform an EXPLAIN Statement
 *
 * EXPLAIN is like other utility statements in that we emit it as a
 * CMD_UTILITY Query node; however, we must first transform the contained
 * query.  We used to postpone that until execution, but it's really necessary
 * to do it during the normal parse analysis phase to ensure that side effects
 * of parser hooks happen at the expected time.
 */
static Query *
transformExplainStmt(ParseState *pstate, ExplainStmt *stmt)
{
	Query	   *result;

	/* transform contained query */
	stmt->query = (Node *) transformStmt(pstate, stmt->query);

	/* represent the command as a utility Query */
	result = makeNode(Query);
	result->commandType = CMD_UTILITY;
	result->utilityStmt = (Node *) stmt;

	return result;
}


/*
 * Check for features that are not supported together with FOR UPDATE/SHARE.
 *
 * exported so planner can check again after rewriting, query pullup, etc
 */
void
CheckSelectLocking(Query *qry)
{
	if (qry->setOperations)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("SELECT FOR UPDATE/SHARE is not allowed with UNION/INTERSECT/EXCEPT")));
	if (qry->distinctClause != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("SELECT FOR UPDATE/SHARE is not allowed with DISTINCT clause")));
	if (qry->groupClause != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("SELECT FOR UPDATE/SHARE is not allowed with GROUP BY clause")));
	if (qry->havingQual != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		errmsg("SELECT FOR UPDATE/SHARE is not allowed with HAVING clause")));
	if (qry->hasAggs)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("SELECT FOR UPDATE/SHARE is not allowed with aggregate functions")));
	if (qry->hasWindowFuncs)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("SELECT FOR UPDATE/SHARE is not allowed with window functions")));
	if (expression_returns_set((Node *) qry->targetList))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("SELECT FOR UPDATE/SHARE is not allowed with set-returning functions in the target list")));
}

/*
 * Transform a FOR UPDATE/SHARE clause
 *
 * This basically involves replacing names by integer relids.
 *
 * NB: if you need to change this, see also markQueryForLocking()
 * in rewriteHandler.c, and isLockedRefname() in parse_relation.c.
 */
static void
transformLockingClause(ParseState *pstate, Query *qry, LockingClause *lc,
					   bool pushedDown)
{
	List	   *lockedRels = lc->lockedRels;
	ListCell   *l;
	ListCell   *rt;
	Index		i;
	LockingClause *allrels;

	CheckSelectLocking(qry);

	/* make a clause we can pass down to subqueries to select all rels */
	allrels = makeNode(LockingClause);
	allrels->lockedRels = NIL;	/* indicates all rels */
	allrels->forUpdate = lc->forUpdate;
	allrels->noWait = lc->noWait;

	if (lockedRels == NIL)
	{
		/* all regular tables used in query */
		i = 0;
		foreach(rt, qry->rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(rt);

			++i;
			switch (rte->rtekind)
			{
				case RTE_RELATION:
					applyLockingClause(qry, i,
									   lc->forUpdate, lc->noWait, pushedDown);
					rte->requiredPerms |= ACL_SELECT_FOR_UPDATE;
					break;
				case RTE_SUBQUERY:
					applyLockingClause(qry, i,
									   lc->forUpdate, lc->noWait, pushedDown);

					/*
					 * FOR UPDATE/SHARE of subquery is propagated to all of
					 * subquery's rels, too.  We could do this later (based on
					 * the marking of the subquery RTE) but it is convenient
					 * to have local knowledge in each query level about which
					 * rels need to be opened with RowShareLock.
					 */
					transformLockingClause(pstate, rte->subquery,
										   allrels, true);
					break;
				default:
					/* ignore JOIN, SPECIAL, FUNCTION, VALUES, CTE RTEs */
					break;
			}
		}
	}
	else
	{
		/* just the named tables */
		foreach(l, lockedRels)
		{
			RangeVar   *thisrel = (RangeVar *) lfirst(l);

			/* For simplicity we insist on unqualified alias names here */
			if (thisrel->catalogname || thisrel->schemaname)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("SELECT FOR UPDATE/SHARE must specify unqualified relation names"),
						 parser_errposition(pstate, thisrel->location)));

			i = 0;
			foreach(rt, qry->rtable)
			{
				RangeTblEntry *rte = (RangeTblEntry *) lfirst(rt);

				++i;
				if (strcmp(rte->eref->aliasname, thisrel->relname) == 0)
				{
					switch (rte->rtekind)
					{
						case RTE_RELATION:
							applyLockingClause(qry, i,
											   lc->forUpdate, lc->noWait,
											   pushedDown);
							rte->requiredPerms |= ACL_SELECT_FOR_UPDATE;
							break;
						case RTE_SUBQUERY:
							applyLockingClause(qry, i,
											   lc->forUpdate, lc->noWait,
											   pushedDown);
							/* see comment above */
							transformLockingClause(pstate, rte->subquery,
												   allrels, true);
							break;
						case RTE_JOIN:
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									 errmsg("SELECT FOR UPDATE/SHARE cannot be applied to a join"),
							 parser_errposition(pstate, thisrel->location)));
							break;
						case RTE_SPECIAL:
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									 errmsg("SELECT FOR UPDATE/SHARE cannot be applied to NEW or OLD"),
							 parser_errposition(pstate, thisrel->location)));
							break;
						case RTE_FUNCTION:
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									 errmsg("SELECT FOR UPDATE/SHARE cannot be applied to a function"),
							 parser_errposition(pstate, thisrel->location)));
							break;
						case RTE_VALUES:
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									 errmsg("SELECT FOR UPDATE/SHARE cannot be applied to VALUES"),
							 parser_errposition(pstate, thisrel->location)));
							break;
						case RTE_CTE:
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									 errmsg("SELECT FOR UPDATE/SHARE cannot be applied to a WITH query"),
							 parser_errposition(pstate, thisrel->location)));
							break;
						default:
							elog(ERROR, "unrecognized RTE type: %d",
								 (int) rte->rtekind);
							break;
					}
					break;		/* out of foreach loop */
				}
			}
			if (rt == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_TABLE),
						 errmsg("relation \"%s\" in FOR UPDATE/SHARE clause not found in FROM clause",
								thisrel->relname),
						 parser_errposition(pstate, thisrel->location)));
		}
	}
}

/*
 * Record locking info for a single rangetable item
 */
void
applyLockingClause(Query *qry, Index rtindex,
				   bool forUpdate, bool noWait, bool pushedDown)
{
	RowMarkClause *rc;

	/* If it's an explicit clause, make sure hasForUpdate gets set */
	if (!pushedDown)
		qry->hasForUpdate = true;

	/* Check for pre-existing entry for same rtindex */
	if ((rc = get_parse_rowmark(qry, rtindex)) != NULL)
	{
		/*
		 * If the same RTE is specified both FOR UPDATE and FOR SHARE, treat
		 * it as FOR UPDATE.  (Reasonable, since you can't take both a shared
		 * and exclusive lock at the same time; it'll end up being exclusive
		 * anyway.)
		 *
		 * We also consider that NOWAIT wins if it's specified both ways. This
		 * is a bit more debatable but raising an error doesn't seem helpful.
		 * (Consider for instance SELECT FOR UPDATE NOWAIT from a view that
		 * internally contains a plain FOR UPDATE spec.)
		 *
		 * And of course pushedDown becomes false if any clause is explicit.
		 */
		rc->forUpdate |= forUpdate;
		rc->noWait |= noWait;
		rc->pushedDown &= pushedDown;
		return;
	}

	/* Make a new RowMarkClause */
	rc = makeNode(RowMarkClause);
	rc->rti = rtindex;
	rc->forUpdate = forUpdate;
	rc->noWait = noWait;
	rc->pushedDown = pushedDown;
	qry->rowMarks = lappend(qry->rowMarks, rc);
}
