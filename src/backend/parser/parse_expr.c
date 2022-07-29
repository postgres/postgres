/*-------------------------------------------------------------------------
 *
 * parse_expr.c
 *	  handle expressions in parser
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_expr.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/analyze.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "parser/parse_type.h"
#include "parser/parse_agg.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/lsyscache.h"
#include "utils/timestamp.h"
#include "utils/xml.h"


/* GUC parameters */
bool		operator_precedence_warning = false;
bool		Transform_null_equals = false;

/*
 * Node-type groups for operator precedence warnings
 * We use zero for everything not otherwise classified
 */
#define PREC_GROUP_POSTFIX_IS	1	/* postfix IS tests (NullTest, etc) */
#define PREC_GROUP_INFIX_IS		2	/* infix IS (IS DISTINCT FROM, etc) */
#define PREC_GROUP_LESS			3	/* < > */
#define PREC_GROUP_EQUAL		4	/* = */
#define PREC_GROUP_LESS_EQUAL	5	/* <= >= <> */
#define PREC_GROUP_LIKE			6	/* LIKE ILIKE SIMILAR */
#define PREC_GROUP_BETWEEN		7	/* BETWEEN */
#define PREC_GROUP_IN			8	/* IN */
#define PREC_GROUP_NOT_LIKE		9	/* NOT LIKE/ILIKE/SIMILAR */
#define PREC_GROUP_NOT_BETWEEN	10	/* NOT BETWEEN */
#define PREC_GROUP_NOT_IN		11	/* NOT IN */
#define PREC_GROUP_POSTFIX_OP	12	/* generic postfix operators */
#define PREC_GROUP_INFIX_OP		13	/* generic infix operators */
#define PREC_GROUP_PREFIX_OP	14	/* generic prefix operators */

/*
 * Map precedence groupings to old precedence ordering
 *
 * Old precedence order:
 * 1. NOT
 * 2. =
 * 3. < >
 * 4. LIKE ILIKE SIMILAR
 * 5. BETWEEN
 * 6. IN
 * 7. generic postfix Op
 * 8. generic Op, including <= => <>
 * 9. generic prefix Op
 * 10. IS tests (NullTest, BooleanTest, etc)
 *
 * NOT BETWEEN etc map to BETWEEN etc when considered as being on the left,
 * but to NOT when considered as being on the right, because of the buggy
 * precedence handling of those productions in the old grammar.
 */
static const int oldprecedence_l[] = {
	0, 10, 10, 3, 2, 8, 4, 5, 6, 4, 5, 6, 7, 8, 9
};
static const int oldprecedence_r[] = {
	0, 10, 10, 3, 2, 8, 4, 5, 6, 1, 1, 1, 7, 8, 9
};

static Node *transformExprRecurse(ParseState *pstate, Node *expr);
static Node *transformParamRef(ParseState *pstate, ParamRef *pref);
static Node *transformAExprOp(ParseState *pstate, A_Expr *a);
static Node *transformAExprOpAny(ParseState *pstate, A_Expr *a);
static Node *transformAExprOpAll(ParseState *pstate, A_Expr *a);
static Node *transformAExprDistinct(ParseState *pstate, A_Expr *a);
static Node *transformAExprNullIf(ParseState *pstate, A_Expr *a);
static Node *transformAExprOf(ParseState *pstate, A_Expr *a);
static Node *transformAExprIn(ParseState *pstate, A_Expr *a);
static Node *transformAExprBetween(ParseState *pstate, A_Expr *a);
static Node *transformBoolExpr(ParseState *pstate, BoolExpr *a);
static Node *transformFuncCall(ParseState *pstate, FuncCall *fn);
static Node *transformMultiAssignRef(ParseState *pstate, MultiAssignRef *maref);
static Node *transformCaseExpr(ParseState *pstate, CaseExpr *c);
static Node *transformSubLink(ParseState *pstate, SubLink *sublink);
static Node *transformArrayExpr(ParseState *pstate, A_ArrayExpr *a,
								Oid array_type, Oid element_type, int32 typmod);
static Node *transformRowExpr(ParseState *pstate, RowExpr *r, bool allowDefault);
static Node *transformCoalesceExpr(ParseState *pstate, CoalesceExpr *c);
static Node *transformMinMaxExpr(ParseState *pstate, MinMaxExpr *m);
static Node *transformSQLValueFunction(ParseState *pstate,
									   SQLValueFunction *svf);
static Node *transformXmlExpr(ParseState *pstate, XmlExpr *x);
static Node *transformXmlSerialize(ParseState *pstate, XmlSerialize *xs);
static Node *transformBooleanTest(ParseState *pstate, BooleanTest *b);
static Node *transformCurrentOfExpr(ParseState *pstate, CurrentOfExpr *cexpr);
static Node *transformColumnRef(ParseState *pstate, ColumnRef *cref);
static Node *transformWholeRowRef(ParseState *pstate, RangeTblEntry *rte,
								  int location);
static Node *transformIndirection(ParseState *pstate, A_Indirection *ind);
static Node *transformTypeCast(ParseState *pstate, TypeCast *tc);
static Node *transformCollateClause(ParseState *pstate, CollateClause *c);
static Node *make_row_comparison_op(ParseState *pstate, List *opname,
									List *largs, List *rargs, int location);
static Node *make_row_distinct_op(ParseState *pstate, List *opname,
								  RowExpr *lrow, RowExpr *rrow, int location);
static Expr *make_distinct_op(ParseState *pstate, List *opname,
							  Node *ltree, Node *rtree, int location);
static Node *make_nulltest_from_distinct(ParseState *pstate,
										 A_Expr *distincta, Node *arg);
static int	operator_precedence_group(Node *node, const char **nodename);
static void emit_precedence_warnings(ParseState *pstate,
									 int opgroup, const char *opname,
									 Node *lchild, Node *rchild,
									 int location);


/*
 * transformExpr -
 *	  Analyze and transform expressions. Type checking and type casting is
 *	  done here.  This processing converts the raw grammar output into
 *	  expression trees with fully determined semantics.
 */
Node *
transformExpr(ParseState *pstate, Node *expr, ParseExprKind exprKind)
{
	Node	   *result;
	ParseExprKind sv_expr_kind;

	/* Save and restore identity of expression type we're parsing */
	Assert(exprKind != EXPR_KIND_NONE);
	sv_expr_kind = pstate->p_expr_kind;
	pstate->p_expr_kind = exprKind;

	result = transformExprRecurse(pstate, expr);

	pstate->p_expr_kind = sv_expr_kind;

	return result;
}

static Node *
transformExprRecurse(ParseState *pstate, Node *expr)
{
	Node	   *result;

	if (expr == NULL)
		return NULL;

	/* Guard against stack overflow due to overly complex expressions */
	check_stack_depth();

	switch (nodeTag(expr))
	{
		case T_ColumnRef:
			result = transformColumnRef(pstate, (ColumnRef *) expr);
			break;

		case T_ParamRef:
			result = transformParamRef(pstate, (ParamRef *) expr);
			break;

		case T_A_Const:
			{
				A_Const    *con = (A_Const *) expr;
				Value	   *val = &con->val;

				result = (Node *) make_const(pstate, val, con->location);
				break;
			}

		case T_A_Indirection:
			result = transformIndirection(pstate, (A_Indirection *) expr);
			break;

		case T_A_ArrayExpr:
			result = transformArrayExpr(pstate, (A_ArrayExpr *) expr,
										InvalidOid, InvalidOid, -1);
			break;

		case T_TypeCast:
			result = transformTypeCast(pstate, (TypeCast *) expr);
			break;

		case T_CollateClause:
			result = transformCollateClause(pstate, (CollateClause *) expr);
			break;

		case T_A_Expr:
			{
				A_Expr	   *a = (A_Expr *) expr;

				switch (a->kind)
				{
					case AEXPR_OP:
						result = transformAExprOp(pstate, a);
						break;
					case AEXPR_OP_ANY:
						result = transformAExprOpAny(pstate, a);
						break;
					case AEXPR_OP_ALL:
						result = transformAExprOpAll(pstate, a);
						break;
					case AEXPR_DISTINCT:
					case AEXPR_NOT_DISTINCT:
						result = transformAExprDistinct(pstate, a);
						break;
					case AEXPR_NULLIF:
						result = transformAExprNullIf(pstate, a);
						break;
					case AEXPR_OF:
						result = transformAExprOf(pstate, a);
						break;
					case AEXPR_IN:
						result = transformAExprIn(pstate, a);
						break;
					case AEXPR_LIKE:
					case AEXPR_ILIKE:
					case AEXPR_SIMILAR:
						/* we can transform these just like AEXPR_OP */
						result = transformAExprOp(pstate, a);
						break;
					case AEXPR_BETWEEN:
					case AEXPR_NOT_BETWEEN:
					case AEXPR_BETWEEN_SYM:
					case AEXPR_NOT_BETWEEN_SYM:
						result = transformAExprBetween(pstate, a);
						break;
					case AEXPR_PAREN:
						result = transformExprRecurse(pstate, a->lexpr);
						break;
					default:
						elog(ERROR, "unrecognized A_Expr kind: %d", a->kind);
						result = NULL;	/* keep compiler quiet */
						break;
				}
				break;
			}

		case T_BoolExpr:
			result = transformBoolExpr(pstate, (BoolExpr *) expr);
			break;

		case T_FuncCall:
			result = transformFuncCall(pstate, (FuncCall *) expr);
			break;

		case T_MultiAssignRef:
			result = transformMultiAssignRef(pstate, (MultiAssignRef *) expr);
			break;

		case T_GroupingFunc:
			result = transformGroupingFunc(pstate, (GroupingFunc *) expr);
			break;

		case T_NamedArgExpr:
			{
				NamedArgExpr *na = (NamedArgExpr *) expr;

				na->arg = (Expr *) transformExprRecurse(pstate, (Node *) na->arg);
				result = expr;
				break;
			}

		case T_SubLink:
			result = transformSubLink(pstate, (SubLink *) expr);
			break;

		case T_CaseExpr:
			result = transformCaseExpr(pstate, (CaseExpr *) expr);
			break;

		case T_RowExpr:
			result = transformRowExpr(pstate, (RowExpr *) expr, false);
			break;

		case T_CoalesceExpr:
			result = transformCoalesceExpr(pstate, (CoalesceExpr *) expr);
			break;

		case T_MinMaxExpr:
			result = transformMinMaxExpr(pstate, (MinMaxExpr *) expr);
			break;

		case T_SQLValueFunction:
			result = transformSQLValueFunction(pstate,
											   (SQLValueFunction *) expr);
			break;

		case T_XmlExpr:
			result = transformXmlExpr(pstate, (XmlExpr *) expr);
			break;

		case T_XmlSerialize:
			result = transformXmlSerialize(pstate, (XmlSerialize *) expr);
			break;

		case T_NullTest:
			{
				NullTest   *n = (NullTest *) expr;

				if (operator_precedence_warning)
					emit_precedence_warnings(pstate, PREC_GROUP_POSTFIX_IS, "IS",
											 (Node *) n->arg, NULL,
											 n->location);

				n->arg = (Expr *) transformExprRecurse(pstate, (Node *) n->arg);
				/* the argument can be any type, so don't coerce it */
				n->argisrow = type_is_rowtype(exprType((Node *) n->arg));
				result = expr;
				break;
			}

		case T_BooleanTest:
			result = transformBooleanTest(pstate, (BooleanTest *) expr);
			break;

		case T_CurrentOfExpr:
			result = transformCurrentOfExpr(pstate, (CurrentOfExpr *) expr);
			break;

			/*
			 * In all places where DEFAULT is legal, the caller should have
			 * processed it rather than passing it to transformExpr().
			 */
		case T_SetToDefault:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("DEFAULT is not allowed in this context"),
					 parser_errposition(pstate,
										((SetToDefault *) expr)->location)));
			break;

			/*
			 * CaseTestExpr doesn't require any processing; it is only
			 * injected into parse trees in a fully-formed state.
			 *
			 * Ordinarily we should not see a Var here, but it is convenient
			 * for transformJoinUsingClause() to create untransformed operator
			 * trees containing already-transformed Vars.  The best
			 * alternative would be to deconstruct and reconstruct column
			 * references, which seems expensively pointless.  So allow it.
			 */
		case T_CaseTestExpr:
		case T_Var:
			{
				result = (Node *) expr;
				break;
			}

		default:
			/* should not reach here */
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(expr));
			result = NULL;		/* keep compiler quiet */
			break;
	}

	return result;
}

/*
 * helper routine for delivering "column does not exist" error message
 *
 * (Usually we don't have to work this hard, but the general case of field
 * selection from an arbitrary node needs it.)
 */
static void
unknown_attribute(ParseState *pstate, Node *relref, const char *attname,
				  int location)
{
	RangeTblEntry *rte;

	if (IsA(relref, Var) &&
		((Var *) relref)->varattno == InvalidAttrNumber)
	{
		/* Reference the RTE by alias not by actual table name */
		rte = GetRTEByRangeTablePosn(pstate,
									 ((Var *) relref)->varno,
									 ((Var *) relref)->varlevelsup);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column %s.%s does not exist",
						rte->eref->aliasname, attname),
				 parser_errposition(pstate, location)));
	}
	else
	{
		/* Have to do it by reference to the type of the expression */
		Oid			relTypeId = exprType(relref);

		if (ISCOMPLEX(relTypeId))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" not found in data type %s",
							attname, format_type_be(relTypeId)),
					 parser_errposition(pstate, location)));
		else if (relTypeId == RECORDOID)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("could not identify column \"%s\" in record data type",
							attname),
					 parser_errposition(pstate, location)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("column notation .%s applied to type %s, "
							"which is not a composite type",
							attname, format_type_be(relTypeId)),
					 parser_errposition(pstate, location)));
	}
}

