/*-------------------------------------------------------------------------
 *
 * parse_jsontable.c
 *	  parsing of JSON_TABLE
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
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

/* Context for JSON_TABLE transformation */
typedef struct JsonTableContext
{
	ParseState *pstate;				/* parsing state */
	JsonTable  *table;				/* untransformed node */
	TableFunc  *tablefunc;			/* transformed node	*/
	List	   *pathNames;			/* list of all path and columns names */
	Oid			contextItemTypid;	/* type oid of context item (json/jsonb) */
} JsonTableContext;

static JsonTableParent * transformJsonTableColumns(JsonTableContext *cxt,
													   List *columns,
													   char *pathSpec,
													   int location);

static Node *
makeStringConst(char *str, int location)
{
	A_Const *n = makeNode(A_Const);

	n->val.node.type = T_String;
	n->val.sval.sval = str;
	n->location = location;

	return (Node *)n;
}

/*
 * Transform JSON_TABLE column
 *   - regular column into JSON_VALUE()
 *   - FORMAT JSON column into JSON_QUERY()
 *   - EXISTS column into JSON_EXISTS()
 */
static Node *
transformJsonTableColumn(JsonTableColumn *jtc, Node *contextItemExpr,
						 List *passingArgs, bool errorOnError)
{
	JsonFuncExpr *jfexpr = makeNode(JsonFuncExpr);
	JsonCommon *common = makeNode(JsonCommon);
	JsonOutput *output = makeNode(JsonOutput);
	JsonPathSpec pathspec;
	JsonFormat *default_format;

	jfexpr->op =
		jtc->coltype == JTC_REGULAR ? JSON_VALUE_OP :
		jtc->coltype == JTC_EXISTS ? JSON_EXISTS_OP : JSON_QUERY_OP;
	jfexpr->common = common;
	jfexpr->output = output;
	jfexpr->on_empty = jtc->on_empty;
	jfexpr->on_error = jtc->on_error;
	if (!jfexpr->on_error && errorOnError)
		jfexpr->on_error = makeJsonBehavior(JSON_BEHAVIOR_ERROR, NULL);
	jfexpr->omit_quotes = jtc->omit_quotes;
	jfexpr->wrapper = jtc->wrapper;
	jfexpr->location = jtc->location;

	output->typeName = jtc->typeName;
	output->returning = makeNode(JsonReturning);
	output->returning->format = jtc->format;

	default_format = makeJsonFormat(JS_FORMAT_DEFAULT, JS_ENC_DEFAULT, -1);

	common->pathname = NULL;
	common->expr = makeJsonValueExpr((Expr *) contextItemExpr, default_format);
	common->passing = passingArgs;

	if (jtc->pathspec)
		pathspec = jtc->pathspec;
	else
	{
		/* Construct default path as '$."column_name"' */
		StringInfoData path;

		initStringInfo(&path);

		appendStringInfoString(&path, "$.");
		escape_json(&path, jtc->name);

		pathspec = path.data;
	}

	common->pathspec = makeStringConst(pathspec, -1);

	return (Node *) jfexpr;
}

static bool
isJsonTablePathNameDuplicate(JsonTableContext *cxt, const char *pathname)
{
	ListCell *lc;

	foreach(lc, cxt->pathNames)
	{
		if (!strcmp(pathname, (const char *) lfirst(lc)))
			return true;
	}

	return false;
}

/* Register the column name in the path name list. */
static void
registerJsonTableColumn(JsonTableContext *cxt, char *colname)
{
	if (isJsonTablePathNameDuplicate(cxt, colname))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_ALIAS),
				 errmsg("duplicate JSON_TABLE column name: %s", colname),
				 errhint("JSON_TABLE column names must be distinct from one another")));

	cxt->pathNames = lappend(cxt->pathNames, colname);
}

/* Recursively register all nested column names in the path name list. */
static void
registerAllJsonTableColumns(JsonTableContext *cxt, List *columns)
{
	ListCell   *lc;

	foreach(lc, columns)
	{
		JsonTableColumn *jtc = castNode(JsonTableColumn, lfirst(lc));

		if (jtc->coltype == JTC_NESTED)
			registerAllJsonTableColumns(cxt, jtc->columns);
		else
			registerJsonTableColumn(cxt, jtc->name);
	}
}

