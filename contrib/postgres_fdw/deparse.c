/*-------------------------------------------------------------------------
 *
 * deparse.c
 *		  Query deparser for postgres_fdw
 *
 * This file includes functions that examine query WHERE clauses to see
 * whether they're safe to send to the remote server for execution, as
 * well as functions to construct the query text to be sent.  The latter
 * functionality is annoyingly duplicative of ruleutils.c, but there are
 * enough special considerations that it seems best to keep this separate.
 * One saving grace is that we only need deparse logic for node types that
 * we consider safe to send.
 *
 * Portions Copyright (c) 2012-2013, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  contrib/postgres_fdw/deparse.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "postgres_fdw.h"

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/transam.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/var.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/*
 * Context for foreign_expr_walker's search of an expression tree.
 */
typedef struct foreign_expr_cxt
{
	/* Input values */
	PlannerInfo *root;
	RelOptInfo *foreignrel;
	/* Result values */
	List	   *param_numbers;	/* Param IDs of PARAM_EXTERN Params */
} foreign_expr_cxt;

/*
 * Functions to determine whether an expression can be evaluated safely on
 * remote server.
 */
static bool is_foreign_expr(PlannerInfo *root, RelOptInfo *baserel,
				Expr *expr, List **param_numbers);
static bool foreign_expr_walker(Node *node, foreign_expr_cxt *context);
static bool is_builtin(Oid procid);

/*
 * Functions to construct string representation of a node tree.
 */
static void deparseColumnRef(StringInfo buf, int varno, int varattno,
				 PlannerInfo *root);
static void deparseRelation(StringInfo buf, Oid relid);
static void deparseStringLiteral(StringInfo buf, const char *val);
static void deparseExpr(StringInfo buf, Expr *expr, PlannerInfo *root);
static void deparseVar(StringInfo buf, Var *node, PlannerInfo *root);
static void deparseConst(StringInfo buf, Const *node, PlannerInfo *root);
static void deparseParam(StringInfo buf, Param *node, PlannerInfo *root);
static void deparseArrayRef(StringInfo buf, ArrayRef *node, PlannerInfo *root);
static void deparseFuncExpr(StringInfo buf, FuncExpr *node, PlannerInfo *root);
static void deparseOpExpr(StringInfo buf, OpExpr *node, PlannerInfo *root);
static void deparseDistinctExpr(StringInfo buf, DistinctExpr *node,
					PlannerInfo *root);
static void deparseScalarArrayOpExpr(StringInfo buf, ScalarArrayOpExpr *node,
						 PlannerInfo *root);
static void deparseRelabelType(StringInfo buf, RelabelType *node,
				   PlannerInfo *root);
static void deparseBoolExpr(StringInfo buf, BoolExpr *node, PlannerInfo *root);
static void deparseNullTest(StringInfo buf, NullTest *node, PlannerInfo *root);
static void deparseArrayExpr(StringInfo buf, ArrayExpr *node,
				 PlannerInfo *root);


/*
 * Examine each restriction clause in baserel's baserestrictinfo list,
 * and classify them into three groups, which are returned as three lists:
 *	- remote_conds contains expressions that can be evaluated remotely,
 *	  and contain no PARAM_EXTERN Params
 *	- param_conds contains expressions that can be evaluated remotely,
 *	  but contain one or more PARAM_EXTERN Params
 *	- local_conds contains all expressions that can't be evaluated remotely
 *
 * In addition, the fourth output parameter param_numbers receives an integer
 * list of the param IDs of the PARAM_EXTERN Params used in param_conds.
 *
 * The reason for segregating param_conds is mainly that it's difficult to
 * use such conditions in remote EXPLAIN.  We could do it, but unless the
 * planner has been given representative values for all the Params, we'd
 * have to guess at representative values to use in EXPLAIN EXECUTE.
 * So for now we don't include them when doing remote EXPLAIN.
 */
