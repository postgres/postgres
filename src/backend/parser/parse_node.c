/*-------------------------------------------------------------------------
 *
 * parse_node.c--
 *	  various routines that make nodes for query plans
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_node.c,v 1.20 1998/09/01 03:24:15 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <ctype.h>
#include <string.h>

#include "postgres.h"
#include "access/heapam.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "nodes/makefuncs.h"
#include "parser/parse_expr.h"
#include "parser/parse_node.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "parser/parse_type.h"
#include "parser/parse_coerce.h"
#include "utils/builtins.h"
#include "utils/syscache.h"
#include "utils/lsyscache.h"

static void disallow_setop(char *op, Type optype, Node *operand);
static Node *
make_operand(char *opname,
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

#ifdef PARSEDEBUG
printf("make_operand: constructing operand for '%s' %s->%s\n",
 opname, typeidTypeName(orig_typeId), typeidTypeName(true_typeId));
#endif
	if (tree != NULL)
	{
		result = tree;
		true_type = typeidType(true_typeId);
		disallow_setop(opname, true_type, result);

		/* must coerce? */
		if (true_typeId != orig_typeId)
		{
#ifdef PARSEDEBUG
printf("make_operand: try to convert node from %s to %s\n",
 typeidTypeName(orig_typeId), typeidTypeName(true_typeId));
#endif
			result = coerce_type(NULL, tree, orig_typeId, true_typeId);
		}
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
} /* make_operand() */


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
#ifdef PARSEDEBUG
printf("make_op: returned from left_oper() with structure at %p\n", (void *)tup);
#endif
		opform = (Form_pg_operator) GETSTRUCT(tup);
#ifdef PARSEDEBUG
printf("make_op: calling make_operand()\n");
#endif
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

	newop = makeOper(oprid(tup),		/* opno */
					 InvalidOid,		/* opid */
					 opform->oprresult,	/* operator result type */
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
} /* make_op() */


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

	varnode = makeVar(vnum, attid, vartypeid, type_mod,
					  sublevels_up, vnum, attid);

	return varnode;
}

/*
 *	make_array_ref() -- Make an array reference node.
 *
 *		Array references can hang off of arbitrary nested dot (or
 *		function invocation) expressions.  This routine takes a
 *		tree generated by ParseFunc() and an array index and
 *		generates a new array reference tree.  We do some simple
 *		typechecking to be sure the dereference is valid in the
 *		type system, but we don't do any bounds checking here.
 *
 *	indirection is a list of A_Indices
 */
ArrayRef   *
make_array_ref(Node *expr,
			   List *indirection)
{
	Oid			typearray;
	HeapTuple	type_tuple;
	Form_pg_type type_struct_array,
				type_struct_element;
	ArrayRef   *aref;
	Oid			reftype;
	List	   *upperIndexpr = NIL;
	List	   *lowerIndexpr = NIL;

	typearray = exprType(expr);

	type_tuple = SearchSysCacheTuple(TYPOID,
									 ObjectIdGetDatum(typearray),
									 0, 0, 0);

	if (!HeapTupleIsValid(type_tuple))
		elog(ERROR, "make_array_ref: Cache lookup failed for type %d\n",
			 typearray);

	/* get the array type struct from the type tuple */
	type_struct_array = (Form_pg_type) GETSTRUCT(type_tuple);

	if (type_struct_array->typelem == InvalidOid)
		elog(ERROR, "make_array_ref: type %s is not an array",
			 type_struct_array->typname);

	/* get the type tuple for the element type */
	type_tuple = SearchSysCacheTuple(TYPOID,
								ObjectIdGetDatum(type_struct_array->typelem),
									 0, 0, 0);
	if (!HeapTupleIsValid(type_tuple))
		elog(ERROR, "make_array_ref: Cache lookup failed for type %d\n",
			 typearray);

	type_struct_element = (Form_pg_type) GETSTRUCT(type_tuple);

	while (indirection != NIL)
	{
		A_Indices  *ind = lfirst(indirection);

		if (ind->lidx)

			/*
			 * XXX assumes all lower indices non null in this case
			 */
			lowerIndexpr = lappend(lowerIndexpr, ind->lidx);

		upperIndexpr = lappend(upperIndexpr, ind->uidx);
		indirection = lnext(indirection);
	}
	aref = makeNode(ArrayRef);
	aref->refattrlength = type_struct_array->typlen;
	aref->refelemlength = type_struct_element->typlen;
	aref->refelemtype = type_struct_array->typelem;
	aref->refelembyval = type_struct_element->typbyval;
	aref->refupperindexpr = upperIndexpr;
	aref->reflowerindexpr = lowerIndexpr;
	aref->refexpr = expr;
	aref->refassgnexpr = NULL;

	if (lowerIndexpr == NIL)	/* accessing a single array element */
		reftype = aref->refelemtype;
	else
/* request to clip a part of the array, the result is another array */
		reftype = typearray;

	/*
	 * we change it to reflect the true type; since the original
	 * refelemtype doesn't seem to get used anywhere. - ay 10/94
	 */
	aref->refelemtype = reftype;

	return aref;
}


/* make_array_set()
 */
ArrayRef   *
make_array_set(Expr *target_expr,
			   List *upperIndexpr,
			   List *lowerIndexpr,
			   Expr *expr)
{
	Oid			typearray;
	HeapTuple	type_tuple;
	Form_pg_type type_struct_array;
	Form_pg_type type_struct_element;
	ArrayRef   *aref;
	Oid			reftype;

	typearray = exprType((Node *) target_expr);

	type_tuple = SearchSysCacheTuple(TYPOID,
									 ObjectIdGetDatum(typearray),
									 0, 0, 0);

	if (!HeapTupleIsValid(type_tuple))
		elog(ERROR, "make_array_ref: Cache lookup failed for type %d\n",
			 typearray);

	/* get the array type struct from the type tuple */
	type_struct_array = (Form_pg_type) GETSTRUCT(type_tuple);

	if (type_struct_array->typelem == InvalidOid)
		elog(ERROR, "make_array_ref: type %s is not an array",
			 type_struct_array->typname);
	/* get the type tuple for the element type */
	type_tuple = SearchSysCacheTuple(TYPOID,
								ObjectIdGetDatum(type_struct_array->typelem),
									 0, 0, 0);

	if (!HeapTupleIsValid(type_tuple))
		elog(ERROR, "make_array_ref: Cache lookup failed for type %d\n",
			 typearray);

	type_struct_element = (Form_pg_type) GETSTRUCT(type_tuple);

	aref = makeNode(ArrayRef);
	aref->refattrlength = type_struct_array->typlen;
	aref->refelemlength = type_struct_element->typlen;
	aref->refelemtype = type_struct_array->typelem;
	aref->refelembyval = type_struct_element->typbyval;
	aref->refupperindexpr = upperIndexpr;
	aref->reflowerindexpr = lowerIndexpr;
	aref->refexpr = (Node *) target_expr;
	aref->refassgnexpr = (Node *) expr;

	/* accessing a single array element? */
	if (lowerIndexpr == NIL)
		reftype = aref->refelemtype;

	/* otherwise, request to set a part of the array, by another array */
	else
		reftype = typearray;

	aref->refelemtype = reftype;

	return aref;
}

/*
 *
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
