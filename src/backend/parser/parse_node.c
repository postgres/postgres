/*-------------------------------------------------------------------------
 *
 * parse_node.c
 *	  various routines that make nodes for query plans
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_node.c,v 1.30 1999/08/22 20:15:04 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <ctype.h>

#include "postgres.h"
#include "access/heapam.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "nodes/makefuncs.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_node.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

static void disallow_setop(char *op, Type optype, Node *operand);
static Node *make_operand(char *opname,
			 Node *tree,
			 Oid orig_typeId,
			 Oid true_typeId);

/* make_parsestate()
 * Allocate and initialize a new ParseState.
 * The CALLER is responsible for freeing the ParseState* returned.
 */
ParseState *
make_parsestate(ParseState *parentParseState)
{
	ParseState *pstate;

	pstate = palloc(sizeof(ParseState));
	MemSet(pstate, 0, sizeof(ParseState));

	pstate->p_last_resno = 1;
	pstate->parentParseState = parentParseState;

	return pstate;
}


/* make_operand()
 * Ensure argument type match by forcing conversion of constants.
 */
static Node *
make_operand(char *opname,
			 Node *tree,
			 Oid orig_typeId,
			 Oid true_typeId)
{
	Node	   *result;
	Type		true_type;

	if (tree != NULL)
	{
		result = tree;
		true_type = typeidType(true_typeId);
		disallow_setop(opname, true_type, result);

		/* must coerce? */
		if (true_typeId != orig_typeId)
			result = coerce_type(NULL, tree, orig_typeId, true_typeId, -1);
	}
	/* otherwise, this is a NULL value */
	else
	{
		Const	   *con = makeNode(Const);

		con->consttype = true_typeId;
		con->constlen = 0;
		con->constvalue = (Datum) (struct varlena *) NULL;
		con->constisnull = true;
		con->constbyval = true;
		con->constisset = false;
		result = (Node *) con;
	}

	return result;
}	/* make_operand() */


static void
disallow_setop(char *op, Type optype, Node *operand)
{
	if (operand == NULL)
		return;

	if (nodeTag(operand) == T_Iter)
	{
		elog(ERROR, "An operand to the '%s' operator returns a set of %s,"
			 "\n\tbut '%s' takes single values, not sets.",
			 op, typeTypeName(optype), op);
	}
}


/* make_op()
 * Operator construction.
 *
 * Transform operator expression ensuring type compatibility.
 * This is where some type conversion happens.
 */
Expr *
make_op(char *opname, Node *ltree, Node *rtree)
{
	Oid			ltypeId,
				rtypeId;
	Operator	tup;
	Form_pg_operator opform;
	Oper	   *newop;
	Node	   *left,
			   *right;
	Expr	   *result;

	/* right operator? */
	if (rtree == NULL)
	{
		ltypeId = (ltree == NULL) ? UNKNOWNOID : exprType(ltree);
		tup = right_oper(opname, ltypeId);
		opform = (Form_pg_operator) GETSTRUCT(tup);
		left = make_operand(opname, ltree, ltypeId, opform->oprleft);
		right = NULL;

	}

	/* left operator? */
	else if (ltree == NULL)
	{
		rtypeId = (rtree == NULL) ? UNKNOWNOID : exprType(rtree);
		tup = left_oper(opname, rtypeId);
		opform = (Form_pg_operator) GETSTRUCT(tup);
		right = make_operand(opname, rtree, rtypeId, opform->oprright);
		left = NULL;

	}

	/* otherwise, binary operator */
	else
	{
		/* binary operator */
		ltypeId = (ltree == NULL) ? UNKNOWNOID : exprType(ltree);
		rtypeId = (rtree == NULL) ? UNKNOWNOID : exprType(rtree);

		/* check for exact match on this operator... */
		if (HeapTupleIsValid(tup = oper_exact(opname, ltypeId, rtypeId, &ltree, &rtree, TRUE)))
		{
			ltypeId = exprType(ltree);
			rtypeId = exprType(rtree);
		}
		/* try to find a match on likely candidates... */
		else if (!HeapTupleIsValid(tup = oper_inexact(opname, ltypeId, rtypeId, &ltree, &rtree, FALSE)))
		{
			/* Won't return from oper_inexact() without a candidate... */
		}

		opform = (Form_pg_operator) GETSTRUCT(tup);
		left = make_operand(opname, ltree, ltypeId, opform->oprleft);
		right = make_operand(opname, rtree, rtypeId, opform->oprright);
	}

	newop = makeOper(oprid(tup),/* opno */
					 InvalidOid,/* opid */
					 opform->oprresult, /* operator result type */
					 0,
					 NULL);

	result = makeNode(Expr);
	result->typeOid = opform->oprresult;
	result->opType = OP_EXPR;
	result->oper = (Node *) newop;

	if (!left)
		result->args = lcons(right, NIL);
	else if (!right)
		result->args = lcons(left, NIL);
	else
		result->args = lcons(left, lcons(right, NIL));

	return result;
}	/* make_op() */