void
classifyConditions(PlannerInfo *root,
				   RelOptInfo *baserel,
				   List **remote_conds,
				   List **param_conds,
				   List **local_conds,
				   List **param_numbers)
{
	ListCell   *lc;

	*remote_conds = NIL;
	*param_conds = NIL;
	*local_conds = NIL;
	*param_numbers = NIL;

	foreach(lc, baserel->baserestrictinfo)
	{
		RestrictInfo *ri = (RestrictInfo *) lfirst(lc);
		List	   *cur_param_numbers;

		if (is_foreign_expr(root, baserel, ri->clause, &cur_param_numbers))
		{
			if (cur_param_numbers == NIL)
				*remote_conds = lappend(*remote_conds, ri);
			else
			{
				*param_conds = lappend(*param_conds, ri);
				/* Use list_concat_unique_int to get rid of duplicates */
				*param_numbers = list_concat_unique_int(*param_numbers,
														cur_param_numbers);
			}
		}
		else
			*local_conds = lappend(*local_conds, ri);
	}
}

/*
 * Returns true if given expr is safe to evaluate on the foreign server.
 *
 * If result is true, we also return a list of param IDs of PARAM_EXTERN
 * Params appearing in the expr into *param_numbers.
 */
static bool
is_foreign_expr(PlannerInfo *root,
				RelOptInfo *baserel,
				Expr *expr,
				List **param_numbers)
{
	foreign_expr_cxt context;

	*param_numbers = NIL;		/* default result */

	/*
	 * Check that the expression consists of nodes that are safe to execute
	 * remotely.
	 */
	context.root = root;
	context.foreignrel = baserel;
	context.param_numbers = NIL;
	if (foreign_expr_walker((Node *) expr, &context))
		return false;

	/*
	 * An expression which includes any mutable functions can't be sent over
	 * because its result is not stable.  For example, sending now() remote
	 * side could cause confusion from clock offsets.  Future versions might
	 * be able to make this choice with more granularity.  (We check this last
	 * because it requires a lot of expensive catalog lookups.)
	 */
	if (contain_mutable_functions((Node *) expr))
		return false;

	/*
	 * OK, so return list of param IDs too.
	 */
	*param_numbers = context.param_numbers;

	return true;
}

/*
 * Return true if expression includes any node that is not safe to execute
 * remotely.  (We use this convention because expression_tree_walker is
 * designed to abort the tree walk as soon as a TRUE result is detected.)
 */
static bool
foreign_expr_walker(Node *node, foreign_expr_cxt *context)
{
	bool		check_type = true;

	if (node == NULL)
		return false;

	switch (nodeTag(node))
	{
		case T_Var:
			{
				/*
				 * Var can be used if it is in the foreign table (we shouldn't
				 * really see anything else in baserestrict clauses, but let's
				 * check anyway).
				 */
				Var		   *var = (Var *) node;

				if (var->varno != context->foreignrel->relid ||
					var->varlevelsup != 0)
					return true;
			}
			break;
		case T_Const:
			/* OK */
			break;
		case T_Param:
			{
				Param	   *p = (Param *) node;

				/*
				 * Only external parameters can be sent to remote.	(XXX This
				 * needs to be improved, but at the point where this code
				 * runs, we should only see PARAM_EXTERN Params anyway.)
				 */
				if (p->paramkind != PARAM_EXTERN)
					return true;

				/*
				 * Report IDs of PARAM_EXTERN Params.  We don't bother to
				 * eliminate duplicate list elements here; classifyConditions
				 * will do that.
				 */
				context->param_numbers = lappend_int(context->param_numbers,
													 p->paramid);
			}
			break;
		case T_ArrayRef:
			{
				ArrayRef   *ar = (ArrayRef *) node;;

				/* Assignment should not be in restrictions. */
				if (ar->refassgnexpr != NULL)
					return true;
			}
			break;
		case T_FuncExpr:
			{
				/*
				 * If function used by the expression is not built-in, it
				 * can't be sent to remote because it might have incompatible
				 * semantics on remote side.
				 */
				FuncExpr   *fe = (FuncExpr *) node;

				if (!is_builtin(fe->funcid))
					return true;
			}
			break;
		case T_OpExpr:
		case T_DistinctExpr:	/* struct-equivalent to OpExpr */
			{
				/*
				 * Similarly, only built-in operators can be sent to remote.
				 * (If the operator is, surely its underlying function is
				 * too.)
				 */
				OpExpr	   *oe = (OpExpr *) node;

				if (!is_builtin(oe->opno))
					return true;
			}
			break;
		case T_ScalarArrayOpExpr:
			{
				/*
				 * Again, only built-in operators can be sent to remote.
				 */
				ScalarArrayOpExpr *oe = (ScalarArrayOpExpr *) node;

				if (!is_builtin(oe->opno))
					return true;
			}
			break;
		case T_RelabelType:
		case T_BoolExpr:
		case T_NullTest:
		case T_ArrayExpr:
			/* OK */
			break;
		case T_List:

			/*
			 * We need only fall through to let expression_tree_walker scan
			 * the list elements --- but don't apply exprType() to the list.
			 */
			check_type = false;
			break;
		default:

			/*
			 * If it's anything else, assume it's unsafe.  This list can be
			 * expanded later, but don't forget to add deparse support below.
			 */
			return true;
	}

	/*
	 * If result type of given expression is not built-in, it can't be sent to
	 * remote because it might have incompatible semantics on remote side.
	 */
	if (check_type && !is_builtin(exprType(node)))
		return true;

	/* Recurse to examine sub-nodes */
	return expression_tree_walker(node, foreign_expr_walker, context);
}