static Node *
transformIndirection(ParseState *pstate, A_Indirection *ind)
{
	Node	   *last_srf = pstate->p_last_srf;
	Node	   *result = transformExprRecurse(pstate, ind->arg);
	List	   *subscripts = NIL;
	int			location = exprLocation(result);
	ListCell   *i;

	/*
	 * We have to split any field-selection operations apart from
	 * subscripting.  Adjacent A_Indices nodes have to be treated as a single
	 * multidimensional subscript operation.
	 */
	foreach(i, ind->indirection)
	{
		Node	   *n = lfirst(i);

		if (IsA(n, A_Indices))
			subscripts = lappend(subscripts, n);
		else if (IsA(n, A_Star))
		{
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("row expansion via \"*\" is not supported here"),
					 parser_errposition(pstate, location)));
		}
		else
		{
			Node	   *newresult;

			Assert(IsA(n, String));

			/* process subscripts before this field selection */
			if (subscripts)
				result = (Node *) transformContainerSubscripts(pstate,
															   result,
															   exprType(result),
															   InvalidOid,
															   exprTypmod(result),
															   subscripts,
															   NULL);
			subscripts = NIL;

			newresult = ParseFuncOrColumn(pstate,
										  list_make1(n),
										  list_make1(result),
										  last_srf,
										  NULL,
										  false,
										  location);
			if (newresult == NULL)
				unknown_attribute(pstate, result, strVal(n), location);
			result = newresult;
		}
	}
	/* process trailing subscripts, if any */
	if (subscripts)
		result = (Node *) transformContainerSubscripts(pstate,
													   result,
													   exprType(result),
													   InvalidOid,
													   exprTypmod(result),
													   subscripts,
													   NULL);

	return result;
}

/*
 * Transform a ColumnRef.
 *
 * If you find yourself changing this code, see also ExpandColumnRefStar.
 */
static Node *
transformColumnRef(ParseState *pstate, ColumnRef *cref)
{
	Node	   *node = NULL;
	char	   *nspname = NULL;
	char	   *relname = NULL;
	char	   *colname = NULL;
	RangeTblEntry *rte;
	int			levels_up;
	enum
	{
		CRERR_NO_COLUMN,
		CRERR_NO_RTE,
		CRERR_WRONG_DB,
		CRERR_TOO_MANY
	}			crerr = CRERR_NO_COLUMN;
	const char *err;

	/*
	 * Check to see if the column reference is in an invalid place within the
	 * query.  We allow column references in most places, except in default
	 * expressions and partition bound expressions.
	 */
	err = NULL;
	switch (pstate->p_expr_kind)
	{
		case EXPR_KIND_NONE:
			Assert(false);		/* can't happen */
			break;
		case EXPR_KIND_OTHER:
		case EXPR_KIND_JOIN_ON:
		case EXPR_KIND_JOIN_USING:
		case EXPR_KIND_FROM_SUBSELECT:
		case EXPR_KIND_FROM_FUNCTION:
		case EXPR_KIND_WHERE:
		case EXPR_KIND_POLICY:
		case EXPR_KIND_HAVING:
		case EXPR_KIND_FILTER:
		case EXPR_KIND_WINDOW_PARTITION:
		case EXPR_KIND_WINDOW_ORDER:
		case EXPR_KIND_WINDOW_FRAME_RANGE:
		case EXPR_KIND_WINDOW_FRAME_ROWS:
		case EXPR_KIND_WINDOW_FRAME_GROUPS:
		case EXPR_KIND_SELECT_TARGET:
		case EXPR_KIND_INSERT_TARGET:
		case EXPR_KIND_UPDATE_SOURCE:
		case EXPR_KIND_UPDATE_TARGET:
		case EXPR_KIND_GROUP_BY:
		case EXPR_KIND_ORDER_BY:
		case EXPR_KIND_DISTINCT_ON:
		case EXPR_KIND_LIMIT:
		case EXPR_KIND_OFFSET:
		case EXPR_KIND_RETURNING:
		case EXPR_KIND_VALUES:
		case EXPR_KIND_VALUES_SINGLE:
		case EXPR_KIND_CHECK_CONSTRAINT:
		case EXPR_KIND_DOMAIN_CHECK:
		case EXPR_KIND_FUNCTION_DEFAULT:
		case EXPR_KIND_INDEX_EXPRESSION:
		case EXPR_KIND_INDEX_PREDICATE:
		case EXPR_KIND_ALTER_COL_TRANSFORM:
		case EXPR_KIND_EXECUTE_PARAMETER:
		case EXPR_KIND_TRIGGER_WHEN:
		case EXPR_KIND_PARTITION_EXPRESSION:
		case EXPR_KIND_CALL_ARGUMENT:
		case EXPR_KIND_COPY_WHERE:
		case EXPR_KIND_GENERATED_COLUMN:
			/* okay */
			break;

		case EXPR_KIND_COLUMN_DEFAULT:
			err = _("cannot use column reference in DEFAULT expression");
			break;
		case EXPR_KIND_PARTITION_BOUND:
			err = _("cannot use column reference in partition bound expression");
			break;

			/*
			 * There is intentionally no default: case here, so that the
			 * compiler will warn if we add a new ParseExprKind without
			 * extending this switch.  If we do see an unrecognized value at
			 * runtime, the behavior will be the same as for EXPR_KIND_OTHER,
			 * which is sane anyway.
			 */
	}
	if (err)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg_internal("%s", err),
				 parser_errposition(pstate, cref->location)));

	/*
	 * Give the PreParseColumnRefHook, if any, first shot.  If it returns
	 * non-null then that's all, folks.
	 */
	if (pstate->p_pre_columnref_hook != NULL)
	{
		node = pstate->p_pre_columnref_hook(pstate, cref);
		if (node != NULL)
			return node;
	}

	/*----------
	 * The allowed syntaxes are:
	 *
	 * A		First try to resolve as unqualified column name;
	 *			if no luck, try to resolve as unqualified table name (A.*).
	 * A.B		A is an unqualified table name; B is either a
	 *			column or function name (trying column name first).
	 * A.B.C	schema A, table B, col or func name C.
	 * A.B.C.D	catalog A, schema B, table C, col or func D.
	 * A.*		A is an unqualified table name; means whole-row value.
	 * A.B.*	whole-row value of table B in schema A.
	 * A.B.C.*	whole-row value of table C in schema B in catalog A.
	 *
	 * We do not need to cope with bare "*"; that will only be accepted by
	 * the grammar at the top level of a SELECT list, and transformTargetList
	 * will take care of it before it ever gets here.  Also, "A.*" etc will
	 * be expanded by transformTargetList if they appear at SELECT top level,
	 * so here we are only going to see them as function or operator inputs.
	 *
	 * Currently, if a catalog name is given then it must equal the current
	 * database name; we check it here and then discard it.
	 *----------
	 */
	switch (list_length(cref->fields))
	{
		case 1:
			{
				Node	   *field1 = (Node *) linitial(cref->fields);

				Assert(IsA(field1, String));
				colname = strVal(field1);

				/* Try to identify as an unqualified column */
				node = colNameToVar(pstate, colname, false, cref->location);

				if (node == NULL)
				{
					/*
					 * Not known as a column of any range-table entry.
					 *
					 * Try to find the name as a relation.  Note that only
					 * relations already entered into the rangetable will be
					 * recognized.
					 *
					 * This is a hack for backwards compatibility with
					 * PostQUEL-inspired syntax.  The preferred form now is
					 * "rel.*".
					 */
					rte = refnameRangeTblEntry(pstate, NULL, colname,
											   cref->location,
											   &levels_up);
					if (rte)
						node = transformWholeRowRef(pstate, rte,
													cref->location);
				}
				break;
			}
		case 2:
			{
				Node	   *field1 = (Node *) linitial(cref->fields);
				Node	   *field2 = (Node *) lsecond(cref->fields);

				Assert(IsA(field1, String));
				relname = strVal(field1);

				/* Locate the referenced RTE */
				rte = refnameRangeTblEntry(pstate, nspname, relname,
										   cref->location,
										   &levels_up);
				if (rte == NULL)
				{
					crerr = CRERR_NO_RTE;
					break;
				}

				/* Whole-row reference? */
				if (IsA(field2, A_Star))
				{
					node = transformWholeRowRef(pstate, rte, cref->location);
					break;
				}

				Assert(IsA(field2, String));
				colname = strVal(field2);

				/* Try to identify as a column of the RTE */
				node = scanRTEForColumn(pstate, rte, colname, cref->location,
										0, NULL);
				if (node == NULL)
				{
					/* Try it as a function call on the whole row */
					node = transformWholeRowRef(pstate, rte, cref->location);
					node = ParseFuncOrColumn(pstate,
											 list_make1(makeString(colname)),
											 list_make1(node),
											 pstate->p_last_srf,
											 NULL,
											 false,
											 cref->location);
				}
				break;
			}
		case 3:
			{
				Node	   *field1 = (Node *) linitial(cref->fields);
				Node	   *field2 = (Node *) lsecond(cref->fields);
				Node	   *field3 = (Node *) lthird(cref->fields);

				Assert(IsA(field1, String));
				nspname = strVal(field1);
				Assert(IsA(field2, String));
				relname = strVal(field2);

				/* Locate the referenced RTE */
				rte = refnameRangeTblEntry(pstate, nspname, relname,
										   cref->location,
										   &levels_up);
				if (rte == NULL)
				{
					crerr = CRERR_NO_RTE;
					break;
				}

				/* Whole-row reference? */
				if (IsA(field3, A_Star))
				{
					node = transformWholeRowRef(pstate, rte, cref->location);
					break;
				}

				Assert(IsA(field3, String));
				colname = strVal(field3);

				/* Try to identify as a column of the RTE */
				node = scanRTEForColumn(pstate, rte, colname, cref->location,
										0, NULL);
				if (node == NULL)
				{
					/* Try it as a function call on the whole row */
					node = transformWholeRowRef(pstate, rte, cref->location);
					node = ParseFuncOrColumn(pstate,
											 list_make1(makeString(colname)),
											 list_make1(node),
											 pstate->p_last_srf,
											 NULL,
											 false,
											 cref->location);
				}
				break;
			}
		case 4:
			{
				Node	   *field1 = (Node *) linitial(cref->fields);
				Node	   *field2 = (Node *) lsecond(cref->fields);
				Node	   *field3 = (Node *) lthird(cref->fields);
				Node	   *field4 = (Node *) lfourth(cref->fields);
				char	   *catname;

				Assert(IsA(field1, String));
				catname = strVal(field1);
				Assert(IsA(field2, String));
				nspname = strVal(field2);
				Assert(IsA(field3, String));
				relname = strVal(field3);

				/*
				 * We check the catalog name and then ignore it.
				 */
				if (strcmp(catname, get_database_name(MyDatabaseId)) != 0)
				{
					crerr = CRERR_WRONG_DB;
					break;
				}

				/* Locate the referenced RTE */
				rte = refnameRangeTblEntry(pstate, nspname, relname,
										   cref->location,
										   &levels_up);
				if (rte == NULL)
				{
					crerr = CRERR_NO_RTE;
					break;
				}

				/* Whole-row reference? */
				if (IsA(field4, A_Star))
				{
					node = transformWholeRowRef(pstate, rte, cref->location);
					break;
				}

				Assert(IsA(field4, String));
				colname = strVal(field4);

				/* Try to identify as a column of the RTE */
				node = scanRTEForColumn(pstate, rte, colname, cref->location,
										0, NULL);
				if (node == NULL)
				{
					/* Try it as a function call on the whole row */
					node = transformWholeRowRef(pstate, rte, cref->location);
					node = ParseFuncOrColumn(pstate,
											 list_make1(makeString(colname)),
											 list_make1(node),
											 pstate->p_last_srf,
											 NULL,
											 false,
											 cref->location);
				}
				break;
			}
		default:
			crerr = CRERR_TOO_MANY; /* too many dotted names */
			break;
	}

	/*
	 * Now give the PostParseColumnRefHook, if any, a chance.  We pass the
	 * translation-so-far so that it can throw an error if it wishes in the
	 * case that it has a conflicting interpretation of the ColumnRef. (If it
	 * just translates anyway, we'll throw an error, because we can't undo
	 * whatever effects the preceding steps may have had on the pstate.) If it
	 * returns NULL, use the standard translation, or throw a suitable error
	 * if there is none.
	 */
	if (pstate->p_post_columnref_hook != NULL)
	{
		Node	   *hookresult;

		hookresult = pstate->p_post_columnref_hook(pstate, cref, node);
		if (node == NULL)
			node = hookresult;
		else if (hookresult != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_AMBIGUOUS_COLUMN),
					 errmsg("column reference \"%s\" is ambiguous",
							NameListToString(cref->fields)),
					 parser_errposition(pstate, cref->location)));
	}

	/*
	 * Throw error if no translation found.
	 */
	if (node == NULL)
	{
		switch (crerr)
		{
			case CRERR_NO_COLUMN:
				errorMissingColumn(pstate, relname, colname, cref->location);
				break;
			case CRERR_NO_RTE:
				errorMissingRTE(pstate, makeRangeVar(nspname, relname,
													 cref->location));
				break;
			case CRERR_WRONG_DB:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cross-database references are not implemented: %s",
								NameListToString(cref->fields)),
						 parser_errposition(pstate, cref->location)));
				break;
			case CRERR_TOO_MANY:
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("improper qualified name (too many dotted names): %s",
								NameListToString(cref->fields)),
						 parser_errposition(pstate, cref->location)));
				break;
		}
	}

	return node;
}

static Node *
transformParamRef(ParseState *pstate, ParamRef *pref)
{
	Node	   *result;

	/*
	 * The core parser knows nothing about Params.  If a hook is supplied,
	 * call it.  If not, or if the hook returns NULL, throw a generic error.
	 */
	if (pstate->p_paramref_hook != NULL)
		result = pstate->p_paramref_hook(pstate, pref);
	else
		result = NULL;

	if (result == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_PARAMETER),
				 errmsg("there is no parameter $%d", pref->number),
				 parser_errposition(pstate, pref->location)));

	return result;
}

/* Test whether an a_expr is a plain NULL constant or not */
static bool
exprIsNullConstant(Node *arg)
{
	if (arg && IsA(arg, A_Const))
	{
		A_Const    *con = (A_Const *) arg;

		if (con->val.type == T_Null)
			return true;
	}
	return false;
}

