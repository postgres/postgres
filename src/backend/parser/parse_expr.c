/*-------------------------------------------------------------------------
 *
 * parse_expr.c
 *	  handle expressions in parser
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_expr.c,v 1.69 2000/02/20 21:32:10 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_operator.h"
#include "nodes/makefuncs.h"
#include "nodes/params.h"
#include "nodes/relation.h"
#include "parse.h"
#include "parser/analyze.h"
#include "parser/gramparse.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "utils/builtins.h"

static Node *parser_typecast_constant(Value *expr, TypeName *typename);
static Node *parser_typecast_expression(ParseState *pstate,
										Node *expr, TypeName *typename);
static Node *transformAttr(ParseState *pstate, Attr *att, int precedence);
static Node *transformIdent(ParseState *pstate, Ident *ident, int precedence);
static Node *transformIndirection(ParseState *pstate, Node *basenode,
								  List *indirection);

/*
 * transformExpr -
 *	  analyze and transform expressions. Type checking and type casting is
 *	  done here. The optimizer and the executor cannot handle the original
 *	  (raw) expressions collected by the parse tree. Hence the transformation
 *	  here.
 */
Node *
transformExpr(ParseState *pstate, Node *expr, int precedence)
{
	Node	   *result = NULL;

	if (expr == NULL)
		return NULL;

	switch (nodeTag(expr))
	{
		case T_Attr:
			{
				result = transformAttr(pstate, (Attr *) expr, precedence);
				break;
			}
		case T_A_Const:
			{
				A_Const    *con = (A_Const *) expr;
				Value	   *val = &con->val;

				if (con->typename != NULL)
					result = parser_typecast_constant(val, con->typename);
				else
					result = (Node *) make_const(val);
				break;
			}
		case T_ParamNo:
			{
				ParamNo    *pno = (ParamNo *) expr;
				int			paramno = pno->number;
				Oid			toid = param_type(paramno);
				Param	   *param;

				if (!OidIsValid(toid))
					elog(ERROR, "Parameter '$%d' is out of range", paramno);
				param = makeNode(Param);
				param->paramkind = PARAM_NUM;
				param->paramid = (AttrNumber) paramno;
				param->paramname = "<unnamed>";
				param->paramtype = (Oid) toid;
				param->param_tlist = (List *) NULL;
				result = transformIndirection(pstate, (Node *) param,
											  pno->indirection);
				/* XXX what about cast (typename) applied to Param ??? */
				break;
			}
		case T_TypeCast:
			{
				TypeCast   *tc = (TypeCast *) expr;
				Node	   *arg = transformExpr(pstate, tc->arg, precedence);

				result = parser_typecast_expression(pstate, arg, tc->typename);
				break;
			}
		case T_A_Expr:
			{
				A_Expr	   *a = (A_Expr *) expr;

				switch (a->oper)
				{
					case OP:
						{
							Node	   *lexpr = transformExpr(pstate, a->lexpr, precedence);
							Node	   *rexpr = transformExpr(pstate, a->rexpr, precedence);

							result = (Node *) make_op(a->opname, lexpr, rexpr);
						}
						break;
					case ISNULL:
						{
							Node	   *lexpr = transformExpr(pstate, a->lexpr, precedence);

							result = ParseFuncOrColumn(pstate,
													   "nullvalue",
													   lcons(lexpr, NIL),
													   false, false,
													   &pstate->p_last_resno,
													   precedence);
						}
						break;
					case NOTNULL:
						{
							Node	   *lexpr = transformExpr(pstate, a->lexpr, precedence);

							result = ParseFuncOrColumn(pstate,
													   "nonnullvalue",
													   lcons(lexpr, NIL),
													   false, false,
													   &pstate->p_last_resno,
													   precedence);
						}
						break;
					case AND:
						{
							Expr	   *expr = makeNode(Expr);
							Node	   *lexpr = transformExpr(pstate, a->lexpr, precedence);
							Node	   *rexpr = transformExpr(pstate, a->rexpr, precedence);

							if (exprType(lexpr) != BOOLOID)
								elog(ERROR, "left-hand side of AND is type '%s', not '%s'",
									 typeidTypeName(exprType(lexpr)), typeidTypeName(BOOLOID));

							if (exprType(rexpr) != BOOLOID)
								elog(ERROR, "right-hand side of AND is type '%s', not '%s'",
									 typeidTypeName(exprType(rexpr)), typeidTypeName(BOOLOID));

							expr->typeOid = BOOLOID;
							expr->opType = AND_EXPR;
							expr->args = makeList(lexpr, rexpr, -1);
							result = (Node *) expr;
						}
						break;
					case OR:
						{
							Expr	   *expr = makeNode(Expr);
							Node	   *lexpr = transformExpr(pstate, a->lexpr, precedence);
							Node	   *rexpr = transformExpr(pstate, a->rexpr, precedence);

							if (exprType(lexpr) != BOOLOID)
								elog(ERROR, "left-hand side of OR is type '%s', not '%s'",
									 typeidTypeName(exprType(lexpr)), typeidTypeName(BOOLOID));
							if (exprType(rexpr) != BOOLOID)
								elog(ERROR, "right-hand side of OR is type '%s', not '%s'",
									 typeidTypeName(exprType(rexpr)), typeidTypeName(BOOLOID));
							expr->typeOid = BOOLOID;
							expr->opType = OR_EXPR;
							expr->args = makeList(lexpr, rexpr, -1);
							result = (Node *) expr;
						}
						break;
					case NOT:
						{
							Expr	   *expr = makeNode(Expr);
							Node	   *rexpr = transformExpr(pstate, a->rexpr, precedence);

							if (exprType(rexpr) != BOOLOID)
								elog(ERROR, "argument to NOT is type '%s', not '%s'",
									 typeidTypeName(exprType(rexpr)), typeidTypeName(BOOLOID));
							expr->typeOid = BOOLOID;
							expr->opType = NOT_EXPR;
							expr->args = makeList(rexpr, -1);
							result = (Node *) expr;
						}
						break;
				}
				break;
			}
		case T_Ident:
			{
				result = transformIdent(pstate, (Ident *) expr, precedence);
				break;
			}
		case T_FuncCall:
			{
				FuncCall   *fn = (FuncCall *) expr;
				List	   *args;

				/* transform the list of arguments */
				foreach(args, fn->args)
					lfirst(args) = transformExpr(pstate, (Node *) lfirst(args), precedence);
				result = ParseFuncOrColumn(pstate,
										   fn->funcname,
										   fn->args,
										   fn->agg_star,
										   fn->agg_distinct,
										   &pstate->p_last_resno,
										   precedence);
				break;
			}
		case T_SubLink:
			{
				SubLink    *sublink = (SubLink *) expr;
				List	   *qtrees;
				Query	   *qtree;

				pstate->p_hasSubLinks = true;
				qtrees = parse_analyze(lcons(sublink->subselect, NIL), pstate);
				if (length(qtrees) != 1)
					elog(ERROR, "Bad query in subselect");
				qtree = (Query *) lfirst(qtrees);
				if (qtree->commandType != CMD_SELECT ||
					qtree->resultRelation != 0)
					elog(ERROR, "Bad query in subselect");
				sublink->subselect = (Node *) qtree;

				if (sublink->subLinkType == EXISTS_SUBLINK)
				{
					/* EXISTS needs no lefthand or combining operator.
					 * These fields should be NIL already, but make sure.
					 */
					sublink->lefthand = NIL;
					sublink->oper = NIL;
				}
				else if (sublink->subLinkType == EXPR_SUBLINK)
				{
					List	   *tlist = qtree->targetList;

					/* Make sure the subselect delivers a single column
					 * (ignoring resjunk targets).
					 */
					if (tlist == NIL ||
						((TargetEntry *) lfirst(tlist))->resdom->resjunk)
						elog(ERROR, "Subselect must have a field");
					while ((tlist = lnext(tlist)) != NIL)
					{
						if (! ((TargetEntry *) lfirst(tlist))->resdom->resjunk)
							elog(ERROR, "Subselect must have only one field");
					}
					/* EXPR needs no lefthand or combining operator.
					 * These fields should be NIL already, but make sure.
					 */
					sublink->lefthand = NIL;
					sublink->oper = NIL;
				}
				else
				{
					/* ALL, ANY, or MULTIEXPR: generate operator list */
					char	   *op = lfirst(sublink->oper);
					List	   *left_list = sublink->lefthand;
					List	   *right_list = qtree->targetList;
					List	   *elist;

					foreach(elist, left_list)
						lfirst(elist) = transformExpr(pstate, lfirst(elist),
													  precedence);

					/* Combining operators other than =/<> is dubious... */
					if (length(left_list) != 1 &&
						strcmp(op, "=") != 0 && strcmp(op, "<>") != 0)
						elog(ERROR, "Row comparison cannot use '%s'",
							 op);

					sublink->oper = NIL;

					/* Scan subquery's targetlist to find values that will be
					 * matched against lefthand values.  We need to ignore
					 * resjunk targets, so doing the outer iteration over
					 * right_list is easier than doing it over left_list.
					 */
					while (right_list != NIL)
					{
						TargetEntry *tent = (TargetEntry *) lfirst(right_list);
						Node	   *lexpr;
						Operator	optup;
						Form_pg_operator opform;
						Oper	   *newop;

						right_list = lnext(right_list);
						if (tent->resdom->resjunk)
							continue;

						if (left_list == NIL)
							elog(ERROR, "Subselect has too many fields");
						lexpr = lfirst(left_list);
						left_list = lnext(left_list);

						optup = oper(op,
									 exprType(lexpr),
									 exprType(tent->expr),
									 FALSE);
						opform = (Form_pg_operator) GETSTRUCT(optup);

						if (opform->oprresult != BOOLOID)
							elog(ERROR, "'%s' result type of '%s' must return '%s'"
								 " to be used with quantified predicate subquery",
								 op, typeidTypeName(opform->oprresult),
								 typeidTypeName(BOOLOID));

						newop = makeOper(oprid(optup),/* opno */
										 InvalidOid, /* opid */
										 opform->oprresult,
										 0,
										 NULL);
						sublink->oper = lappend(sublink->oper, newop);
					}
					if (left_list != NIL)
						elog(ERROR, "Subselect has too few fields");
				}
				result = (Node *) expr;
				break;
			}

		case T_CaseExpr:
			{
				CaseExpr   *c = (CaseExpr *) expr;
				CaseWhen   *w;
				List	   *args;
				Oid			ptype;
				CATEGORY	pcategory;

				/* transform the list of arguments */
				foreach(args, c->args)
				{
					w = lfirst(args);
					if (c->arg != NULL)
					{
						/* shorthand form was specified, so expand... */
						A_Expr	   *a = makeNode(A_Expr);

						a->oper = OP;
						a->opname = "=";
						a->lexpr = c->arg;
						a->rexpr = w->expr;
						w->expr = (Node *) a;
					}
					lfirst(args) = transformExpr(pstate, (Node *) w, precedence);
				}

				/*
				 * It's not shorthand anymore, so drop the implicit
				 * argument. This is necessary to keep the executor from
				 * seeing an untransformed expression...
				 */
				c->arg = NULL;

				/* transform the default clause */
				if (c->defresult == NULL)
				{
					A_Const    *n = makeNode(A_Const);

					n->val.type = T_Null;
					c->defresult = (Node *) n;
				}
				c->defresult = transformExpr(pstate, c->defresult, precedence);

				/* now check types across result clauses... */
				c->casetype = exprType(c->defresult);
				ptype = c->casetype;
				pcategory = TypeCategory(ptype);
				foreach(args, c->args)
				{
					Oid			wtype;

					w = lfirst(args);
					wtype = exprType(w->result);
					/* move on to next one if no new information... */
					if (wtype && (wtype != UNKNOWNOID)
						&& (wtype != ptype))
					{
						if (!ptype || ptype == UNKNOWNOID)
						{
							/* so far, only nulls so take anything... */
							ptype = wtype;
							pcategory = TypeCategory(ptype);
						}
						else if ((TypeCategory(wtype) != pcategory)
								 || ((TypeCategory(wtype) == USER_TYPE)
							&& (TypeCategory(c->casetype) == USER_TYPE)))
						{
							/*
							 * both types in different categories?
							 * then not much hope...
							 */
							elog(ERROR, "CASE/WHEN types '%s' and '%s' not matched",
								 typeidTypeName(c->casetype), typeidTypeName(wtype));
						}
						else if (IsPreferredType(pcategory, wtype)
								 && can_coerce_type(1, &ptype, &wtype))
						{
							/*
							 * new one is preferred and can convert?
							 * then take it...
							 */
							ptype = wtype;
							pcategory = TypeCategory(ptype);
						}
					}
				}

				/* Convert default result clause, if necessary */
				if (c->casetype != ptype)
				{
					if (!c->casetype || c->casetype == UNKNOWNOID)
					{
						/*
						 * default clause is NULL, so assign preferred
						 * type from WHEN clauses...
						 */
						c->casetype = ptype;
					}
					else if (can_coerce_type(1, &c->casetype, &ptype))
					{
						c->defresult = coerce_type(pstate, c->defresult,
												 c->casetype, ptype, -1);
						c->casetype = ptype;
					}
					else
					{
						elog(ERROR, "CASE/ELSE unable to convert to type '%s'",
							 typeidTypeName(ptype));
					}
				}

				/* Convert when clauses, if not null and if necessary */
				foreach(args, c->args)
				{
					Oid			wtype;

					w = lfirst(args);
					wtype = exprType(w->result);

					/*
					 * only bother with conversion if not NULL and
					 * different type...
					 */
					if (wtype && (wtype != UNKNOWNOID)
						&& (wtype != ptype))
					{
						if (can_coerce_type(1, &wtype, &ptype))
						{
							w->result = coerce_type(pstate, w->result, wtype,
													ptype, -1);
						}
						else
						{
							elog(ERROR, "CASE/WHEN unable to convert to type '%s'",
								 typeidTypeName(ptype));
						}
					}
				}

				result = expr;
				break;
			}

		case T_CaseWhen:
			{
				CaseWhen   *w = (CaseWhen *) expr;

				w->expr = transformExpr(pstate, (Node *) w->expr, precedence);
				if (exprType(w->expr) != BOOLOID)
					elog(ERROR, "WHEN clause must have a boolean result");

				/*
				 * result is NULL for NULLIF() construct - thomas
				 * 1998-11-11
				 */
				if (w->result == NULL)
				{
					A_Const    *n = makeNode(A_Const);

					n->val.type = T_Null;
					w->result = (Node *) n;
				}
				w->result = transformExpr(pstate, (Node *) w->result, precedence);
				result = expr;
				break;
			}

/* Some nodes do _not_ come from the original parse tree,
 *	but result from parser transformation in this phase.
 * At least one construct (BETWEEN/AND) puts the same nodes
 *	into two branches of the parse tree; hence, some nodes
 *	are transformed twice.
 * Another way it can happen is that coercion of an operator or
 *	function argument to the required type (via coerce_type())
 *	can apply transformExpr to an already-transformed subexpression.
 *	An example here is "SELECT count(*) + 1.0 FROM table".
 * Thus, we can see node types in this routine that do not appear in the
 *	original parse tree.  Assume they are already transformed, and just
 *	pass them through.
 * Do any other node types need to be accepted?  For now we are taking
 *	a conservative approach, and only accepting node types that are
 *	demonstrably necessary to accept.
 */
		case T_Expr:
		case T_Var:
		case T_Const:
		case T_Param:
		case T_Aggref:
		case T_ArrayRef:
		case T_RelabelType:
			{
				result = (Node *) expr;
				break;
			}
		default:
			/* should not reach here */
			elog(ERROR, "transformExpr: does not know how to transform node %d"
				 " (internal error)", nodeTag(expr));
			break;
	}

	return result;
}