/*
 * Return true if given object is one of PostgreSQL's built-in objects.
 *
 * XXX there is a problem with this, which is that the set of built-in
 * objects expands over time.  Something that is built-in to us might not
 * be known to the remote server, if it's of an older version.  But keeping
 * track of that would be a huge exercise.
 */
static bool
is_builtin(Oid oid)
{
	return (oid < FirstNormalObjectId);
}


/*
 * Construct a simple SELECT statement that retrieves interesting columns
 * of the specified foreign table, and append it to "buf".	The output
 * contains just "SELECT ... FROM tablename".
 *
 * "Interesting" columns are those appearing in the rel's targetlist or
 * in local_conds (conditions which can't be executed remotely).
 */
void
deparseSimpleSql(StringInfo buf,
				 PlannerInfo *root,
				 RelOptInfo *baserel,
				 List *local_conds)
{
	RangeTblEntry *rte = root->simple_rte_array[baserel->relid];
	Bitmapset  *attrs_used = NULL;
	bool		first;
	AttrNumber	attr;
	ListCell   *lc;

	/* Collect all the attributes needed for joins or final output. */
	pull_varattnos((Node *) baserel->reltargetlist, baserel->relid,
				   &attrs_used);

	/* Add all the attributes used by local_conds. */
	foreach(lc, local_conds)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		pull_varattnos((Node *) rinfo->clause, baserel->relid,
					   &attrs_used);
	}

	/*
	 * Construct SELECT list
	 *
	 * We list attributes in order of the foreign table's columns, but replace
	 * any attributes that need not be fetched with NULL constants. (We can't
	 * just omit such attributes, or we'll lose track of which columns are
	 * which at runtime.)  Note however that any dropped columns are ignored.
	 */
	appendStringInfo(buf, "SELECT ");
	first = true;
	for (attr = 1; attr <= baserel->max_attr; attr++)
	{
		/* Ignore dropped attributes. */
		if (get_rte_attribute_is_dropped(rte, attr))
			continue;

		if (!first)
			appendStringInfo(buf, ", ");
		first = false;

		if (bms_is_member(attr - FirstLowInvalidHeapAttributeNumber,
						  attrs_used))
			deparseColumnRef(buf, baserel->relid, attr, root);
		else
			appendStringInfo(buf, "NULL");
	}

	/* Don't generate bad syntax if no undropped columns */
	if (first)
		appendStringInfo(buf, "NULL");

	/*
	 * Construct FROM clause
	 */
	appendStringInfo(buf, " FROM ");
	deparseRelation(buf, rte->relid);
}

/*
 * Deparse WHERE clauses in given list of RestrictInfos and append them to buf.
 *
 * If no WHERE clause already exists in the buffer, is_first should be true.
 */
