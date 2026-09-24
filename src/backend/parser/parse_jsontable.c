/*-------------------------------------------------------------------------
 *
 * parse_jsontable.c
 *	  parsing of JSON_TABLE
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_jsontable.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_clause.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/parse_type.h"
#include "utils/fmgrprotos.h"
#include "utils/json.h"
#include "utils/lsyscache.h"

/* Context for transformJsonTableColumns() */
typedef struct JsonTableParseContext
{
	ParseState *pstate;
	JsonTable  *jt;
	TableFunc  *tf;
	List	   *pathNames;		/* list of all path and columns names */
	int			pathNameId;		/* path name id counter */
} JsonTableParseContext;

static JsonTablePlan *transformJsonTableColumns(JsonTableParseContext *cxt,
												JsonTablePlanSpec *planspec,
												List *columns,
												List *passingArgs,
												JsonTablePathSpec *pathspec);
static JsonTablePlan *transformJsonTableNestedColumns(JsonTableParseContext *cxt,
													  JsonTablePlanSpec *plan,
													  List *passingArgs,
													  List *columns);
static JsonFuncExpr *transformJsonTableColumn(JsonTableColumn *jtc,
											  Node *contextItemExpr,
											  List *passingArgs);
static bool isCompositeType(Oid typid);
static JsonTablePlan *makeJsonTablePathScan(JsonTableParseContext *cxt,
											JsonTablePathSpec *pathspec,
											JsonTablePlanSpec *planspec,
											bool errorOnError,
											int colMin, int colMax,
											JsonTablePlan *childplan);
static void CheckDuplicateColumnOrPathNames(JsonTableParseContext *cxt,
											List *columns);
static bool LookupPathOrColumnName(JsonTableParseContext *cxt, char *name);
static char *generateJsonTablePathName(JsonTableParseContext *cxt);
static void validateJsonTableChildPlan(JsonTableParseContext *cxt,
									   JsonTablePlanSpec *plan,
									   List *columns);
static JsonTablePlan *makeJsonTableSiblingJoin(bool cross,
											   JsonTablePlan *lplan,
											   JsonTablePlan *rplan);
static void
			appendJsonTableColumns(JsonTableParseContext *cxt, List *columns, List *passingArgs);

/*
 * transformJsonTable -
 *			Transform a raw JsonTable into TableFunc
 *
 * Mainly, this transforms the JSON_TABLE() document-generating expression
 * (jt->context_item) and the column-generating expressions (jt->columns) to
 * populate TableFunc.docexpr and TableFunc.colvalexprs, respectively. Also,
 * the PASSING values (jt->passing) are transformed and added into
 * TableFunc.passingvalexprs.
 */
