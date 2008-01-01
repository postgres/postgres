/*-------------------------------------------------------------------------
 *
 * parse_expr.c
 *	  handle expressions in parser
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/parser/parse_expr.c,v 1.226 2008/01/01 19:45:50 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/plannodes.h"
#include "optimizer/clauses.h"
#include "parser/analyze.h"
#include "parser/gramparse.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/xml.h"


bool		Transform_null_equals = false;

static Node *transformParamRef(ParseState *pstate, ParamRef *pref);
static Node *transformAExprOp(ParseState *pstate, A_Expr *a);
static Node *transformAExprAnd(ParseState *pstate, A_Expr *a);
static Node *transformAExprOr(ParseState *pstate, A_Expr *a);
static Node *transformAExprNot(ParseState *pstate, A_Expr *a);
static Node *transformAExprOpAny(ParseState *pstate, A_Expr *a);
static Node *transformAExprOpAll(ParseState *pstate, A_Expr *a);
static Node *transformAExprDistinct(ParseState *pstate, A_Expr *a);
static Node *transformAExprNullIf(ParseState *pstate, A_Expr *a);
static Node *transformAExprOf(ParseState *pstate, A_Expr *a);
static Node *transformAExprIn(ParseState *pstate, A_Expr *a);
static Node *transformFuncCall(ParseState *pstate, FuncCall *fn);
static Node *transformCaseExpr(ParseState *pstate, CaseExpr *c);
static Node *transformSubLink(ParseState *pstate, SubLink *sublink);
static Node *transformArrayExpr(ParseState *pstate, ArrayExpr *a);
static Node *transformRowExpr(ParseState *pstate, RowExpr *r);
static Node *transformCoalesceExpr(ParseState *pstate, CoalesceExpr *c);
static Node *transformMinMaxExpr(ParseState *pstate, MinMaxExpr *m);
static Node *transformXmlExpr(ParseState *pstate, XmlExpr *x);
static Node *transformXmlSerialize(ParseState *pstate, XmlSerialize *xs);
static Node *transformBooleanTest(ParseState *pstate, BooleanTest *b);
static Node *transformCurrentOfExpr(ParseState *pstate, CurrentOfExpr *cexpr);
static Node *transformColumnRef(ParseState *pstate, ColumnRef *cref);
static Node *transformWholeRowRef(ParseState *pstate, char *schemaname,
					 char *relname, int location);
static Node *transformIndirection(ParseState *pstate, Node *basenode,
					 List *indirection);
static Node *typecast_expression(ParseState *pstate, Node *expr,
					TypeName *typename);
static Node *make_row_comparison_op(ParseState *pstate, List *opname,
					   List *largs, List *rargs, int location);
static Node *make_row_distinct_op(ParseState *pstate, List *opname,
					 RowExpr *lrow, RowExpr *rrow, int location);
static Expr *make_distinct_op(ParseState *pstate, List *opname,
				 Node *ltree, Node *rtree, int location);


/*
 * transformExpr -
 *	  Analyze and transform expressions. Type checking and type casting is
 *	  done here. The optimizer and the executor cannot handle the original
 *	  (raw) expressions collected by the parse tree. Hence the transformation
 *	  here.
 *
 * NOTE: there are various cases in which this routine will get applied to
 * an already-transformed expression.  Some examples:
 *	1. At least one construct (BETWEEN/AND) puts the same nodes
 *	into two branches of the parse tree; hence, some nodes
 *	are transformed twice.
 *	2. Another way it can happen is that coercion of an operator or
 *	function argument to the required type (via coerce_type())
 *	can apply transformExpr to an already-transformed subexpression.
 *	An example here is "SELECT count(*) + 1.0 FROM table".
 * While it might be possible to eliminate these cases, the path of
 * least resistance so far has been to ensure that transformExpr() does
 * no damage if applied to an already-transformed tree.  This is pretty
 * easy for cases where the transformation replaces one node type with
 * another, such as A_Const => Const; we just do nothing when handed
 * a Const.  More care is needed for node types that are used as both
 * input and output of transformExpr; see SubLink for example.
 */
Node *
transformExpr(ParseState *pstate, Node *expr)
{
	Node	   *result = NULL;

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

				result = (Node *) make_const(val);
				if (con->typename != NULL)
					result = typecast_expression(pstate, result,
												 con->typename);
				break;
			}

		case T_A_Indirection:
			{
				A_Indirection *ind = (A_Indirection *) expr;

				result = transformExpr(pstate, ind->arg);
				result = transformIndirection(pstate, result,
											  ind->indirection);
				break;
			}

		case T_TypeCast:
			{
				TypeCast   *tc = (TypeCast *) expr;
				Node	   *arg = transformExpr(pstate, tc->arg);

				result = typecast_expression(pstate, arg, tc->typename);
				break;
			}

		case T_A_Expr:
			{
				A_Expr	   *a = (A_Expr *) expr;

				switch (a->kind)
				{
					case AEXPR_OP:
						result = transformAExprOp(pstate, a);
						break;
					case AEXPR_AND:
						result = transformAExprAnd(pstate, a);
						break;
					case AEXPR_OR:
						result = transformAExprOr(pstate, a);
						break;
					case AEXPR_NOT:
						result = transformAExprNot(pstate, a);
						break;
					case AEXPR_OP_ANY:
						result = transformAExprOpAny(pstate, a);
						break;
					case AEXPR_OP_ALL:
						result = transformAExprOpAll(pstate, a);
						break;
					case AEXPR_DISTINCT:
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
					default:
						elog(ERROR, "unrecognized A_Expr kind: %d", a->kind);
				}
				break;
			}

		case T_FuncCall:
			result = transformFuncCall(pstate, (FuncCall *) expr);
			break;

		case T_SubLink:
			result = transformSubLink(pstate, (SubLink *) expr);
			break;

		case T_CaseExpr:
			result = transformCaseExpr(pstate, (CaseExpr *) expr);
			break;

		case T_ArrayExpr:
			result = transformArrayExpr(pstate, (ArrayExpr *) expr);
			break;

		case T_RowExpr:
			result = transformRowExpr(pstate, (RowExpr *) expr);
			break;

		case T_CoalesceExpr:
			result = transformCoalesceExpr(pstate, (CoalesceExpr *) expr);
			break;

		case T_MinMaxExpr:
			result = transformMinMaxExpr(pstate, (MinMaxExpr *) expr);
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

				n->arg = (Expr *) transformExpr(pstate, (Node *) n->arg);
				/* the argument can be any type, so don't coerce it */
				result = expr;
				break;
			}

		case T_BooleanTest:
			result = transformBooleanTest(pstate, (BooleanTest *) expr);
			break;

		case T_CurrentOfExpr:
			result = transformCurrentOfExpr(pstate, (CurrentOfExpr *) expr);
			break;

			/*********************************************
			 * Quietly accept node types that may be presented when we are
			 * called on an already-transformed tree.
			 *
			 * Do any other node types need to be accepted?  For now we are
			 * taking a conservative approach, and only accepting node
			 * types that are demonstrably necessary to accept.
			 *********************************************/
		case T_Var:
		case T_Const:
		case T_Param:
		case T_Aggref:
		case T_ArrayRef:
		case T_FuncExpr:
		case T_OpExpr:
		case T_DistinctExpr:
		case T_ScalarArrayOpExpr:
		case T_NullIfExpr:
		case T_BoolExpr:
		case T_FieldSelect:
		case T_FieldStore:
		case T_RelabelType:
		case T_CoerceViaIO:
		case T_ArrayCoerceExpr:
		case T_ConvertRowtypeExpr:
		case T_CaseTestExpr:
		case T_CoerceToDomain:
		case T_CoerceToDomainValue:
		case T_SetToDefault:
			{
				result = (Node *) expr;
				break;
			}

		default:
			/* should not reach here */
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(expr));
			break;
	}

	return result;
}