static Node *
transformAExprOp(ParseState *pstate, A_Expr *a)
{
	Node	   *lexpr = a->lexpr;
	Node	   *rexpr = a->rexpr;
	Node	   *result;

	if (operator_precedence_warning)
	{
		int			opgroup;
		const char *opname;

		opgroup = operator_precedence_group((Node *) a, &opname);
		if (opgroup > 0)
			emit_precedence_warnings(pstate, opgroup, opname,
									 lexpr, rexpr,
									 a->location);

		/* Look through AEXPR_PAREN nodes so they don't affect tests below */
		while (lexpr && IsA(lexpr, A_Expr) &&
			   ((A_Expr *) lexpr)->kind == AEXPR_PAREN)
			lexpr = ((A_Expr *) lexpr)->lexpr;
		while (rexpr && IsA(rexpr, A_Expr) &&
			   ((A_Expr *) rexpr)->kind == AEXPR_PAREN)
			rexpr = ((A_Expr *) rexpr)->lexpr;
	}

	/*
	 * Special-case "foo = NULL" and "NULL = foo" for compatibility with
	 * standards-broken products (like Microsoft's).  Turn these into IS NULL
	 * exprs. (If either side is a CaseTestExpr, then the expression was
	 * generated internally from a CASE-WHEN expression, and
	 * transform_null_equals does not apply.)
	 */
	if (Transform_null_equals &&
		list_length(a->name) == 1 &&
		strcmp(strVal(linitial(a->name)), "=") == 0 &&
		(exprIsNullConstant(lexpr) || exprIsNullConstant(rexpr)) &&
		(!IsA(lexpr, CaseTestExpr) &&!IsA(rexpr, CaseTestExpr)))
	{
		NullTest   *n = makeNode(NullTest);

		n->nulltesttype = IS_NULL;
		n->location = a->location;

		if (exprIsNullConstant(lexpr))
			n->arg = (Expr *) rexpr;
		else
			n->arg = (Expr *) lexpr;

		result = transformExprRecurse(pstate, (Node *) n);
	}
	else if (lexpr && IsA(lexpr, RowExpr) &&
			 rexpr && IsA(rexpr, SubLink) &&
			 ((SubLink *) rexpr)->subLinkType == EXPR_SUBLINK)
	{
		/*
		 * Convert "row op subselect" into a ROWCOMPARE sublink. Formerly the
		 * grammar did this, but now that a row construct is allowed anywhere
		 * in expressions, it's easier to do it here.
		 */
		SubLink    *s = (SubLink *) rexpr;

		s->subLinkType = ROWCOMPARE_SUBLINK;
		s->testexpr = lexpr;
		s->operName = a->name;
		s->location = a->location;
		result = transformExprRecurse(pstate, (Node *) s);
	}
	else if (lexpr && IsA(lexpr, RowExpr) &&
			 rexpr && IsA(rexpr, RowExpr))
	{
		/* ROW() op ROW() is handled specially */
		lexpr = transformExprRecurse(pstate, lexpr);
		rexpr = transformExprRecurse(pstate, rexpr);

		result = make_row_comparison_op(pstate,
										a->name,
										castNode(RowExpr, lexpr)->args,
										castNode(RowExpr, rexpr)->args,
										a->location);
	}
	else
	{
		/* Ordinary scalar operator */
		Node	   *last_srf = pstate->p_last_srf;

		lexpr = transformExprRecurse(pstate, lexpr);
		rexpr = transformExprRecurse(pstate, rexpr);

		result = (Node *) make_op(pstate,
								  a->name,
								  lexpr,
								  rexpr,
								  last_srf,
								  a->location);
	}

	return result;
}

static Node *
transformAExprOpAny(ParseState *pstate, A_Expr *a)
{
	Node	   *lexpr = a->lexpr;
	Node	   *rexpr = a->rexpr;

	if (operator_precedence_warning)
		emit_precedence_warnings(pstate, PREC_GROUP_POSTFIX_OP,
								 strVal(llast(a->name)),
								 lexpr, NULL,
								 a->location);

	lexpr = transformExprRecurse(pstate, lexpr);
	rexpr = transformExprRecurse(pstate, rexpr);

	return (Node *) make_scalar_array_op(pstate,
										 a->name,
										 true,
										 lexpr,
										 rexpr,
										 a->location);
}

static Node *
transformAExprOpAll(ParseState *pstate, A_Expr *a)
{
	Node	   *lexpr = a->lexpr;
	Node	   *rexpr = a->rexpr;

	if (operator_precedence_warning)
		emit_precedence_warnings(pstate, PREC_GROUP_POSTFIX_OP,
								 strVal(llast(a->name)),
								 lexpr, NULL,
								 a->location);

	lexpr = transformExprRecurse(pstate, lexpr);
	rexpr = transformExprRecurse(pstate, rexpr);

	return (Node *) make_scalar_array_op(pstate,
										 a->name,
										 false,
										 lexpr,
										 rexpr,
										 a->location);
}

static Node *
transformAExprDistinct(ParseState *pstate, A_Expr *a)
{
	Node	   *lexpr = a->lexpr;
	Node	   *rexpr = a->rexpr;
	Node	   *result;

	if (operator_precedence_warning)
		emit_precedence_warnings(pstate, PREC_GROUP_INFIX_IS, "IS",
								 lexpr, rexpr,
								 a->location);

	/*
	 * If either input is an undecorated NULL literal, transform to a NullTest
	 * on the other input. That's simpler to process than a full DistinctExpr,
	 * and it avoids needing to require that the datatype have an = operator.
	 */
	if (exprIsNullConstant(rexpr))
		return make_nulltest_from_distinct(pstate, a, lexpr);
	if (exprIsNullConstant(lexpr))
		return make_nulltest_from_distinct(pstate, a, rexpr);

	lexpr = transformExprRecurse(pstate, lexpr);
	rexpr = transformExprRecurse(pstate, rexpr);

	if (lexpr && IsA(lexpr, RowExpr) &&
		rexpr && IsA(rexpr, RowExpr))
	{
		/* ROW() op ROW() is handled specially */
		result = make_row_distinct_op(pstate, a->name,
									  (RowExpr *) lexpr,
									  (RowExpr *) rexpr,
									  a->location);
	}
	else
	{
		/* Ordinary scalar operator */
		result = (Node *) make_distinct_op(pstate,
										   a->name,
										   lexpr,
										   rexpr,
										   a->location);
	}

	/*
	 * If it's NOT DISTINCT, we first build a DistinctExpr and then stick a
	 * NOT on top.
	 */
	if (a->kind == AEXPR_NOT_DISTINCT)
		result = (Node *) makeBoolExpr(NOT_EXPR,
									   list_make1(result),
									   a->location);

	return result;
}

static Node *
transformAExprNullIf(ParseState *pstate, A_Expr *a)
{
	Node	   *lexpr = transformExprRecurse(pstate, a->lexpr);
	Node	   *rexpr = transformExprRecurse(pstate, a->rexpr);
	OpExpr	   *result;

	result = (OpExpr *) make_op(pstate,
								a->name,
								lexpr,
								rexpr,
								pstate->p_last_srf,
								a->location);

	/*
	 * The comparison operator itself should yield boolean ...
	 */
	if (result->opresulttype != BOOLOID)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("NULLIF requires = operator to yield boolean"),
				 parser_errposition(pstate, a->location)));
	if (result->opretset)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
		/* translator: %s is name of a SQL construct, eg NULLIF */
				 errmsg("%s must not return a set", "NULLIF"),
				 parser_errposition(pstate, a->location)));

	/*
	 * ... but the NullIfExpr will yield the first operand's type.
	 */
	result->opresulttype = exprType((Node *) linitial(result->args));

	/*
	 * We rely on NullIfExpr and OpExpr being the same struct
	 */
	NodeSetTag(result, T_NullIfExpr);

	return (Node *) result;
}

/*
 * Checking an expression for match to a list of type names. Will result
 * in a boolean constant node.
 */
static Node *
transformAExprOf(ParseState *pstate, A_Expr *a)
{
	Node	   *lexpr = a->lexpr;
	Const	   *result;
	ListCell   *telem;
	Oid			ltype,
				rtype;
	bool		matched = false;

	if (operator_precedence_warning)
		emit_precedence_warnings(pstate, PREC_GROUP_POSTFIX_IS, "IS",
								 lexpr, NULL,
								 a->location);

	lexpr = transformExprRecurse(pstate, lexpr);

	ltype = exprType(lexpr);
	foreach(telem, (List *) a->rexpr)
	{
		rtype = typenameTypeId(pstate, lfirst(telem));
		matched = (rtype == ltype);
		if (matched)
			break;
	}

	/*
	 * We have two forms: equals or not equals. Flip the sense of the result
	 * for not equals.
	 */
	if (strcmp(strVal(linitial(a->name)), "<>") == 0)
		matched = (!matched);

	result = (Const *) makeBoolConst(matched, false);

	/* Make the result have the original input's parse location */
	result->location = exprLocation((Node *) a);

	return (Node *) result;
}

static Node *
transformAExprIn(ParseState *pstate, A_Expr *a)
{
	Node	   *result = NULL;
	Node	   *lexpr;
	List	   *rexprs;
	List	   *rvars;
	List	   *rnonvars;
	bool		useOr;
	ListCell   *l;

	/*
	 * If the operator is <>, combine with AND not OR.
	 */
	if (strcmp(strVal(linitial(a->name)), "<>") == 0)
		useOr = false;
	else
		useOr = true;

	if (operator_precedence_warning)
		emit_precedence_warnings(pstate,
								 useOr ? PREC_GROUP_IN : PREC_GROUP_NOT_IN,
								 "IN",
								 a->lexpr, NULL,
								 a->location);

	/*
	 * We try to generate a ScalarArrayOpExpr from IN/NOT IN, but this is only
	 * possible if there is a suitable array type available.  If not, we fall
	 * back to a boolean condition tree with multiple copies of the lefthand
	 * expression.  Also, any IN-list items that contain Vars are handled as
	 * separate boolean conditions, because that gives the planner more scope
	 * for optimization on such clauses.
	 *
	 * First step: transform all the inputs, and detect whether any contain
	 * Vars.
	 */
	lexpr = transformExprRecurse(pstate, a->lexpr);
	rexprs = rvars = rnonvars = NIL;
	foreach(l, (List *) a->rexpr)
	{
		Node	   *rexpr = transformExprRecurse(pstate, lfirst(l));

		rexprs = lappend(rexprs, rexpr);
		if (contain_vars_of_level(rexpr, 0))
			rvars = lappend(rvars, rexpr);
		else
			rnonvars = lappend(rnonvars, rexpr);
	}

	/*
	 * ScalarArrayOpExpr is only going to be useful if there's more than one
	 * non-Var righthand item.
	 */
	if (list_length(rnonvars) > 1)
	{
		List	   *allexprs;
		Oid			scalar_type;
		Oid			array_type;

		/*
		 * Try to select a common type for the array elements.  Note that
		 * since the LHS' type is first in the list, it will be preferred when
		 * there is doubt (eg, when all the RHS items are unknown literals).
		 *
		 * Note: use list_concat here not lcons, to avoid damaging rnonvars.
		 */
		allexprs = list_concat(list_make1(lexpr), rnonvars);
		scalar_type = select_common_type(pstate, allexprs, NULL, NULL);

		/*
		 * Do we have an array type to use?  Aside from the case where there
		 * isn't one, we don't risk using ScalarArrayOpExpr when the common
		 * type is RECORD, because the RowExpr comparison logic below can cope
		 * with some cases of non-identical row types.
		 */
		if (OidIsValid(scalar_type) && scalar_type != RECORDOID)
			array_type = get_array_type(scalar_type);
		else
			array_type = InvalidOid;
		if (array_type != InvalidOid)
		{
			/*
			 * OK: coerce all the right-hand non-Var inputs to the common type
			 * and build an ArrayExpr for them.
			 */
			List	   *aexprs;
			ArrayExpr  *newa;

			aexprs = NIL;
			foreach(l, rnonvars)
			{
				Node	   *rexpr = (Node *) lfirst(l);

				rexpr = coerce_to_common_type(pstate, rexpr,
											  scalar_type,
											  "IN");
				aexprs = lappend(aexprs, rexpr);
			}
			newa = makeNode(ArrayExpr);
			newa->array_typeid = array_type;
			/* array_collid will be set by parse_collate.c */
			newa->element_typeid = scalar_type;
			newa->elements = aexprs;
			newa->multidims = false;
			newa->location = -1;

			result = (Node *) make_scalar_array_op(pstate,
												   a->name,
												   useOr,
												   lexpr,
												   (Node *) newa,
												   a->location);

			/* Consider only the Vars (if any) in the loop below */
			rexprs = rvars;
		}
	}

	/*
	 * Must do it the hard way, ie, with a boolean expression tree.
	 */
	foreach(l, rexprs)
	{
		Node	   *rexpr = (Node *) lfirst(l);
		Node	   *cmp;

		if (IsA(lexpr, RowExpr) &&
			IsA(rexpr, RowExpr))
		{
			/* ROW() op ROW() is handled specially */
			cmp = make_row_comparison_op(pstate,
										 a->name,
										 copyObject(((RowExpr *) lexpr)->args),
										 ((RowExpr *) rexpr)->args,
										 a->location);
		}
		else
		{
			/* Ordinary scalar operator */
			cmp = (Node *) make_op(pstate,
								   a->name,
								   copyObject(lexpr),
								   rexpr,
								   pstate->p_last_srf,
								   a->location);
		}

		cmp = coerce_to_boolean(pstate, cmp, "IN");
		if (result == NULL)
			result = cmp;
		else
			result = (Node *) makeBoolExpr(useOr ? OR_EXPR : AND_EXPR,
										   list_make2(result, cmp),
										   a->location);
	}

	return result;
}