ParseNamespaceItem *
transformJsonTable(ParseState *pstate, JsonTable *jt)
{
	TableFunc  *tf;
	JsonFuncExpr *jfe;
	JsonExpr   *je;
	JsonTablePlanSpec *plan = jt->planspec;
	JsonTablePathSpec *rootPathSpec = jt->pathspec;
	bool		is_lateral;
	JsonTableParseContext cxt = {pstate};

	Assert(IsA(rootPathSpec->string, A_Const) &&
		   castNode(A_Const, rootPathSpec->string)->val.node.type == T_String);

	if (jt->on_error &&
		jt->on_error->btype != JSON_BEHAVIOR_ERROR &&
		jt->on_error->btype != JSON_BEHAVIOR_EMPTY &&
		jt->on_error->btype != JSON_BEHAVIOR_EMPTY_ARRAY)
		ereport(ERROR,
				errcode(ERRCODE_SYNTAX_ERROR),
				errmsg("invalid %s behavior", "ON ERROR"),
				errdetail("Only EMPTY [ ARRAY ] or ERROR is allowed in the top-level ON ERROR clause."),
				parser_errposition(pstate, jt->on_error->location));

	cxt.pathNameId = 0;

	/*
	 * Collect the user-supplied path and column names, checking that they are
	 * distinct.  If the row pattern path has an explicit name, it shares this
	 * namespace, so seed the list with it.
	 */
	if (rootPathSpec->name != NULL)
		cxt.pathNames = list_make1(rootPathSpec->name);
	CheckDuplicateColumnOrPathNames(&cxt, jt->columns);

	/*
	 * Generate a name for the row pattern path if it was not given one.  Path
	 * names are optional for every path, including when a PLAN clause is
	 * present; a specific PLAN() can only reference named paths, so an
	 * unnamed path that the plan must mention is caught later as a path name
	 * mismatch or a path not covered by the plan.  We generate the name only
	 * after collecting the user-supplied names above, so that it cannot
	 * collide with any of them.
	 */
	if (rootPathSpec->name == NULL)
		rootPathSpec->name = generateJsonTablePathName(&cxt);

	/*
	 * We make lateral_only names of this level visible, whether or not the
	 * RangeTableFunc is explicitly marked LATERAL.  This is needed for SQL
	 * spec compliance and seems useful on convenience grounds for all
	 * functions in FROM.
	 *
	 * (LATERAL can't nest within a single pstate level, so we don't need
	 * save/restore logic here.)
	 */
	Assert(!pstate->p_lateral_active);
	pstate->p_lateral_active = true;

	tf = makeNode(TableFunc);
	tf->functype = TFT_JSON_TABLE;

	/*
	 * Transform JsonFuncExpr representing the top JSON_TABLE context_item and
	 * pathspec into a dummy JSON_TABLE_OP JsonExpr.
	 */
	jfe = makeNode(JsonFuncExpr);
	jfe->op = JSON_TABLE_OP;
	jfe->context_item = jt->context_item;
	jfe->pathspec = (Node *) rootPathSpec->string;
	jfe->passing = jt->passing;
	jfe->on_empty = NULL;
	jfe->on_error = jt->on_error;
	jfe->location = jt->location;
	tf->docexpr = transformExpr(pstate, (Node *) jfe, EXPR_KIND_FROM_FUNCTION);

	/*
	 * Create a JsonTablePlan that will generate row pattern that becomes
	 * source data for JSON path expressions in jt->columns.  This also adds
	 * the columns' transformed JsonExpr nodes into tf->colvalexprs.
	 */
	cxt.jt = jt;
	cxt.tf = tf;
	tf->plan = (Node *) transformJsonTableColumns(&cxt, plan, jt->columns,
												  jt->passing,
												  rootPathSpec);

	/*
	 * Copy the transformed PASSING arguments into the TableFunc node, because
	 * they are evaluated separately from the JsonExpr that we just put in
	 * TableFunc.docexpr.  JsonExpr.passing_values is still kept around for
	 * get_json_table().
	 */
	je = (JsonExpr *) tf->docexpr;
	tf->passingvalexprs = copyObject(je->passing_values);

	tf->ordinalitycol = -1;		/* undefine ordinality column number */
	tf->location = jt->location;

	pstate->p_lateral_active = false;

	/*
	 * Mark the RTE as LATERAL if the user said LATERAL explicitly, or if
	 * there are any lateral cross-references in it.
	 */
	is_lateral = jt->lateral || contain_vars_of_level((Node *) tf, 0);

	return addRangeTableEntryForTableFunc(pstate,
										  tf, jt->alias, is_lateral, true);
}

/*
 * Check if a column / path name is duplicated in the given shared list of
 * names.
 */