static Node *
transformIndirection(ParseState *pstate, Node *basenode, List *indirection)
{
	Node	   *result = basenode;
	List	   *subscripts = NIL;
	ListCell   *i;

	/*
	 * We have to split any field-selection operations apart from
	 * subscripting.  Adjacent A_Indices nodes have to be treated as a single
	 * multidimensional subscript operation.
	 */
	foreach(i, indirection)
	{
		Node	   *n = lfirst(i);

		if (IsA(n, A_Indices))
			subscripts = lappend(subscripts, n);
		else
		{
			Assert(IsA(n, String));

			/* process subscripts before this field selection */
			if (subscripts)
				result = (Node *) transformArraySubscripts(pstate,
														   result,
														   exprType(result),
														   InvalidOid,
														   exprTypmod(result),
														   subscripts,
														   NULL);
			subscripts = NIL;

			result = ParseFuncOrColumn(pstate,
									   list_make1(n),
									   list_make1(result),
									   false, false, true,
									   -1);
		}
	}
	/* process trailing subscripts, if any */
	if (subscripts)
		result = (Node *) transformArraySubscripts(pstate,
												   result,
												   exprType(result),
												   InvalidOid,
												   exprTypmod(result),
												   subscripts,
												   NULL);

	return result;
}

static Node *
transformColumnRef(ParseState *pstate, ColumnRef *cref)
{
	int			numnames = list_length(cref->fields);
	Node	   *node;
	int			levels_up;

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
	switch (numnames)
	{
		case 1:
			{
				char	   *name = strVal(linitial(cref->fields));

				/* Try to identify as an unqualified column */
				node = colNameToVar(pstate, name, false, cref->location);

				if (node == NULL)
				{
					/*
					 * Not known as a column of any range-table entry.
					 *
					 * Consider the possibility that it's VALUE in a domain
					 * check expression.  (We handle VALUE as a name, not a
					 * keyword, to avoid breaking a lot of applications that
					 * have used VALUE as a column name in the past.)
					 */
					if (pstate->p_value_substitute != NULL &&
						strcmp(name, "value") == 0)
					{
						node = (Node *) copyObject(pstate->p_value_substitute);
						break;
					}

					/*
					 * Try to find the name as a relation.	Note that only
					 * relations already entered into the rangetable will be
					 * recognized.
					 *
					 * This is a hack for backwards compatibility with
					 * PostQUEL-inspired syntax.  The preferred form now is
					 * "rel.*".
					 */
					if (refnameRangeTblEntry(pstate, NULL, name,
											 &levels_up) != NULL)
						node = transformWholeRowRef(pstate, NULL, name,
													cref->location);
					else
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_COLUMN),
								 errmsg("column \"%s\" does not exist",
										name),
								 parser_errposition(pstate, cref->location)));
				}
				break;
			}
		case 2:
			{
				char	   *name1 = strVal(linitial(cref->fields));
				char	   *name2 = strVal(lsecond(cref->fields));

				/* Whole-row reference? */
				if (strcmp(name2, "*") == 0)
				{
					node = transformWholeRowRef(pstate, NULL, name1,
												cref->location);
					break;
				}

				/* Try to identify as a once-qualified column */
				node = qualifiedNameToVar(pstate, NULL, name1, name2, true,
										  cref->location);
				if (node == NULL)
				{
					/*
					 * Not known as a column of any range-table entry, so try
					 * it as a function call.  Here, we will create an
					 * implicit RTE for tables not already entered.
					 */
					node = transformWholeRowRef(pstate, NULL, name1,
												cref->location);
					node = ParseFuncOrColumn(pstate,
											 list_make1(makeString(name2)),
											 list_make1(node),
											 false, false, true,
											 cref->location);
				}
				break;
			}
		case 3:
			{
				char	   *name1 = strVal(linitial(cref->fields));
				char	   *name2 = strVal(lsecond(cref->fields));
				char	   *name3 = strVal(lthird(cref->fields));

				/* Whole-row reference? */
				if (strcmp(name3, "*") == 0)
				{
					node = transformWholeRowRef(pstate, name1, name2,
												cref->location);
					break;
				}

				/* Try to identify as a twice-qualified column */
				node = qualifiedNameToVar(pstate, name1, name2, name3, true,
										  cref->location);
				if (node == NULL)
				{
					/* Try it as a function call */
					node = transformWholeRowRef(pstate, name1, name2,
												cref->location);
					node = ParseFuncOrColumn(pstate,
											 list_make1(makeString(name3)),
											 list_make1(node),
											 false, false, true,
											 cref->location);
				}
				break;
			}
		case 4:
			{
				char	   *name1 = strVal(linitial(cref->fields));
				char	   *name2 = strVal(lsecond(cref->fields));
				char	   *name3 = strVal(lthird(cref->fields));
				char	   *name4 = strVal(lfourth(cref->fields));

				/*
				 * We check the catalog name and then ignore it.
				 */
				if (strcmp(name1, get_database_name(MyDatabaseId)) != 0)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cross-database references are not implemented: %s",
									NameListToString(cref->fields)),
							 parser_errposition(pstate, cref->location)));

				/* Whole-row reference? */
				if (strcmp(name4, "*") == 0)
				{
					node = transformWholeRowRef(pstate, name2, name3,
												cref->location);
					break;
				}

				/* Try to identify as a twice-qualified column */
				node = qualifiedNameToVar(pstate, name2, name3, name4, true,
										  cref->location);
				if (node == NULL)
				{
					/* Try it as a function call */
					node = transformWholeRowRef(pstate, name2, name3,
												cref->location);
					node = ParseFuncOrColumn(pstate,
											 list_make1(makeString(name4)),
											 list_make1(node),
											 false, false, true,
											 cref->location);
				}
				break;
			}
		default:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
				errmsg("improper qualified name (too many dotted names): %s",
					   NameListToString(cref->fields)),
					 parser_errposition(pstate, cref->location)));
			node = NULL;		/* keep compiler quiet */
			break;
	}

	return node;
}

/*
 * Locate the parameter type info for the given parameter number, and
 * return a pointer to it.
 */