static Node *
transformAExprBetween(ParseState *pstate, A_Expr *a)
{
	Node	   *aexpr;
	Node	   *bexpr;
	Node	   *cexpr;
	Node	   *result;
	Node	   *sub1;
	Node	   *sub2;
	List	   *args;

	/* Deconstruct A_Expr into three subexprs */
	aexpr = a->lexpr;
	args = castNode(List, a->rexpr);
	Assert(list_length(args) == 2);
	bexpr = (Node *) linitial(args);
	cexpr = (Node *) lsecond(args);

	if (operator_precedence_warning)
	{
		int			opgroup;
		const char *opname;

		opgroup = operator_precedence_group((Node *) a, &opname);
		emit_precedence_warnings(pstate, opgroup, opname,
								 aexpr, cexpr,
								 a->location);
		/* We can ignore bexpr thanks to syntactic restrictions */
		/* Wrap subexpressions to prevent extra warnings */
		aexpr = (Node *) makeA_Expr(AEXPR_PAREN, NIL, aexpr, NULL, -1);
		bexpr = (Node *) makeA_Expr(AEXPR_PAREN, NIL, bexpr, NULL, -1);
		cexpr = (Node *) makeA_Expr(AEXPR_PAREN, NIL, cexpr, NULL, -1);
	}

	/*
	 * Build the equivalent comparison expression.  Make copies of
	 * multiply-referenced subexpressions for safety.  (XXX this is really
	 * wrong since it results in multiple runtime evaluations of what may be
	 * volatile expressions ...)
	 *
	 * Ideally we would not use hard-wired operators here but instead use
	 * opclasses.  However, mixed data types and other issues make this
	 * difficult:
	 * http://archives.postgresql.org/pgsql-hackers/2008-08/msg01142.php
	 */
	switch (a->kind)
	{
		case AEXPR_BETWEEN:
			args = list_make2(makeSimpleA_Expr(AEXPR_OP, ">=",
											   aexpr, bexpr,
											   a->location),
							  makeSimpleA_Expr(AEXPR_OP, "<=",
											   copyObject(aexpr), cexpr,
											   a->location));
			result = (Node *) makeBoolExpr(AND_EXPR, args, a->location);
			break;
		case AEXPR_NOT_BETWEEN:
			args = list_make2(makeSimpleA_Expr(AEXPR_OP, "<",
											   aexpr, bexpr,
											   a->location),
							  makeSimpleA_Expr(AEXPR_OP, ">",
											   copyObject(aexpr), cexpr,
											   a->location));
			result = (Node *) makeBoolExpr(OR_EXPR, args, a->location);
			break;
		case AEXPR_BETWEEN_SYM:
			args = list_make2(makeSimpleA_Expr(AEXPR_OP, ">=",
											   aexpr, bexpr,
											   a->location),
							  makeSimpleA_Expr(AEXPR_OP, "<=",
											   copyObject(aexpr), cexpr,
											   a->location));
			sub1 = (Node *) makeBoolExpr(AND_EXPR, args, a->location);
			args = list_make2(makeSimpleA_Expr(AEXPR_OP, ">=",
											   copyObject(aexpr), copyObject(cexpr),
											   a->location),
							  makeSimpleA_Expr(AEXPR_OP, "<=",
											   copyObject(aexpr), copyObject(bexpr),
											   a->location));
			sub2 = (Node *) makeBoolExpr(AND_EXPR, args, a->location);
			args = list_make2(sub1, sub2);
			result = (Node *) makeBoolExpr(OR_EXPR, args, a->location);
			break;
		case AEXPR_NOT_BETWEEN_SYM:
			args = list_make2(makeSimpleA_Expr(AEXPR_OP, "<",
											   aexpr, bexpr,
											   a->location),
							  makeSimpleA_Expr(AEXPR_OP, ">",
											   copyObject(aexpr), cexpr,
											   a->location));
			sub1 = (Node *) makeBoolExpr(OR_EXPR, args, a->location);
			args = list_make2(makeSimpleA_Expr(AEXPR_OP, "<",
											   copyObject(aexpr), copyObject(cexpr),
											   a->location),
							  makeSimpleA_Expr(AEXPR_OP, ">",
											   copyObject(aexpr), copyObject(bexpr),
											   a->location));
			sub2 = (Node *) makeBoolExpr(OR_EXPR, args, a->location);
			args = list_make2(sub1, sub2);
			result = (Node *) makeBoolExpr(AND_EXPR, args, a->location);
			break;
		default:
			elog(ERROR, "unrecognized A_Expr kind: %d", a->kind);
			result = NULL;		/* keep compiler quiet */
			break;
	}

	return transformExprRecurse(pstate, result);
}

static Node *
transformBoolExpr(ParseState *pstate, BoolExpr *a)
{
	List	   *args = NIL;
	const char *opname;
	ListCell   *lc;

	switch (a->boolop)
	{
		case AND_EXPR:
			opname = "AND";
			break;
		case OR_EXPR:
			opname = "OR";
			break;
		case NOT_EXPR:
			opname = "NOT";
			break;
		default:
			elog(ERROR, "unrecognized boolop: %d", (int) a->boolop);
			opname = NULL;		/* keep compiler quiet */
			break;
	}

	foreach(lc, a->args)
	{
		Node	   *arg = (Node *) lfirst(lc);

		arg = transformExprRecurse(pstate, arg);
		arg = coerce_to_boolean(pstate, arg, opname);
		args = lappend(args, arg);
	}

	return (Node *) makeBoolExpr(a->boolop, args, a->location);
}

static Node *
transformFuncCall(ParseState *pstate, FuncCall *fn)
{
	Node	   *last_srf = pstate->p_last_srf;
	List	   *targs;
	ListCell   *args;

	/* Transform the list of arguments ... */
	targs = NIL;
	foreach(args, fn->args)
	{
		targs = lappend(targs, transformExprRecurse(pstate,
													(Node *) lfirst(args)));
	}

	/*
	 * When WITHIN GROUP is used, we treat its ORDER BY expressions as
	 * additional arguments to the function, for purposes of function lookup
	 * and argument type coercion.  So, transform each such expression and add
	 * them to the targs list.  We don't explicitly mark where each argument
	 * came from, but ParseFuncOrColumn can tell what's what by reference to
	 * list_length(fn->agg_order).
	 */
	if (fn->agg_within_group)
	{
		Assert(fn->agg_order != NIL);
		foreach(args, fn->agg_order)
		{
			SortBy	   *arg = (SortBy *) lfirst(args);

			targs = lappend(targs, transformExpr(pstate, arg->node,
												 EXPR_KIND_ORDER_BY));
		}
	}

	/* ... and hand off to ParseFuncOrColumn */
	return ParseFuncOrColumn(pstate,
							 fn->funcname,
							 targs,
							 last_srf,
							 fn,
							 false,
							 fn->location);
}

static Node *
transformMultiAssignRef(ParseState *pstate, MultiAssignRef *maref)
{
	SubLink    *sublink;
	RowExpr    *rexpr;
	Query	   *qtree;
	TargetEntry *tle;

	/* We should only see this in first-stage processing of UPDATE tlists */
	Assert(pstate->p_expr_kind == EXPR_KIND_UPDATE_SOURCE);

	/* We only need to transform the source if this is the first column */
	if (maref->colno == 1)
	{
		/*
		 * For now, we only allow EXPR SubLinks and RowExprs as the source of
		 * an UPDATE multiassignment.  This is sufficient to cover interesting
		 * cases; at worst, someone would have to write (SELECT * FROM expr)
		 * to expand a composite-returning expression of another form.
		 */
		if (IsA(maref->source, SubLink) &&
			((SubLink *) maref->source)->subLinkType == EXPR_SUBLINK)
		{
			/* Relabel it as a MULTIEXPR_SUBLINK */
			sublink = (SubLink *) maref->source;
			sublink->subLinkType = MULTIEXPR_SUBLINK;
			/* And transform it */
			sublink = (SubLink *) transformExprRecurse(pstate,
													   (Node *) sublink);

			qtree = castNode(Query, sublink->subselect);

			/* Check subquery returns required number of columns */
			if (count_nonjunk_tlist_entries(qtree->targetList) != maref->ncolumns)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("number of columns does not match number of values"),
						 parser_errposition(pstate, sublink->location)));

			/*
			 * Build a resjunk tlist item containing the MULTIEXPR SubLink,
			 * and add it to pstate->p_multiassign_exprs, whence it will later
			 * get appended to the completed targetlist.  We needn't worry
			 * about selecting a resno for it; transformUpdateStmt will do
			 * that.
			 */
			tle = makeTargetEntry((Expr *) sublink, 0, NULL, true);
			pstate->p_multiassign_exprs = lappend(pstate->p_multiassign_exprs,
												  tle);

			/*
			 * Assign a unique-within-this-targetlist ID to the MULTIEXPR
			 * SubLink.  We can just use its position in the
			 * p_multiassign_exprs list.
			 */
			sublink->subLinkId = list_length(pstate->p_multiassign_exprs);
		}
		else if (IsA(maref->source, RowExpr))
		{
			/* Transform the RowExpr, allowing SetToDefault items */
			rexpr = (RowExpr *) transformRowExpr(pstate,
												 (RowExpr *) maref->source,
												 true);

			/* Check it returns required number of columns */
			if (list_length(rexpr->args) != maref->ncolumns)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("number of columns does not match number of values"),
						 parser_errposition(pstate, rexpr->location)));

			/*
			 * Temporarily append it to p_multiassign_exprs, so we can get it
			 * back when we come back here for additional columns.
			 */
			tle = makeTargetEntry((Expr *) rexpr, 0, NULL, true);
			pstate->p_multiassign_exprs = lappend(pstate->p_multiassign_exprs,
												  tle);
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("source for a multiple-column UPDATE item must be a sub-SELECT or ROW() expression"),
					 parser_errposition(pstate, exprLocation(maref->source))));
	}
	else
	{
		/*
		 * Second or later column in a multiassignment.  Re-fetch the
		 * transformed SubLink or RowExpr, which we assume is still the last
		 * entry in p_multiassign_exprs.
		 */
		Assert(pstate->p_multiassign_exprs != NIL);
		tle = (TargetEntry *) llast(pstate->p_multiassign_exprs);
	}

	/*
	 * Emit the appropriate output expression for the current column
	 */
	if (IsA(tle->expr, SubLink))
	{
		Param	   *param;

		sublink = (SubLink *) tle->expr;
		Assert(sublink->subLinkType == MULTIEXPR_SUBLINK);
		qtree = castNode(Query, sublink->subselect);

		/* Build a Param representing the current subquery output column */
		tle = (TargetEntry *) list_nth(qtree->targetList, maref->colno - 1);
		Assert(!tle->resjunk);

		param = makeNode(Param);
		param->paramkind = PARAM_MULTIEXPR;
		param->paramid = (sublink->subLinkId << 16) | maref->colno;
		param->paramtype = exprType((Node *) tle->expr);
		param->paramtypmod = exprTypmod((Node *) tle->expr);
		param->paramcollid = exprCollation((Node *) tle->expr);
		param->location = exprLocation((Node *) tle->expr);

		return (Node *) param;
	}

	if (IsA(tle->expr, RowExpr))
	{
		Node	   *result;

		rexpr = (RowExpr *) tle->expr;

		/* Just extract and return the next element of the RowExpr */
		result = (Node *) list_nth(rexpr->args, maref->colno - 1);

		/*
		 * If we're at the last column, delete the RowExpr from
		 * p_multiassign_exprs; we don't need it anymore, and don't want it in
		 * the finished UPDATE tlist.
		 */
		if (maref->colno == maref->ncolumns)
			pstate->p_multiassign_exprs =
				list_delete_ptr(pstate->p_multiassign_exprs, tle);

		return result;
	}

	elog(ERROR, "unexpected expr type in multiassign list");
	return NULL;				/* keep compiler quiet */
}

static Node *
transformCaseExpr(ParseState *pstate, CaseExpr *c)
{
	CaseExpr   *newc = makeNode(CaseExpr);
	Node	   *last_srf = pstate->p_last_srf;
	Node	   *arg;
	CaseTestExpr *placeholder;
	List	   *newargs;
	List	   *resultexprs;
	ListCell   *l;
	Node	   *defresult;
	Oid			ptype;

	/* transform the test expression, if any */
	arg = transformExprRecurse(pstate, (Node *) c->arg);

	/* generate placeholder for test expression */
	if (arg)
	{
		/*
		 * If test expression is an untyped literal, force it to text. We have
		 * to do something now because we won't be able to do this coercion on
		 * the placeholder.  This is not as flexible as what was done in 7.4
		 * and before, but it's good enough to handle the sort of silly coding
		 * commonly seen.
		 */
		if (exprType(arg) == UNKNOWNOID)
			arg = coerce_to_common_type(pstate, arg, TEXTOID, "CASE");

		/*
		 * Run collation assignment on the test expression so that we know
		 * what collation to mark the placeholder with.  In principle we could
		 * leave it to parse_collate.c to do that later, but propagating the
		 * result to the CaseTestExpr would be unnecessarily complicated.
		 */
		assign_expr_collations(pstate, arg);

		placeholder = makeNode(CaseTestExpr);
		placeholder->typeId = exprType(arg);
		placeholder->typeMod = exprTypmod(arg);
		placeholder->collation = exprCollation(arg);
	}
	else
		placeholder = NULL;

	newc->arg = (Expr *) arg;

	/* transform the list of arguments */
	newargs = NIL;
	resultexprs = NIL;
	foreach(l, c->args)
	{
		CaseWhen   *w = lfirst_node(CaseWhen, l);
		CaseWhen   *neww = makeNode(CaseWhen);
		Node	   *warg;

		warg = (Node *) w->expr;
		if (placeholder)
		{
			/* shorthand form was specified, so expand... */
			warg = (Node *) makeSimpleA_Expr(AEXPR_OP, "=",
											 (Node *) placeholder,
											 warg,
											 w->location);
		}
		neww->expr = (Expr *) transformExprRecurse(pstate, warg);

		neww->expr = (Expr *) coerce_to_boolean(pstate,
												(Node *) neww->expr,
												"CASE/WHEN");

		warg = (Node *) w->result;
		neww->result = (Expr *) transformExprRecurse(pstate, warg);
		neww->location = w->location;

		newargs = lappend(newargs, neww);
		resultexprs = lappend(resultexprs, neww->result);
	}

	newc->args = newargs;

	/* transform the default clause */
	defresult = (Node *) c->defresult;
	if (defresult == NULL)
	{
		A_Const    *n = makeNode(A_Const);

		n->val.type = T_Null;
		n->location = -1;
		defresult = (Node *) n;
	}
	newc->defresult = (Expr *) transformExprRecurse(pstate, defresult);

	/*
	 * Note: default result is considered the most significant type in
	 * determining preferred type. This is how the code worked before, but it
	 * seems a little bogus to me --- tgl
	 */
	resultexprs = lcons(newc->defresult, resultexprs);

	ptype = select_common_type(pstate, resultexprs, "CASE", NULL);
	Assert(OidIsValid(ptype));
	newc->casetype = ptype;
	/* casecollid will be set by parse_collate.c */

	/* Convert default result clause, if necessary */
	newc->defresult = (Expr *)
		coerce_to_common_type(pstate,
							  (Node *) newc->defresult,
							  ptype,
							  "CASE/ELSE");

	/* Convert when-clause results, if necessary */
	foreach(l, newc->args)
	{
		CaseWhen   *w = (CaseWhen *) lfirst(l);

		w->result = (Expr *)
			coerce_to_common_type(pstate,
								  (Node *) w->result,
								  ptype,
								  "CASE/WHEN");
	}

	/* if any subexpression contained a SRF, complain */
	if (pstate->p_last_srf != last_srf)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/* translator: %s is name of a SQL construct, eg GROUP BY */
				 errmsg("set-returning functions are not allowed in %s",
						"CASE"),
				 errhint("You might be able to move the set-returning function into a LATERAL FROM item."),
				 parser_errposition(pstate,
									exprLocation(pstate->p_last_srf))));

	newc->location = c->location;

	return (Node *) newc;
}

