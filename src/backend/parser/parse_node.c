/*-------------------------------------------------------------------------
 *
 * parse_node.c--
 *	  various routines that make nodes for query plans
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_node.c,v 1.7 1998/01/17 04:53:19 momjian Exp $
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
#include "utils/builtins.h"
#include "utils/syscache.h"

static void disallow_setop(char *op, Type optype, Node *operand);
static Node *make_operand(char *opname,
			 Node *tree,
			 Oid orig_typeId,
			 Oid true_typeId);

/*
 * make_parsestate() --
 *	  allocate and initialize a new ParseState.
 *	the CALLERS is responsible for freeing the ParseState* returned
 *
 */

ParseState *
make_parsestate(void)
{
	ParseState *pstate;

	pstate = palloc(sizeof(ParseState));
	MemSet(pstate, 0, sizeof(ParseState));

	pstate->p_last_resno = 1;

	return (pstate);
}

static Node *
make_operand(char *opname,
			 Node *tree,
			 Oid orig_typeId,
			 Oid true_typeId)
{
	Node	   *result;
	Type		true_type;
	Datum		val;
	Oid			infunc;

	if (tree != NULL)
	{
		result = tree;
		true_type = typeidType(true_typeId);
		disallow_setop(opname, true_type, result);
		if (true_typeId != orig_typeId)
		{						/* must coerce */
			Const	   *con = (Const *) result;

			Assert(nodeTag(result) == T_Const);
			val = (Datum) textout((struct varlena *)
								  con->constvalue);
			infunc = typeidRetinfunc(true_typeId);
			con = makeNode(Const);
			con->consttype = true_typeId;
			con->constlen = typeLen(true_type);
			con->constvalue = (Datum) fmgr(infunc,
										   val,
										   typeidTypElem(true_typeId),
										   -1 /* for varchar() type */ );
			con->constisnull = false;
			con->constbyval = true;
			con->constisset = false;
			result = (Node *) con;
		}
	}
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
}


static void
disallow_setop(char *op, Type optype, Node *operand)
{
	if (operand == NULL)
		return;

	if (nodeTag(operand) == T_Iter)
	{
		elog(NOTICE, "An operand to the '%s' operator returns a set of %s,",
			 op, typeTypeName(optype));
		elog(ERROR, "but '%s' takes single values, not sets.",
			 op);
	}
}

Expr	   *
make_op(char *opname, Node *ltree, Node *rtree)
{
	Oid			ltypeId,
				rtypeId;
	Operator	temp;
	OperatorTupleForm opform;
	Oper	   *newop;
	Node	   *left,
			   *right;
	Expr	   *result;

	if (rtree == NULL)
	{

		/* right operator */
		ltypeId = (ltree == NULL) ? UNKNOWNOID : exprType(ltree);
		temp = right_oper(opname, ltypeId);
		opform = (OperatorTupleForm) GETSTRUCT(temp);
		left = make_operand(opname, ltree, ltypeId, opform->oprleft);
		right = NULL;

	}
	else if (ltree == NULL)
	{

		/* left operator */
		rtypeId = (rtree == NULL) ? UNKNOWNOID : exprType(rtree);
		temp = left_oper(opname, rtypeId);
		opform = (OperatorTupleForm) GETSTRUCT(temp);
		right = make_operand(opname, rtree, rtypeId, opform->oprright);
		left = NULL;

	}
	else
	{
		char	   *outstr;
		Oid			infunc,
					outfunc;
		Type		newtype;

#define CONVERTABLE_TYPE(t) (	(t) == INT2OID || \
								(t) == INT4OID || \
								(t) == OIDOID || \
								(t) == FLOAT4OID || \
								(t) == FLOAT8OID || \
								(t) == CASHOID)

		/* binary operator */
		ltypeId = (ltree == NULL) ? UNKNOWNOID : exprType(ltree);
		rtypeId = (rtree == NULL) ? UNKNOWNOID : exprType(rtree);

		/*
		 * convert constant when using a const of a numeric type and a
		 * non-const of another numeric type
		 */
		if (CONVERTABLE_TYPE(ltypeId) && nodeTag(ltree) != T_Const &&
			CONVERTABLE_TYPE(rtypeId) && nodeTag(rtree) == T_Const &&
			!((Const *) rtree)->constiscast)
		{
			outfunc = typeidRetoutfunc(rtypeId);
			infunc = typeidRetinfunc(ltypeId);
			outstr = (char *) fmgr(outfunc, ((Const *) rtree)->constvalue);
			((Const *) rtree)->constvalue = (Datum) fmgr(infunc, outstr);
			pfree(outstr);
			((Const *) rtree)->consttype = rtypeId = ltypeId;
			newtype = typeidType(rtypeId);
			((Const *) rtree)->constlen = typeLen(newtype);
			((Const *) rtree)->constbyval = typeByVal(newtype);
		}

		if (CONVERTABLE_TYPE(rtypeId) && nodeTag(rtree) != T_Const &&
			CONVERTABLE_TYPE(ltypeId) && nodeTag(ltree) == T_Const &&
			!((Const *) ltree)->constiscast)
		{
			outfunc = typeidRetoutfunc(ltypeId);
			infunc = typeidRetinfunc(rtypeId);
			outstr = (char *) fmgr(outfunc, ((Const *) ltree)->constvalue);
			((Const *) ltree)->constvalue = (Datum) fmgr(infunc, outstr);
			pfree(outstr);
			((Const *) ltree)->consttype = ltypeId = rtypeId;
			newtype = typeidType(ltypeId);
			((Const *) ltree)->constlen = typeLen(newtype);
			((Const *) ltree)->constbyval = typeByVal(newtype);
		}

		temp = oper(opname, ltypeId, rtypeId, false);
		opform = (OperatorTupleForm) GETSTRUCT(temp);
		left = make_operand(opname, ltree, ltypeId, opform->oprleft);
		right = make_operand(opname, rtree, rtypeId, opform->oprright);
	}

	newop = makeOper(oprid(temp),		/* opno */
					 InvalidOid,/* opid */
					 opform->oprresult, /* operator result type */
					 0,
					 NULL);

	result = makeNode(Expr);
	result->typeOid = opform->oprresult;
	result->opType = OP_EXPR;
	result->oper = (Node *) newop;

	if (!left)
	{
		result->args = lcons(right, NIL);
	}
	else if (!right)
	{
		result->args = lcons(left, NIL);
	}
	else
	{
		result->args = lcons(left, lcons(right, NIL));
	}

	return result;
}

