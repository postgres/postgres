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
	int			pathNameId;			/* path name id counter */
	Oid			contextItemTypid;	/* type oid of context item (json/jsonb) */
} JsonTableContext;

static JsonTableParent * transformJsonTableColumns(JsonTableContext *cxt,
												   JsonTablePlan *plan,
												   List *columns,
												   char *pathSpec,
												   char **pathName,
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
		{
			if (jtc->pathname)
				registerJsonTableColumn(cxt, jtc->pathname);

			registerAllJsonTableColumns(cxt, jtc->columns);
		}
		else
		{
			registerJsonTableColumn(cxt, jtc->name);
		}
	}
}

/* Generate a new unique JSON_TABLE path name. */
static char *
generateJsonTablePathName(JsonTableContext *cxt)
{
	char		namebuf[32];
	char	   *name = namebuf;

	do
	{
		snprintf(namebuf, sizeof(namebuf), "json_table_path_%d",
				 ++cxt->pathNameId);
	} while (isJsonTablePathNameDuplicate(cxt, name));

	name = pstrdup(name);
	cxt->pathNames = lappend(cxt->pathNames, name);

	return name;
}

/* Collect sibling path names from plan to the specified list. */
static void
collectSiblingPathsInJsonTablePlan(JsonTablePlan *plan, List **paths)
{
	if (plan->plan_type == JSTP_SIMPLE)
		*paths = lappend(*paths, plan->pathname);
	else if (plan->plan_type == JSTP_JOINED)
	{
		if (plan->join_type == JSTPJ_INNER ||
			plan->join_type == JSTPJ_OUTER)
		{
			Assert(plan->plan1->plan_type == JSTP_SIMPLE);
			*paths = lappend(*paths, plan->plan1->pathname);
		}
		else if (plan->join_type == JSTPJ_CROSS ||
				 plan->join_type == JSTPJ_UNION)
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
validateJsonTableChildPlan(ParseState *pstate, JsonTablePlan *plan,
						   List *columns)
{
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

			if (!jtc->pathname)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("nested JSON_TABLE columns must contain an explicit AS pathname specification if an explicit PLAN clause is used"),
						 parser_errposition(pstate, jtc->location)));

			/* find nested path name in the list of sibling path names */
			foreach(lc2, siblings)
			{
				if ((found = !strcmp(jtc->pathname, lfirst(lc2))))
					break;
			}

			if (!found)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("invalid JSON_TABLE plan"),
						 errdetail("plan node for nested path %s was not found in plan", jtc->pathname),
						 parser_errposition(pstate, jtc->location)));

			nchildren++;
		}
	}

	if (list_length(siblings) > nchildren)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("invalid JSON_TABLE plan"),
				 errdetail("plan node contains some extra or duplicate sibling nodes"),
				 parser_errposition(pstate, plan ? plan->location : -1)));
}

static JsonTableColumn *
findNestedJsonTableColumn(List *columns, const char *pathname)
{
	ListCell   *lc;

	foreach(lc, columns)
	{
		JsonTableColumn *jtc = castNode(JsonTableColumn, lfirst(lc));

		if (jtc->coltype == JTC_NESTED &&
			jtc->pathname &&
			!strcmp(jtc->pathname, pathname))
			return jtc;
	}

	return NULL;
}

static Node *
transformNestedJsonTableColumn(JsonTableContext *cxt, JsonTableColumn *jtc,
							   JsonTablePlan *plan)
{
	JsonTableParent *node;
	char	   *pathname = jtc->pathname;

	node = transformJsonTableColumns(cxt, plan, jtc->columns, jtc->pathspec,
									 &pathname, jtc->location);
	node->name = pstrdup(pathname);

	return (Node *) node;
}

static Node *
makeJsonTableSiblingJoin(bool cross, Node *lnode, Node *rnode)
{
	JsonTableSibling *join = makeNode(JsonTableSibling);

	join->larg = lnode;
	join->rarg = rnode;
	join->cross = cross;

	return (Node *) join;
}

/*
 * Recursively transform child JSON_TABLE plan.
 *
 * Default plan is transformed into a cross/union join of its nested columns.
 * Simple and outer/inner plans are transformed into a JsonTableParent by
 * finding and transforming corresponding nested column.
 * Sibling plans are recursively transformed into a JsonTableSibling.
 */