static Node *
transformIndirection(ParseState *pstate, Node *basenode, List *indirection)
{
	if (indirection == NIL)
		return basenode;
	return (Node *) transformArraySubscripts(pstate, basenode,
											 indirection, false, NULL);
}

static Node *
transformAttr(ParseState *pstate, Attr *att, int precedence)
{
	Node	   *basenode;

	basenode = ParseNestedFuncOrColumn(pstate, att, &pstate->p_last_resno,
									   precedence);
	return transformIndirection(pstate, basenode, att->indirection);
}

static Node *
transformIdent(ParseState *pstate, Ident *ident, int precedence)
{
	Node	   *result = NULL;
	RangeTblEntry *rte;

	/* try to find the ident as a relation ... but not if subscripts appear */
	if (ident->indirection == NIL &&
		refnameRangeTableEntry(pstate, ident->name) != NULL)
	{
		ident->isRel = TRUE;
		result = (Node *) ident;
	}

	if (result == NULL || precedence == EXPR_COLUMN_FIRST)
	{
		/* try to find the ident as a column */
		if ((rte = colnameRangeTableEntry(pstate, ident->name)) != NULL)
		{
			/* Convert it to a fully qualified Attr, and transform that */
#ifndef DISABLE_JOIN_SYNTAX
			Attr	   *att = makeAttr(rte->ref->relname, ident->name);
#else
			Attr	   *att = makeNode(Attr);

			att->relname = rte->refname;
			att->paramNo = NULL;
			att->attrs = lcons(makeString(ident->name), NIL);
#endif
			att->indirection = ident->indirection;
			return transformAttr(pstate, att, precedence);
		}
	}

	if (result == NULL)
		elog(ERROR, "Attribute '%s' not found", ident->name);

	return result;
}