static Oid *
find_param_type(ParseState *pstate, int paramno)
{
	Oid		   *result;

	/*
	 * Find topmost ParseState, which is where paramtype info lives.
	 */
	while (pstate->parentParseState != NULL)
		pstate = pstate->parentParseState;

	/* Check parameter number is in range */
	if (paramno <= 0)			/* probably can't happen? */
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_PARAMETER),
				 errmsg("there is no parameter $%d", paramno)));
	if (paramno > pstate->p_numparams)
	{
		if (!pstate->p_variableparams)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_PARAMETER),
					 errmsg("there is no parameter $%d",
							paramno)));
		/* Okay to enlarge param array */
		if (pstate->p_paramtypes)
			pstate->p_paramtypes = (Oid *) repalloc(pstate->p_paramtypes,
													paramno * sizeof(Oid));
		else
			pstate->p_paramtypes = (Oid *) palloc(paramno * sizeof(Oid));
		/* Zero out the previously-unreferenced slots */
		MemSet(pstate->p_paramtypes + pstate->p_numparams,
			   0,
			   (paramno - pstate->p_numparams) * sizeof(Oid));
		pstate->p_numparams = paramno;
	}

	result = &pstate->p_paramtypes[paramno - 1];

	if (pstate->p_variableparams)
	{
		/* If not seen before, initialize to UNKNOWN type */
		if (*result == InvalidOid)
			*result = UNKNOWNOID;
	}

	return result;
}

static Node *
transformParamRef(ParseState *pstate, ParamRef *pref)
{
	int			paramno = pref->number;
	Oid		   *pptype = find_param_type(pstate, paramno);
	Param	   *param;

	param = makeNode(Param);
	param->paramkind = PARAM_EXTERN;
	param->paramid = paramno;
	param->paramtype = *pptype;
	param->paramtypmod = -1;

	return (Node *) param;
}

/* Test whether an a_expr is a plain NULL constant or not */
static bool
exprIsNullConstant(Node *arg)
{
	if (arg && IsA(arg, A_Const))
	{
		A_Const    *con = (A_Const *) arg;

		if (con->val.type == T_Null &&
			con->typename == NULL)
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

	/*
	 * Special-case "foo = NULL" and "NULL = foo" for compatibility with
	 * standards-broken products (like Microsoft's).  Turn these into IS NULL
	 * exprs.
	 */
	if (Transform_null_equals &&
		list_length(a->name) == 1 &&
		strcmp(strVal(linitial(a->name)), "=") == 0 &&
		(exprIsNullConstant(lexpr) || exprIsNullConstant(rexpr)))
	{
		NullTest   *n = makeNode(NullTest);

		n->nulltesttype = IS_NULL;

		if (exprIsNullConstant(lexpr))
			n->arg = (Expr *) rexpr;
		else
			n->arg = (Expr *) lexpr;

		result = transformExpr(pstate, (Node *) n);
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
		result = transformExpr(pstate, (Node *) s);
	}
	else if (lexpr && IsA(lexpr, RowExpr) &&
			 rexpr && IsA(rexpr, RowExpr))
	{
		/* "row op row" */
		lexpr = transformExpr(pstate, lexpr);
		rexpr = transformExpr(pstate, rexpr);
		Assert(IsA(lexpr, RowExpr));
		Assert(IsA(rexpr, RowExpr));

		result = make_row_comparison_op(pstate,
										a->name,
										((RowExpr *) lexpr)->args,
										((RowExpr *) rexpr)->args,
										a->location);
	}
	else
	{
		/* Ordinary scalar operator */
		lexpr = transformExpr(pstate, lexpr);
		rexpr = transformExpr(pstate, rexpr);

		result = (Node *) make_op(pstate,
								  a->name,
								  lexpr,
								  rexpr,
								  a->location);
	}

	return result;
}

static Node *
transformAExprAnd(ParseState *pstate, A_Expr *a)
{
	Node	   *lexpr = transformExpr(pstate, a->lexpr);
	Node	   *rexpr = transformExpr(pstate, a->rexpr);

	lexpr = coerce_to_boolean(pstate, lexpr, "AND");
	rexpr = coerce_to_boolean(pstate, rexpr, "AND");

	return (Node *) makeBoolExpr(AND_EXPR,
								 list_make2(lexpr, rexpr));
}

static Node *
transformAExprOr(ParseState *pstate, A_Expr *a)
{
	Node	   *lexpr = transformExpr(pstate, a->lexpr);
	Node	   *rexpr = transformExpr(pstate, a->rexpr);

	lexpr = coerce_to_boolean(pstate, lexpr, "OR");
	rexpr = coerce_to_boolean(pstate, rexpr, "OR");

	return (Node *) makeBoolExpr(OR_EXPR,
								 list_make2(lexpr, rexpr));
}

static Node *
transformAExprNot(ParseState *pstate, A_Expr *a)
{
	Node	   *rexpr = transformExpr(pstate, a->rexpr);

	rexpr = coerce_to_boolean(pstate, rexpr, "NOT");

	return (Node *) makeBoolExpr(NOT_EXPR,
								 list_make1(rexpr));
}

static Node *
transformAExprOpAny(ParseState *pstate, A_Expr *a)
{
	Node	   *lexpr = transformExpr(pstate, a->lexpr);
	Node	   *rexpr = transformExpr(pstate, a->rexpr);

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
	Node	   *lexpr = transformExpr(pstate, a->lexpr);
	Node	   *rexpr = transformExpr(pstate, a->rexpr);

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
	Node	   *lexpr = transformExpr(pstate, a->lexpr);
	Node	   *rexpr = transformExpr(pstate, a->rexpr);

	if (lexpr && IsA(lexpr, RowExpr) &&
		rexpr && IsA(rexpr, RowExpr))
	{
		/* "row op row" */
		return make_row_distinct_op(pstate, a->name,
									(RowExpr *) lexpr,
									(RowExpr *) rexpr,
									a->location);
	}
	else
	{
		/* Ordinary scalar operator */
		return (Node *) make_distinct_op(pstate,
										 a->name,
										 lexpr,
										 rexpr,
										 a->location);
	}
}

static Node *
transformAExprNullIf(ParseState *pstate, A_Expr *a)
{
	Node	   *lexpr = transformExpr(pstate, a->lexpr);
	Node	   *rexpr = transformExpr(pstate, a->rexpr);
	Node	   *result;

	result = (Node *) make_op(pstate,
							  a->name,
							  lexpr,
							  rexpr,
							  a->location);
	if (((OpExpr *) result)->opresulttype != BOOLOID)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("NULLIF requires = operator to yield boolean"),
				 parser_errposition(pstate, a->location)));

	/*
	 * We rely on NullIfExpr and OpExpr being the same struct
	 */
	NodeSetTag(result, T_NullIfExpr);

	return result;
}