static void
CheckDuplicateColumnOrPathNames(JsonTableParseContext *cxt,
								List *columns)
{
	ListCell   *lc1;

	foreach(lc1, columns)
	{
		JsonTableColumn *jtc = castNode(JsonTableColumn, lfirst(lc1));

		if (jtc->coltype == JTC_NESTED)
		{
			if (jtc->pathspec->name)
			{
				if (LookupPathOrColumnName(cxt, jtc->pathspec->name))
					ereport(ERROR,
							errcode(ERRCODE_DUPLICATE_ALIAS),
							errmsg("duplicate JSON_TABLE column or path name: %s",
								   jtc->pathspec->name),
							parser_errposition(cxt->pstate,
											   jtc->pathspec->name_location));
				cxt->pathNames = lappend(cxt->pathNames, jtc->pathspec->name);
			}

			CheckDuplicateColumnOrPathNames(cxt, jtc->columns);
		}
		else
		{
			if (LookupPathOrColumnName(cxt, jtc->name))
				ereport(ERROR,
						errcode(ERRCODE_DUPLICATE_ALIAS),
						errmsg("duplicate JSON_TABLE column or path name: %s",
							   jtc->name),
						parser_errposition(cxt->pstate, jtc->location));
			cxt->pathNames = lappend(cxt->pathNames, jtc->name);
		}
	}
}

/*
 * Lookup a column/path name in the given name list, returning true if already
 * there.
 */
static bool
LookupPathOrColumnName(JsonTableParseContext *cxt, char *name)
{
	ListCell   *lc;

	foreach(lc, cxt->pathNames)
	{
		if (strcmp(name, (const char *) lfirst(lc)) == 0)
			return true;
	}

	return false;
}

/* Generate a new unique JSON_TABLE path name. */
static char *
generateJsonTablePathName(JsonTableParseContext *cxt)
{
	char		namebuf[32];
	char	   *name;

	/*
	 * Bump the counter until we produce a name that is not already used as a
	 * path or column name.  Otherwise a generated name could coincide with a
	 * user-supplied one, which would confuse PLAN clause matching and could
	 * silently drop a column.
	 */
	do
	{
		snprintf(namebuf, sizeof(namebuf), "json_table_path_%d",
				 cxt->pathNameId++);
	} while (LookupPathOrColumnName(cxt, namebuf));

	name = pstrdup(namebuf);
	cxt->pathNames = lappend(cxt->pathNames, name);

	return name;
}

/*
 * Create a JsonTablePlan that will supply the source row for 'columns'
 * using 'pathspec' and append the columns' transformed JsonExpr nodes and
 * their type/collation information to cxt->tf.
 */
static JsonTablePlan *
transformJsonTableColumns(JsonTableParseContext *cxt,
						  JsonTablePlanSpec *planspec,
						  List *columns,
						  List *passingArgs,
						  JsonTablePathSpec *pathspec)
{
	JsonTable  *jt = cxt->jt;
	TableFunc  *tf = cxt->tf;
	JsonTablePathScan *scan;
	JsonTablePlanSpec *childPlanSpec;
	bool		defaultPlan = planspec == NULL ||
		planspec->plan_type == JSTP_DEFAULT;
	bool		errorOnError = jt->on_error &&
		jt->on_error->btype == JSON_BEHAVIOR_ERROR;
	int			colMin,
				colMax;
	JsonTablePlan *childplan = NULL;

	/* Start of column range */
	colMin = list_length(tf->colvalexprs);

	if (defaultPlan)
		childPlanSpec = planspec;
	else
	{
		/* validate parent and child plans */
		JsonTablePlanSpec *parentPlanSpec;

		if (planspec->plan_type == JSTP_JOINED)
		{
			if (planspec->join_type != JSTP_JOIN_INNER &&
				planspec->join_type != JSTP_JOIN_OUTER)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("invalid JSON_TABLE plan clause"),
						 errdetail("Expected INNER or OUTER."),
						 parser_errposition(cxt->pstate, planspec->location)));

			parentPlanSpec = planspec->plan1;
			childPlanSpec = planspec->plan2;

			Assert(parentPlanSpec->plan_type != JSTP_JOINED);
			Assert(parentPlanSpec->pathname);
		}
		else
		{
			parentPlanSpec = planspec;
			childPlanSpec = NULL;
		}

		if (strcmp(parentPlanSpec->pathname, pathspec->name) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("invalid JSON_TABLE plan"),
					 errdetail("PATH name mismatch: expected %s but %s is given.",
							   pathspec->name, parentPlanSpec->pathname),
					 parser_errposition(cxt->pstate, planspec->location)));

		validateJsonTableChildPlan(cxt, childPlanSpec, columns);
	}

	appendJsonTableColumns(cxt, columns, passingArgs);

	/* End of column range. */
	if (list_length(tf->colvalexprs) == colMin)
	{
		/* No columns in this Scan beside the nested ones. */
		colMax = colMin = -1;
	}
	else
		colMax = list_length(tf->colvalexprs) - 1;

	if (childPlanSpec || defaultPlan)
	{
		/* transform recursively nested columns */
		childplan = transformJsonTableNestedColumns(cxt, childPlanSpec,
													columns, passingArgs);
	}

	/* transform only non-nested columns */
	scan = (JsonTablePathScan *) makeJsonTablePathScan(cxt, pathspec,
													   planspec,
													   errorOnError,
													   colMin,
													   colMax,
													   childplan);

	return (JsonTablePlan *) scan;
}

