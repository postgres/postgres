/*-------------------------------------------------------------------------
 *
 * parse_expr.c
 *	  handle expressions in parser
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_expr.c,v 1.53 1999/07/16 22:32:25 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "postgres.h"
#include "nodes/makefuncs.h"
#include "nodes/params.h"
#include "nodes/relation.h"
#include "parse.h"
#include "parser/analyze.h"
#include "parser/gramparse.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "utils/builtins.h"

static Node *parser_typecast(Value *expr, TypeName *typename, int32 atttypmod);
static Node *transformAttr(ParseState *pstate, Attr *att, int precedence);
static Node *transformIdent(ParseState *pstate, Ident *ident, int precedence);
static Node *transformIndirection(ParseState *pstate, Node *basenode,
								  List *indirection, int precedence);

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
					result = parser_typecast(val, con->typename, -1);
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
											  pno->indirection, precedence);
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
										  "nullvalue", lcons(lexpr, NIL),
												   &pstate->p_last_resno,
													   precedence);
						}
						break;
					case NOTNULL:
						{
							Node	   *lexpr = transformExpr(pstate, a->lexpr, precedence);

							result = ParseFuncOrColumn(pstate,
									   "nonnullvalue", lcons(lexpr, NIL),
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
								elog(ERROR, "left-hand side of AND is type '%s', not bool",
									 typeidTypeName(exprType(lexpr)));

							if (exprType(rexpr) != BOOLOID)
								elog(ERROR, "right-hand side of AND is type '%s', not bool",
									 typeidTypeName(exprType(rexpr)));

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
								elog(ERROR, "left-hand side of OR is type '%s', not bool",
									 typeidTypeName(exprType(lexpr)));
							if (exprType(rexpr) != BOOLOID)
								elog(ERROR, "right-hand side of OR is type '%s', not bool",
									 typeidTypeName(exprType(rexpr)));
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
								elog(ERROR, "argument to NOT is type '%s', not bool",
									 typeidTypeName(exprType(rexpr)));
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
					elog(ERROR, "parser: bad query in subselect");
				qtree = (Query *) lfirst(qtrees);
				if (qtree->commandType != CMD_SELECT ||
					qtree->resultRelation != 0)
					elog(ERROR, "parser: bad query in subselect");
				sublink->subselect = (Node *) qtree;

				if (sublink->subLinkType != EXISTS_SUBLINK)
				{
					char	   *op = lfirst(sublink->oper);
					List	   *left_list = sublink->lefthand;
					List	   *right_list = qtree->targetList;
					List	   *elist;

					foreach(elist, left_list)
						lfirst(elist) = transformExpr(pstate, lfirst(elist),
													  precedence);

					if (length(left_list) > 1 &&
						strcmp(op, "=") != 0 && strcmp(op, "<>") != 0)
						elog(ERROR, "parser: '%s' is not relational operator",
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
						Expr	   *op_expr;

						if (! tent->resdom->resjunk)
						{
							if (left_list == NIL)
								elog(ERROR, "parser: Subselect has too many fields.");
							lexpr = lfirst(left_list);
							left_list = lnext(left_list);
							op_expr = make_op(op, lexpr, tent->expr);
							if (op_expr->typeOid != BOOLOID &&
								sublink->subLinkType != EXPR_SUBLINK)
								elog(ERROR, "parser: '%s' must return 'bool' to be used with quantified predicate subquery", op);
							sublink->oper = lappend(sublink->oper, op_expr);
						}
						right_list = lnext(right_list);
					}
					if (left_list != NIL)
						elog(ERROR, "parser: Subselect has too few fields.");
				}
				else
					sublink->oper = NIL;
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
				c->defresult = transformExpr(pstate, (Node *) c->defresult, precedence);

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
						/* so far, only nulls so take anything... */
						if (!ptype)
						{
							ptype = wtype;
							pcategory = TypeCategory(ptype);
						}

						/*
						 * both types in different categories? then not
						 * much hope...
						 */
						else if ((TypeCategory(wtype) != pcategory)
								 || ((TypeCategory(wtype) == USER_TYPE)
							&& (TypeCategory(c->casetype) == USER_TYPE)))
						{
							elog(ERROR, "CASE/WHEN types '%s' and '%s' not matched",
								 typeidTypeName(c->casetype), typeidTypeName(wtype));
						}

						/*
						 * new one is preferred and can convert? then take
						 * it...
						 */
						else if (IsPreferredType(pcategory, wtype)
								 && can_coerce_type(1, &ptype, &wtype))
						{
							ptype = wtype;
							pcategory = TypeCategory(ptype);
						}
					}
				}

				/* Convert default result clause, if necessary */
				if (c->casetype != ptype)
				{
					if (!c->casetype)
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
						elog(ERROR, "CASE/ELSE unable to convert to type %s",
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
					if (wtype && (wtype != ptype))
					{
						if (can_coerce_type(1, &wtype, &ptype))
						{
							w->result = coerce_type(pstate, w->result, wtype,
													ptype, -1);
						}
						else
						{
							elog(ERROR, "CASE/WHEN unable to convert to type %s",
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
			{
				result = (Node *) expr;
				break;
			}
		default:
			/* should not reach here */
			elog(ERROR, "transformExpr: does not know how to transform node %d",
				 nodeTag(expr));
			break;
	}

	return result;
}

static Node *
transformIndirection(ParseState *pstate, Node *basenode,
					 List *indirection, int precedence)
{
	List	   *idx;

	if (indirection == NIL)
		return basenode;
	foreach (idx, indirection)
	{
		A_Indices  *ai = (A_Indices *) lfirst(idx);
		Node	   *lexpr = NULL,
				   *uexpr;

		/* uidx is always present, but lidx might be null */
		if (ai->lidx != NULL)
		{
			lexpr = transformExpr(pstate, ai->lidx, precedence);
			if (exprType(lexpr) != INT4OID)
				elog(ERROR, "array index expressions must be int4's");
		}
		uexpr = transformExpr(pstate, ai->uidx, precedence);
		if (exprType(uexpr) != INT4OID)
			elog(ERROR, "array index expressions must be int4's");
		ai->lidx = lexpr;
		ai->uidx = uexpr;
		/*
		 * note we reuse the list of A_Indices nodes, make sure
		 * we don't free them! Otherwise, make a new list here
		 */
	}
	return (Node *) make_array_ref(basenode, indirection);
}

static Node *
transformAttr(ParseState *pstate, Attr *att, int precedence)
{
	Node	   *basenode;

	/* what if att->attrs == "*"? */
	basenode = ParseNestedFuncOrColumn(pstate, att, &pstate->p_last_resno,
									   precedence);
	return transformIndirection(pstate, basenode,
								att->indirection, precedence);
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
			Attr	   *att = makeNode(Attr);

			att->relname = rte->refname;
			att->paramNo = NULL;
			att->attrs = lcons(makeString(ident->name), NIL);
			att->indirection = ident->indirection;
			return transformAttr(pstate, att, precedence);
		}
	}

	if (result == NULL)
		elog(ERROR, "attribute '%s' not found", ident->name);

	return result;
}

/*
 *	exprType -
 *	  returns the Oid of the type of the expression. (Used for typechecking.)
 */
Oid
exprType(Node *expr)
{
	Oid			type = (Oid) 0;

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
		case T_SubLink:
			{
				SubLink    *sublink = (SubLink *) expr;

				if (sublink->subLinkType == EXPR_SUBLINK)
				{
					/* return the result type of the combining operator */
					Expr	   *op_expr = (Expr *) lfirst(sublink->oper);

					type = op_expr->typeOid;
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
			elog(ERROR, "exprType: don't know how to get type for %d node",
				 nodeTag(expr));
			break;
	}
	return type;
}

static Node *
parser_typecast(Value *expr, TypeName *typename, int32 atttypmod)
{
	/* check for passing non-ints */
	Const	   *adt;
	Datum		lcp;
	Type		tp;
	char		type_string[NAMEDATALEN];
	int32		len;
	char	   *cp = NULL;
	char	   *const_string = NULL;
	bool		string_palloced = false;

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
		default:
			elog(ERROR,
			 "parser_typecast: cannot cast this expression to type '%s'",
				 typename->name);
	}

	if (typename->arrayBounds != NIL)
	{
		sprintf(type_string, "_%s", typename->name);
		tp = (Type) typenameType(type_string);
	}
	else
		tp = (Type) typenameType(typename->name);

	len = typeLen(tp);

	cp = stringTypeString(tp, const_string, atttypmod);

	if (!typeByVal(tp))
		lcp = PointerGetDatum(cp);
	else
	{
		switch (len)
		{
			case 1:
				lcp = Int8GetDatum(cp);
				break;
			case 2:
				lcp = Int16GetDatum(cp);
				break;
			case 4:
				lcp = Int32GetDatum(cp);
				break;
			default:
				lcp = PointerGetDatum(cp);
				break;
		}
	}

	adt = makeConst(typeTypeId(tp),
					len,
					(Datum) lcp,
					false,
					typeByVal(tp),
					false,		/* not a set */
					true /* is cast */ );

	if (string_palloced)
		pfree(const_string);

	return (Node *) adt;
}


/* parser_typecast2()
 * Convert (only) constants to specified type.
 */
Node *
parser_typecast2(Node *expr, Oid exprType, Type tp, int32 atttypmod)
{
	/* check for passing non-ints */
	Const	   *adt;
	Datum		lcp;
	int32		len = typeLen(tp);
	char	   *cp = NULL;

	char	   *const_string = NULL;
	bool		string_palloced = false;

	Assert(IsA(expr, Const));

	switch (exprType)
	{
		case 0:			/* NULL */
			break;
		case INT4OID:			/* int4 */
			const_string = (char *) palloc(256);
			string_palloced = true;
			sprintf(const_string, "%d",
					(int) ((Const *) expr)->constvalue);
			break;
		case NAMEOID:			/* name */
			const_string = (char *) palloc(256);
			string_palloced = true;
			sprintf(const_string, "%s",
					(char *) ((Const *) expr)->constvalue);
			break;
		case CHAROID:			/* char */
			const_string = (char *) palloc(256);
			string_palloced = true;
			sprintf(const_string, "%c",
					(char) ((Const *) expr)->constvalue);
			break;
		case FLOAT4OID: /* float4 */
			{
				float32		floatVal = DatumGetFloat32(((Const *) expr)->constvalue);

				const_string = (char *) palloc(256);
				string_palloced = true;
				sprintf(const_string, "%f", *floatVal);
				break;
			}
		case FLOAT8OID: /* float8 */
			{
				float64		floatVal = DatumGetFloat64(((Const *) expr)->constvalue);

				const_string = (char *) palloc(256);
				string_palloced = true;
				sprintf(const_string, "%f", *floatVal);
				break;
			}
		case CASHOID:			/* money */
			const_string = (char *) palloc(256);
			string_palloced = true;
			sprintf(const_string, "%ld",
					(long) ((Const *) expr)->constvalue);
			break;
		case TEXTOID:			/* text */
			const_string = DatumGetPointer(((Const *) expr)->constvalue);
			const_string = (char *) textout((struct varlena *) const_string);
			break;
		case UNKNOWNOID:		/* unknown */
			const_string = DatumGetPointer(((Const *) expr)->constvalue);
			const_string = (char *) textout((struct varlena *) const_string);
			break;
		default:
			elog(ERROR, "unknown type %u", exprType);
	}

	if (!exprType)
	{
		adt = makeConst(typeTypeId(tp),
						(Size) 0,
						(Datum) NULL,
						true,	/* isnull */
						false,	/* was omitted */
						false,	/* not a set */
						true /* is cast */ );
		return (Node *) adt;
	}

	cp = stringTypeString(tp, const_string, atttypmod);

	if (!typeByVal(tp))
		lcp = PointerGetDatum(cp);
	else
	{
		switch (len)
		{
			case 1:
				lcp = Int8GetDatum(cp);
				break;
			case 2:
				lcp = Int16GetDatum(cp);
				break;
			case 4:
				lcp = Int32GetDatum(cp);
				break;
			default:
				lcp = PointerGetDatum(cp);
				break;
		}
	}

	adt = makeConst(typeTypeId(tp),
					(Size) len,
					(Datum) lcp,
					false,
					typeByVal(tp),
					false,		/* not a set */
					true /* is cast */ );

	/*
	 * printf("adt %s : %u %d %d\n",CString(expr),typeTypeId(tp) ,
	 * len,cp);
	 */
	if (string_palloced)
		pfree(const_string);

	return (Node *) adt;
}