static Node *
transformNestedJsonTableColumn(JsonTableContext *cxt, JsonTableColumn *jtc)
{
	JsonTableParent *node;

	node = transformJsonTableColumns(cxt, jtc->columns, jtc->pathspec,
									 jtc->location);

	return (Node *) node;
}

static Node *
makeJsonTableSiblingJoin(Node *lnode, Node *rnode)
{
	JsonTableSibling *join = makeNode(JsonTableSibling);

	join->larg = lnode;
	join->rarg = rnode;

	return (Node *) join;
}

/*
 * Recursively transform child (nested) JSON_TABLE columns.
 *
 * Child columns are transformed into a binary tree of union-joined
 * JsonTableSiblings.
 */
static Node *
transformJsonTableChildColumns(JsonTableContext *cxt, List *columns)
{
	Node	   *res = NULL;
	ListCell   *lc;

	/* transform all nested columns into union join */
	foreach(lc, columns)
	{
		JsonTableColumn *jtc = castNode(JsonTableColumn, lfirst(lc));
		Node	   *node;

		if (jtc->coltype != JTC_NESTED)
			continue;

		node = transformNestedJsonTableColumn(cxt, jtc);

		/* join transformed node with previous sibling nodes */
		res = res ? makeJsonTableSiblingJoin(res, node) : node;
	}

	return res;
}

/* Check whether type is json/jsonb, array, or record. */
static bool
typeIsComposite(Oid typid)
{
	char typtype;

	if (typid == JSONOID ||
		typid == JSONBOID ||
		typid == RECORDOID ||
		type_is_array(typid))
		return true;

	typtype = get_typtype(typid);

	if (typtype ==	TYPTYPE_COMPOSITE)
		return true;

	if (typtype == TYPTYPE_DOMAIN)
		return typeIsComposite(getBaseType(typid));

	return false;
}

/* Append transformed non-nested JSON_TABLE columns to the TableFunc node */
static void
appendJsonTableColumns(JsonTableContext *cxt, List *columns)
{
	ListCell   *col;
	ParseState *pstate = cxt->pstate;
	JsonTable  *jt = cxt->table;
	TableFunc  *tf = cxt->tablefunc;
	bool		errorOnError = jt->on_error &&
							   jt->on_error->btype == JSON_BEHAVIOR_ERROR;

	foreach(col, columns)
	{
		JsonTableColumn *rawc = castNode(JsonTableColumn, lfirst(col));
		Oid			typid;
		int32		typmod;
		Node	   *colexpr;

		if (rawc->name)
		{
			/* make sure column names are unique */
			ListCell *colname;

			foreach(colname, tf->colnames)
				if (!strcmp((const char *) colname, rawc->name))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("column name \"%s\" is not unique",
									rawc->name),
							 parser_errposition(pstate, rawc->location)));

			tf->colnames = lappend(tf->colnames,
								   makeString(pstrdup(rawc->name)));
		}

		/*
		 * Determine the type and typmod for the new column. FOR
		 * ORDINALITY columns are INTEGER by standard; the others are
		 * user-specified.
		 */
		switch (rawc->coltype)
		{
			case JTC_FOR_ORDINALITY:
				colexpr = NULL;
				typid = INT4OID;
				typmod = -1;
				break;

			case JTC_REGULAR:
				typenameTypeIdAndMod(pstate, rawc->typeName, &typid, &typmod);

				/*
				 * Use implicit FORMAT JSON for composite types (arrays and
				 * records)
				 */
				if (typeIsComposite(typid))
					rawc->coltype = JTC_FORMATTED;
				else if (rawc->wrapper != JSW_NONE)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("cannot use WITH WRAPPER clause with scalar columns"),
							 parser_errposition(pstate, rawc->location)));
				else if (rawc->omit_quotes)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("cannot use OMIT QUOTES clause with scalar columns"),
							 parser_errposition(pstate, rawc->location)));

				/* FALLTHROUGH */
			case JTC_EXISTS:
			case JTC_FORMATTED:
				{
					Node	   *je;
					CaseTestExpr *param = makeNode(CaseTestExpr);

					param->collation = InvalidOid;
					param->typeId = cxt->contextItemTypid;
					param->typeMod = -1;

					je = transformJsonTableColumn(rawc, (Node *) param,
												  NIL, errorOnError);

					colexpr = transformExpr(pstate, je, EXPR_KIND_FROM_FUNCTION);
					assign_expr_collations(pstate, colexpr);

					typid = exprType(colexpr);
					typmod = exprTypmod(colexpr);
					break;
				}

			case JTC_NESTED:
				continue;

			default:
				elog(ERROR, "unknown JSON_TABLE column type: %d", rawc->coltype);
				break;
		}

		tf->coltypes = lappend_oid(tf->coltypes, typid);
		tf->coltypmods = lappend_int(tf->coltypmods, typmod);
		tf->colcollations = lappend_oid(tf->colcollations,
										type_is_collatable(typid)
											? DEFAULT_COLLATION_OID
											: InvalidOid);
		tf->colvalexprs = lappend(tf->colvalexprs, colexpr);
	}
}