/* Append transformed non-nested JSON_TABLE columns to the TableFunc node */
static void
appendJsonTableColumns(JsonTableParseContext *cxt, List *columns, List *passingArgs)
{
	ListCell   *col;
	ParseState *pstate = cxt->pstate;
	TableFunc  *tf = cxt->tf;
	bool		ordinality_found = false;
	Oid			contextItemTypid = exprType(tf->docexpr);

	foreach(col, columns)
	{
		JsonTableColumn *rawc = castNode(JsonTableColumn, lfirst(col));
		Oid			typid;
		int32		typmod;
		Oid			typcoll = InvalidOid;
		Node	   *colexpr;

		if (rawc->name)
			tf->colnames = lappend(tf->colnames,
								   makeString(pstrdup(rawc->name)));

		/*
		 * Determine the type and typmod for the new column. FOR ORDINALITY
		 * columns are INTEGER by standard; the others are user-specified.
		 */
		switch (rawc->coltype)
		{
			case JTC_FOR_ORDINALITY:
				if (ordinality_found)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("only one FOR ORDINALITY column is allowed"),
							 parser_errposition(pstate, rawc->location)));
				ordinality_found = true;
				colexpr = NULL;
				typid = INT4OID;
				typmod = -1;
				break;

			case JTC_REGULAR:
				typenameTypeIdAndMod(pstate, rawc->typeName, &typid, &typmod);

				/*
				 * Use JTC_FORMATTED so as to use JSON_QUERY for this column
				 * if the specified type is one that's better handled using
				 * JSON_QUERY() or if non-default WRAPPER or QUOTES behavior
				 * is specified.
				 */
				if (isCompositeType(typid) ||
					rawc->quotes != JS_QUOTES_UNSPEC ||
					rawc->wrapper != JSW_UNSPEC)
					rawc->coltype = JTC_FORMATTED;

				pg_fallthrough;
			case JTC_FORMATTED:
			case JTC_EXISTS:
				{
					JsonFuncExpr *jfe;
					CaseTestExpr *param = makeNode(CaseTestExpr);

					param->collation = InvalidOid;
					param->typeId = contextItemTypid;
					param->typeMod = -1;

					jfe = transformJsonTableColumn(rawc, (Node *) param,
												   passingArgs);

					colexpr = transformExpr(pstate, (Node *) jfe,
											EXPR_KIND_FROM_FUNCTION);
					assign_expr_collations(pstate, colexpr);

					typid = exprType(colexpr);
					typmod = exprTypmod(colexpr);
					typcoll = exprCollation(colexpr);
					break;
				}

			case JTC_NESTED:
				continue;

			default:
				elog(ERROR, "unknown JSON_TABLE column type: %d", (int) rawc->coltype);
				break;
		}

		tf->coltypes = lappend_oid(tf->coltypes, typid);
		tf->coltypmods = lappend_int(tf->coltypmods, typmod);
		tf->colcollations = lappend_oid(tf->colcollations, typcoll);
		tf->colvalexprs = lappend(tf->colvalexprs, colexpr);
	}
}

