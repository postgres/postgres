/*-------------------------------------------------------------------------
 *
 * parse_expr.c
 *	  handle expressions in parser
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_expr.c,v 1.163.2.4 2005/11/18 23:08:43 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "commands/dbcommands.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/params.h"
#include "nodes/plannodes.h"
#include "parser/analyze.h"
#include "parser/gramparse.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


int			max_expr_depth = DEFAULT_MAX_EXPR_DEPTH;
static int	expr_depth_counter = 0;

bool		Transform_null_equals = false;

static Node *typecast_expression(ParseState *pstate, Node *expr,
					TypeName *typename);
static Node *transformColumnRef(ParseState *pstate, ColumnRef *cref);
static Node *transformIndirection(ParseState *pstate, Node *basenode,
					 List *indirection);


/*
 * Initialize for parsing a new query.
 *
 * We reset the expression depth counter here, in case it was left nonzero
 * due to ereport()'ing out of the last parsing operation.
 */
void
parse_expr_init(void)
{
	expr_depth_counter = 0;
}


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

	/*
	 * Guard against an overly complex expression leading to coredump due
	 * to stack overflow here, or in later recursive routines that
	 * traverse expression trees.  Note that this is very unlikely to
	 * happen except with pathological queries; but we don't want someone
	 * to be able to crash the backend quite that easily...
	 */
	if (++expr_depth_counter > max_expr_depth)
		ereport(ERROR,
				(errcode(ERRCODE_STATEMENT_TOO_COMPLEX),
				 errmsg("expression too complex"),
				 errdetail("Nesting depth exceeds maximum expression depth %d.",
						   max_expr_depth),
				 errhint("Increase the configuration parameter \"max_expr_depth\".")));

	switch (nodeTag(expr))
	{
		case T_ColumnRef:
			{
				result = transformColumnRef(pstate, (ColumnRef *) expr);
				break;
			}
		case T_ParamRef:
			{
				ParamRef   *pref = (ParamRef *) expr;
				int			paramno = pref->number;
				ParseState *toppstate;
				Param	   *param;
				List	   *fields;

				/*
				 * Find topmost ParseState, which is where paramtype info
				 * lives.
				 */
				toppstate = pstate;
				while (toppstate->parentParseState != NULL)
					toppstate = toppstate->parentParseState;

				/* Check parameter number is in range */
				if (paramno <= 0)		/* probably can't happen? */
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_PARAMETER),
						  errmsg("there is no parameter $%d", paramno)));
				if (paramno > toppstate->p_numparams)
				{
					if (!toppstate->p_variableparams)
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_PARAMETER),
								 errmsg("there is no parameter $%d",
										paramno)));
					/* Okay to enlarge param array */
					if (toppstate->p_paramtypes)
						toppstate->p_paramtypes =
							(Oid *) repalloc(toppstate->p_paramtypes,
											 paramno * sizeof(Oid));
					else
						toppstate->p_paramtypes =
							(Oid *) palloc(paramno * sizeof(Oid));
					/* Zero out the previously-unreferenced slots */
					MemSet(toppstate->p_paramtypes + toppstate->p_numparams,
						   0,
					   (paramno - toppstate->p_numparams) * sizeof(Oid));
					toppstate->p_numparams = paramno;
				}
				if (toppstate->p_variableparams)
				{
					/* If not seen before, initialize to UNKNOWN type */
					if (toppstate->p_paramtypes[paramno - 1] == InvalidOid)
						toppstate->p_paramtypes[paramno - 1] = UNKNOWNOID;
				}

				param = makeNode(Param);
				param->paramkind = PARAM_NUM;
				param->paramid = (AttrNumber) paramno;
				param->paramtype = toppstate->p_paramtypes[paramno - 1];
				result = (Node *) param;

				/* handle qualification, if any */
				foreach(fields, pref->fields)
				{
					result = ParseFuncOrColumn(pstate,
											   makeList1(lfirst(fields)),
											   makeList1(result),
											   false, false, true);
				}
				/* handle subscripts, if any */
				result = transformIndirection(pstate, result,
											  pref->indirection);
				break;
			}
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
		case T_ExprFieldSelect:
			{
				ExprFieldSelect *efs = (ExprFieldSelect *) expr;
				List	   *fields;

				result = transformExpr(pstate, efs->arg);
				/* handle qualification, if any */
				foreach(fields, efs->fields)
				{
					result = ParseFuncOrColumn(pstate,
											   makeList1(lfirst(fields)),
											   makeList1(result),
											   false, false, true);
				}
				/* handle subscripts, if any */
				result = transformIndirection(pstate, result,
											  efs->indirection);
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
						{
							/*
							 * Special-case "foo = NULL" and "NULL = foo"
							 * for compatibility with standards-broken
							 * products (like Microsoft's).  Turn these
							 * into IS NULL exprs.
							 */
							if (Transform_null_equals &&
								length(a->name) == 1 &&
							 strcmp(strVal(lfirst(a->name)), "=") == 0 &&
								(exprIsNullConstant(a->lexpr) ||
								 exprIsNullConstant(a->rexpr)))
							{
								NullTest   *n = makeNode(NullTest);

								n->nulltesttype = IS_NULL;

								if (exprIsNullConstant(a->lexpr))
									n->arg = (Expr *) a->rexpr;
								else
									n->arg = (Expr *) a->lexpr;

								result = transformExpr(pstate,
													   (Node *) n);
							}
							else
							{
								Node	   *lexpr = transformExpr(pstate,
															   a->lexpr);
								Node	   *rexpr = transformExpr(pstate,
															   a->rexpr);

								result = (Node *) make_op(pstate,
														  a->name,
														  lexpr,
														  rexpr);
							}
						}
						break;
					case AEXPR_AND:
						{
							Node	   *lexpr = transformExpr(pstate,
															  a->lexpr);
							Node	   *rexpr = transformExpr(pstate,
															  a->rexpr);

							lexpr = coerce_to_boolean(pstate, lexpr, "AND");
							rexpr = coerce_to_boolean(pstate, rexpr, "AND");

							result = (Node *) makeBoolExpr(AND_EXPR,
														 makeList2(lexpr,
																 rexpr));
						}
						break;
					case AEXPR_OR:
						{
							Node	   *lexpr = transformExpr(pstate,
															  a->lexpr);
							Node	   *rexpr = transformExpr(pstate,
															  a->rexpr);

							lexpr = coerce_to_boolean(pstate, lexpr, "OR");
							rexpr = coerce_to_boolean(pstate, rexpr, "OR");

							result = (Node *) makeBoolExpr(OR_EXPR,
														 makeList2(lexpr,
																 rexpr));
						}
						break;
					case AEXPR_NOT:
						{
							Node	   *rexpr = transformExpr(pstate,
															  a->rexpr);

							rexpr = coerce_to_boolean(pstate, rexpr, "NOT");

							result = (Node *) makeBoolExpr(NOT_EXPR,
													   makeList1(rexpr));
						}
						break;
					case AEXPR_OP_ANY:
						{
							Node	   *lexpr = transformExpr(pstate,
															  a->lexpr);
							Node	   *rexpr = transformExpr(pstate,
															  a->rexpr);

							result = (Node *) make_scalar_array_op(pstate,
																 a->name,
																   true,
																   lexpr,
																   rexpr);
						}
						break;
					case AEXPR_OP_ALL:
						{
							Node	   *lexpr = transformExpr(pstate,
															  a->lexpr);
							Node	   *rexpr = transformExpr(pstate,
															  a->rexpr);

							result = (Node *) make_scalar_array_op(pstate,
																 a->name,
																   false,
																   lexpr,
																   rexpr);
						}
						break;
					case AEXPR_DISTINCT:
						{
							Node	   *lexpr = transformExpr(pstate,
															  a->lexpr);
							Node	   *rexpr = transformExpr(pstate,
															  a->rexpr);

							result = (Node *) make_op(pstate,
													  a->name,
													  lexpr,
													  rexpr);
							if (((OpExpr *) result)->opresulttype != BOOLOID)
								ereport(ERROR,
									 (errcode(ERRCODE_DATATYPE_MISMATCH),
									  errmsg("IS DISTINCT FROM requires = operator to yield boolean")));

							/*
							 * We rely on DistinctExpr and OpExpr being
							 * same struct
							 */
							NodeSetTag(result, T_DistinctExpr);
						}
						break;
					case AEXPR_NULLIF:
						{
							Node	   *lexpr = transformExpr(pstate,
															  a->lexpr);
							Node	   *rexpr = transformExpr(pstate,
															  a->rexpr);

							result = (Node *) make_op(pstate,
													  a->name,
													  lexpr,
													  rexpr);
							if (((OpExpr *) result)->opresulttype != BOOLOID)
								ereport(ERROR,
									 (errcode(ERRCODE_DATATYPE_MISMATCH),
									  errmsg("NULLIF requires = operator to yield boolean")));

							/*
							 * We rely on NullIfExpr and OpExpr being same
							 * struct
							 */
							NodeSetTag(result, T_NullIfExpr);
						}
						break;
					case AEXPR_OF:
						{
							/*
							 * Checking an expression for match to type.
							 * Will result in a boolean constant node.
							 */
							List	   *telem;
							A_Const    *n;
							Oid			ltype,
										rtype;
							bool		matched = FALSE;
							Node	   *lexpr = transformExpr(pstate,
															  a->lexpr);

							ltype = exprType(lexpr);
							foreach(telem, (List *) a->rexpr)
							{
								rtype = LookupTypeName(lfirst(telem));
								matched = (rtype == ltype);
								if (matched)
									break;
							}

							/*
							 * Expect two forms: equals or not equals.
							 * Flip the sense of the result for not
							 * equals.
							 */
							if (strcmp(strVal(lfirst(a->name)), "!=") == 0)
								matched = (!matched);

							n = makeNode(A_Const);
							n->val.type = T_String;
							n->val.val.str = (matched ? "t" : "f");
							n->typename = SystemTypeName("bool");

							result = transformExpr(pstate, (Node *) n);
						}
						break;
				}
				break;
			}
		case T_FuncCall:
			{
				FuncCall   *fn = (FuncCall *) expr;
				List	   *targs;
				List	   *args;

				/*
				 * Transform the list of arguments.  We use a shallow list
				 * copy and then transform-in-place to avoid O(N^2)
				 * behavior from repeated lappend's.
				 */
				targs = listCopy(fn->args);
				foreach(args, targs)
				{
					lfirst(args) = transformExpr(pstate,
												 (Node *) lfirst(args));
				}
				result = ParseFuncOrColumn(pstate,
										   fn->funcname,
										   targs,
										   fn->agg_star,
										   fn->agg_distinct,
										   false);
				break;
			}
		case T_SubLink:
			{
				SubLink    *sublink = (SubLink *) expr;
				List	   *qtrees;
				Query	   *qtree;

				/* If we already transformed this node, do nothing */
				if (IsA(sublink->subselect, Query))
				{
					result = expr;
					break;
				}
				pstate->p_hasSubLinks = true;
				qtrees = parse_sub_analyze(sublink->subselect, pstate);
				if (length(qtrees) != 1)
					elog(ERROR, "bad query in sub-select");
				qtree = (Query *) lfirst(qtrees);
				if (qtree->commandType != CMD_SELECT ||
					qtree->resultRelation != 0)
					elog(ERROR, "bad query in sub-select");
				sublink->subselect = (Node *) qtree;

				if (sublink->subLinkType == EXISTS_SUBLINK)
				{
					/*
					 * EXISTS needs no lefthand or combining operator.
					 * These fields should be NIL already, but make sure.
					 */
					sublink->lefthand = NIL;
					sublink->operName = NIL;
					sublink->operOids = NIL;
					sublink->useOr = FALSE;
				}
				else if (sublink->subLinkType == EXPR_SUBLINK ||
						 sublink->subLinkType == ARRAY_SUBLINK)
				{
					List	   *tlist = qtree->targetList;

					/*
					 * Make sure the subselect delivers a single column
					 * (ignoring resjunk targets).
					 */
					if (tlist == NIL ||
						((TargetEntry *) lfirst(tlist))->resdom->resjunk)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("subquery must return a column")));
					while ((tlist = lnext(tlist)) != NIL)
					{
						if (!((TargetEntry *) lfirst(tlist))->resdom->resjunk)
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("subquery must return only one column")));
					}

					/*
					 * EXPR and ARRAY need no lefthand or combining
					 * operator. These fields should be NIL already, but
					 * make sure.
					 */
					sublink->lefthand = NIL;
					sublink->operName = NIL;
					sublink->operOids = NIL;
					sublink->useOr = FALSE;
				}
				else
				{
					/* ALL, ANY, or MULTIEXPR: generate operator list */
					List	   *left_list = sublink->lefthand;
					List	   *right_list = qtree->targetList;
					int			row_length = length(left_list);
					bool		needNot = false;
					List	   *op = sublink->operName;
					char	   *opname = strVal(llast(op));
					List	   *elist;

					/* transform lefthand expressions */
					foreach(elist, left_list)
						lfirst(elist) = transformExpr(pstate, lfirst(elist));

					/*
					 * If the expression is "<> ALL" (with unqualified
					 * opname) then convert it to "NOT IN".  This is a
					 * hack to improve efficiency of expressions output by
					 * pre-7.4 Postgres.
					 */
					if (sublink->subLinkType == ALL_SUBLINK &&
						length(op) == 1 && strcmp(opname, "<>") == 0)
					{
						sublink->subLinkType = ANY_SUBLINK;
						opname = pstrdup("=");
						op = makeList1(makeString(opname));
						sublink->operName = op;
						needNot = true;
					}

					/* Set useOr if op is "<>" (possibly qualified) */
					if (strcmp(opname, "<>") == 0)
						sublink->useOr = TRUE;
					else
						sublink->useOr = FALSE;

					/* Combining operators other than =/<> is dubious... */
					if (row_length != 1 &&
						strcmp(opname, "=") != 0 &&
						strcmp(opname, "<>") != 0)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						  errmsg("row comparison cannot use operator %s",
								 opname)));

					/*
					 * To build the list of combining operator OIDs, we
					 * must scan subquery's targetlist to find values that
					 * will be matched against lefthand values.  We need
					 * to ignore resjunk targets, so doing the outer
					 * iteration over right_list is easier than doing it
					 * over left_list.
					 */
					sublink->operOids = NIL;

					while (right_list != NIL)
					{
						TargetEntry *tent = (TargetEntry *) lfirst(right_list);
						Node	   *lexpr;
						Operator	optup;
						Form_pg_operator opform;

						right_list = lnext(right_list);
						if (tent->resdom->resjunk)
							continue;

						if (left_list == NIL)
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("subquery has too many columns")));
						lexpr = lfirst(left_list);
						left_list = lnext(left_list);

						/*
						 * It's OK to use oper() not compatible_oper()
						 * here, because make_subplan() will insert type
						 * coercion calls if needed.
						 */
						optup = oper(op,
									 exprType(lexpr),
									 exprType((Node *) tent->expr),
									 false);
						opform = (Form_pg_operator) GETSTRUCT(optup);

						if (opform->oprresult != BOOLOID)
							ereport(ERROR,
									(errcode(ERRCODE_DATATYPE_MISMATCH),
									 errmsg("operator %s must return type boolean, not type %s",
											opname,
									  format_type_be(opform->oprresult)),
									 errhint("The operator of a quantified predicate subquery must return type boolean.")));

						if (get_func_retset(opform->oprcode))
							ereport(ERROR,
									(errcode(ERRCODE_DATATYPE_MISMATCH),
							  errmsg("operator %s must not return a set",
									 opname),
									 errhint("The operator of a quantified predicate subquery must return type boolean.")));

						sublink->operOids = lappendo(sublink->operOids,
													 oprid(optup));

						ReleaseSysCache(optup);
					}
					if (left_list != NIL)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
							  errmsg("subquery has too few columns")));

					if (needNot)
					{
						expr = coerce_to_boolean(pstate, expr, "NOT");
						expr = (Node *) makeBoolExpr(NOT_EXPR,
													 makeList1(expr));
					}
				}
				result = (Node *) expr;
				break;
			}

		case T_CaseExpr:
			{
				CaseExpr   *c = (CaseExpr *) expr;
				CaseExpr   *newc = makeNode(CaseExpr);
				List	   *newargs = NIL;
				List	   *typeids = NIL;
				List	   *args;
				Node	   *defresult;
				Oid			ptype;

				/* transform the list of arguments */
				foreach(args, c->args)
				{
					CaseWhen   *w = (CaseWhen *) lfirst(args);
					CaseWhen   *neww = makeNode(CaseWhen);
					Node	   *warg;

					Assert(IsA(w, CaseWhen));

					warg = (Node *) w->expr;
					if (c->arg != NULL)
					{
						/* shorthand form was specified, so expand... */
						warg = (Node *) makeSimpleA_Expr(AEXPR_OP, "=",
														 (Node *) c->arg,
														 warg);
					}
					neww->expr = (Expr *) transformExpr(pstate, warg);

					neww->expr = (Expr *) coerce_to_boolean(pstate,
													 (Node *) neww->expr,
															"CASE/WHEN");

					/*
					 * result is NULL for NULLIF() construct - thomas
					 * 1998-11-11
					 */
					warg = (Node *) w->result;
					if (warg == NULL)
					{
						A_Const    *n = makeNode(A_Const);

						n->val.type = T_Null;
						warg = (Node *) n;
					}
					neww->result = (Expr *) transformExpr(pstate, warg);

					newargs = lappend(newargs, neww);
					typeids = lappendo(typeids, exprType((Node *) neww->result));
				}

				newc->args = newargs;

				/*
				 * It's not shorthand anymore, so drop the implicit
				 * argument. This is necessary to keep any re-application
				 * of transformExpr from doing the wrong thing.
				 */
				newc->arg = NULL;

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
				 * Note: default result is considered the most significant
				 * type in determining preferred type.	This is how the
				 * code worked before, but it seems a little bogus to me
				 * --- tgl
				 */
				typeids = lconso(exprType((Node *) newc->defresult), typeids);

				ptype = select_common_type(typeids, "CASE");
				newc->casetype = ptype;

				/* Convert default result clause, if necessary */
				newc->defresult = (Expr *)
					coerce_to_common_type(pstate,
										  (Node *) newc->defresult,
										  ptype,
										  "CASE/ELSE");

				/* Convert when-clause results, if necessary */
				foreach(args, newc->args)
				{
					CaseWhen   *w = (CaseWhen *) lfirst(args);

					w->result = (Expr *)
						coerce_to_common_type(pstate,
											  (Node *) w->result,
											  ptype,
											  "CASE/WHEN");
				}

				result = (Node *) newc;
				break;
			}

		case T_ArrayExpr:
			{
				ArrayExpr  *a = (ArrayExpr *) expr;
				ArrayExpr  *newa = makeNode(ArrayExpr);
				List	   *newelems = NIL;
				List	   *newcoercedelems = NIL;
				List	   *typeids = NIL;
				List	   *element;
				Oid			array_type;
				Oid			element_type;

				/* Transform the element expressions */
				foreach(element, a->elements)
				{
					Node	   *e = (Node *) lfirst(element);
					Node	   *newe;

					newe = transformExpr(pstate, e);
					newelems = lappend(newelems, newe);
					typeids = lappendo(typeids, exprType(newe));
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

				result = (Node *) newa;
				break;
			}

		case T_CoalesceExpr:
			{
				CoalesceExpr *c = (CoalesceExpr *) expr;
				CoalesceExpr *newc = makeNode(CoalesceExpr);
				List	   *newargs = NIL;
				List	   *newcoercedargs = NIL;
				List	   *typeids = NIL;
				List	   *args;

				foreach(args, c->args)
				{
					Node	   *e = (Node *) lfirst(args);
					Node	   *newe;

					newe = transformExpr(pstate, e);
					newargs = lappend(newargs, newe);
					typeids = lappendo(typeids, exprType(newe));
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
				result = (Node *) newc;
				break;
			}

		case T_NullTest:
			{
				NullTest   *n = (NullTest *) expr;

				n->arg = (Expr *) transformExpr(pstate, (Node *) n->arg);
				/* the argument can be any type, so don't coerce it */
				result = expr;
				break;
			}

		case T_BooleanTest:
			{
				BooleanTest *b = (BooleanTest *) expr;
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
						clausename = NULL;		/* keep compiler quiet */
				}

				b->arg = (Expr *) transformExpr(pstate, (Node *) b->arg);

				b->arg = (Expr *) coerce_to_boolean(pstate,
													(Node *) b->arg,
													clausename);

				result = expr;
				break;
			}

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
		case T_RelabelType:
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

	expr_depth_counter--;

	return result;
}