static Node *
transformSubLink(ParseState *pstate, SubLink *sublink)
{
	Node	   *result = (Node *) sublink;
	Query	   *qtree;
	const char *err;

	/*
	 * Check to see if the sublink is in an invalid place within the query. We
	 * allow sublinks everywhere in SELECT/INSERT/UPDATE/DELETE, but generally
	 * not in utility statements.
	 */
	err = NULL;
	switch (pstate->p_expr_kind)
	{
		case EXPR_KIND_NONE:
			Assert(false);		/* can't happen */
			break;
		case EXPR_KIND_OTHER:
			/* Accept sublink here; caller must throw error if wanted */
			break;
		case EXPR_KIND_JOIN_ON:
		case EXPR_KIND_JOIN_USING:
		case EXPR_KIND_FROM_SUBSELECT:
		case EXPR_KIND_FROM_FUNCTION:
		case EXPR_KIND_WHERE:
		case EXPR_KIND_POLICY:
		case EXPR_KIND_HAVING:
		case EXPR_KIND_FILTER:
		case EXPR_KIND_WINDOW_PARTITION:
		case EXPR_KIND_WINDOW_ORDER:
		case EXPR_KIND_WINDOW_FRAME_RANGE:
		case EXPR_KIND_WINDOW_FRAME_ROWS:
		case EXPR_KIND_WINDOW_FRAME_GROUPS:
		case EXPR_KIND_SELECT_TARGET:
		case EXPR_KIND_INSERT_TARGET:
		case EXPR_KIND_UPDATE_SOURCE:
		case EXPR_KIND_UPDATE_TARGET:
		case EXPR_KIND_GROUP_BY:
		case EXPR_KIND_ORDER_BY:
		case EXPR_KIND_DISTINCT_ON:
		case EXPR_KIND_LIMIT:
		case EXPR_KIND_OFFSET:
		case EXPR_KIND_RETURNING:
		case EXPR_KIND_VALUES:
		case EXPR_KIND_VALUES_SINGLE:
			/* okay */
			break;
		case EXPR_KIND_CHECK_CONSTRAINT:
		case EXPR_KIND_DOMAIN_CHECK:
			err = _("cannot use subquery in check constraint");
			break;
		case EXPR_KIND_COLUMN_DEFAULT:
		case EXPR_KIND_FUNCTION_DEFAULT:
			err = _("cannot use subquery in DEFAULT expression");
			break;
		case EXPR_KIND_INDEX_EXPRESSION:
			err = _("cannot use subquery in index expression");
			break;
		case EXPR_KIND_INDEX_PREDICATE:
			err = _("cannot use subquery in index predicate");
			break;
		case EXPR_KIND_ALTER_COL_TRANSFORM:
			err = _("cannot use subquery in transform expression");
			break;
		case EXPR_KIND_EXECUTE_PARAMETER:
			err = _("cannot use subquery in EXECUTE parameter");
			break;
		case EXPR_KIND_TRIGGER_WHEN:
			err = _("cannot use subquery in trigger WHEN condition");
			break;
		case EXPR_KIND_PARTITION_BOUND:
			err = _("cannot use subquery in partition bound");
			break;
		case EXPR_KIND_PARTITION_EXPRESSION:
			err = _("cannot use subquery in partition key expression");
			break;
		case EXPR_KIND_CALL_ARGUMENT:
			err = _("cannot use subquery in CALL argument");
			break;
		case EXPR_KIND_COPY_WHERE:
			err = _("cannot use subquery in COPY FROM WHERE condition");
			break;
		case EXPR_KIND_GENERATED_COLUMN:
			err = _("cannot use subquery in column generation expression");
			break;

			/*
			 * There is intentionally no default: case here, so that the
			 * compiler will warn if we add a new ParseExprKind without
			 * extending this switch.  If we do see an unrecognized value at
			 * runtime, the behavior will be the same as for EXPR_KIND_OTHER,
			 * which is sane anyway.
			 */
	}
	if (err)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg_internal("%s", err),
				 parser_errposition(pstate, sublink->location)));

	pstate->p_hasSubLinks = true;

	/*
	 * OK, let's transform the sub-SELECT.
	 */
	qtree = parse_sub_analyze(sublink->subselect, pstate, NULL, false, true);

	/*
	 * Check that we got a SELECT.  Anything else should be impossible given
	 * restrictions of the grammar, but check anyway.
	 */
	if (!IsA(qtree, Query) ||
		qtree->commandType != CMD_SELECT)
		elog(ERROR, "unexpected non-SELECT command in SubLink");

	sublink->subselect = (Node *) qtree;

	if (sublink->subLinkType == EXISTS_SUBLINK)
	{
		/*
		 * EXISTS needs no test expression or combining operator. These fields
		 * should be null already, but make sure.
		 */
		sublink->testexpr = NULL;
		sublink->operName = NIL;
	}
	else if (sublink->subLinkType == EXPR_SUBLINK ||
			 sublink->subLinkType == ARRAY_SUBLINK)
	{
		/*
		 * Make sure the subselect delivers a single column (ignoring resjunk
		 * targets).
		 */
		if (count_nonjunk_tlist_entries(qtree->targetList) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("subquery must return only one column"),
					 parser_errposition(pstate, sublink->location)));

		/*
		 * EXPR and ARRAY need no test expression or combining operator. These
		 * fields should be null already, but make sure.
		 */
		sublink->testexpr = NULL;
		sublink->operName = NIL;
	}
	else if (sublink->subLinkType == MULTIEXPR_SUBLINK)
	{
		/* Same as EXPR case, except no restriction on number of columns */
		sublink->testexpr = NULL;
		sublink->operName = NIL;
	}
	else
	{
		/* ALL, ANY, or ROWCOMPARE: generate row-comparing expression */
		Node	   *lefthand;
		List	   *left_list;
		List	   *right_list;
		ListCell   *l;

		if (operator_precedence_warning)
		{
			if (sublink->operName == NIL)
				emit_precedence_warnings(pstate, PREC_GROUP_IN, "IN",
										 sublink->testexpr, NULL,
										 sublink->location);
			else
				emit_precedence_warnings(pstate, PREC_GROUP_POSTFIX_OP,
										 strVal(llast(sublink->operName)),
										 sublink->testexpr, NULL,
										 sublink->location);
		}

		/*
		 * If the source was "x IN (select)", convert to "x = ANY (select)".
		 */
		if (sublink->operName == NIL)
			sublink->operName = list_make1(makeString("="));

		/*
		 * Transform lefthand expression, and convert to a list
		 */
		lefthand = transformExprRecurse(pstate, sublink->testexpr);
		if (lefthand && IsA(lefthand, RowExpr))
			left_list = ((RowExpr *) lefthand)->args;
		else
			left_list = list_make1(lefthand);

		/*
		 * Build a list of PARAM_SUBLINK nodes representing the output columns
		 * of the subquery.
		 */
		right_list = NIL;
		foreach(l, qtree->targetList)
		{
			TargetEntry *tent = (TargetEntry *) lfirst(l);
			Param	   *param;

			if (tent->resjunk)
				continue;

			param = makeNode(Param);
			param->paramkind = PARAM_SUBLINK;
			param->paramid = tent->resno;
			param->paramtype = exprType((Node *) tent->expr);
			param->paramtypmod = exprTypmod((Node *) tent->expr);
			param->paramcollid = exprCollation((Node *) tent->expr);
			param->location = -1;

			right_list = lappend(right_list, param);
		}

		/*
		 * We could rely on make_row_comparison_op to complain if the list
		 * lengths differ, but we prefer to generate a more specific error
		 * message.
		 */
		if (list_length(left_list) < list_length(right_list))
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("subquery has too many columns"),
					 parser_errposition(pstate, sublink->location)));
		if (list_length(left_list) > list_length(right_list))
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("subquery has too few columns"),
					 parser_errposition(pstate, sublink->location)));

		/*
		 * Identify the combining operator(s) and generate a suitable
		 * row-comparison expression.
		 */
		sublink->testexpr = make_row_comparison_op(pstate,
												   sublink->operName,
												   left_list,
												   right_list,
												   sublink->location);
	}

	return result;
}

/*
 * transformArrayExpr
 *
 * If the caller specifies the target type, the resulting array will
 * be of exactly that type.  Otherwise we try to infer a common type
 * for the elements using select_common_type().
 */
static Node *
transformArrayExpr(ParseState *pstate, A_ArrayExpr *a,
				   Oid array_type, Oid element_type, int32 typmod)
{
	ArrayExpr  *newa = makeNode(ArrayExpr);
	List	   *newelems = NIL;
	List	   *newcoercedelems = NIL;
	ListCell   *element;
	Oid			coerce_type;
	bool		coerce_hard;

	/*
	 * Transform the element expressions
	 *
	 * Assume that the array is one-dimensional unless we find an array-type
	 * element expression.
	 */
	newa->multidims = false;
	foreach(element, a->elements)
	{
		Node	   *e = (Node *) lfirst(element);
		Node	   *newe;

		/* Look through AEXPR_PAREN nodes so they don't affect test below */
		while (e && IsA(e, A_Expr) &&
			   ((A_Expr *) e)->kind == AEXPR_PAREN)
			e = ((A_Expr *) e)->lexpr;

		/*
		 * If an element is itself an A_ArrayExpr, recurse directly so that we
		 * can pass down any target type we were given.
		 */
		if (IsA(e, A_ArrayExpr))
		{
			newe = transformArrayExpr(pstate,
									  (A_ArrayExpr *) e,
									  array_type,
									  element_type,
									  typmod);
			/* we certainly have an array here */
			Assert(array_type == InvalidOid || array_type == exprType(newe));
			newa->multidims = true;
		}
		else
		{
			newe = transformExprRecurse(pstate, e);

			/*
			 * Check for sub-array expressions, if we haven't already found
			 * one.
			 */
			if (!newa->multidims && type_is_array(exprType(newe)))
				newa->multidims = true;
		}

		newelems = lappend(newelems, newe);
	}

	/*
	 * Select a target type for the elements.
	 *
	 * If we haven't been given a target array type, we must try to deduce a
	 * common type based on the types of the individual elements present.
	 */
	if (OidIsValid(array_type))
	{
		/* Caller must ensure array_type matches element_type */
		Assert(OidIsValid(element_type));
		coerce_type = (newa->multidims ? array_type : element_type);
		coerce_hard = true;
	}
	else
	{
		/* Can't handle an empty array without a target type */
		if (newelems == NIL)
			ereport(ERROR,
					(errcode(ERRCODE_INDETERMINATE_DATATYPE),
					 errmsg("cannot determine type of empty array"),
					 errhint("Explicitly cast to the desired type, "
							 "for example ARRAY[]::integer[]."),
					 parser_errposition(pstate, a->location)));

		/* Select a common type for the elements */
		coerce_type = select_common_type(pstate, newelems, "ARRAY", NULL);

		if (newa->multidims)
		{
			array_type = coerce_type;
			element_type = get_element_type(array_type);
			if (!OidIsValid(element_type))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("could not find element type for data type %s",
								format_type_be(array_type)),
						 parser_errposition(pstate, a->location)));
		}
		else
		{
			element_type = coerce_type;
			array_type = get_array_type(element_type);
			if (!OidIsValid(array_type))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("could not find array type for data type %s",
								format_type_be(element_type)),
						 parser_errposition(pstate, a->location)));
		}
		coerce_hard = false;
	}

	/*
	 * Coerce elements to target type
	 *
	 * If the array has been explicitly cast, then the elements are in turn
	 * explicitly coerced.
	 *
	 * If the array's type was merely derived from the common type of its
	 * elements, then the elements are implicitly coerced to the common type.
	 * This is consistent with other uses of select_common_type().
	 */
	foreach(element, newelems)
	{
		Node	   *e = (Node *) lfirst(element);
		Node	   *newe;

		if (coerce_hard)
		{
			newe = coerce_to_target_type(pstate, e,
										 exprType(e),
										 coerce_type,
										 typmod,
										 COERCION_EXPLICIT,
										 COERCE_EXPLICIT_CAST,
										 -1);
			if (newe == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_CANNOT_COERCE),
						 errmsg("cannot cast type %s to %s",
								format_type_be(exprType(e)),
								format_type_be(coerce_type)),
						 parser_errposition(pstate, exprLocation(e))));
		}
		else
			newe = coerce_to_common_type(pstate, e,
										 coerce_type,
										 "ARRAY");
		newcoercedelems = lappend(newcoercedelems, newe);
	}

	newa->array_typeid = array_type;
	/* array_collid will be set by parse_collate.c */
	newa->element_typeid = element_type;
	newa->elements = newcoercedelems;
	newa->location = a->location;

	return (Node *) newa;
}