/*
 *	exprType -
 *	  returns the Oid of the type of the expression. (Used for typechecking.)
 */
Oid
exprType(Node *expr)
{
	Oid			type = (Oid) InvalidOid;

	if (!expr)
		return type;

	switch (nodeTag(expr))
	{
		case T_Func:
			type = ((Func *) expr)->functype;
			break;
		case T_Iter:
			type = ((Iter *) expr)->itertype;
			break;
		case T_Var:
			type = ((Var *) expr)->vartype;
			break;
		case T_Expr:
			type = ((Expr *) expr)->typeOid;
			break;
		case T_Const:
			type = ((Const *) expr)->consttype;
			break;
		case T_ArrayRef:
			type = ((ArrayRef *) expr)->refelemtype;
			break;
		case T_Aggref:
			type = ((Aggref *) expr)->aggtype;
			break;
		case T_Param:
			type = ((Param *) expr)->paramtype;
			break;
		case T_RelabelType:
			type = ((RelabelType *) expr)->resulttype;
			break;
		case T_SubLink:
			{
				SubLink    *sublink = (SubLink *) expr;

				if (sublink->subLinkType == EXPR_SUBLINK)
				{
					/* get the type of the subselect's first target column */
					Query	   *qtree = (Query *) sublink->subselect;
					TargetEntry *tent;

					if (! qtree || ! IsA(qtree, Query))
						elog(ERROR, "Cannot get type for untransformed sublink");
					tent = (TargetEntry *) lfirst(qtree->targetList);
					type = tent->resdom->restype;
				}
				else
				{
					/* for all other sublink types, result is boolean */
					type = BOOLOID;
				}
			}
			break;
		case T_CaseExpr:
			type = ((CaseExpr *) expr)->casetype;
			break;
		case T_CaseWhen:
			type = exprType(((CaseWhen *) expr)->result);
			break;
		case T_Ident:
			/* is this right? */
			type = UNKNOWNOID;
			break;
		default:
			elog(ERROR, "Do not know how to get type for %d node",
				 nodeTag(expr));
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
				Const  *con = (Const *) expr;
				switch (con->consttype)
				{
					case BPCHAROID:
						if (! con->constisnull)
							return VARSIZE(DatumGetPointer(con->constvalue));
						break;
					default:
						break;
				}
			}
			break;
		case T_RelabelType:
			return ((RelabelType *) expr)->resulttypmod;
			break;
		default:
			break;
	}
	return -1;
}