/*
 * Check if the type is "composite" for the purpose of checking whether to use
 * JSON_VALUE() or JSON_QUERY() for a given JsonTableColumn.
 */
static bool
isCompositeType(Oid typid)
{
	char		typtype = get_typtype(typid);

	return typid == JSONOID ||
		typid == JSONBOID ||
		typid == RECORDOID ||
		type_is_array(typid) ||
		typtype == TYPTYPE_COMPOSITE ||
	/* domain over one of the above? */
		(typtype == TYPTYPE_DOMAIN &&
		 isCompositeType(getBaseType(typid)));
}

/*
 * Transform JSON_TABLE column definition into a JsonFuncExpr
 * This turns:
 *   - regular column into JSON_VALUE()
 *   - FORMAT JSON column into JSON_QUERY()
 *   - EXISTS column into JSON_EXISTS()
 */
static JsonFuncExpr *
transformJsonTableColumn(JsonTableColumn *jtc, Node *contextItemExpr,
						 List *passingArgs)
{
	Node	   *pathspec;
	JsonFuncExpr *jfexpr = makeNode(JsonFuncExpr);

	if (jtc->coltype == JTC_REGULAR)
		jfexpr->op = JSON_VALUE_OP;
	else if (jtc->coltype == JTC_EXISTS)
		jfexpr->op = JSON_EXISTS_OP;
	else
		jfexpr->op = JSON_QUERY_OP;

	/* Pass the column name so any runtime JsonExpr errors can print it. */
	Assert(jtc->name != NULL);
	jfexpr->column_name = pstrdup(jtc->name);

	jfexpr->context_item = makeJsonValueExpr((Expr *) contextItemExpr, NULL,
											 makeJsonFormat(JS_FORMAT_DEFAULT,
															JS_ENC_DEFAULT,
															-1));
	if (jtc->pathspec)
		pathspec = (Node *) jtc->pathspec->string;
	else
	{
		/* Construct default path as '$."column_name"' */
		StringInfoData path;

		initStringInfo(&path);

		appendStringInfoString(&path, "$.");
		escape_json(&path, jtc->name);

		pathspec = makeStringConst(path.data, -1);
	}
	jfexpr->pathspec = pathspec;
	jfexpr->passing = passingArgs;
	jfexpr->output = makeNode(JsonOutput);
	jfexpr->output->typeName = jtc->typeName;
	jfexpr->output->returning = makeNode(JsonReturning);
	jfexpr->output->returning->format = jtc->format;
	jfexpr->on_empty = jtc->on_empty;
	jfexpr->on_error = jtc->on_error;
	jfexpr->quotes = jtc->quotes;
	jfexpr->wrapper = jtc->wrapper;
	jfexpr->location = jtc->location;

	return jfexpr;
}

static JsonTableColumn *
findNestedJsonTableColumn(List *columns, const char *pathname)
{
	ListCell   *lc;

	foreach(lc, columns)
	{
		JsonTableColumn *jtc = castNode(JsonTableColumn, lfirst(lc));

		if (jtc->coltype == JTC_NESTED &&
			jtc->pathspec->name &&
			!strcmp(jtc->pathspec->name, pathname))
			return jtc;
	}

	return NULL;
}

/*
 * Recursively transform nested columns and create child plan(s) that will be
 * used to evaluate their row patterns.
 *
 * Default plan is transformed into a cross/union join of its nested columns.
 * Simple and outer/inner plans are transformed into a JsonTablePlan by
 * finding and transforming corresponding nested column.
 * Sibling plans are recursively transformed into a JsonTableSiblingJoin.
 */