static Node *
transformRowExpr(ParseState *pstate, RowExpr *r, bool allowDefault)
{
	RowExpr    *newr;
	char		fname[16];
	int			fnum;
	ListCell   *lc;

	newr = makeNode(RowExpr);

	/* Transform the field expressions */
	newr->args = transformExpressionList(pstate, r->args,
										 pstate->p_expr_kind, allowDefault);

	/* Disallow more columns than will fit in a tuple */
	if (list_length(newr->args) > MaxTupleAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("ROW expressions can have at most %d entries",
						MaxTupleAttributeNumber),
				 parser_errposition(pstate, r->location)));

	/* Barring later casting, we consider the type RECORD */
	newr->row_typeid = RECORDOID;
	newr->row_format = COERCE_IMPLICIT_CAST;

	/* ROW() has anonymous columns, so invent some field names */
	newr->colnames = NIL;
	fnum = 1;
	foreach(lc, newr->args)
	{
		snprintf(fname, sizeof(fname), "f%d", fnum++);
		newr->colnames = lappend(newr->colnames, makeString(pstrdup(fname)));
	}

	newr->location = r->location;

	return (Node *) newr;
}

static Node *
transformCoalesceExpr(ParseState *pstate, CoalesceExpr *c)
{
	CoalesceExpr *newc = makeNode(CoalesceExpr);
	Node	   *last_srf = pstate->p_last_srf;
	List	   *newargs = NIL;
	List	   *newcoercedargs = NIL;
	ListCell   *args;

	foreach(args, c->args)
	{
		Node	   *e = (Node *) lfirst(args);
		Node	   *newe;

		newe = transformExprRecurse(pstate, e);
		newargs = lappend(newargs, newe);
	}

	newc->coalescetype = select_common_type(pstate, newargs, "COALESCE", NULL);
	/* coalescecollid will be set by parse_collate.c */

	/* Convert arguments if necessary */
	foreach(args, newargs)
	{
		Node	   *e = (Node *) lfirst(args);
		Node	   *newe;

		newe = coerce_to_common_type(pstate, e,
									 newc->coalescetype,
									 "COALESCE");
		newcoercedargs = lappend(newcoercedargs, newe);
	}

	/* if any subexpression contained a SRF, complain */
	if (pstate->p_last_srf != last_srf)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/* translator: %s is name of a SQL construct, eg GROUP BY */
				 errmsg("set-returning functions are not allowed in %s",
						"COALESCE"),
				 errhint("You might be able to move the set-returning function into a LATERAL FROM item."),
				 parser_errposition(pstate,
									exprLocation(pstate->p_last_srf))));

	newc->args = newcoercedargs;
	newc->location = c->location;
	return (Node *) newc;
}

static Node *
transformMinMaxExpr(ParseState *pstate, MinMaxExpr *m)
{
	MinMaxExpr *newm = makeNode(MinMaxExpr);
	List	   *newargs = NIL;
	List	   *newcoercedargs = NIL;
	const char *funcname = (m->op == IS_GREATEST) ? "GREATEST" : "LEAST";
	ListCell   *args;

	newm->op = m->op;
	foreach(args, m->args)
	{
		Node	   *e = (Node *) lfirst(args);
		Node	   *newe;

		newe = transformExprRecurse(pstate, e);
		newargs = lappend(newargs, newe);
	}

	newm->minmaxtype = select_common_type(pstate, newargs, funcname, NULL);
	/* minmaxcollid and inputcollid will be set by parse_collate.c */

	/* Convert arguments if necessary */
	foreach(args, newargs)
	{
		Node	   *e = (Node *) lfirst(args);
		Node	   *newe;

		newe = coerce_to_common_type(pstate, e,
									 newm->minmaxtype,
									 funcname);
		newcoercedargs = lappend(newcoercedargs, newe);
	}

	newm->args = newcoercedargs;
	newm->location = m->location;
	return (Node *) newm;
}

static Node *
transformSQLValueFunction(ParseState *pstate, SQLValueFunction *svf)
{
	/*
	 * All we need to do is insert the correct result type and (where needed)
	 * validate the typmod, so we just modify the node in-place.
	 */
	switch (svf->op)
	{
		case SVFOP_CURRENT_DATE:
			svf->type = DATEOID;
			break;
		case SVFOP_CURRENT_TIME:
			svf->type = TIMETZOID;
			break;
		case SVFOP_CURRENT_TIME_N:
			svf->type = TIMETZOID;
			svf->typmod = anytime_typmod_check(true, svf->typmod);
			break;
		case SVFOP_CURRENT_TIMESTAMP:
			svf->type = TIMESTAMPTZOID;
			break;
		case SVFOP_CURRENT_TIMESTAMP_N:
			svf->type = TIMESTAMPTZOID;
			svf->typmod = anytimestamp_typmod_check(true, svf->typmod);
			break;
		case SVFOP_LOCALTIME:
			svf->type = TIMEOID;
			break;
		case SVFOP_LOCALTIME_N:
			svf->type = TIMEOID;
			svf->typmod = anytime_typmod_check(false, svf->typmod);
			break;
		case SVFOP_LOCALTIMESTAMP:
			svf->type = TIMESTAMPOID;
			break;
		case SVFOP_LOCALTIMESTAMP_N:
			svf->type = TIMESTAMPOID;
			svf->typmod = anytimestamp_typmod_check(false, svf->typmod);
			break;
		case SVFOP_CURRENT_ROLE:
		case SVFOP_CURRENT_USER:
		case SVFOP_USER:
		case SVFOP_SESSION_USER:
		case SVFOP_CURRENT_CATALOG:
		case SVFOP_CURRENT_SCHEMA:
			svf->type = NAMEOID;
			break;
	}

	return (Node *) svf;
}

static Node *
transformXmlExpr(ParseState *pstate, XmlExpr *x)
{
	XmlExpr    *newx;
	ListCell   *lc;
	int			i;

	if (operator_precedence_warning && x->op == IS_DOCUMENT)
		emit_precedence_warnings(pstate, PREC_GROUP_POSTFIX_IS, "IS",
								 (Node *) linitial(x->args), NULL,
								 x->location);

	newx = makeNode(XmlExpr);
	newx->op = x->op;
	if (x->name)
		newx->name = map_sql_identifier_to_xml_name(x->name, false, false);
	else
		newx->name = NULL;
	newx->xmloption = x->xmloption;
	newx->type = XMLOID;		/* this just marks the node as transformed */
	newx->typmod = -1;
	newx->location = x->location;

	/*
	 * gram.y built the named args as a list of ResTarget.  Transform each,
	 * and break the names out as a separate list.
	 */
	newx->named_args = NIL;
	newx->arg_names = NIL;

	foreach(lc, x->named_args)
	{
		ResTarget  *r = lfirst_node(ResTarget, lc);
		Node	   *expr;
		char	   *argname;

		expr = transformExprRecurse(pstate, r->val);

		if (r->name)
			argname = map_sql_identifier_to_xml_name(r->name, false, false);
		else if (IsA(r->val, ColumnRef))
			argname = map_sql_identifier_to_xml_name(FigureColname(r->val),
													 true, false);
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 x->op == IS_XMLELEMENT
					 ? errmsg("unnamed XML attribute value must be a column reference")
					 : errmsg("unnamed XML element value must be a column reference"),
					 parser_errposition(pstate, r->location)));
			argname = NULL;		/* keep compiler quiet */
		}

		/* reject duplicate argnames in XMLELEMENT only */
		if (x->op == IS_XMLELEMENT)
		{
			ListCell   *lc2;

			foreach(lc2, newx->arg_names)
			{
				if (strcmp(argname, strVal(lfirst(lc2))) == 0)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("XML attribute name \"%s\" appears more than once",
									argname),
							 parser_errposition(pstate, r->location)));
			}
		}

		newx->named_args = lappend(newx->named_args, expr);
		newx->arg_names = lappend(newx->arg_names, makeString(argname));
	}

	/* The other arguments are of varying types depending on the function */
	newx->args = NIL;
	i = 0;
	foreach(lc, x->args)
	{
		Node	   *e = (Node *) lfirst(lc);
		Node	   *newe;

		newe = transformExprRecurse(pstate, e);
		switch (x->op)
		{
			case IS_XMLCONCAT:
				newe = coerce_to_specific_type(pstate, newe, XMLOID,
											   "XMLCONCAT");
				break;
			case IS_XMLELEMENT:
				/* no coercion necessary */
				break;
			case IS_XMLFOREST:
				newe = coerce_to_specific_type(pstate, newe, XMLOID,
											   "XMLFOREST");
				break;
			case IS_XMLPARSE:
				if (i == 0)
					newe = coerce_to_specific_type(pstate, newe, TEXTOID,
												   "XMLPARSE");
				else
					newe = coerce_to_boolean(pstate, newe, "XMLPARSE");
				break;
			case IS_XMLPI:
				newe = coerce_to_specific_type(pstate, newe, TEXTOID,
											   "XMLPI");
				break;
			case IS_XMLROOT:
				if (i == 0)
					newe = coerce_to_specific_type(pstate, newe, XMLOID,
												   "XMLROOT");
				else if (i == 1)
					newe = coerce_to_specific_type(pstate, newe, TEXTOID,
												   "XMLROOT");
				else
					newe = coerce_to_specific_type(pstate, newe, INT4OID,
												   "XMLROOT");
				break;
			case IS_XMLSERIALIZE:
				/* not handled here */
				Assert(false);
				break;
			case IS_DOCUMENT:
				newe = coerce_to_specific_type(pstate, newe, XMLOID,
											   "IS DOCUMENT");
				break;
		}
		newx->args = lappend(newx->args, newe);
		i++;
	}

	return (Node *) newx;
}

static Node *
transformXmlSerialize(ParseState *pstate, XmlSerialize *xs)
{
	Node	   *result;
	XmlExpr    *xexpr;
	Oid			targetType;
	int32		targetTypmod;

	xexpr = makeNode(XmlExpr);
	xexpr->op = IS_XMLSERIALIZE;
	xexpr->args = list_make1(coerce_to_specific_type(pstate,
													 transformExprRecurse(pstate, xs->expr),
													 XMLOID,
													 "XMLSERIALIZE"));

	typenameTypeIdAndMod(pstate, xs->typeName, &targetType, &targetTypmod);

	xexpr->xmloption = xs->xmloption;
	xexpr->location = xs->location;
	/* We actually only need these to be able to parse back the expression. */
	xexpr->type = targetType;
	xexpr->typmod = targetTypmod;

	/*
	 * The actual target type is determined this way.  SQL allows char and
	 * varchar as target types.  We allow anything that can be cast implicitly
	 * from text.  This way, user-defined text-like data types automatically
	 * fit in.
	 */
	result = coerce_to_target_type(pstate, (Node *) xexpr,
								   TEXTOID, targetType, targetTypmod,
								   COERCION_IMPLICIT,
								   COERCE_IMPLICIT_CAST,
								   -1);
	if (result == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_CANNOT_COERCE),
				 errmsg("cannot cast XMLSERIALIZE result to %s",
						format_type_be(targetType)),
				 parser_errposition(pstate, xexpr->location)));
	return result;
}

static Node *
transformBooleanTest(ParseState *pstate, BooleanTest *b)
{
	const char *clausename;

	if (operator_precedence_warning)
		emit_precedence_warnings(pstate, PREC_GROUP_POSTFIX_IS, "IS",
								 (Node *) b->arg, NULL,
								 b->location);

	switch (b->booltesttype)
	{
		case IS_TRUE:
			clausename = "IS TRUE";
			break;
		case IS_NOT_TRUE:
			clausename = "IS NOT TRUE";
			break;
		case IS_FALSE:
			clausename = "IS FALSE";
			break;
		case IS_NOT_FALSE:
			clausename = "IS NOT FALSE";
			break;
		case IS_UNKNOWN:
			clausename = "IS UNKNOWN";
			break;
		case IS_NOT_UNKNOWN:
			clausename = "IS NOT UNKNOWN";
			break;
		default:
			elog(ERROR, "unrecognized booltesttype: %d",
				 (int) b->booltesttype);
			clausename = NULL;	/* keep compiler quiet */
	}

	b->arg = (Expr *) transformExprRecurse(pstate, (Node *) b->arg);

	b->arg = (Expr *) coerce_to_boolean(pstate,
										(Node *) b->arg,
										clausename);

	return (Node *) b;
}

static Node *
transformCurrentOfExpr(ParseState *pstate, CurrentOfExpr *cexpr)
{
	int			sublevels_up;

	/* CURRENT OF can only appear at top level of UPDATE/DELETE */
	Assert(pstate->p_target_rangetblentry != NULL);
	cexpr->cvarno = RTERangeTablePosn(pstate,
									  pstate->p_target_rangetblentry,
									  &sublevels_up);
	Assert(sublevels_up == 0);

	/*
	 * Check to see if the cursor name matches a parameter of type REFCURSOR.
	 * If so, replace the raw name reference with a parameter reference. (This
	 * is a hack for the convenience of plpgsql.)
	 */
	if (cexpr->cursor_name != NULL) /* in case already transformed */
	{
		ColumnRef  *cref = makeNode(ColumnRef);
		Node	   *node = NULL;

		/* Build an unqualified ColumnRef with the given name */
		cref->fields = list_make1(makeString(cexpr->cursor_name));
		cref->location = -1;

		/* See if there is a translation available from a parser hook */
		if (pstate->p_pre_columnref_hook != NULL)
			node = pstate->p_pre_columnref_hook(pstate, cref);
		if (node == NULL && pstate->p_post_columnref_hook != NULL)
			node = pstate->p_post_columnref_hook(pstate, cref, NULL);

		/*
		 * XXX Should we throw an error if we get a translation that isn't a
		 * refcursor Param?  For now it seems best to silently ignore false
		 * matches.
		 */
		if (node != NULL && IsA(node, Param))
		{
			Param	   *p = (Param *) node;

			if (p->paramkind == PARAM_EXTERN &&
				p->paramtype == REFCURSOROID)
			{
				/* Matches, so convert CURRENT OF to a param reference */
				cexpr->cursor_name = NULL;
				cexpr->cursor_param = p->paramid;
			}
		}
	}

	return (Node *) cexpr;
}