static Node *
transformAExprOf(ParseState *pstate, A_Expr *a)
{
	/*
	 * Checking an expression for match to a list of type names. Will result
	 * in a boolean constant node.
	 */
	Node	   *lexpr = transformExpr(pstate, a->lexpr);
	ListCell   *telem;
	Oid			ltype,
				rtype;
	bool		matched = false;

	ltype = exprType(lexpr);
	foreach(telem, (List *) a->rexpr)
	{
		rtype = typenameTypeId(pstate, lfirst(telem), NULL);
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

	return makeBoolConst(matched, false);
}

static Node *
transformAExprIn(ParseState *pstate, A_Expr *a)
{
	Node	   *lexpr;
	List	   *rexprs;
	List	   *typeids;
	bool		useOr;
	bool		haveRowExpr;
	Node	   *result;
	ListCell   *l;

	/*
	 * If the operator is <>, combine with AND not OR.
	 */
	if (strcmp(strVal(linitial(a->name)), "<>") == 0)
		useOr = false;
	else
		useOr = true;

	/*
	 * We try to generate a ScalarArrayOpExpr from IN/NOT IN, but this is only
	 * possible if the inputs are all scalars (no RowExprs) and there is a
	 * suitable array type available.  If not, we fall back to a boolean
	 * condition tree with multiple copies of the lefthand expression.
	 *
	 * First step: transform all the inputs, and detect whether any are
	 * RowExprs.
	 */
	lexpr = transformExpr(pstate, a->lexpr);
	haveRowExpr = (lexpr && IsA(lexpr, RowExpr));
	typeids = list_make1_oid(exprType(lexpr));
	rexprs = NIL;
	foreach(l, (List *) a->rexpr)
	{
		Node	   *rexpr = transformExpr(pstate, lfirst(l));

		haveRowExpr |= (rexpr && IsA(rexpr, RowExpr));
		rexprs = lappend(rexprs, rexpr);
		typeids = lappend_oid(typeids, exprType(rexpr));
	}

	/*
	 * If not forced by presence of RowExpr, try to resolve a common scalar
	 * type for all the expressions, and see if it has an array type. (But if
	 * there's only one righthand expression, we may as well just fall through
	 * and generate a simple = comparison.)
	 */
	if (!haveRowExpr && list_length(rexprs) != 1)
	{
		Oid			scalar_type;
		Oid			array_type;

		/*
		 * Select a common type for the array elements.  Note that since the
		 * LHS' type is first in the list, it will be preferred when there is
		 * doubt (eg, when all the RHS items are unknown literals).
		 */
		scalar_type = select_common_type(typeids, "IN");

		/* Do we have an array type to use? */
		array_type = get_array_type(scalar_type);
		if (array_type != InvalidOid)
		{
			/*
			 * OK: coerce all the right-hand inputs to the common type and
			 * build an ArrayExpr for them.
			 */
			List	   *aexprs;
			ArrayExpr  *newa;

			aexprs = NIL;
			foreach(l, rexprs)
			{
				Node	   *rexpr = (Node *) lfirst(l);

				rexpr = coerce_to_common_type(pstate, rexpr,
											  scalar_type,
											  "IN");
				aexprs = lappend(aexprs, rexpr);
			}
			newa = makeNode(ArrayExpr);
			newa->array_typeid = array_type;
			newa->element_typeid = scalar_type;
			newa->elements = aexprs;
			newa->multidims = false;

			return (Node *) make_scalar_array_op(pstate,
												 a->name,
												 useOr,
												 lexpr,
												 (Node *) newa,
												 a->location);
		}
	}

	/*
	 * Must do it the hard way, ie, with a boolean expression tree.
	 */
	result = NULL;
	foreach(l, rexprs)
	{
		Node	   *rexpr = (Node *) lfirst(l);
		Node	   *cmp;

		if (haveRowExpr)
		{
			if (!IsA(lexpr, RowExpr) ||
				!IsA(rexpr, RowExpr))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
				   errmsg("arguments of row IN must all be row expressions"),
						 parser_errposition(pstate, a->location)));
			cmp = make_row_comparison_op(pstate,
										 a->name,
							  (List *) copyObject(((RowExpr *) lexpr)->args),
										 ((RowExpr *) rexpr)->args,
										 a->location);
		}
		else
			cmp = (Node *) make_op(pstate,
								   a->name,
								   copyObject(lexpr),
								   rexpr,
								   a->location);

		cmp = coerce_to_boolean(pstate, cmp, "IN");
		if (result == NULL)
			result = cmp;
		else
			result = (Node *) makeBoolExpr(useOr ? OR_EXPR : AND_EXPR,
										   list_make2(result, cmp));
	}

	return result;
}

static Node *
transformFuncCall(ParseState *pstate, FuncCall *fn)
{
	List	   *targs;
	ListCell   *args;

	/*
	 * Transform the list of arguments.  We use a shallow list copy and then
	 * transform-in-place to avoid O(N^2) behavior from repeated lappend's.
	 *
	 * XXX: repeated lappend() would no longer result in O(n^2) behavior;
	 * worth reconsidering this design?
	 */
	targs = list_copy(fn->args);
	foreach(args, targs)
	{
		lfirst(args) = transformExpr(pstate,
									 (Node *) lfirst(args));
	}

	return ParseFuncOrColumn(pstate,
							 fn->funcname,
							 targs,
							 fn->agg_star,
							 fn->agg_distinct,
							 false,
							 fn->location);
}

static Node *
transformCaseExpr(ParseState *pstate, CaseExpr *c)
{
	CaseExpr   *newc;
	Node	   *arg;
	CaseTestExpr *placeholder;
	List	   *newargs;
	List	   *typeids;
	ListCell   *l;
	Node	   *defresult;
	Oid			ptype;

	/* If we already transformed this node, do nothing */
	if (OidIsValid(c->casetype))
		return (Node *) c;

	newc = makeNode(CaseExpr);

	/* transform the test expression, if any */
	arg = transformExpr(pstate, (Node *) c->arg);

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

		placeholder = makeNode(CaseTestExpr);
		placeholder->typeId = exprType(arg);
		placeholder->typeMod = exprTypmod(arg);
	}
	else
		placeholder = NULL;

	newc->arg = (Expr *) arg;

	/* transform the list of arguments */
	newargs = NIL;
	typeids = NIL;
	foreach(l, c->args)
	{
		CaseWhen   *w = (CaseWhen *) lfirst(l);
		CaseWhen   *neww = makeNode(CaseWhen);
		Node	   *warg;

		Assert(IsA(w, CaseWhen));

		warg = (Node *) w->expr;
		if (placeholder)
		{
			/* shorthand form was specified, so expand... */
			warg = (Node *) makeSimpleA_Expr(AEXPR_OP, "=",
											 (Node *) placeholder,
											 warg,
											 -1);
		}
		neww->expr = (Expr *) transformExpr(pstate, warg);

		neww->expr = (Expr *) coerce_to_boolean(pstate,
												(Node *) neww->expr,
												"CASE/WHEN");

		warg = (Node *) w->result;
		neww->result = (Expr *) transformExpr(pstate, warg);

		newargs = lappend(newargs, neww);
		typeids = lappend_oid(typeids, exprType((Node *) neww->result));
	}

	newc->args = newargs;

	/* transform the default clause */
	defresult = (Node *) c->defresult;
	if (defresult == NULL)
	{
		A_Const    *n = makeNode(A_Const);

		n->val.type = T_Null;
		defresult = (Node *) n;
	}
	newc->defresult = (Expr *) transformExpr(pstate, defresult);

	/*
	 * Note: default result is considered the most significant type in
	 * determining preferred type. This is how the code worked before, but it
	 * seems a little bogus to me --- tgl
	 */
	typeids = lcons_oid(exprType((Node *) newc->defresult), typeids);

	ptype = select_common_type(typeids, "CASE");
	Assert(OidIsValid(ptype));
	newc->casetype = ptype;

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

	return (Node *) newc;
}