/*
 * Produce an appropriate Const node from a constant value produced
 * by the parser and an explicit type name to cast to.
 */
static Node *
parser_typecast_constant(Value *expr, TypeName *typename)
{
	Const	   *con;
	Type		tp;
	Datum		datum;
	char	   *const_string = NULL;
	bool		string_palloced = false;
	bool		isNull = false;

	switch (nodeTag(expr))
	{
		case T_String:
			const_string = DatumGetPointer(expr->val.str);
			break;
		case T_Integer:
			string_palloced = true;
			const_string = int4out(expr->val.ival);
			break;
		case T_Float:
			string_palloced = true;
			const_string = float8out(&expr->val.dval);
			break;
		case T_Null:
			isNull = true;
			break;
		default:
			elog(ERROR,
				 "Cannot cast this expression to type '%s'",
				 typename->name);
	}

	if (typename->arrayBounds != NIL)
	{
		char		type_string[NAMEDATALEN+2];

		sprintf(type_string, "_%s", typename->name);
		tp = (Type) typenameType(type_string);
	}
	else
		tp = (Type) typenameType(typename->name);

	if (isNull)
		datum = (Datum) NULL;
	else
		datum = stringTypeDatum(tp, const_string, typename->typmod);

	con = makeConst(typeTypeId(tp),
					typeLen(tp),
					datum,
					isNull,
					typeByVal(tp),
					false,		/* not a set */
					true /* is cast */ );

	if (string_palloced)
		pfree(const_string);

	return (Node *) con;
}