void
appendWhereClause(StringInfo buf,
				  bool is_first,
				  List *exprs,
				  PlannerInfo *root)
{
	ListCell   *lc;

	foreach(lc, exprs)
	{
		RestrictInfo *ri = (RestrictInfo *) lfirst(lc);

		/* Connect expressions with "AND" and parenthesize each condition. */
		if (is_first)
			appendStringInfo(buf, " WHERE ");
		else
			appendStringInfo(buf, " AND ");

		appendStringInfoChar(buf, '(');
		deparseExpr(buf, ri->clause, root);
		appendStringInfoChar(buf, ')');

		is_first = false;
	}
}

/*
 * Construct SELECT statement to acquire sample rows of given relation.
 *
 * Note: command is appended to whatever might be in buf already.
 */
void
deparseAnalyzeSql(StringInfo buf, Relation rel)
{
	Oid			relid = RelationGetRelid(rel);
	TupleDesc	tupdesc = RelationGetDescr(rel);
	int			i;
	char	   *colname;
	List	   *options;
	ListCell   *lc;
	bool		first = true;

	appendStringInfo(buf, "SELECT ");
	for (i = 0; i < tupdesc->natts; i++)
	{
		/* Ignore dropped columns. */
		if (tupdesc->attrs[i]->attisdropped)
			continue;

		/* Use attribute name or column_name option. */
		colname = NameStr(tupdesc->attrs[i]->attname);
		options = GetForeignColumnOptions(relid, i + 1);

		foreach(lc, options)
		{
			DefElem    *def = (DefElem *) lfirst(lc);

			if (strcmp(def->defname, "column_name") == 0)
			{
				colname = defGetString(def);
				break;
			}
		}

		if (!first)
			appendStringInfo(buf, ", ");
		appendStringInfoString(buf, quote_identifier(colname));
		first = false;
	}

	/* Don't generate bad syntax for zero-column relation. */
	if (first)
		appendStringInfo(buf, "NULL");

	/*
	 * Construct FROM clause
	 */
	appendStringInfo(buf, " FROM ");
	deparseRelation(buf, relid);
}

/*
 * Construct name to use for given column, and emit it into buf.
 * If it has a column_name FDW option, use that instead of attribute name.
 */
static void
deparseColumnRef(StringInfo buf, int varno, int varattno, PlannerInfo *root)
{
	RangeTblEntry *rte;
	char	   *colname = NULL;
	List	   *options;
	ListCell   *lc;

	/* varno must not be any of OUTER_VAR, INNER_VAR and INDEX_VAR. */
	Assert(varno >= 1 && varno <= root->simple_rel_array_size);

	/* Get RangeTblEntry from array in PlannerInfo. */
	rte = root->simple_rte_array[varno];

	/*
	 * If it's a column of a foreign table, and it has the column_name FDW
	 * option, use that value.
	 */
	options = GetForeignColumnOptions(rte->relid, varattno);
	foreach(lc, options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "column_name") == 0)
		{
			colname = defGetString(def);
			break;
		}
	}

	/*
	 * If it's a column of a regular table or it doesn't have column_name FDW
	 * option, use attribute name.
	 */
	if (colname == NULL)
		colname = get_relid_attribute_name(rte->relid, varattno);

	appendStringInfoString(buf, quote_identifier(colname));
}

/*
 * Append remote name of specified foreign table to buf.
 * Use value of table_name FDW option (if any) instead of relation's name.
 * Similarly, schema_name FDW option overrides schema name.
 */
static void
deparseRelation(StringInfo buf, Oid relid)
{
	ForeignTable *table;
	const char *nspname = NULL;
	const char *relname = NULL;
	ListCell   *lc;

	/* obtain additional catalog information. */
	table = GetForeignTable(relid);

	/*
	 * Use value of FDW options if any, instead of the name of object itself.
	 */
	foreach(lc, table->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "schema_name") == 0)
			nspname = defGetString(def);
		else if (strcmp(def->defname, "table_name") == 0)
			relname = defGetString(def);
	}

	if (nspname == NULL)
		nspname = get_namespace_name(get_rel_namespace(relid));
	if (relname == NULL)
		relname = get_rel_name(relid);

	appendStringInfo(buf, "%s.%s",
					 quote_identifier(nspname), quote_identifier(relname));
}