/*
 * Create transformed JSON_TABLE parent plan node by appending all non-nested
 * columns to the TableFunc node and remembering their indices in the
 * colvalexprs list.
 */
static JsonTableParent *
makeParentJsonTableNode(JsonTableContext *cxt, char *pathSpec, List *columns)
{
	JsonTableParent *node = makeNode(JsonTableParent);

	node->path = makeConst(JSONPATHOID, -1, InvalidOid, -1,
						   DirectFunctionCall1(jsonpath_in,
											   CStringGetDatum(pathSpec)),
						   false, false);

	/* save start of column range */
	node->colMin = list_length(cxt->tablefunc->colvalexprs);

	appendJsonTableColumns(cxt, columns);

	/* save end of column range */
	node->colMax = list_length(cxt->tablefunc->colvalexprs) - 1;

	node->errorOnError =
		cxt->table->on_error &&
		cxt->table->on_error->btype == JSON_BEHAVIOR_ERROR;

	return node;
}

static JsonTableParent *
transformJsonTableColumns(JsonTableContext *cxt, List *columns, char *pathSpec,
						  int location)
{
	JsonTableParent *node;

	/* transform only non-nested columns */
	node = makeParentJsonTableNode(cxt, pathSpec, columns);

	/* transform recursively nested columns */
	node->child = transformJsonTableChildColumns(cxt, columns);

	return node;
}

/*
 * transformJsonTable -
 *			Transform a raw JsonTable into TableFunc.
 *
 * Transform the document-generating expression, the row-generating expression,
 * the column-generating expressions, and the default value expressions.
 */
ParseNamespaceItem *
transformJsonTable(ParseState *pstate, JsonTable *jt)
{
	JsonTableContext cxt;
	TableFunc  *tf = makeNode(TableFunc);
	JsonFuncExpr *jfe = makeNode(JsonFuncExpr);
	JsonCommon *jscommon;
	char	   *rootPath;
	bool		is_lateral;

	cxt.pstate = pstate;
	cxt.table = jt;
	cxt.tablefunc = tf;
	cxt.pathNames = NIL;

	registerAllJsonTableColumns(&cxt, jt->columns);

	jscommon = copyObject(jt->common);
	jscommon->pathspec = makeStringConst(pstrdup("$"), -1);

	jfe->op = JSON_TABLE_OP;
	jfe->common = jscommon;
	jfe->on_error = jt->on_error;
	jfe->location = jt->common->location;

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

	tf->functype = TFT_JSON_TABLE;
	tf->docexpr = transformExpr(pstate, (Node *) jfe, EXPR_KIND_FROM_FUNCTION);

	cxt.contextItemTypid = exprType(tf->docexpr);

	if (!IsA(jt->common->pathspec, A_Const) ||
		castNode(A_Const, jt->common->pathspec)->val.node.type != T_String)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("only string constants supported in JSON_TABLE path specification"),
				 parser_errposition(pstate,
									exprLocation(jt->common->pathspec))));

	rootPath = castNode(A_Const, jt->common->pathspec)->val.sval.sval;

	tf->plan = (Node *) transformJsonTableColumns(&cxt, jt->columns, rootPath,
												  jt->common->location);

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