static Node *
transformSubLink(ParseState *pstate, SubLink *sublink)
{
	Node	   *result = (Node *) sublink;
	Query	   *qtree;

	/* If we already transformed this node, do nothing */
	if (IsA(sublink->subselect, Query))
		return result;

	pstate->p_hasSubLinks = true;
	qtree = parse_sub_analyze(sublink->subselect, pstate);
	if (qtree->commandType != CMD_SELECT ||
		qtree->utilityStmt != NULL ||
		qtree->intoClause != NULL)
		elog(ERROR, "bad query in sub-select");
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
		ListCell   *tlist_item = list_head(qtree->targetList);

		/*
		 * Make sure the subselect delivers a single column (ignoring resjunk
		 * targets).
		 */
		if (tlist_item == NULL ||
			((TargetEntry *) lfirst(tlist_item))->resjunk)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("subquery must return a column")));
		while ((tlist_item = lnext(tlist_item)) != NULL)
		{
			if (!((TargetEntry *) lfirst(tlist_item))->resjunk)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("subquery must return only one column")));
		}

		/*
		 * EXPR and ARRAY need no test expression or combining operator. These
		 * fields should be null already, but make sure.
		 */
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

		/*
		 * Transform lefthand expression, and convert to a list
		 */
		lefthand = transformExpr(pstate, sublink->testexpr);
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
					 errmsg("subquery has too many columns")));
		if (list_length(left_list) > list_length(right_list))
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("subquery has too few columns")));

		/*
		 * Identify the combining operator(s) and generate a suitable
		 * row-comparison expression.
		 */
		sublink->testexpr = make_row_comparison_op(pstate,
												   sublink->operName,
												   left_list,
												   right_list,
												   -1);
	}

	return result;
}

static Node *
transformArrayExpr(ParseState *pstate, ArrayExpr *a)
{
	ArrayExpr  *newa = makeNode(ArrayExpr);
	List	   *newelems = NIL;
	List	   *newcoercedelems = NIL;
	List	   *typeids = NIL;
	ListCell   *element;
	Oid			array_type;
	Oid			element_type;

	/* Transform the element expressions */
	foreach(element, a->elements)
	{
		Node	   *e = (Node *) lfirst(element);
		Node	   *newe;

		newe = transformExpr(pstate, e);
		newelems = lappend(newelems, newe);
		typeids = lappend_oid(typeids, exprType(newe));
	}

	/* Select a common type for the elements */
	element_type = select_common_type(typeids, "ARRAY");

	/* Coerce arguments to common type if necessary */
	foreach(element, newelems)
	{
		Node	   *e = (Node *) lfirst(element);
		Node	   *newe;

		newe = coerce_to_common_type(pstate, e,
									 element_type,
									 "ARRAY");
		newcoercedelems = lappend(newcoercedelems, newe);
	}

	/* Do we have an array type to use? */
	array_type = get_array_type(element_type);
	if (array_type != InvalidOid)
	{
		/* Elements are presumably of scalar type */
		newa->multidims = false;
	}
	else
	{
		/* Must be nested array expressions */
		newa->multidims = true;

		array_type = element_type;
		element_type = get_element_type(array_type);
		if (!OidIsValid(element_type))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("could not find array type for data type %s",
							format_type_be(array_type))));
	}

	newa->array_typeid = array_type;
	newa->element_typeid = element_type;
	newa->elements = newcoercedelems;

	return (Node *) newa;
}

static Node *
transformRowExpr(ParseState *pstate, RowExpr *r)
{
	RowExpr    *newr = makeNode(RowExpr);

	/* Transform the field expressions */
	newr->args = transformExpressionList(pstate, r->args);

	/* Barring later casting, we consider the type RECORD */
	newr->row_typeid = RECORDOID;
	newr->row_format = COERCE_IMPLICIT_CAST;

	return (Node *) newr;
}

static Node *
transformCoalesceExpr(ParseState *pstate, CoalesceExpr *c)
{
	CoalesceExpr *newc = makeNode(CoalesceExpr);
	List	   *newargs = NIL;
	List	   *newcoercedargs = NIL;
	List	   *typeids = NIL;
	ListCell   *args;

	foreach(args, c->args)
	{
		Node	   *e = (Node *) lfirst(args);
		Node	   *newe;

		newe = transformExpr(pstate, e);
		newargs = lappend(newargs, newe);
		typeids = lappend_oid(typeids, exprType(newe));
	}

	newc->coalescetype = select_common_type(typeids, "COALESCE");

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

	newc->args = newcoercedargs;
	return (Node *) newc;
}

static Node *
transformMinMaxExpr(ParseState *pstate, MinMaxExpr *m)
{
	MinMaxExpr *newm = makeNode(MinMaxExpr);
	List	   *newargs = NIL;
	List	   *newcoercedargs = NIL;
	List	   *typeids = NIL;
	ListCell   *args;

	newm->op = m->op;
	foreach(args, m->args)
	{
		Node	   *e = (Node *) lfirst(args);
		Node	   *newe;

		newe = transformExpr(pstate, e);
		newargs = lappend(newargs, newe);
		typeids = lappend_oid(typeids, exprType(newe));
	}

	newm->minmaxtype = select_common_type(typeids, "GREATEST/LEAST");

	/* Convert arguments if necessary */
	foreach(args, newargs)
	{
		Node	   *e = (Node *) lfirst(args);
		Node	   *newe;

		newe = coerce_to_common_type(pstate, e,
									 newm->minmaxtype,
									 "GREATEST/LEAST");
		newcoercedargs = lappend(newcoercedargs, newe);
	}

	newm->args = newcoercedargs;
	return (Node *) newm;
}

static Node *
transformXmlExpr(ParseState *pstate, XmlExpr *x)
{
	XmlExpr    *newx = makeNode(XmlExpr);
	ListCell   *lc;
	int			i;

	newx->op = x->op;
	if (x->name)
		newx->name = map_sql_identifier_to_xml_name(x->name, false, false);
	else
		newx->name = NULL;

	/*
	 * gram.y built the named args as a list of ResTarget.	Transform each,
	 * and break the names out as a separate list.
	 */
	newx->named_args = NIL;
	newx->arg_names = NIL;

	foreach(lc, x->named_args)
	{
		ResTarget  *r = (ResTarget *) lfirst(lc);
		Node	   *expr;
		char	   *argname;

		Assert(IsA(r, ResTarget));

		expr = transformExpr(pstate, r->val);

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
					 : errmsg("unnamed XML element value must be a column reference")));
			argname = NULL;		/* keep compiler quiet */
		}

		newx->named_args = lappend(newx->named_args, expr);
		newx->arg_names = lappend(newx->arg_names, makeString(argname));
	}

	newx->xmloption = x->xmloption;

	if (x->op == IS_XMLELEMENT)
	{
		foreach(lc, newx->arg_names)
		{
			ListCell   *lc2;

			for_each_cell(lc2, lnext(lc))
			{
				if (strcmp(strVal(lfirst(lc)), strVal(lfirst(lc2))) == 0)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("XML attribute name \"%s\" appears more than once", strVal(lfirst(lc)))));
			}
		}
	}

	/* The other arguments are of varying types depending on the function */
	newx->args = NIL;
	i = 0;
	foreach(lc, x->args)
	{
		Node	   *e = (Node *) lfirst(lc);
		Node	   *newe;

		newe = transformExpr(pstate, e);
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
	Oid			targetType;
	int32		targetTypmod;
	XmlExpr    *xexpr;

	xexpr = makeNode(XmlExpr);
	xexpr->op = IS_XMLSERIALIZE;
	xexpr->args = list_make1(coerce_to_specific_type(pstate,
											 transformExpr(pstate, xs->expr),
													 XMLOID,
													 "XMLSERIALIZE"));

	targetType = typenameTypeId(pstate, xs->typename, &targetTypmod);

	xexpr->xmloption = xs->xmloption;
	/* We actually only need these to be able to parse back the expression. */
	xexpr->type = targetType;
	xexpr->typmod = targetTypmod;

	/*
	 * The actual target type is determined this way.  SQL allows char and
	 * varchar as target types.  We allow anything that can be cast implicitly
	 * from text.  This way, user-defined text-like data types automatically
	 * fit in.
	 */
	return (Node *) coerce_to_target_type(pstate, (Node *) xexpr, TEXTOID, targetType, targetTypmod,
									COERCION_IMPLICIT, COERCE_IMPLICIT_CAST);
}