/*
 * Append a SQL string literal representing "val" to buf.
 */
static void
deparseStringLiteral(StringInfo buf, const char *val)
{
	const char *valptr;

	/*
	 * Rather than making assumptions about the remote server's value of
	 * standard_conforming_strings, always use E'foo' syntax if there are any
	 * backslashes.  This will fail on remote servers before 8.1, but those
	 * are long out of support.
	 */
	if (strchr(val, '\\') != NULL)
		appendStringInfoChar(buf, ESCAPE_STRING_SYNTAX);
	appendStringInfoChar(buf, '\'');
	for (valptr = val; *valptr; valptr++)
	{
		char		ch = *valptr;

		if (SQL_STR_DOUBLE(ch, true))
			appendStringInfoChar(buf, ch);
		appendStringInfoChar(buf, ch);
	}
	appendStringInfoChar(buf, '\'');
}

/*
 * Deparse given expression into buf.
 *
 * This function must support all the same node types that foreign_expr_walker
 * accepts.
 *
 * Note: unlike ruleutils.c, we just use a simple hard-wired parenthesization
 * scheme: anything more complex than a Var, Const, function call or cast
 * should be self-parenthesized.
 */
static void
deparseExpr(StringInfo buf, Expr *node, PlannerInfo *root)
{
	if (node == NULL)
		return;

	switch (nodeTag(node))
	{
		case T_Var:
			deparseVar(buf, (Var *) node, root);
			break;
		case T_Const:
			deparseConst(buf, (Const *) node, root);
			break;
		case T_Param:
			deparseParam(buf, (Param *) node, root);
			break;
		case T_ArrayRef:
			deparseArrayRef(buf, (ArrayRef *) node, root);
			break;
		case T_FuncExpr:
			deparseFuncExpr(buf, (FuncExpr *) node, root);
			break;
		case T_OpExpr:
			deparseOpExpr(buf, (OpExpr *) node, root);
			break;
		case T_DistinctExpr:
			deparseDistinctExpr(buf, (DistinctExpr *) node, root);
			break;
		case T_ScalarArrayOpExpr:
			deparseScalarArrayOpExpr(buf, (ScalarArrayOpExpr *) node, root);
			break;
		case T_RelabelType:
			deparseRelabelType(buf, (RelabelType *) node, root);
			break;
		case T_BoolExpr:
			deparseBoolExpr(buf, (BoolExpr *) node, root);
			break;
		case T_NullTest:
			deparseNullTest(buf, (NullTest *) node, root);
			break;
		case T_ArrayExpr:
			deparseArrayExpr(buf, (ArrayExpr *) node, root);
			break;
		default:
			elog(ERROR, "unsupported expression type for deparse: %d",
				 (int) nodeTag(node));
			break;
	}
}

/*
 * Deparse given Var node into buf.
 */
static void
deparseVar(StringInfo buf, Var *node, PlannerInfo *root)
{
	Assert(node->varlevelsup == 0);
	deparseColumnRef(buf, node->varno, node->varattno, root);
}

/*
 * Deparse given constant value into buf.
 *
 * This function has to be kept in sync with ruleutils.c's get_const_expr.
 */