static JsonTablePlan *
transformJsonTableNestedColumns(JsonTableParseContext *cxt,
								JsonTablePlanSpec *planspec,
								List *columns,
								List *passingArgs)
{
	JsonTableColumn *jtc = NULL;

	if (!planspec || planspec->plan_type == JSTP_DEFAULT)
	{
		/* unspecified or default plan */
		JsonTablePlan *plan = NULL;
		ListCell   *lc;
		bool		cross = planspec && (planspec->join_type & JSTP_JOIN_CROSS);

		/*
		 * If there are multiple NESTED COLUMNS clauses in 'columns', their
		 * respective plans will be combined using a "sibling join" plan,
		 * which effectively does a UNION of the sets of rows coming from each
		 * nested plan.
		 */
		foreach(lc, columns)
		{
			JsonTableColumn *col = castNode(JsonTableColumn, lfirst(lc));
			JsonTablePlan *nested;

			if (col->coltype != JTC_NESTED)
				continue;

			if (col->pathspec->name == NULL)
			{
				col->pathspec->name = generateJsonTablePathName(cxt);
			}

			nested = transformJsonTableColumns(cxt, planspec, col->columns,
											   passingArgs,
											   col->pathspec);

			/* Join nested plan with previous sibling nested plans. */
			if (plan)
				plan = makeJsonTableSiblingJoin(cross, plan, nested);
			else
				plan = nested;
		}

		return plan;
	}
	else if (planspec->plan_type == JSTP_SIMPLE)
	{
		jtc = findNestedJsonTableColumn(columns, planspec->pathname);
	}
	else if (planspec->plan_type == JSTP_JOINED)
	{
		if (planspec->join_type == JSTP_JOIN_INNER ||
			planspec->join_type == JSTP_JOIN_OUTER)
		{
			Assert(planspec->plan1->plan_type == JSTP_SIMPLE);
			jtc = findNestedJsonTableColumn(columns, planspec->plan1->pathname);
		}
		else
		{
			JsonTablePlan *lplan = transformJsonTableNestedColumns(cxt,
																   planspec->plan1,
																   columns,
																   passingArgs);
			JsonTablePlan *rplan = transformJsonTableNestedColumns(cxt,
																   planspec->plan2,
																   columns,
																   passingArgs);

			return makeJsonTableSiblingJoin(planspec->join_type == JSTP_JOIN_CROSS,
											lplan, rplan);
		}
	}
	else
		elog(ERROR, "invalid JSON_TABLE plan type %d", planspec->plan_type);

	/*
	 * The plan's path names were already matched one-to-one against the
	 * nested columns by validateJsonTableChildPlan(), so a nested column with
	 * this path name must exist.
	 */
	Assert(jtc != NULL);

	return transformJsonTableColumns(cxt, planspec, jtc->columns,
									 passingArgs,
									 jtc->pathspec);
}

/*
 * Create transformed JSON_TABLE parent plan node by appending all non-nested
 * columns to the TableFunc node and remembering their indices in the
 * colvalexprs list.
 *
 * colMin and colMax give the range of columns computed by this scan in the
 * global flat list of column expressions that will be passed to the
 * JSON_TABLE's TableFunc.  Both are -1 when all of columns are nested and
 * thus computed by 'childplan'.
 */
static JsonTablePlan *
makeJsonTablePathScan(JsonTableParseContext *cxt, JsonTablePathSpec *pathspec,
					  JsonTablePlanSpec *planspec,
					  bool errorOnError,
					  int colMin, int colMax,
					  JsonTablePlan *childplan)
{
	JsonTablePathScan *scan = makeNode(JsonTablePathScan);
	char	   *pathstring;
	Const	   *value;

	Assert(IsA(pathspec->string, A_Const));
	pathstring = castNode(A_Const, pathspec->string)->val.sval.sval;
	value = makeConst(JSONPATHOID, -1, InvalidOid, -1,
					  DirectFunctionCall1(jsonpath_in,
										  CStringGetDatum(pathstring)),
					  false, false);

	scan->plan.type = T_JsonTablePathScan;
	scan->path = makeJsonTablePath(value, pathspec->name);
	scan->errorOnError = errorOnError;

	scan->child = childplan;

	scan->colMin = colMin;
	scan->colMax = colMax;

	if (scan->child)
		scan->outerJoin = planspec == NULL ||
			(planspec->join_type & JSTP_JOIN_OUTER);
	/* else: default plan case, no children found */

	return (JsonTablePlan *) scan;
}