static Node *
transformBooleanTest(ParseState *pstate, BooleanTest *b)
{
	const char *clausename;

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

	b->arg = (Expr *) transformExpr(pstate, (Node *) b->arg);

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

	/* If a parameter is used, it must be of type REFCURSOR */
	if (cexpr->cursor_name == NULL)
	{
		Oid		   *pptype = find_param_type(pstate, cexpr->cursor_param);

		if (pstate->p_variableparams && *pptype == UNKNOWNOID)
		{
			/* resolve unknown param type as REFCURSOR */
			*pptype = REFCURSOROID;
		}
		else if (*pptype != REFCURSOROID)
		{
			ereport(ERROR,
					(errcode(ERRCODE_AMBIGUOUS_PARAMETER),
					 errmsg("inconsistent types deduced for parameter $%d",
							cexpr->cursor_param),
					 errdetail("%s versus %s",
							   format_type_be(*pptype),
							   format_type_be(REFCURSOROID))));
		}
	}

	return (Node *) cexpr;
}

/*
 * Construct a whole-row reference to represent the notation "relation.*".
 *
 * A whole-row reference is a Var with varno set to the correct range
 * table entry, and varattno == 0 to signal that it references the whole
 * tuple.  (Use of zero here is unclean, since it could easily be confused
 * with error cases, but it's not worth changing now.)  The vartype indicates
 * a rowtype; either a named composite type, or RECORD.
 */
static Node *
transformWholeRowRef(ParseState *pstate, char *schemaname, char *relname,
					 int location)
{
	Node	   *result;
	RangeTblEntry *rte;
	int			vnum;
	int			sublevels_up;
	Oid			toid;

	/* Look up the referenced RTE, creating it if needed */

	rte = refnameRangeTblEntry(pstate, schemaname, relname,
							   &sublevels_up);

	if (rte == NULL)
		rte = addImplicitRTE(pstate, makeRangeVar(schemaname, relname),
							 location);

	vnum = RTERangeTablePosn(pstate, rte, &sublevels_up);

	/* Build the appropriate referencing node */

	switch (rte->rtekind)
	{
		case RTE_RELATION:
			/* relation: the rowtype is a named composite type */
			toid = get_rel_type_id(rte->relid);
			if (!OidIsValid(toid))
				elog(ERROR, "could not find type OID for relation %u",
					 rte->relid);
			result = (Node *) makeVar(vnum,
									  InvalidAttrNumber,
									  toid,
									  -1,
									  sublevels_up);
			break;
		case RTE_FUNCTION:
			toid = exprType(rte->funcexpr);
			if (type_is_rowtype(toid))
			{
				/* func returns composite; same as relation case */
				result = (Node *) makeVar(vnum,
										  InvalidAttrNumber,
										  toid,
										  -1,
										  sublevels_up);
			}
			else
			{
				/*
				 * func returns scalar; instead of making a whole-row Var,
				 * just reference the function's scalar output.  (XXX this
				 * seems a tad inconsistent, especially if "f.*" was
				 * explicitly written ...)
				 */
				result = (Node *) makeVar(vnum,
										  1,
										  toid,
										  -1,
										  sublevels_up);
			}
			break;
		case RTE_VALUES:
			toid = RECORDOID;
			/* returns composite; same as relation case */
			result = (Node *) makeVar(vnum,
									  InvalidAttrNumber,
									  toid,
									  -1,
									  sublevels_up);
			break;
		default:

			/*
			 * RTE is a join or subselect.	We represent this as a whole-row
			 * Var of RECORD type.	(Note that in most cases the Var will be
			 * expanded to a RowExpr during planning, but that is not our
			 * concern here.)
			 */
			result = (Node *) makeVar(vnum,
									  InvalidAttrNumber,
									  RECORDOID,
									  -1,
									  sublevels_up);
			break;
	}

	return result;
}

/*
 *	exprType -
 *	  returns the Oid of the type of the expression. (Used for typechecking.)
 */