static Node *
transformIndirection(ParseState *pstate, Node *basenode, List *indirection)
{
	if (indirection == NIL)
		return basenode;
	return (Node *) transformArraySubscripts(pstate,
											 basenode,
											 exprType(basenode),
											 exprTypmod(basenode),
											 indirection,
											 false,
											 NULL);
}

static Node *
transformColumnRef(ParseState *pstate, ColumnRef *cref)
{
	int			numnames = length(cref->fields);
	Node	   *node;
	RangeVar   *rv;
	int			levels_up;

	/*----------
	 * The allowed syntaxes are:
	 *
	 * A		First try to resolve as unqualified column name;
	 *			if no luck, try to resolve as unqual. table name (A.*).
	 * A.B		A is an unqual. table name; B is either a
	 *			column or function name (trying column name first).
	 * A.B.C	schema A, table B, col or func name C.
	 * A.B.C.D	catalog A, schema B, table C, col or func D.
	 * A.*		A is an unqual. table name; means whole-row value.
	 * A.B.*	whole-row value of table B in schema A.
	 * A.B.C.*	whole-row value of table C in schema B in catalog A.
	 *
	 * We do not need to cope with bare "*"; that will only be accepted by
	 * the grammar at the top level of a SELECT list, and transformTargetList
	 * will take care of it before it ever gets here.
	 *
	 * Currently, if a catalog name is given then it must equal the current
	 * database name; we check it here and then discard it.
	 *
	 * For whole-row references, the result is an untransformed RangeVar,
	 * which will work as the argument to a function call, but not in any
	 * other context at present.  (We could instead coerce to a whole-row Var,
	 * but that will fail for subselect and join RTEs, because there is no
	 * pg_type entry for their rowtypes.)
	 *----------
	 */
	switch (numnames)
	{
		case 1:
			{
				char	   *name = strVal(lfirst(cref->fields));

				/* Try to identify as an unqualified column */
				node = colNameToVar(pstate, name, false);

				if (node == NULL)
				{
					/*
					 * Not known as a column of any range-table entry.
					 *
					 * Consider the possibility that it's VALUE in a domain
					 * check expression.  (We handle VALUE as a name, not
					 * a keyword, to avoid breaking a lot of applications
					 * that have used VALUE as a column name in the past.)
					 */
					if (pstate->p_value_substitute != NULL &&
						strcmp(name, "value") == 0)
					{
						node = (Node *) copyObject(pstate->p_value_substitute);
						break;
					}

					/*
					 * Try to find the name as a relation ... but not if
					 * subscripts appear.  Note also that only relations
					 * already entered into the rangetable will be
					 * recognized.
					 *
					 * This is a hack for backwards compatibility with
					 * PostQUEL-inspired syntax.  The preferred form now
					 * is "rel.*".
					 */
					if (cref->indirection == NIL &&
						refnameRangeTblEntry(pstate, NULL, name,
											 &levels_up) != NULL)
					{
						rv = makeNode(RangeVar);
						rv->relname = name;
						rv->inhOpt = INH_DEFAULT;
						node = (Node *) rv;
					}
					else
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_COLUMN),
							errmsg("column \"%s\" does not exist", name)));
				}
				break;
			}
		case 2:
			{
				char	   *name1 = strVal(lfirst(cref->fields));
				char	   *name2 = strVal(lsecond(cref->fields));

				/* Whole-row reference? */
				if (strcmp(name2, "*") == 0)
				{
					rv = makeNode(RangeVar);
					rv->relname = name1;
					rv->inhOpt = INH_DEFAULT;
					node = (Node *) rv;
					break;
				}

				/* Try to identify as a once-qualified column */
				node = qualifiedNameToVar(pstate, NULL, name1, name2, true);
				if (node == NULL)
				{
					/*
					 * Not known as a column of any range-table entry, so
					 * try it as a function call.  Here, we will create an
					 * implicit RTE for tables not already entered.
					 */
					rv = makeNode(RangeVar);
					rv->relname = name1;
					rv->inhOpt = INH_DEFAULT;
					node = ParseFuncOrColumn(pstate,
											 makeList1(makeString(name2)),
											 makeList1(rv),
											 false, false, true);
				}
				break;
			}
		case 3:
			{
				char	   *name1 = strVal(lfirst(cref->fields));
				char	   *name2 = strVal(lsecond(cref->fields));
				char	   *name3 = strVal(lthird(cref->fields));

				/* Whole-row reference? */
				if (strcmp(name3, "*") == 0)
				{
					rv = makeNode(RangeVar);
					rv->schemaname = name1;
					rv->relname = name2;
					rv->inhOpt = INH_DEFAULT;
					node = (Node *) rv;
					break;
				}

				/* Try to identify as a twice-qualified column */
				node = qualifiedNameToVar(pstate, name1, name2, name3, true);
				if (node == NULL)
				{
					/* Try it as a function call */
					rv = makeNode(RangeVar);
					rv->schemaname = name1;
					rv->relname = name2;
					rv->inhOpt = INH_DEFAULT;
					node = ParseFuncOrColumn(pstate,
											 makeList1(makeString(name3)),
											 makeList1(rv),
											 false, false, true);
				}
				break;
			}
		case 4:
			{
				char	   *name1 = strVal(lfirst(cref->fields));
				char	   *name2 = strVal(lsecond(cref->fields));
				char	   *name3 = strVal(lthird(cref->fields));
				char	   *name4 = strVal(lfourth(cref->fields));

				/*
				 * We check the catalog name and then ignore it.
				 */
				if (strcmp(name1, get_database_name(MyDatabaseId)) != 0)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cross-database references are not implemented")));

				/* Whole-row reference? */
				if (strcmp(name4, "*") == 0)
				{
					rv = makeNode(RangeVar);
					rv->schemaname = name2;
					rv->relname = name3;
					rv->inhOpt = INH_DEFAULT;
					node = (Node *) rv;
					break;
				}

				/* Try to identify as a twice-qualified column */
				node = qualifiedNameToVar(pstate, name2, name3, name4, true);
				if (node == NULL)
				{
					/* Try it as a function call */
					rv = makeNode(RangeVar);
					rv->schemaname = name2;
					rv->relname = name3;
					rv->inhOpt = INH_DEFAULT;
					node = ParseFuncOrColumn(pstate,
											 makeList1(makeString(name4)),
											 makeList1(rv),
											 false, false, true);
				}
				break;
			}
		default:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
			errmsg("improper qualified name (too many dotted names): %s",
				   NameListToString(cref->fields))));
			node = NULL;		/* keep compiler quiet */
			break;
	}

	return transformIndirection(pstate, node, cref->indirection);
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
			type = ((ArrayRef *) expr)->refrestype;
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
					tent = (TargetEntry *) lfirst(qtree->targetList);
					Assert(IsA(tent, TargetEntry));
					Assert(!tent->resdom->resjunk);
					if (sublink->subLinkType == EXPR_SUBLINK)
						type = tent->resdom->restype;
					else
