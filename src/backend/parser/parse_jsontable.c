/*-------------------------------------------------------------------------
 *
 * parse_jsontable.c
 *	  parsing of JSON_TABLE
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_jsontable.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_clause.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
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
												List *columns,
												List *passingArgs,
												JsonTablePathSpec *pathspec);
static JsonTablePlan *transformJsonTableNestedColumns(JsonTableParseContext *cxt,
													  List *passingArgs,
													  List *columns);
static JsonFuncExpr *transformJsonTableColumn(JsonTableColumn *jtc,
											  Node *contextItemExpr,
											  List *passingArgs);
static bool isCompositeType(Oid typid);
static JsonTablePlan *makeJsonTablePathScan(JsonTablePathSpec *pathspec,
											bool errorOnError,
											int colMin, int colMax,
											JsonTablePlan *childplan);
static void CheckDuplicateColumnOrPathNames(JsonTableParseContext *cxt,
											List *columns);
static bool LookupPathOrColumnName(JsonTableParseContext *cxt, char *name);
static char *generateJsonTablePathName(JsonTableParseContext *cxt);
static JsonTablePlan *makeJsonTableSiblingJoin(JsonTablePlan *lplan,
											   JsonTablePlan *rplan);

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
	if (rootPathSpec->name == NULL)
		rootPathSpec->name = generateJsonTablePathName(&cxt);
	cxt.pathNames = list_make1(rootPathSpec->name);
	CheckDuplicateColumnOrPathNames(&cxt, jt->columns);

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
	tf->plan = (Node *) transformJsonTableColumns(&cxt, jt->columns,
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
	char	   *name = namebuf;

	snprintf(namebuf, sizeof(namebuf), "json_table_path_%d",
			 cxt->pathNameId++);

	name = pstrdup(name);
	cxt->pathNames = lappend(cxt->pathNames, name);

	return name;
}

/*
 * Create a JsonTablePlan that will supply the source row for 'columns'
 * using 'pathspec' and append the columns' transformed JsonExpr nodes and
 * their type/collation information to cxt->tf.
 */
static JsonTablePlan *
transformJsonTableColumns(JsonTableParseContext *cxt, List *columns,
						  List *passingArgs,
						  JsonTablePathSpec *pathspec)
{
	ParseState *pstate = cxt->pstate;
	JsonTable  *jt = cxt->jt;
	TableFunc  *tf = cxt->tf;
	ListCell   *col;
	bool		ordinality_found = false;
	bool		errorOnError = jt->on_error &&
		jt->on_error->btype == JSON_BEHAVIOR_ERROR;
	Oid			contextItemTypid = exprType(tf->docexpr);
	int			colMin,
				colMax;
	JsonTablePlan *childplan;

	/* Start of column range */
	colMin = list_length(tf->colvalexprs);

	foreach(col, columns)
	{
		JsonTableColumn *rawc = castNode(JsonTableColumn, lfirst(col));
		Oid			typid;
		int32		typmod;
		Oid			typcoll = InvalidOid;
		Node	   *colexpr;

		if (rawc->coltype != JTC_NESTED)
		{
			Assert(rawc->name);
			tf->colnames = lappend(tf->colnames,
								   makeString(pstrdup(rawc->name)));
		}

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

				/* FALLTHROUGH */
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

	/* End of column range. */
	if (list_length(tf->colvalexprs) == colMin)
	{
		/* No columns in this Scan beside the nested ones. */
		colMax = colMin = -1;
	}
	else
		colMax = list_length(tf->colvalexprs) - 1;

	/* Recursively transform nested columns */
	childplan = transformJsonTableNestedColumns(cxt, passingArgs, columns);

	/* Create a "parent" scan responsible for all columns handled above. */
	return makeJsonTablePathScan(pathspec, errorOnError, colMin, colMax,
								 childplan);
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

/*
 * Recursively transform nested columns and create child plan(s) that will be
 * used to evaluate their row patterns.
 */
static JsonTablePlan *
transformJsonTableNestedColumns(JsonTableParseContext *cxt,
								List *passingArgs,
								List *columns)
{
	JsonTablePlan *plan = NULL;
	ListCell   *lc;

	/*
	 * If there are multiple NESTED COLUMNS clauses in 'columns', their
	 * respective plans will be combined using a "sibling join" plan, which
	 * effectively does a UNION of the sets of rows coming from each nested
	 * plan.
	 */
	foreach(lc, columns)
	{
		JsonTableColumn *jtc = castNode(JsonTableColumn, lfirst(lc));
		JsonTablePlan *nested;

		if (jtc->coltype != JTC_NESTED)
			continue;

		if (jtc->pathspec->name == NULL)
			jtc->pathspec->name = generateJsonTablePathName(cxt);

		nested = transformJsonTableColumns(cxt, jtc->columns, passingArgs,
										   jtc->pathspec);

		if (plan)
			plan = makeJsonTableSiblingJoin(plan, nested);
		else
			plan = nested;
	}

	return plan;
}

/*
 * Create a JsonTablePlan for given path and ON ERROR behavior.
 *
 * colMin and colMin give the range of columns computed by this scan in the
 * global flat list of column expressions that will be passed to the
 * JSON_TABLE's TableFunc.  Both are -1 when all of columns are nested and
 * thus computed by 'childplan'.
 */
static JsonTablePlan *
makeJsonTablePathScan(JsonTablePathSpec *pathspec, bool errorOnError,
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
makeJsonTableSiblingJoin(JsonTablePlan *lplan, JsonTablePlan *rplan)
{
	JsonTableSiblingJoin *join = makeNode(JsonTableSiblingJoin);

	join->plan.type = T_JsonTableSiblingJoin;
	join->lplan = lplan;
	join->rplan = rplan;

	return (JsonTablePlan *) join;
}