/*
 * Create a JsonTablePlan that will perform a join of the rows coming from
 * 'lplan' and 'rplan'.
 *
 * The default way of "joining" the rows is to perform a UNION between the
 * sets of rows from 'lplan' and 'rplan'.
 */
static JsonTablePlan *
makeJsonTableSiblingJoin(bool cross, JsonTablePlan *lplan, JsonTablePlan *rplan)
{
	JsonTableSiblingJoin *join = makeNode(JsonTableSiblingJoin);

	join->plan.type = T_JsonTableSiblingJoin;
	join->lplan = lplan;
	join->rplan = rplan;
	join->cross = cross;

	return (JsonTablePlan *) join;
}

/* Collect sibling path names from plan to the specified list. */
static void
collectSiblingPathsInJsonTablePlan(JsonTablePlanSpec *plan, List **paths)
{
	if (plan->plan_type == JSTP_SIMPLE)
		*paths = lappend(*paths, plan->pathname);
	else if (plan->plan_type == JSTP_JOINED)
	{
		if (plan->join_type == JSTP_JOIN_INNER ||
			plan->join_type == JSTP_JOIN_OUTER)
		{
			Assert(plan->plan1->plan_type == JSTP_SIMPLE);
			*paths = lappend(*paths, plan->plan1->pathname);
		}
		else if (plan->join_type == JSTP_JOIN_CROSS ||
				 plan->join_type == JSTP_JOIN_UNION)
		{
			collectSiblingPathsInJsonTablePlan(plan->plan1, paths);
			collectSiblingPathsInJsonTablePlan(plan->plan2, paths);
		}
		else
			elog(ERROR, "invalid JSON_TABLE join type %d",
				 plan->join_type);
	}
}

/*
 * Validate child JSON_TABLE plan by checking that:
 *  - all nested columns have path names specified
 *  - all nested columns have corresponding node in the sibling plan
 *  - plan does not contain duplicate or extra nodes
 */
static void
validateJsonTableChildPlan(JsonTableParseContext *cxt, JsonTablePlanSpec *plan,
						   List *columns)
{
	ParseState *pstate = cxt->pstate;
	ListCell   *lc1;
	List	   *siblings = NIL;
	int			nchildren = 0;

	if (plan)
		collectSiblingPathsInJsonTablePlan(plan, &siblings);

	foreach(lc1, columns)
	{
		JsonTableColumn *jtc = castNode(JsonTableColumn, lfirst(lc1));

		if (jtc->coltype == JTC_NESTED)
		{
			ListCell   *lc2;
			bool		found = false;

			/*
			 * A NESTED path need not be named; generate a name if it was left
			 * unnamed.  A generated name cannot be referenced by a specific
			 * PLAN(), so such a path is reported just below as a nested path
			 * not covered by the plan.
			 */
			if (jtc->pathspec->name == NULL)
				jtc->pathspec->name = generateJsonTablePathName(cxt);

			/* find nested path name in the list of sibling path names */
			foreach(lc2, siblings)
			{
				if ((found = !strcmp(jtc->pathspec->name, lfirst(lc2))))
					break;
			}

			if (!found)
				ereport(ERROR,
						errcode(ERRCODE_SYNTAX_ERROR),
						errmsg("invalid JSON_TABLE specification"),
						errdetail("PLAN clause for nested path %s was not found.",
								  jtc->pathspec->name),
						parser_errposition(pstate, jtc->location));

			nchildren++;
		}
	}

	if (list_length(siblings) > nchildren)
		ereport(ERROR,
				errcode(ERRCODE_SYNTAX_ERROR),
				errmsg("invalid JSON_TABLE plan clause"),
				errdetail("PLAN clause contains some extra or duplicate sibling nodes."),
				parser_errposition(pstate, plan ? plan->location : -1));
}