/* ARRAY_SUBLINK */
					{
						type = get_array_type(tent->resdom->restype);
						if (!OidIsValid(type))
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_OBJECT),
									 errmsg("could not find array type for data type %s",
								format_type_be(tent->resdom->restype))));
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
				 * Although the parser does not ever deal with
				 * already-planned expression trees, we support SubPlan
				 * nodes in this routine for the convenience of
				 * ruleutils.c.
				 */
				SubPlan    *subplan = (SubPlan *) expr;

				if (subplan->subLinkType == EXPR_SUBLINK ||
					subplan->subLinkType == ARRAY_SUBLINK)
				{
					/* get the type of the subselect's first target column */
					TargetEntry *tent;

					tent = (TargetEntry *) lfirst(subplan->plan->targetlist);
					Assert(IsA(tent, TargetEntry));
					Assert(!tent->resdom->resjunk);
					if (subplan->subLinkType == EXPR_SUBLINK)
						type = tent->resdom->restype;
					else
/* ARRAY_SUBLINK */
					{
						type = get_array_type(tent->resdom->restype);
						if (!OidIsValid(type))
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_OBJECT),
									 errmsg("could not find array type for data type %s",
								format_type_be(tent->resdom->restype))));
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
		case T_RelabelType:
			type = ((RelabelType *) expr)->resulttype;
			break;
		case T_CaseExpr:
			type = ((CaseExpr *) expr)->casetype;
			break;
		case T_CaseWhen:
			type = exprType((Node *) ((CaseWhen *) expr)->result);
			break;
		case T_ArrayExpr:
			type = ((ArrayExpr *) expr)->array_typeid;
			break;
		case T_CoalesceExpr:
			type = ((CoalesceExpr *) expr)->coalescetype;
			break;
		case T_NullIfExpr:
			type = exprType((Node *) lfirst(((NullIfExpr *) expr)->args));
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
		case T_RangeVar:

			/*
			 * If someone uses a bare relation name in an expression, we
			 * will likely first notice a problem here (see comments in
			 * transformColumnRef()).  Issue an appropriate error message.
			 */
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("relation reference \"%s\" cannot be used in an expression",
							((RangeVar *) expr)->relname)));
			type = InvalidOid;	/* keep compiler quiet */
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
			{
				/* Be smart about string constants... */
				Const	   *con = (Const *) expr;

				switch (con->consttype)
				{
					case BPCHAROID:
						if (!con->constisnull)
						{
							int32 len = VARSIZE(DatumGetPointer(con->constvalue)) - VARHDRSZ;

							if (pg_database_encoding_max_length() > 1)
								len = pg_mbstrlen_with_len(VARDATA(DatumGetPointer(con->constvalue)), len);
							return len + VARHDRSZ;
						}
						break;
					default:
						break;
				}
			}
			break;
		case T_FuncExpr:
			{
				int32		coercedTypmod;

				/* Be smart about length-coercion functions... */
				if (exprIsLengthCoercion(expr, &coercedTypmod))
					return coercedTypmod;
			}
			break;
		case T_FieldSelect:
			return ((FieldSelect *) expr)->resulttypmod;
		case T_RelabelType:
			return ((RelabelType *) expr)->resulttypmod;
		case T_CaseExpr:
			{
				/*
				 * If all the alternatives agree on type/typmod, return
				 * that typmod, else use -1
				 */
				CaseExpr   *cexpr = (CaseExpr *) expr;
				Oid			casetype = cexpr->casetype;
				int32		typmod;
				List	   *arg;

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
		case T_CoalesceExpr:
			{
				/*
				 * If all the alternatives agree on type/typmod, return
				 * that typmod, else use -1
				 */
				CoalesceExpr *cexpr = (CoalesceExpr *) expr;
				Oid			coalescetype = cexpr->coalescetype;
				int32		typmod;
				List	   *arg;

				if (exprType((Node *) lfirst(cexpr->args)) != coalescetype)
					return -1;
				typmod = exprTypmod((Node *) lfirst(cexpr->args));
				if (typmod < 0)
					return -1;	/* no point in trying harder */
				foreach(arg, lnext(cexpr->args))
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
		case T_NullIfExpr:
			{
				NullIfExpr *nexpr = (NullIfExpr *) expr;

				return exprTypmod((Node *) lfirst(nexpr->args));
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
 */
bool
exprIsLengthCoercion(Node *expr, int32 *coercedTypmod)
{
	FuncExpr   *func;
	int			nargs;
	Const	   *second_arg;

	if (coercedTypmod != NULL)
		*coercedTypmod = -1;	/* default result on failure */

	/* Is it a function-call at all? */
	if (expr == NULL || !IsA(expr, FuncExpr))
		return false;
	func = (FuncExpr *) expr;

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
	nargs = length(func->args);
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

	targetType = typenameTypeId(typename);

	if (inputType == InvalidOid)
		return expr;			/* do nothing if NULL input */

	expr = coerce_to_target_type(pstate, expr, inputType,
								 targetType, typename->typmod,
								 COERCION_EXPLICIT,
								 COERCE_EXPLICIT_CAST);
	if (expr == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_CANNOT_COERCE),
				 errmsg("cannot cast type %s to %s",
						format_type_be(inputType),
						format_type_be(targetType))));

	return expr;
}