Var		   *
make_var(ParseState *pstate, char *refname, char *attrname, Oid *type_id)
{
	Var		   *varnode;
	int			vnum,
				attid;
	Oid			vartypeid;
	Relation	rd;
	RangeTblEntry *rte;

	rte = refnameRangeTableEntry(pstate->p_rtable, refname);
	if (rte == NULL)
		rte = addRangeTableEntry(pstate, refname, refname, FALSE, FALSE);

	vnum = refnameRangeTablePosn(pstate->p_rtable, refname);

	rd = heap_open(rte->relid);

	attid = attnameAttNum(rd, attrname); /* could elog(ERROR) */
	vartypeid = attnumTypeId(rd, attid);

	varnode = makeVar(vnum, attid, vartypeid, vnum, attid);

	heap_close(rd);

	*type_id = vartypeid;
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
	TypeTupleForm type_struct_array,
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
	type_struct_array = (TypeTupleForm) GETSTRUCT(type_tuple);

	if (type_struct_array->typelem == InvalidOid)
	{
		elog(ERROR, "make_array_ref: type %s is not an array",
			 (Name) &(type_struct_array->typname.data[0]));
	}

	/* get the type tuple for the element type */
	type_tuple = SearchSysCacheTuple(TYPOID,
							ObjectIdGetDatum(type_struct_array->typelem),
									 0, 0, 0);
	if (!HeapTupleIsValid(type_tuple))
		elog(ERROR, "make_array_ref: Cache lookup failed for type %d\n",
			 typearray);

	type_struct_element = (TypeTupleForm) GETSTRUCT(type_tuple);

	while (indirection != NIL)
	{
		A_Indices  *ind = lfirst(indirection);

		if (ind->lidx)
		{

			/*
			 * XXX assumes all lower indices non null in this case
			 */
			lowerIndexpr = lappend(lowerIndexpr, ind->lidx);
		}
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

ArrayRef   *
make_array_set(Expr *target_expr,
			   List *upperIndexpr,
			   List *lowerIndexpr,
			   Expr *expr)
{
	Oid			typearray;
	HeapTuple	type_tuple;
	TypeTupleForm type_struct_array;
	TypeTupleForm type_struct_element;
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
	type_struct_array = (TypeTupleForm) GETSTRUCT(type_tuple);

	if (type_struct_array->typelem == InvalidOid)
	{
		elog(ERROR, "make_array_ref: type %s is not an array",
			 (Name) &(type_struct_array->typname.data[0]));
	}
	/* get the type tuple for the element type */
	type_tuple = SearchSysCacheTuple(TYPOID,
							ObjectIdGetDatum(type_struct_array->typelem),
									 0, 0, 0);

	if (!HeapTupleIsValid(type_tuple))
		elog(ERROR, "make_array_ref: Cache lookup failed for type %d\n",
			 typearray);

	type_struct_element = (TypeTupleForm) GETSTRUCT(type_tuple);

	aref = makeNode(ArrayRef);
	aref->refattrlength = type_struct_array->typlen;
	aref->refelemlength = type_struct_element->typlen;
	aref->refelemtype = type_struct_array->typelem;
	aref->refelembyval = type_struct_element->typbyval;
	aref->refupperindexpr = upperIndexpr;
	aref->reflowerindexpr = lowerIndexpr;
	aref->refexpr = (Node *) target_expr;
	aref->refassgnexpr = (Node *) expr;

	if (lowerIndexpr == NIL)	/* accessing a single array element */
		reftype = aref->refelemtype;
	else
/* request to set a part of the array, by another array */
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
Const	   *
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
			tp = typeidType(UNKNOWNOID);	/* unknown for now, will be type
										 * coerced */
			val = PointerGetDatum(textin(strVal(value)));
			break;

		case T_Null:
		default:
			{
				if (nodeTag(value) != T_Null)
					elog(NOTICE, "unknown type : %d\n", nodeTag(value));

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

	return (con);
}