static void
deparseConst(StringInfo buf, Const *node, PlannerInfo *root)
{
	Oid			typoutput;
	bool		typIsVarlena;
	char	   *extval;
	bool		isfloat = false;
	bool		needlabel;

	if (node->constisnull)
	{
		appendStringInfo(buf, "NULL");
		appendStringInfo(buf, "::%s",
						 format_type_with_typemod(node->consttype,
												  node->consttypmod));
		return;
	}

	getTypeOutputInfo(node->consttype,
					  &typoutput, &typIsVarlena);
	extval = OidOutputFunctionCall(typoutput, node->constvalue);

	switch (node->consttype)
	{
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case OIDOID:
		case FLOAT4OID:
		case FLOAT8OID:
		case NUMERICOID:
			{
				/*
				 * No need to quote unless it's a special value such as 'NaN'.
				 * See comments in get_const_expr().
				 */
				if (strspn(extval, "0123456789+-eE.") == strlen(extval))
				{
					if (extval[0] == '+' || extval[0] == '-')
						appendStringInfo(buf, "(%s)", extval);
					else
						appendStringInfoString(buf, extval);
					if (strcspn(extval, "eE.") != strlen(extval))
						isfloat = true; /* it looks like a float */
				}
				else
					appendStringInfo(buf, "'%s'", extval);
			}
			break;
		case BITOID:
		case VARBITOID:
			appendStringInfo(buf, "B'%s'", extval);
			break;
		case BOOLOID:
			if (strcmp(extval, "t") == 0)
				appendStringInfoString(buf, "true");
			else
				appendStringInfoString(buf, "false");
			break;
		default:
			deparseStringLiteral(buf, extval);
			break;
	}

	/*
	 * Append ::typename unless the constant will be implicitly typed as the
	 * right type when it is read in.
	 *
	 * XXX this code has to be kept in sync with the behavior of the parser,
	 * especially make_const.
	 */
	switch (node->consttype)
	{
		case BOOLOID:
		case INT4OID:
		case UNKNOWNOID:
			needlabel = false;
			break;
		case NUMERICOID:
			needlabel = !isfloat || (node->consttypmod >= 0);
			break;
		default:
			needlabel = true;
			break;
	}
	if (needlabel)
		appendStringInfo(buf, "::%s",
						 format_type_with_typemod(node->consttype,
												  node->consttypmod));
}

/*
 * Deparse given Param node into buf.
 *
 * We don't need to renumber the parameter ID, because the executor functions
 * in postgres_fdw.c preserve the numbering of PARAM_EXTERN Params.
 * (This might change soon.)
 */
static void
deparseParam(StringInfo buf, Param *node, PlannerInfo *root)
{
	Assert(node->paramkind == PARAM_EXTERN);
	appendStringInfo(buf, "$%d", node->paramid);
}

/*
 * Deparse an array subscript expression.
 */
static void
deparseArrayRef(StringInfo buf, ArrayRef *node, PlannerInfo *root)
{
	ListCell   *lowlist_item;
	ListCell   *uplist_item;

	/* Always parenthesize the expression. */
	appendStringInfoChar(buf, '(');

	/*
	 * Deparse referenced array expression first.  If that expression includes
	 * a cast, we have to parenthesize to prevent the array subscript from
	 * being taken as typename decoration.	We can avoid that in the typical
	 * case of subscripting a Var, but otherwise do it.
	 */
	if (IsA(node->refexpr, Var))
		deparseExpr(buf, node->refexpr, root);
	else
	{
		appendStringInfoChar(buf, '(');
		deparseExpr(buf, node->refexpr, root);
		appendStringInfoChar(buf, ')');
	}

	/* Deparse subscript expressions. */
	lowlist_item = list_head(node->reflowerindexpr);	/* could be NULL */
	foreach(uplist_item, node->refupperindexpr)
	{
		appendStringInfoChar(buf, '[');
		if (lowlist_item)
		{
			deparseExpr(buf, lfirst(lowlist_item), root);
			appendStringInfoChar(buf, ':');
			lowlist_item = lnext(lowlist_item);
		}
		deparseExpr(buf, lfirst(uplist_item), root);
		appendStringInfoChar(buf, ']');
	}

	appendStringInfoChar(buf, ')');
}

/*
 * Deparse given node which represents a function call into buf.
 *
 * Here not only explicit function calls and explicit casts but also implicit
 * casts are deparsed to avoid problems caused by different cast settings
 * between local and remote.
 *
 * Function name is always qualified by schema name to avoid problems caused
 * by different setting of search_path on remote side.
 */