Oid
exprType(Node *expr)
{
	Oid			type;

	if (!expr)
		return InvalidOid;

	switch (nodeTag(expr))
	{
		case T_Var:
			type = ((Var *) expr)->vartype;
			break;
		case T_Const:
			type = ((Const *) expr)->consttype;
			break;
		case T_Param:
			type = ((Param *) expr)->paramtype;
			break;
		case T_Aggref:
			type = ((Aggref *) expr)->aggtype;
			break;
		case T_ArrayRef:
			{
				ArrayRef   *arrayref = (ArrayRef *) expr;

				/* slice and/or store operations yield the array type */
				if (arrayref->reflowerindexpr || arrayref->refassgnexpr)
					type = arrayref->refarraytype;
				else
					type = arrayref->refelemtype;
			}
			break;
		case T_FuncExpr:
			type = ((FuncExpr *) expr)->funcresulttype;
			break;
		case T_OpExpr:
			type = ((OpExpr *) expr)->opresulttype;
			break;
		case T_DistinctExpr:
			type = ((DistinctExpr *) expr)->opresulttype;
			break;
		case T_ScalarArrayOpExpr:
			type = BOOLOID;
			break;
		case T_BoolExpr:
			type = BOOLOID;
			break;
		case T_SubLink:
			{
				SubLink    *sublink = (SubLink *) expr;

				if (sublink->subLinkType == EXPR_SUBLINK ||
					sublink->subLinkType == ARRAY_SUBLINK)
				{
					/* get the type of the subselect's first target column */
					Query	   *qtree = (Query *) sublink->subselect;
					TargetEntry *tent;

					if (!qtree || !IsA(qtree, Query))
						elog(ERROR, "cannot get type for untransformed sublink");
					tent = (TargetEntry *) linitial(qtree->targetList);
					Assert(IsA(tent, TargetEntry));
					Assert(!tent->resjunk);
					type = exprType((Node *) tent->expr);
					if (sublink->subLinkType == ARRAY_SUBLINK)
					{
						type = get_array_type(type);
						if (!OidIsValid(type))
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_OBJECT),
									 errmsg("could not find array type for data type %s",
							format_type_be(exprType((Node *) tent->expr)))));
					}
				}
				else
				{
					/* for all other sublink types, result is boolean */
					type = BOOLOID;
				}
			}
			break;
		case T_SubPlan:
			{
				/*
				 * Although the parser does not ever deal with already-planned
				 * expression trees, we support SubPlan nodes in this routine
				 * for the convenience of ruleutils.c.
				 */
				SubPlan    *subplan = (SubPlan *) expr;

				if (subplan->subLinkType == EXPR_SUBLINK ||
					subplan->subLinkType == ARRAY_SUBLINK)
				{
					/* get the type of the subselect's first target column */
					type = subplan->firstColType;
					if (subplan->subLinkType == ARRAY_SUBLINK)
					{
						type = get_array_type(type);
						if (!OidIsValid(type))
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_OBJECT),
									 errmsg("could not find array type for data type %s",
									format_type_be(subplan->firstColType))));
					}
				}
				else
				{
					/* for all other subplan types, result is boolean */
					type = BOOLOID;
				}
			}
			break;
		case T_FieldSelect:
			type = ((FieldSelect *) expr)->resulttype;
			break;
		case T_FieldStore:
			type = ((FieldStore *) expr)->resulttype;
			break;
		case T_RelabelType:
			type = ((RelabelType *) expr)->resulttype;
			break;
		case T_CoerceViaIO:
			type = ((CoerceViaIO *) expr)->resulttype;
			break;
		case T_ArrayCoerceExpr:
			type = ((ArrayCoerceExpr *) expr)->resulttype;
			break;
		case T_ConvertRowtypeExpr:
			type = ((ConvertRowtypeExpr *) expr)->resulttype;
			break;
		case T_CaseExpr:
			type = ((CaseExpr *) expr)->casetype;
			break;
		case T_CaseTestExpr:
			type = ((CaseTestExpr *) expr)->typeId;
			break;
		case T_ArrayExpr:
			type = ((ArrayExpr *) expr)->array_typeid;
			break;
		case T_RowExpr:
			type = ((RowExpr *) expr)->row_typeid;
			break;
		case T_RowCompareExpr:
			type = BOOLOID;
			break;
		case T_CoalesceExpr:
			type = ((CoalesceExpr *) expr)->coalescetype;
			break;
		case T_MinMaxExpr:
			type = ((MinMaxExpr *) expr)->minmaxtype;
			break;
		case T_XmlExpr:
			if (((XmlExpr *) expr)->op == IS_DOCUMENT)
				type = BOOLOID;
			else if (((XmlExpr *) expr)->op == IS_XMLSERIALIZE)
				type = TEXTOID;
			else
				type = XMLOID;
			break;
		case T_NullIfExpr:
			type = exprType((Node *) linitial(((NullIfExpr *) expr)->args));
			break;
		case T_NullTest:
			type = BOOLOID;
			break;
		case T_BooleanTest:
			type = BOOLOID;
			break;
		case T_CoerceToDomain:
			type = ((CoerceToDomain *) expr)->resulttype;
			break;
		case T_CoerceToDomainValue:
			type = ((CoerceToDomainValue *) expr)->typeId;
			break;
		case T_SetToDefault:
			type = ((SetToDefault *) expr)->typeId;
			break;
		case T_CurrentOfExpr:
			type = BOOLOID;
			break;
		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(expr));
			type = InvalidOid;	/* keep compiler quiet */
			break;
	}
	return type;
}

/*
 *	exprTypmod -
 *	  returns the type-specific attrmod of the expression, if it can be
 *	  determined.  In most cases, it can't and we return -1.
 */
int32
exprTypmod(Node *expr)
{
	if (!expr)
		return -1;

	switch (nodeTag(expr))
	{
		case T_Var:
			return ((Var *) expr)->vartypmod;
		case T_Const:
			return ((Const *) expr)->consttypmod;
		case T_Param:
			return ((Param *) expr)->paramtypmod;
		case T_ArrayRef:
			/* typmod is the same for array or element */
			return ((ArrayRef *) expr)->reftypmod;
		case T_FuncExpr:
			{
				int32		coercedTypmod;

				/* Be smart about length-coercion functions... */
				if (exprIsLengthCoercion(expr, &coercedTypmod))
					return coercedTypmod;
			}
			break;
		case T_SubLink:
			{
				SubLink    *sublink = (SubLink *) expr;

				if (sublink->subLinkType == EXPR_SUBLINK ||
					sublink->subLinkType == ARRAY_SUBLINK)
				{
					/* get the typmod of the subselect's first target column */
					Query	   *qtree = (Query *) sublink->subselect;
					TargetEntry *tent;

					if (!qtree || !IsA(qtree, Query))
						elog(ERROR, "cannot get type for untransformed sublink");
					tent = (TargetEntry *) linitial(qtree->targetList);
					Assert(IsA(tent, TargetEntry));
					Assert(!tent->resjunk);
					return exprTypmod((Node *) tent->expr);
					/* note we don't need to care if it's an array */
				}
			}
			break;
		case T_FieldSelect:
			return ((FieldSelect *) expr)->resulttypmod;
		case T_RelabelType:
			return ((RelabelType *) expr)->resulttypmod;
		case T_ArrayCoerceExpr:
			return ((ArrayCoerceExpr *) expr)->resulttypmod;
		case T_CaseExpr:
			{
				/*
				 * If all the alternatives agree on type/typmod, return that
				 * typmod, else use -1
				 */
				CaseExpr   *cexpr = (CaseExpr *) expr;
				Oid			casetype = cexpr->casetype;
				int32		typmod;
				ListCell   *arg;

				if (!cexpr->defresult)
					return -1;
				if (exprType((Node *) cexpr->defresult) != casetype)
					return -1;
				typmod = exprTypmod((Node *) cexpr->defresult);
				if (typmod < 0)
					return -1;	/* no point in trying harder */
				foreach(arg, cexpr->args)
				{
					CaseWhen   *w = (CaseWhen *) lfirst(arg);

					Assert(IsA(w, CaseWhen));
					if (exprType((Node *) w->result) != casetype)
						return -1;
					if (exprTypmod((Node *) w->result) != typmod)
						return -1;
				}
				return typmod;
			}
			break;
		case T_CaseTestExpr:
			return ((CaseTestExpr *) expr)->typeMod;
		case T_ArrayExpr:
			{
				/*
				 * If all the elements agree on type/typmod, return that
				 * typmod, else use -1
				 */
				ArrayExpr  *arrayexpr = (ArrayExpr *) expr;
				Oid			commontype;
				int32		typmod;
				ListCell   *elem;

				if (arrayexpr->elements == NIL)
					return -1;
				typmod = exprTypmod((Node *) linitial(arrayexpr->elements));
				if (typmod < 0)
					return -1;	/* no point in trying harder */
				if (arrayexpr->multidims)
					commontype = arrayexpr->array_typeid;
				else
					commontype = arrayexpr->element_typeid;
				foreach(elem, arrayexpr->elements)
				{
					Node	   *e = (Node *) lfirst(elem);

					if (exprType(e) != commontype)
						return -1;
					if (exprTypmod(e) != typmod)
						return -1;
				}
				return typmod;
			}
			break;
		case T_CoalesceExpr:
			{
				/*
				 * If all the alternatives agree on type/typmod, return that
				 * typmod, else use -1
				 */
				CoalesceExpr *cexpr = (CoalesceExpr *) expr;
				Oid			coalescetype = cexpr->coalescetype;
				int32		typmod;
				ListCell   *arg;

				if (exprType((Node *) linitial(cexpr->args)) != coalescetype)
					return -1;
				typmod = exprTypmod((Node *) linitial(cexpr->args));
				if (typmod < 0)
					return -1;	/* no point in trying harder */
				for_each_cell(arg, lnext(list_head(cexpr->args)))
				{
					Node	   *e = (Node *) lfirst(arg);

					if (exprType(e) != coalescetype)
						return -1;
					if (exprTypmod(e) != typmod)
						return -1;
				}
				return typmod;
			}
			break;
		case T_MinMaxExpr:
			{
				/*
				 * If all the alternatives agree on type/typmod, return that
				 * typmod, else use -1
				 */
				MinMaxExpr *mexpr = (MinMaxExpr *) expr;
				Oid			minmaxtype = mexpr->minmaxtype;
				int32		typmod;
				ListCell   *arg;

				if (exprType((Node *) linitial(mexpr->args)) != minmaxtype)
					return -1;
				typmod = exprTypmod((Node *) linitial(mexpr->args));
				if (typmod < 0)
					return -1;	/* no point in trying harder */
				for_each_cell(arg, lnext(list_head(mexpr->args)))
				{
					Node	   *e = (Node *) lfirst(arg);

					if (exprType(e) != minmaxtype)
						return -1;
					if (exprTypmod(e) != typmod)
						return -1;
				}
				return typmod;
			}
			break;
		case T_NullIfExpr:
			{
				NullIfExpr *nexpr = (NullIfExpr *) expr;

				return exprTypmod((Node *) linitial(nexpr->args));
			}
			break;
		case T_CoerceToDomain:
			return ((CoerceToDomain *) expr)->resulttypmod;
		case T_CoerceToDomainValue:
			return ((CoerceToDomainValue *) expr)->typeMod;
		case T_SetToDefault:
			return ((SetToDefault *) expr)->typeMod;
		default:
			break;
	}
	return -1;
}