Var *
make_var(ParseState *pstate, Oid relid, char *refname,
		 char *attrname)
{
	Var		   *varnode;
	int			vnum,
				attid;
	Oid			vartypeid;
	int32		type_mod;
	int			sublevels_up;

	vnum = refnameRangeTablePosn(pstate, refname, &sublevels_up);

	attid = get_attnum(relid, attrname);
	if (attid == InvalidAttrNumber)
		elog(ERROR, "Relation %s does not have attribute %s",
			 refname, attrname);
	vartypeid = get_atttype(relid, attid);
	type_mod = get_atttypmod(relid, attid);

	varnode = makeVar(vnum, attid, vartypeid, type_mod, sublevels_up);

	return varnode;
}

/*
 * transformArraySubscripts()
 *		Transform array subscripting.  This is used for both
 *		array fetch and array assignment.
 *
 * In an array fetch, we are given a source array value and we produce an
 * expression that represents the result of extracting a single array element
 * or an array slice.
 *
 * In an array assignment, we are given a destination array value plus a
 * source value that is to be assigned to a single element or a slice of
 * that array.  We produce an expression that represents the new array value
 * with the source data inserted into the right part of the array.
 *
 * pstate		Parse state
 * arrayBase	Already-transformed expression for the array as a whole
 * indirection	Untransformed list of subscripts (must not be NIL)
 * forceSlice	If true, treat subscript as array slice in all cases
 * assignFrom	NULL for array fetch, else transformed expression for source.
 */