static void
deparseFuncExpr(StringInfo buf, FuncExpr *node, PlannerInfo *root)
{
	HeapTuple	proctup;
	Form_pg_proc procform;
	const char *proname;
	const char *schemaname;
	bool		use_variadic;
	bool		first;
	ListCell   *arg;

	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(node->funcid));
	if (!HeapTupleIsValid(proctup))
		elog(ERROR, "cache lookup failed for function %u", node->funcid);
	procform = (Form_pg_proc) GETSTRUCT(proctup);
	proname = NameStr(procform->proname);

	/* Check if need to print VARIADIC (cf. ruleutils.c) */
	if (OidIsValid(procform->provariadic))
	{
		if (procform->provariadic != ANYOID)
			use_variadic = true;
		else
			use_variadic = node->funcvariadic;
	}
	else
		use_variadic = false;

	/* Deparse the function name ... */
	schemaname = get_namespace_name(procform->pronamespace);
	appendStringInfo(buf, "%s.%s(",
					 quote_identifier(schemaname),
					 quote_identifier(proname));
	/* ... and all the arguments */
	first = true;
	foreach(arg, node->args)
	{
		if (!first)
			appendStringInfoString(buf, ", ");
		if (use_variadic && lnext(arg) == NULL)
			appendStringInfoString(buf, "VARIADIC ");
		deparseExpr(buf, (Expr *) lfirst(arg), root);
		first = false;
	}
	appendStringInfoChar(buf, ')');

	ReleaseSysCache(proctup);
}

/*
 * Deparse given operator expression into buf.	To avoid problems around
 * priority of operations, we always parenthesize the arguments.  Also we use
 * OPERATOR(schema.operator) notation to determine remote operator exactly.
 */
static void
deparseOpExpr(StringInfo buf, OpExpr *node, PlannerInfo *root)
{
	HeapTuple	tuple;
	Form_pg_operator form;
	const char *opnspname;
	char	   *opname;
	char		oprkind;
	ListCell   *arg;

	/* Retrieve information about the operator from system catalog. */
	tuple = SearchSysCache1(OPEROID, ObjectIdGetDatum(node->opno));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for operator %u", node->opno);
	form = (Form_pg_operator) GETSTRUCT(tuple);

	opnspname = quote_identifier(get_namespace_name(form->oprnamespace));
	/* opname is not a SQL identifier, so we don't need to quote it. */
	opname = NameStr(form->oprname);
	oprkind = form->oprkind;

	/* Sanity check. */
	Assert((oprkind == 'r' && list_length(node->args) == 1) ||
		   (oprkind == 'l' && list_length(node->args) == 1) ||
		   (oprkind == 'b' && list_length(node->args) == 2));

	/* Always parenthesize the expression. */
	appendStringInfoChar(buf, '(');

	/* Deparse left operand. */
	if (oprkind == 'r' || oprkind == 'b')
	{
		arg = list_head(node->args);
		deparseExpr(buf, lfirst(arg), root);
		appendStringInfoChar(buf, ' ');
	}

	/* Deparse fully qualified operator name. */
	appendStringInfo(buf, "OPERATOR(%s.%s)", opnspname, opname);

	/* Deparse right operand. */
	if (oprkind == 'l' || oprkind == 'b')
	{
		arg = list_tail(node->args);
		appendStringInfoChar(buf, ' ');
		deparseExpr(buf, lfirst(arg), root);
	}

	appendStringInfoChar(buf, ')');

	ReleaseSysCache(tuple);
}

/*
 * Deparse IS DISTINCT FROM.
 */
static void
deparseDistinctExpr(StringInfo buf, DistinctExpr *node, PlannerInfo *root)
{
	Assert(list_length(node->args) == 2);

	appendStringInfoChar(buf, '(');
	deparseExpr(buf, linitial(node->args), root);
	appendStringInfo(buf, " IS DISTINCT FROM ");
	deparseExpr(buf, lsecond(node->args), root);
	appendStringInfoChar(buf, ')');
}

/*
 * Deparse given ScalarArrayOpExpr expression into buf.  To avoid problems
 * around priority of operations, we always parenthesize the arguments.  Also
 * we use OPERATOR(schema.operator) notation to determine remote operator
 * exactly.
 */