static Node *
transformJsonTableChildPlan(JsonTableContext *cxt, JsonTablePlan *plan,
							List *columns)
{
	JsonTableColumn *jtc = NULL;

	if (!plan || plan->plan_type == JSTP_DEFAULT)
	{
		/* unspecified or default plan */
		Node	   *res = NULL;
		ListCell   *lc;
		bool		cross = plan && (plan->join_type & JSTPJ_CROSS);

		/* transform all nested columns into cross/union join */
		foreach(lc, columns)
		{
			JsonTableColumn *jtc = castNode(JsonTableColumn, lfirst(lc));
			Node *node;

			if (jtc->coltype != JTC_NESTED)
				continue;

			node = transformNestedJsonTableColumn(cxt, jtc, plan);

			/* join transformed node with previous sibling nodes */
			res = res ? makeJsonTableSiblingJoin(cross, res, node) : node;
		}

		return res;
	}
	else if (plan->plan_type == JSTP_SIMPLE)
	{
		jtc = findNestedJsonTableColumn(columns, plan->pathname);
	}
	else if (plan->plan_type == JSTP_JOINED)
	{
		if (plan->join_type == JSTPJ_INNER ||
			plan->join_type == JSTPJ_OUTER)
		{
			Assert(plan->plan1->plan_type == JSTP_SIMPLE);
			jtc = findNestedJsonTableColumn(columns, plan->plan1->pathname);
		}
		else
		{
			Node	   *node1 =
				transformJsonTableChildPlan(cxt, plan->plan1, columns);
			Node	   *node2 =
				transformJsonTableChildPlan(cxt, plan->plan2, columns);

			return makeJsonTableSiblingJoin(plan->join_type == JSTPJ_CROSS,
											node1, node2);
		}
	}
	else
		elog(ERROR, "invalid JSON_TABLE plan type %d", plan->plan_type);

	if (!jtc)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("invalid JSON_TABLE plan"),
				 errdetail("path name was %s not found in nested columns list",
						   plan->pathname),
				 parser_errposition(cxt->pstate, plan->location)));

	return transformNestedJsonTableColumn(cxt, jtc, plan);
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
transformJsonTableColumns(JsonTableContext *cxt, JsonTablePlan *plan,
						  List *columns, char *pathSpec, char **pathName,
						  int location)
{
	JsonTableParent *node;
	JsonTablePlan *childPlan;
	bool		defaultPlan = !plan || plan->plan_type == JSTP_DEFAULT;

	if (!*pathName)
	{
		if (cxt->table->plan)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("invalid JSON_TABLE expression"),
					 errdetail("JSON_TABLE columns must contain "
							   "explicit AS pathname specification if "
							   "explicit PLAN clause is used"),
					parser_errposition(cxt->pstate, location)));

		*pathName = generateJsonTablePathName(cxt);
	}

	if (defaultPlan)
		childPlan = plan;
	else
	{
		/* validate parent and child plans */
		JsonTablePlan *parentPlan;

		if (plan->plan_type == JSTP_JOINED)
		{
			if (plan->join_type != JSTPJ_INNER &&
				plan->join_type != JSTPJ_OUTER)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("invalid JSON_TABLE plan"),
						 errdetail("expected INNER or OUTER JSON_TABLE plan node"),
						 parser_errposition(cxt->pstate, plan->location)));

			parentPlan = plan->plan1;
			childPlan = plan->plan2;

			Assert(parentPlan->plan_type != JSTP_JOINED);
			Assert(parentPlan->pathname);
		}
		else
		{
			parentPlan = plan;
			childPlan = NULL;
		}

		if (strcmp(parentPlan->pathname, *pathName))
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("invalid JSON_TABLE plan"),
					 errdetail("path name mismatch: expected %s but %s is given",
							   *pathName, parentPlan->pathname),
					 parser_errposition(cxt->pstate, plan->location)));

		validateJsonTableChildPlan(cxt->pstate, childPlan, columns);
	}

	/* transform only non-nested columns */
	node = makeParentJsonTableNode(cxt, pathSpec, columns);
	node->name = pstrdup(*pathName);

	if (childPlan || defaultPlan)
	{
		/* transform recursively nested columns */
		node->child = transformJsonTableChildPlan(cxt, childPlan, columns);
		if (node->child)
			node->outerJoin = !plan || (plan->join_type & JSTPJ_OUTER);
		/* else: default plan case, no children found */
	}

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
	JsonTablePlan *plan = jt->plan;
	JsonCommon *jscommon;
	char	   *rootPathName = jt->common->pathname;
	char	   *rootPath;
	bool		is_lateral;

	cxt.pstate = pstate;
	cxt.table = jt;
	cxt.tablefunc = tf;
	cxt.pathNames = NIL;
	cxt.pathNameId = 0;

	if (rootPathName)
		registerJsonTableColumn(&cxt, rootPathName);

	registerAllJsonTableColumns(&cxt, jt->columns);

#if 0 /* XXX it' unclear from the standard whether root path name is mandatory or not */
	if (plan && plan->plan_type != JSTP_DEFAULT && !rootPathName)
	{
		/* Assign root path name and create corresponding plan node */
		JsonTablePlan *rootNode = makeNode(JsonTablePlan);
		JsonTablePlan *rootPlan = (JsonTablePlan *)
				makeJsonTableJoinedPlan(JSTPJ_OUTER, (Node *) rootNode,
										(Node *) plan, jt->location);

		rootPathName = generateJsonTablePathName(&cxt);

		rootNode->plan_type = JSTP_SIMPLE;
		rootNode->pathname = rootPathName;

		plan = rootPlan;
	}
#endif

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

	tf->plan = (Node *) transformJsonTableColumns(&cxt, plan, jt->columns,
												  rootPath, &rootPathName,
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