/*
 * Handle an explicit CAST applied to a non-constant expression.
 * (Actually, this works for constants too, but gram.y won't generate
 * a TypeCast node if the argument is just a constant.)
 *
 * The given expr has already been transformed, but we need to lookup
 * the type name and then apply any necessary coercion function(s).
 */
static Node *
parser_typecast_expression(ParseState *pstate,
						   Node *expr, TypeName *typename)
{
	Oid			inputType = exprType(expr);
	Type		tp;
	Oid			targetType;

	if (typename->arrayBounds != NIL)
	{
		char		type_string[NAMEDATALEN+2];

		sprintf(type_string, "_%s", typename->name);
		tp = (Type) typenameType(type_string);
	}
	else
		tp = (Type) typenameType(typename->name);
	targetType = typeTypeId(tp);

	if (inputType == InvalidOid)
		return expr;			/* do nothing if NULL input */

	if (inputType != targetType)
	{
		expr = CoerceTargetExpr(pstate, expr, inputType,
								targetType, typename->typmod);
		if (expr == NULL)
			elog(ERROR, "Cannot cast type '%s' to '%s'",
				 typeidTypeName(inputType),
				 typeidTypeName(targetType));
	}
	/*
	 * If the target is a fixed-length type, it may need a length
	 * coercion as well as a type coercion.
	 */
	expr = coerce_type_typmod(pstate, expr,
							  targetType, typename->typmod);

	return expr;
}