ArrayRef *
transformArraySubscripts(ParseState *pstate,
						 Node *arrayBase,
						 List *indirection,
						 bool forceSlice,
						 Node *assignFrom)
{
	Oid			typearray,
				typeelement,
				typeresult;
	HeapTuple	type_tuple;
	Form_pg_type type_struct_array,
				type_struct_element;
	bool		isSlice = forceSlice;
	List	   *upperIndexpr = NIL;
	List	   *lowerIndexpr = NIL;
	List	   *idx;
	ArrayRef   *aref;

	/* Get the type tuple for the array */
	typearray = exprType(arrayBase);

	type_tuple = SearchSysCacheTuple(TYPOID,
									 ObjectIdGetDatum(typearray),
									 0, 0, 0);
	if (!HeapTupleIsValid(type_tuple))
		elog(ERROR, "transformArraySubscripts: Cache lookup failed for array type %u",
			 typearray);
	type_struct_array = (Form_pg_type) GETSTRUCT(type_tuple);

	typeelement = type_struct_array->typelem;
	if (typeelement == InvalidOid)
		elog(ERROR, "transformArraySubscripts: type %s is not an array",
			 type_struct_array->typname);

	/* Get the type tuple for the array element type */
	type_tuple = SearchSysCacheTuple(TYPOID,
									 ObjectIdGetDatum(typeelement),
									 0, 0, 0);
	if (!HeapTupleIsValid(type_tuple))
		elog(ERROR, "transformArraySubscripts: Cache lookup failed for array element type %u",
			 typeelement);
	type_struct_element = (Form_pg_type) GETSTRUCT(type_tuple);

	/*
	 * A list containing only single subscripts refers to a single array
	 * element.  If any of the items are double subscripts (lower:upper),
	 * then the subscript expression means an array slice operation.
	 * In this case, we supply a default lower bound of 1 for any items
	 * that contain only a single subscript.
	 * The forceSlice parameter forces us to treat the operation as a
	 * slice, even if no lower bounds are mentioned.  Otherwise,
	 * we have to prescan the indirection list to see if there are any
	 * double subscripts.
	 */
	if (! isSlice)
	{
		foreach (idx, indirection)
		{
			A_Indices  *ai = (A_Indices *) lfirst(idx);
			if (ai->lidx != NULL)
			{
				isSlice = true;
				break;
			}
		}
	}

	/* The type represented by the subscript expression is the element type
	 * if we are fetching a single element, but it is the same as the array
	 * type if we are fetching a slice or storing.
	 */
	if (isSlice || assignFrom != NULL)
		typeresult = typearray;
	else
		typeresult = typeelement;

	/*
	 * Transform the subscript expressions.
	 */
	foreach (idx, indirection)
	{
		A_Indices  *ai = (A_Indices *) lfirst(idx);
		Node	   *subexpr;

		if (isSlice)
		{
			if (ai->lidx)
			{
				subexpr = transformExpr(pstate, ai->lidx, EXPR_COLUMN_FIRST);
				/* If it's not int4 already, try to coerce */
				subexpr = CoerceTargetExpr(pstate, subexpr,
										   exprType(subexpr), INT4OID);
				if (subexpr == NULL)
					elog(ERROR, "array index expressions must be integers");
			}
			else
			{
				/* Make a constant 1 */
				subexpr = (Node *) makeConst(INT4OID,
											 sizeof(int32),
											 Int32GetDatum(1),
											 false,
											 true, /* pass by value */
											 false,
											 false);
			}
			lowerIndexpr = lappend(lowerIndexpr, subexpr);
		}
		subexpr = transformExpr(pstate, ai->uidx, EXPR_COLUMN_FIRST);
		/* If it's not int4 already, try to coerce */
		subexpr = CoerceTargetExpr(pstate, subexpr,
								   exprType(subexpr), INT4OID);
		if (subexpr == NULL)
			elog(ERROR, "array index expressions must be integers");
		upperIndexpr = lappend(upperIndexpr, subexpr);
	}

	/*
	 * If doing an array store, coerce the source value to the right type.
	 */
	if (assignFrom != NULL)
	{
		Oid			typesource = exprType(assignFrom);
		Oid			typeneeded = isSlice ? typearray : typeelement;

		if (typesource != InvalidOid)
		{
			if (typesource != typeneeded)
			{
				assignFrom = CoerceTargetExpr(pstate, assignFrom,
											  typesource, typeneeded);
				if (assignFrom == NULL)
					elog(ERROR, "Array assignment requires type '%s'"
						 " but expression is of type '%s'"
						 "\n\tYou will need to rewrite or cast the expression",
						 typeidTypeName(typeneeded),
						 typeidTypeName(typesource));
			}
		}
	}

	/*
	 * Ready to build the ArrayRef node.
	 */
	aref = makeNode(ArrayRef);
	aref->refattrlength = type_struct_array->typlen;
	aref->refelemlength = type_struct_element->typlen;
	aref->refelemtype = typeresult; /* XXX should save element type too */
	aref->refelembyval = type_struct_element->typbyval;
	aref->refupperindexpr = upperIndexpr;
	aref->reflowerindexpr = lowerIndexpr;
	aref->refexpr = arrayBase;
	aref->refassgnexpr = assignFrom;

	return aref;
}

/*
 * make_const -
 *
 * - takes a lispvalue, (as returned to the yacc routine by the lexer)
 *	 extracts the type, and makes the appropriate type constant
 *	 by invoking the (c-callable) lisp routine c-make-const
 *	 via the lisp_call() mechanism
 *
 * eventually, produces a "const" lisp-struct as per nodedefs.cl
 */
Const *
make_const(Value *value)
{
	Type		tp;
	Datum		val;
	Const	   *con;

	switch (nodeTag(value))
	{
		case T_Integer:
			tp = typeidType(INT4OID);
			val = Int32GetDatum(intVal(value));
			break;

		case T_Float:
			{
				float64		dummy;

				tp = typeidType(FLOAT8OID);

				dummy = (float64) palloc(sizeof(float64data));
				*dummy = floatVal(value);

				val = Float64GetDatum(dummy);
			}
			break;

		case T_String:
			tp = typeidType(UNKNOWNOID);		/* unknown for now, will
												 * be type coerced */
			val = PointerGetDatum(textin(strVal(value)));
			break;

		case T_Null:
		default:
			{
				if (nodeTag(value) != T_Null)
					elog(NOTICE, "make_const: unknown type %d\n", nodeTag(value));

				/* null const */
				con = makeConst(0, 0, (Datum) NULL, true, false, false, false);
				return con;
			}
	}

	con = makeConst(typeTypeId(tp),
					typeLen(tp),
					val,
					false,
					typeByVal(tp),
					false,		/* not a set */
					false);

	return con;
}