/*
 * Construct a whole-row reference to represent the notation "relation.*".
 */
static Node *
transformWholeRowRef(ParseState *pstate, RangeTblEntry *rte, int location)
{
	Var		   *result;
	int			vnum;
	int			sublevels_up;

	/* Find the RTE's rangetable location */
	vnum = RTERangeTablePosn(pstate, rte, &sublevels_up);

	/*
	 * Build the appropriate referencing node.  Note that if the RTE is a
	 * function returning scalar, we create just a plain reference to the
	 * function value, not a composite containing a single column.  This is
	 * pretty inconsistent at first sight, but it's what we've done
	 * historically.  One argument for it is that "rel" and "rel.*" mean the
	 * same thing for composite relations, so why not for scalar functions...
	 */
	result = makeWholeRowVar(rte, vnum, sublevels_up, true);

	/* location is not filled in by makeWholeRowVar */
	result->location = location;

	/* mark relation as requiring whole-row SELECT access */
	markVarForSelectPriv(pstate, result, rte);

	return (Node *) result;
}

/*
 * Handle an explicit CAST construct.
 *
 * Transform the argument, look up the type name, and apply any necessary
 * coercion function(s).
 */
static Node *
transformTypeCast(ParseState *pstate, TypeCast *tc)
{
	Node	   *result;
	Node	   *arg = tc->arg;
	Node	   *expr;
	Oid			inputType;
	Oid			targetType;
	int32		targetTypmod;
	int			location;

	/* Look up the type name first */
	typenameTypeIdAndMod(pstate, tc->typeName, &targetType, &targetTypmod);

	/*
	 * Look through any AEXPR_PAREN nodes that may have been inserted thanks
	 * to operator_precedence_warning.  Otherwise, ARRAY[]::foo[] behaves
	 * differently from (ARRAY[])::foo[].
	 */
	while (arg && IsA(arg, A_Expr) &&
		   ((A_Expr *) arg)->kind == AEXPR_PAREN)
		arg = ((A_Expr *) arg)->lexpr;

	/*
	 * If the subject of the typecast is an ARRAY[] construct and the target
	 * type is an array type, we invoke transformArrayExpr() directly so that
	 * we can pass down the type information.  This avoids some cases where
	 * transformArrayExpr() might not infer the correct type.  Otherwise, just
	 * transform the argument normally.
	 */
	if (IsA(arg, A_ArrayExpr))
	{
		Oid			targetBaseType;
		int32		targetBaseTypmod;
		Oid			elementType;

		/*
		 * If target is a domain over array, work with the base array type
		 * here.  Below, we'll cast the array type to the domain.  In the
		 * usual case that the target is not a domain, the remaining steps
		 * will be a no-op.
		 */
		targetBaseTypmod = targetTypmod;
		targetBaseType = getBaseTypeAndTypmod(targetType, &targetBaseTypmod);
		elementType = get_element_type(targetBaseType);
		if (OidIsValid(elementType))
		{
			expr = transformArrayExpr(pstate,
									  (A_ArrayExpr *) arg,
									  targetBaseType,
									  elementType,
									  targetBaseTypmod);
		}
		else
			expr = transformExprRecurse(pstate, arg);
	}
	else
		expr = transformExprRecurse(pstate, arg);

	inputType = exprType(expr);
	if (inputType == InvalidOid)
		return expr;			/* do nothing if NULL input */

	/*
	 * Location of the coercion is preferentially the location of the :: or
	 * CAST symbol, but if there is none then use the location of the type
	 * name (this can happen in TypeName 'string' syntax, for instance).
	 */
	location = tc->location;
	if (location < 0)
		location = tc->typeName->location;

	result = coerce_to_target_type(pstate, expr, inputType,
								   targetType, targetTypmod,
								   COERCION_EXPLICIT,
								   COERCE_EXPLICIT_CAST,
								   location);
	if (result == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_CANNOT_COERCE),
				 errmsg("cannot cast type %s to %s",
						format_type_be(inputType),
						format_type_be(targetType)),
				 parser_coercion_errposition(pstate, location, expr)));

	return result;
}

/*
 * Handle an explicit COLLATE clause.
 *
 * Transform the argument, and look up the collation name.
 */
static Node *
transformCollateClause(ParseState *pstate, CollateClause *c)
{
	CollateExpr *newc;
	Oid			argtype;

	newc = makeNode(CollateExpr);
	newc->arg = (Expr *) transformExprRecurse(pstate, c->arg);

	argtype = exprType((Node *) newc->arg);

	/*
	 * The unknown type is not collatable, but coerce_type() takes care of it
	 * separately, so we'll let it go here.
	 */
	if (!type_is_collatable(argtype) && argtype != UNKNOWNOID)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("collations are not supported by type %s",
						format_type_be(argtype)),
				 parser_errposition(pstate, c->location)));

	newc->collOid = LookupCollation(pstate, c->collname, c->location);
	newc->location = c->location;

	return (Node *) newc;
}

/*
 * Transform a "row compare-op row" construct
 *
 * The inputs are lists of already-transformed expressions.
 * As with coerce_type, pstate may be NULL if no special unknown-Param
 * processing is wanted.
 *
 * The output may be a single OpExpr, an AND or OR combination of OpExprs,
 * or a RowCompareExpr.  In all cases it is guaranteed to return boolean.
 * The AND, OR, and RowCompareExpr cases further imply things about the
 * behavior of the operators (ie, they behave as =, <>, or < <= > >=).
 */
static Node *
make_row_comparison_op(ParseState *pstate, List *opname,
					   List *largs, List *rargs, int location)
{
	RowCompareExpr *rcexpr;
	RowCompareType rctype;
	List	   *opexprs;
	List	   *opnos;
	List	   *opfamilies;
	ListCell   *l,
			   *r;
	List	  **opinfo_lists;
	Bitmapset  *strats;
	int			nopers;
	int			i;

	nopers = list_length(largs);
	if (nopers != list_length(rargs))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("unequal number of entries in row expressions"),
				 parser_errposition(pstate, location)));

	/*
	 * We can't compare zero-length rows because there is no principled basis
	 * for figuring out what the operator is.
	 */
	if (nopers == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot compare rows of zero length"),
				 parser_errposition(pstate, location)));

	/*
	 * Identify all the pairwise operators, using make_op so that behavior is
	 * the same as in the simple scalar case.
	 */
	opexprs = NIL;
	forboth(l, largs, r, rargs)
	{
		Node	   *larg = (Node *) lfirst(l);
		Node	   *rarg = (Node *) lfirst(r);
		OpExpr	   *cmp;

		cmp = castNode(OpExpr, make_op(pstate, opname, larg, rarg,
									   pstate->p_last_srf, location));

		/*
		 * We don't use coerce_to_boolean here because we insist on the
		 * operator yielding boolean directly, not via coercion.  If it
		 * doesn't yield bool it won't be in any index opfamilies...
		 */
		if (cmp->opresulttype != BOOLOID)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("row comparison operator must yield type boolean, "
							"not type %s",
							format_type_be(cmp->opresulttype)),
					 parser_errposition(pstate, location)));
		if (expression_returns_set((Node *) cmp))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("row comparison operator must not return a set"),
					 parser_errposition(pstate, location)));
		opexprs = lappend(opexprs, cmp);
	}

	/*
	 * If rows are length 1, just return the single operator.  In this case we
	 * don't insist on identifying btree semantics for the operator (but we
	 * still require it to return boolean).
	 */
	if (nopers == 1)
		return (Node *) linitial(opexprs);

	/*
	 * Now we must determine which row comparison semantics (= <> < <= > >=)
	 * apply to this set of operators.  We look for btree opfamilies
	 * containing the operators, and see which interpretations (strategy
	 * numbers) exist for each operator.
	 */
	opinfo_lists = (List **) palloc(nopers * sizeof(List *));
	strats = NULL;
	i = 0;
	foreach(l, opexprs)
	{
		Oid			opno = ((OpExpr *) lfirst(l))->opno;
		Bitmapset  *this_strats;
		ListCell   *j;

		opinfo_lists[i] = get_op_btree_interpretation(opno);

		/*
		 * convert strategy numbers into a Bitmapset to make the intersection
		 * calculation easy.
		 */
		this_strats = NULL;
		foreach(j, opinfo_lists[i])
		{
			OpBtreeInterpretation *opinfo = lfirst(j);

			this_strats = bms_add_member(this_strats, opinfo->strategy);
		}
		if (i == 0)
			strats = this_strats;
		else
			strats = bms_int_members(strats, this_strats);
		i++;
	}

	/*
	 * If there are multiple common interpretations, we may use any one of
	 * them ... this coding arbitrarily picks the lowest btree strategy
	 * number.
	 */
	i = bms_first_member(strats);
	if (i < 0)
	{
		/* No common interpretation, so fail */
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not determine interpretation of row comparison operator %s",
						strVal(llast(opname))),
				 errhint("Row comparison operators must be associated with btree operator families."),
				 parser_errposition(pstate, location)));
	}
	rctype = (RowCompareType) i;

	/*
	 * For = and <> cases, we just combine the pairwise operators with AND or
	 * OR respectively.
	 */
	if (rctype == ROWCOMPARE_EQ)
		return (Node *) makeBoolExpr(AND_EXPR, opexprs, location);
	if (rctype == ROWCOMPARE_NE)
		return (Node *) makeBoolExpr(OR_EXPR, opexprs, location);

	/*
	 * Otherwise we need to choose exactly which opfamily to associate with
	 * each operator.
	 */
	opfamilies = NIL;
	for (i = 0; i < nopers; i++)
	{
		Oid			opfamily = InvalidOid;
		ListCell   *j;

		foreach(j, opinfo_lists[i])
		{
			OpBtreeInterpretation *opinfo = lfirst(j);

			if (opinfo->strategy == rctype)
			{
				opfamily = opinfo->opfamily_id;
				break;
			}
		}
		if (OidIsValid(opfamily))
			opfamilies = lappend_oid(opfamilies, opfamily);
		else					/* should not happen */
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("could not determine interpretation of row comparison operator %s",
							strVal(llast(opname))),
					 errdetail("There are multiple equally-plausible candidates."),
					 parser_errposition(pstate, location)));
	}

	/*
	 * Now deconstruct the OpExprs and create a RowCompareExpr.
	 *
	 * Note: can't just reuse the passed largs/rargs lists, because of
	 * possibility that make_op inserted coercion operations.
	 */
	opnos = NIL;
	largs = NIL;
	rargs = NIL;
	foreach(l, opexprs)
	{
		OpExpr	   *cmp = (OpExpr *) lfirst(l);

		opnos = lappend_oid(opnos, cmp->opno);
		largs = lappend(largs, linitial(cmp->args));
		rargs = lappend(rargs, lsecond(cmp->args));
	}

	rcexpr = makeNode(RowCompareExpr);
	rcexpr->rctype = rctype;
	rcexpr->opnos = opnos;
	rcexpr->opfamilies = opfamilies;
	rcexpr->inputcollids = NIL; /* assign_expr_collations will fix this */
	rcexpr->largs = largs;
	rcexpr->rargs = rargs;

	return (Node *) rcexpr;
}

/*
 * Transform a "row IS DISTINCT FROM row" construct
 *
 * The input RowExprs are already transformed
 */
static Node *
make_row_distinct_op(ParseState *pstate, List *opname,
					 RowExpr *lrow, RowExpr *rrow,
					 int location)
{
	Node	   *result = NULL;
	List	   *largs = lrow->args;
	List	   *rargs = rrow->args;
	ListCell   *l,
			   *r;

	if (list_length(largs) != list_length(rargs))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("unequal number of entries in row expressions"),
				 parser_errposition(pstate, location)));

	forboth(l, largs, r, rargs)
	{
		Node	   *larg = (Node *) lfirst(l);
		Node	   *rarg = (Node *) lfirst(r);
		Node	   *cmp;

		cmp = (Node *) make_distinct_op(pstate, opname, larg, rarg, location);
		if (result == NULL)
			result = cmp;
		else
			result = (Node *) makeBoolExpr(OR_EXPR,
										   list_make2(result, cmp),
										   location);
	}

	if (result == NULL)
	{
		/* zero-length rows?  Generate constant FALSE */
		result = makeBoolConst(false, false);
	}

	return result;
}

/*
 * make the node for an IS DISTINCT FROM operator
 */
static Expr *
make_distinct_op(ParseState *pstate, List *opname, Node *ltree, Node *rtree,
				 int location)
{
	Expr	   *result;

	result = make_op(pstate, opname, ltree, rtree,
					 pstate->p_last_srf, location);
	if (((OpExpr *) result)->opresulttype != BOOLOID)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("IS DISTINCT FROM requires = operator to yield boolean"),
				 parser_errposition(pstate, location)));
	if (((OpExpr *) result)->opretset)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
		/* translator: %s is name of a SQL construct, eg NULLIF */
				 errmsg("%s must not return a set", "IS DISTINCT FROM"),
				 parser_errposition(pstate, location)));

	/*
	 * We rely on DistinctExpr and OpExpr being same struct
	 */
	NodeSetTag(result, T_DistinctExpr);

	return result;
}

/*
 * Produce a NullTest node from an IS [NOT] DISTINCT FROM NULL construct
 *
 * "arg" is the untransformed other argument
 */