static void
deparseScalarArrayOpExpr(StringInfo buf,
						 ScalarArrayOpExpr *node,
						 PlannerInfo *root)
{
	HeapTuple	tuple;
	Form_pg_operator form;
	const char *opnspname;
	char	   *opname;
	Expr	   *arg1;
	Expr	   *arg2;

	/* Retrieve information about the operator from system catalog. */
	tuple = SearchSysCache1(OPEROID, ObjectIdGetDatum(node->opno));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for operator %u", node->opno);
	form = (Form_pg_operator) GETSTRUCT(tuple);

	opnspname = quote_identifier(get_namespace_name(form->oprnamespace));
	/* opname is not a SQL identifier, so we don't need to quote it. */
	opname = NameStr(form->oprname);

	/* Sanity check. */
	Assert(list_length(node->args) == 2);

	/* Always parenthesize the expression. */
	appendStringInfoChar(buf, '(');

	/* Deparse left operand. */
	arg1 = linitial(node->args);
	deparseExpr(buf, arg1, root);

	/* Deparse fully qualified operator name plus decoration. */
	appendStringInfo(buf, " OPERATOR(%s.%s) %s (",
					 opnspname, opname, node->useOr ? "ANY" : "ALL");

	/* Deparse right operand. */
	arg2 = lsecond(node->args);
	deparseExpr(buf, arg2, root);

	appendStringInfoChar(buf, ')');

	/* Always parenthesize the expression. */
	appendStringInfoChar(buf, ')');

	ReleaseSysCache(tuple);
}

/*
 * Deparse a RelabelType (binary-compatible cast) node.
 */
static void
deparseRelabelType(StringInfo buf, RelabelType *node, PlannerInfo *root)
{
	deparseExpr(buf, node->arg, root);
	appendStringInfo(buf, "::%s",
					 format_type_with_typemod(node->resulttype,
											  node->resulttypmod));
}

/*
 * Deparse a BoolExpr node.
 *
 * Note: by the time we get here, AND and OR expressions have been flattened
 * into N-argument form, so we'd better be prepared to deal with that.
 */
static void
deparseBoolExpr(StringInfo buf, BoolExpr *node, PlannerInfo *root)
{
	const char *op = NULL;		/* keep compiler quiet */
	bool		first;
	ListCell   *lc;

	switch (node->boolop)
	{
		case AND_EXPR:
			op = "AND";
			break;
		case OR_EXPR:
			op = "OR";
			break;
		case NOT_EXPR:
			appendStringInfo(buf, "(NOT ");
			deparseExpr(buf, linitial(node->args), root);
			appendStringInfoChar(buf, ')');
			return;
	}

	appendStringInfoChar(buf, '(');
	first = true;
	foreach(lc, node->args)
	{
		if (!first)
			appendStringInfo(buf, " %s ", op);
		deparseExpr(buf, (Expr *) lfirst(lc), root);
		first = false;
	}
	appendStringInfoChar(buf, ')');
}

/*
 * Deparse IS [NOT] NULL expression.
 */
static void
deparseNullTest(StringInfo buf, NullTest *node, PlannerInfo *root)
{
	appendStringInfoChar(buf, '(');
	deparseExpr(buf, node->arg, root);
	if (node->nulltesttype == IS_NULL)
		appendStringInfo(buf, " IS NULL)");
	else
		appendStringInfo(buf, " IS NOT NULL)");
}

/*
 * Deparse ARRAY[...] construct.
 */
static void
deparseArrayExpr(StringInfo buf, ArrayExpr *node, PlannerInfo *root)
{
	bool		first = true;
	ListCell   *lc;

	appendStringInfo(buf, "ARRAY[");
	foreach(lc, node->elements)
	{
		if (!first)
			appendStringInfo(buf, ", ");
		deparseExpr(buf, lfirst(lc), root);
		first = false;
	}
	appendStringInfoChar(buf, ']');

	/* If the array is empty, we need an explicit cast to the array type. */
	if (node->elements == NIL)
		appendStringInfo(buf, "::%s",
						 format_type_with_typemod(node->array_typeid, -1));
}