/*
 * exprIsLengthCoercion
 *		Detect whether an expression tree is an application of a datatype's
 *		typmod-coercion function.  Optionally extract the result's typmod.
 *
 * If coercedTypmod is not NULL, the typmod is stored there if the expression
 * is a length-coercion function, else -1 is stored there.
 *
 * Note that a combined type-and-length coercion will be treated as a
 * length coercion by this routine.
 */
bool
exprIsLengthCoercion(Node *expr, int32 *coercedTypmod)
{
	if (coercedTypmod != NULL)
		*coercedTypmod = -1;	/* default result on failure */

	/*
	 * Scalar-type length coercions are FuncExprs, array-type length coercions
	 * are ArrayCoerceExprs
	 */
	if (expr && IsA(expr, FuncExpr))
	{
		FuncExpr   *func = (FuncExpr *) expr;
		int			nargs;
		Const	   *second_arg;

		/*
		 * If it didn't come from a coercion context, reject.
		 */
		if (func->funcformat != COERCE_EXPLICIT_CAST &&
			func->funcformat != COERCE_IMPLICIT_CAST)
			return false;

		/*
		 * If it's not a two-argument or three-argument function with the
		 * second argument being an int4 constant, it can't have been created
		 * from a length coercion (it must be a type coercion, instead).
		 */
		nargs = list_length(func->args);
		if (nargs < 2 || nargs > 3)
			return false;

		second_arg = (Const *) lsecond(func->args);
		if (!IsA(second_arg, Const) ||
			second_arg->consttype != INT4OID ||
			second_arg->constisnull)
			return false;

		/*
		 * OK, it is indeed a length-coercion function.
		 */
		if (coercedTypmod != NULL)
			*coercedTypmod = DatumGetInt32(second_arg->constvalue);

		return true;
	}

	if (expr && IsA(expr, ArrayCoerceExpr))
	{
		ArrayCoerceExpr *acoerce = (ArrayCoerceExpr *) expr;

		/* It's not a length coercion unless there's a nondefault typmod */
		if (acoerce->resulttypmod < 0)
			return false;

		/*
		 * OK, it is indeed a length-coercion expression.
		 */
		if (coercedTypmod != NULL)
			*coercedTypmod = acoerce->resulttypmod;

		return true;
	}

	return false;
}

/*
 * Handle an explicit CAST construct.
 *
 * The given expr has already been transformed, but we need to lookup
 * the type name and then apply any necessary coercion function(s).
 */
static Node *
typecast_expression(ParseState *pstate, Node *expr, TypeName *typename)
{
	Oid			inputType = exprType(expr);
	Oid			targetType;
	int32		targetTypmod;

	targetType = typenameTypeId(pstate, typename, &targetTypmod);

	if (inputType == InvalidOid)
		return expr;			/* do nothing if NULL input */

	expr = coerce_to_target_type(pstate, expr, inputType,
								 targetType, targetTypmod,
								 COERCION_EXPLICIT,
								 COERCE_EXPLICIT_CAST);
	if (expr == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_CANNOT_COERCE),
				 errmsg("cannot cast type %s to %s",
						format_type_be(inputType),
						format_type_be(targetType)),
				 parser_errposition(pstate, typename->location)));

	return expr;
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
	List	  **opfamily_lists;
	List	  **opstrat_lists;
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

		cmp = (OpExpr *) make_op(pstate, opname, larg, rarg, location);
		Assert(IsA(cmp, OpExpr));

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
	 * apply to this set of operators.	We look for btree opfamilies
	 * containing the operators, and see which interpretations (strategy
	 * numbers) exist for each operator.
	 */
	opfamily_lists = (List **) palloc(nopers * sizeof(List *));
	opstrat_lists = (List **) palloc(nopers * sizeof(List *));
	strats = NULL;
	i = 0;
	foreach(l, opexprs)
	{
		Oid			opno = ((OpExpr *) lfirst(l))->opno;
		Bitmapset  *this_strats;
		ListCell   *j;

		get_op_btree_interpretation(opno,
									&opfamily_lists[i], &opstrat_lists[i]);

		/*
		 * convert strategy number list to a Bitmapset to make the
		 * intersection calculation easy.
		 */
		this_strats = NULL;
		foreach(j, opstrat_lists[i])
		{
			this_strats = bms_add_member(this_strats, lfirst_int(j));
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
	 *
	 * Note: this is presently the only place where the parser generates
	 * BoolExpr with more than two arguments.  Should be OK since the rest of
	 * the system thinks BoolExpr is N-argument anyway.
	 */
	if (rctype == ROWCOMPARE_EQ)
		return (Node *) makeBoolExpr(AND_EXPR, opexprs);
	if (rctype == ROWCOMPARE_NE)
		return (Node *) makeBoolExpr(OR_EXPR, opexprs);

	/*
	 * Otherwise we need to choose exactly which opfamily to associate with
	 * each operator.
	 */
	opfamilies = NIL;
	for (i = 0; i < nopers; i++)
	{
		Oid			opfamily = InvalidOid;

		forboth(l, opfamily_lists[i], r, opstrat_lists[i])
		{
			int			opstrat = lfirst_int(r);

			if (opstrat == rctype)
			{
				opfamily = lfirst_oid(l);
				break;
			}
		}
		if (OidIsValid(opfamily))
			opfamilies = lappend_oid(opfamilies, opfamily);
		else	/* should not happen */
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
										   list_make2(result, cmp));
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

	result = make_op(pstate, opname, ltree, rtree, location);
	if (((OpExpr *) result)->opresulttype != BOOLOID)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
			 errmsg("IS DISTINCT FROM requires = operator to yield boolean"),
				 parser_errposition(pstate, location)));

	/*
	 * We rely on DistinctExpr and OpExpr being same struct
	 */
	NodeSetTag(result, T_DistinctExpr);

	return result;
}