static Node *
make_nulltest_from_distinct(ParseState *pstate, A_Expr *distincta, Node *arg)
{
	NullTest   *nt = makeNode(NullTest);

	nt->arg = (Expr *) transformExprRecurse(pstate, arg);
	/* the argument can be any type, so don't coerce it */
	if (distincta->kind == AEXPR_NOT_DISTINCT)
		nt->nulltesttype = IS_NULL;
	else
		nt->nulltesttype = IS_NOT_NULL;
	/* argisrow = false is correct whether or not arg is composite */
	nt->argisrow = false;
	nt->location = distincta->location;
	return (Node *) nt;
}

/*
 * Identify node's group for operator precedence warnings
 *
 * For items in nonzero groups, also return a suitable node name into *nodename
 *
 * Note: group zero is used for nodes that are higher or lower precedence
 * than everything that changed precedence; we need never issue warnings
 * related to such nodes.
 */
static int
operator_precedence_group(Node *node, const char **nodename)
{
	int			group = 0;

	*nodename = NULL;
	if (node == NULL)
		return 0;

	if (IsA(node, A_Expr))
	{
		A_Expr	   *aexpr = (A_Expr *) node;

		if (aexpr->kind == AEXPR_OP &&
			aexpr->lexpr != NULL &&
			aexpr->rexpr != NULL)
		{
			/* binary operator */
			if (list_length(aexpr->name) == 1)
			{
				*nodename = strVal(linitial(aexpr->name));
				/* Ignore if op was always higher priority than IS-tests */
				if (strcmp(*nodename, "+") == 0 ||
					strcmp(*nodename, "-") == 0 ||
					strcmp(*nodename, "*") == 0 ||
					strcmp(*nodename, "/") == 0 ||
					strcmp(*nodename, "%") == 0 ||
					strcmp(*nodename, "^") == 0)
					group = 0;
				else if (strcmp(*nodename, "<") == 0 ||
						 strcmp(*nodename, ">") == 0)
					group = PREC_GROUP_LESS;
				else if (strcmp(*nodename, "=") == 0)
					group = PREC_GROUP_EQUAL;
				else if (strcmp(*nodename, "<=") == 0 ||
						 strcmp(*nodename, ">=") == 0 ||
						 strcmp(*nodename, "<>") == 0)
					group = PREC_GROUP_LESS_EQUAL;
				else
					group = PREC_GROUP_INFIX_OP;
			}
			else
			{
				/* schema-qualified operator syntax */
				*nodename = "OPERATOR()";
				group = PREC_GROUP_INFIX_OP;
			}
		}
		else if (aexpr->kind == AEXPR_OP &&
				 aexpr->lexpr == NULL &&
				 aexpr->rexpr != NULL)
		{
			/* prefix operator */
			if (list_length(aexpr->name) == 1)
			{
				*nodename = strVal(linitial(aexpr->name));
				/* Ignore if op was always higher priority than IS-tests */
				if (strcmp(*nodename, "+") == 0 ||
					strcmp(*nodename, "-") == 0)
					group = 0;
				else
					group = PREC_GROUP_PREFIX_OP;
			}
			else
			{
				/* schema-qualified operator syntax */
				*nodename = "OPERATOR()";
				group = PREC_GROUP_PREFIX_OP;
			}
		}
		else if (aexpr->kind == AEXPR_OP &&
				 aexpr->lexpr != NULL &&
				 aexpr->rexpr == NULL)
		{
			/* postfix operator */
			if (list_length(aexpr->name) == 1)
			{
				*nodename = strVal(linitial(aexpr->name));
				group = PREC_GROUP_POSTFIX_OP;
			}
			else
			{
				/* schema-qualified operator syntax */
				*nodename = "OPERATOR()";
				group = PREC_GROUP_POSTFIX_OP;
			}
		}
		else if (aexpr->kind == AEXPR_OP_ANY ||
				 aexpr->kind == AEXPR_OP_ALL)
		{
			*nodename = strVal(llast(aexpr->name));
			group = PREC_GROUP_POSTFIX_OP;
		}
		else if (aexpr->kind == AEXPR_DISTINCT ||
				 aexpr->kind == AEXPR_NOT_DISTINCT)
		{
			*nodename = "IS";
			group = PREC_GROUP_INFIX_IS;
		}
		else if (aexpr->kind == AEXPR_OF)
		{
			*nodename = "IS";
			group = PREC_GROUP_POSTFIX_IS;
		}
		else if (aexpr->kind == AEXPR_IN)
		{
			*nodename = "IN";
			if (strcmp(strVal(linitial(aexpr->name)), "=") == 0)
				group = PREC_GROUP_IN;
			else
				group = PREC_GROUP_NOT_IN;
		}
		else if (aexpr->kind == AEXPR_LIKE)
		{
			*nodename = "LIKE";
			if (strcmp(strVal(linitial(aexpr->name)), "~~") == 0)
				group = PREC_GROUP_LIKE;
			else
				group = PREC_GROUP_NOT_LIKE;
		}
		else if (aexpr->kind == AEXPR_ILIKE)
		{
			*nodename = "ILIKE";
			if (strcmp(strVal(linitial(aexpr->name)), "~~*") == 0)
				group = PREC_GROUP_LIKE;
			else
				group = PREC_GROUP_NOT_LIKE;
		}
		else if (aexpr->kind == AEXPR_SIMILAR)
		{
			*nodename = "SIMILAR";
			if (strcmp(strVal(linitial(aexpr->name)), "~") == 0)
				group = PREC_GROUP_LIKE;
			else
				group = PREC_GROUP_NOT_LIKE;
		}
		else if (aexpr->kind == AEXPR_BETWEEN ||
				 aexpr->kind == AEXPR_BETWEEN_SYM)
		{
			Assert(list_length(aexpr->name) == 1);
			*nodename = strVal(linitial(aexpr->name));
			group = PREC_GROUP_BETWEEN;
		}
		else if (aexpr->kind == AEXPR_NOT_BETWEEN ||
				 aexpr->kind == AEXPR_NOT_BETWEEN_SYM)
		{
			Assert(list_length(aexpr->name) == 1);
			*nodename = strVal(linitial(aexpr->name));
			group = PREC_GROUP_NOT_BETWEEN;
		}
	}
	else if (IsA(node, NullTest) ||
			 IsA(node, BooleanTest))
	{
		*nodename = "IS";
		group = PREC_GROUP_POSTFIX_IS;
	}
	else if (IsA(node, XmlExpr))
	{
		XmlExpr    *x = (XmlExpr *) node;

		if (x->op == IS_DOCUMENT)
		{
			*nodename = "IS";
			group = PREC_GROUP_POSTFIX_IS;
		}
	}
	else if (IsA(node, SubLink))
	{
		SubLink    *s = (SubLink *) node;

		if (s->subLinkType == ANY_SUBLINK ||
			s->subLinkType == ALL_SUBLINK)
		{
			if (s->operName == NIL)
			{
				*nodename = "IN";
				group = PREC_GROUP_IN;
			}
			else
			{
				*nodename = strVal(llast(s->operName));
				group = PREC_GROUP_POSTFIX_OP;
			}
		}
	}
	else if (IsA(node, BoolExpr))
	{
		/*
		 * Must dig into NOTs to see if it's IS NOT DOCUMENT or NOT IN.  This
		 * opens us to possibly misrecognizing, eg, NOT (x IS DOCUMENT) as a
		 * problematic construct.  We can tell the difference by checking
		 * whether the parse locations of the two nodes are identical.
		 *
		 * Note that when we are comparing the child node to its own children,
		 * we will not know that it was a NOT.  Fortunately, that doesn't
		 * matter for these cases.
		 */
		BoolExpr   *b = (BoolExpr *) node;

		if (b->boolop == NOT_EXPR)
		{
			Node	   *child = (Node *) linitial(b->args);

			if (IsA(child, XmlExpr))
			{
				XmlExpr    *x = (XmlExpr *) child;

				if (x->op == IS_DOCUMENT &&
					x->location == b->location)
				{
					*nodename = "IS";
					group = PREC_GROUP_POSTFIX_IS;
				}
			}
			else if (IsA(child, SubLink))
			{
				SubLink    *s = (SubLink *) child;

				if (s->subLinkType == ANY_SUBLINK && s->operName == NIL &&
					s->location == b->location)
				{
					*nodename = "IN";
					group = PREC_GROUP_NOT_IN;
				}
			}
		}
	}
	return group;
}

/*
 * helper routine for delivering 9.4-to-9.5 operator precedence warnings
 *
 * opgroup/opname/location represent some parent node
 * lchild, rchild are its left and right children (either could be NULL)
 *
 * This should be called before transforming the child nodes, since if a
 * precedence-driven parsing change has occurred in a query that used to work,
 * it's quite possible that we'll get a semantic failure while analyzing the
 * child expression.  We want to produce the warning before that happens.
 * In any case, operator_precedence_group() expects untransformed input.
 */
static void
emit_precedence_warnings(ParseState *pstate,
						 int opgroup, const char *opname,
						 Node *lchild, Node *rchild,
						 int location)
{
	int			cgroup;
	const char *copname;

	Assert(opgroup > 0);

	/*
	 * Complain if left child, which should be same or higher precedence
	 * according to current rules, used to be lower precedence.
	 *
	 * Exception to precedence rules: if left child is IN or NOT IN or a
	 * postfix operator, the grouping is syntactically forced regardless of
	 * precedence.
	 */
	cgroup = operator_precedence_group(lchild, &copname);
	if (cgroup > 0)
	{
		if (oldprecedence_l[cgroup] < oldprecedence_r[opgroup] &&
			cgroup != PREC_GROUP_IN &&
			cgroup != PREC_GROUP_NOT_IN &&
			cgroup != PREC_GROUP_POSTFIX_OP &&
			cgroup != PREC_GROUP_POSTFIX_IS)
			ereport(WARNING,
					(errmsg("operator precedence change: %s is now lower precedence than %s",
							opname, copname),
					 parser_errposition(pstate, location)));
	}

	/*
	 * Complain if right child, which should be higher precedence according to
	 * current rules, used to be same or lower precedence.
	 *
	 * Exception to precedence rules: if right child is a prefix operator, the
	 * grouping is syntactically forced regardless of precedence.
	 */
	cgroup = operator_precedence_group(rchild, &copname);
	if (cgroup > 0)
	{
		if (oldprecedence_r[cgroup] <= oldprecedence_l[opgroup] &&
			cgroup != PREC_GROUP_PREFIX_OP)
			ereport(WARNING,
					(errmsg("operator precedence change: %s is now lower precedence than %s",
							opname, copname),
					 parser_errposition(pstate, location)));
	}
}

/*
 * Produce a string identifying an expression by kind.
 *
 * Note: when practical, use a simple SQL keyword for the result.  If that
 * doesn't work well, check call sites to see whether custom error message
 * strings are required.
 */
const char *
ParseExprKindName(ParseExprKind exprKind)
{
	switch (exprKind)
	{
		case EXPR_KIND_NONE:
			return "invalid expression context";
		case EXPR_KIND_OTHER:
			return "extension expression";
		case EXPR_KIND_JOIN_ON:
			return "JOIN/ON";
		case EXPR_KIND_JOIN_USING:
			return "JOIN/USING";
		case EXPR_KIND_FROM_SUBSELECT:
			return "sub-SELECT in FROM";
		case EXPR_KIND_FROM_FUNCTION:
			return "function in FROM";
		case EXPR_KIND_WHERE:
			return "WHERE";
		case EXPR_KIND_POLICY:
			return "POLICY";
		case EXPR_KIND_HAVING:
			return "HAVING";
		case EXPR_KIND_FILTER:
			return "FILTER";
		case EXPR_KIND_WINDOW_PARTITION:
			return "window PARTITION BY";
		case EXPR_KIND_WINDOW_ORDER:
			return "window ORDER BY";
		case EXPR_KIND_WINDOW_FRAME_RANGE:
			return "window RANGE";
		case EXPR_KIND_WINDOW_FRAME_ROWS:
			return "window ROWS";
		case EXPR_KIND_WINDOW_FRAME_GROUPS:
			return "window GROUPS";
		case EXPR_KIND_SELECT_TARGET:
			return "SELECT";
		case EXPR_KIND_INSERT_TARGET:
			return "INSERT";
		case EXPR_KIND_UPDATE_SOURCE:
		case EXPR_KIND_UPDATE_TARGET:
			return "UPDATE";
		case EXPR_KIND_GROUP_BY:
			return "GROUP BY";
		case EXPR_KIND_ORDER_BY:
			return "ORDER BY";
		case EXPR_KIND_DISTINCT_ON:
			return "DISTINCT ON";
		case EXPR_KIND_LIMIT:
			return "LIMIT";
		case EXPR_KIND_OFFSET:
			return "OFFSET";
		case EXPR_KIND_RETURNING:
			return "RETURNING";
		case EXPR_KIND_VALUES:
		case EXPR_KIND_VALUES_SINGLE:
			return "VALUES";
		case EXPR_KIND_CHECK_CONSTRAINT:
		case EXPR_KIND_DOMAIN_CHECK:
			return "CHECK";
		case EXPR_KIND_COLUMN_DEFAULT:
		case EXPR_KIND_FUNCTION_DEFAULT:
			return "DEFAULT";
		case EXPR_KIND_INDEX_EXPRESSION:
			return "index expression";
		case EXPR_KIND_INDEX_PREDICATE:
			return "index predicate";
		case EXPR_KIND_ALTER_COL_TRANSFORM:
			return "USING";
		case EXPR_KIND_EXECUTE_PARAMETER:
			return "EXECUTE";
		case EXPR_KIND_TRIGGER_WHEN:
			return "WHEN";
		case EXPR_KIND_PARTITION_BOUND:
			return "partition bound";
		case EXPR_KIND_PARTITION_EXPRESSION:
			return "PARTITION BY";
		case EXPR_KIND_CALL_ARGUMENT:
			return "CALL";
		case EXPR_KIND_COPY_WHERE:
			return "WHERE";
		case EXPR_KIND_GENERATED_COLUMN:
			return "GENERATED AS";

			/*
			 * There is intentionally no default: case here, so that the
			 * compiler will warn if we add a new ParseExprKind without
			 * extending this switch.  If we do see an unrecognized value at
			 * runtime, we'll fall through to the "unrecognized" return.
			 */
	}
	return "unrecognized expression kind";
}
