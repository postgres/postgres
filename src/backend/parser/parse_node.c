/*-------------------------------------------------------------------------
 *
 * parse_node.c
 *	  various routines that make nodes for query plans
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_node.c,v 1.47 2000/09/29 18:21:36 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <ctype.h>
#include <errno.h>
#include <float.h>

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

static bool fitsInFloat(Value *value);


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

	pstate->parentParseState = parentParseState;
	pstate->p_last_resno = 1;

	return pstate;
}


/* make_operand()
 * Ensure argument type match by forcing conversion of constants.
 */
Node *
make_operand(char *opname,
			 Node *tree,
			 Oid orig_typeId,
			 Oid target_typeId)
{
	Node	   *result;
	Type		target_type = typeidType(target_typeId);

	if (tree != NULL)
	{
		/* must coerce? */
		if (target_typeId != orig_typeId)
			result = coerce_type(NULL, tree, orig_typeId, target_typeId, -1);
		else
			result = tree;
	}
	else
	{
		/* otherwise, this is a NULL value */
		Const	   *con = makeNode(Const);

		con->consttype = target_typeId;
		con->constlen = typeLen(target_type);
		con->constvalue = (Datum) NULL;
		con->constisnull = true;
		con->constbyval = typeByVal(target_type);
		con->constisset = false;
		result = (Node *) con;
	}

	return result;
}	/* make_operand() */


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

	ltypeId = (ltree == NULL) ? UNKNOWNOID : exprType(ltree);
	rtypeId = (rtree == NULL) ? UNKNOWNOID : exprType(rtree);

	/* right operator? */
	if (rtree == NULL)
	{
		tup = right_oper(opname, ltypeId);
		opform = (Form_pg_operator) GETSTRUCT(tup);
		left = make_operand(opname, ltree, ltypeId, opform->oprleft);
		right = NULL;
	}

	/* left operator? */
	else if (ltree == NULL)
	{
		tup = left_oper(opname, rtypeId);
		opform = (Form_pg_operator) GETSTRUCT(tup);
		right = make_operand(opname, rtree, rtypeId, opform->oprright);
		left = NULL;
	}

	/* otherwise, binary operator */
	else
	{
		tup = oper(opname, ltypeId, rtypeId, FALSE);
		opform = (Form_pg_operator) GETSTRUCT(tup);
		left = make_operand(opname, ltree, ltypeId, opform->oprleft);
		right = make_operand(opname, rtree, rtypeId, opform->oprright);
	}

	newop = makeOper(oprid(tup),/* opno */
					 InvalidOid,/* opid */
					 opform->oprresult); /* operator result type */

	result = makeNode(Expr);
	result->typeOid = opform->oprresult;
	result->opType = OP_EXPR;
	result->oper = (Node *) newop;

	if (!left)
		result->args = makeList1(right);
	else if (!right)
		result->args = makeList1(left);
	else
		result->args = makeList2(left, right);

	return result;
}	/* make_op() */


/*
 * make_var
 *		Build a Var node for an attribute identified by RTE and attrno
 */
Var *
make_var(ParseState *pstate, RangeTblEntry *rte, int attrno)
{
	int			vnum,
				sublevels_up;
	Oid			vartypeid = 0;
	int32		type_mod = 0;

	vnum = RTERangeTablePosn(pstate, rte, &sublevels_up);

	if (rte->relid != InvalidOid)
	{
		/* Plain relation RTE --- get the attribute's type info */
		HeapTuple	tp;
		Form_pg_attribute att_tup;

		tp = SearchSysCacheTuple(ATTNUM,
								 ObjectIdGetDatum(rte->relid),
								 Int16GetDatum(attrno),
								 0, 0);
		/* this shouldn't happen... */
		if (!HeapTupleIsValid(tp))
			elog(ERROR, "Relation %s does not have attribute %d",
				 rte->relname, attrno);
		att_tup = (Form_pg_attribute) GETSTRUCT(tp);
		vartypeid = att_tup->atttypid;
		type_mod = att_tup->atttypmod;
	}
	else
	{
		/* Subselect RTE --- get type info from subselect's tlist */
		List	   *tlistitem;

		foreach(tlistitem, rte->subquery->targetList)
		{
			TargetEntry *te = (TargetEntry *) lfirst(tlistitem);

			if (te->resdom->resjunk || te->resdom->resno != attrno)
				continue;
			vartypeid = te->resdom->restype;
			type_mod = te->resdom->restypmod;
			break;
		}
		/* falling off end of list shouldn't happen... */
		if (tlistitem == NIL)
			elog(ERROR, "Subquery %s does not have attribute %d",
				 rte->eref->relname, attrno);
	}

	return makeVar(vnum, attrno, vartypeid, type_mod, sublevels_up);
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
 * that array.	We produce an expression that represents the new array value
 * with the source data inserted into the right part of the array.
 *
 * pstate		Parse state
 * arrayBase	Already-transformed expression for the array as a whole
 * indirection	Untransformed list of subscripts (must not be NIL)
 * forceSlice	If true, treat subscript as array slice in all cases
 * assignFrom	NULL for array fetch, else transformed expression for source.
 */
ArrayRef   *
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

	type_tuple = SearchSysCacheTuple(TYPEOID,
									 ObjectIdGetDatum(typearray),
									 0, 0, 0);
	if (!HeapTupleIsValid(type_tuple))
		elog(ERROR, "transformArraySubscripts: Cache lookup failed for array type %u",
			 typearray);
	type_struct_array = (Form_pg_type) GETSTRUCT(type_tuple);

	typeelement = type_struct_array->typelem;
	if (typeelement == InvalidOid)
		elog(ERROR, "transformArraySubscripts: type %s is not an array",
			 NameStr(type_struct_array->typname));

	/* Get the type tuple for the array element type */
	type_tuple = SearchSysCacheTuple(TYPEOID,
									 ObjectIdGetDatum(typeelement),
									 0, 0, 0);
	if (!HeapTupleIsValid(type_tuple))
		elog(ERROR, "transformArraySubscripts: Cache lookup failed for array element type %u",
			 typeelement);
	type_struct_element = (Form_pg_type) GETSTRUCT(type_tuple);

	/*
	 * A list containing only single subscripts refers to a single array
	 * element.  If any of the items are double subscripts (lower:upper),
	 * then the subscript expression means an array slice operation. In
	 * this case, we supply a default lower bound of 1 for any items that
	 * contain only a single subscript. The forceSlice parameter forces us
	 * to treat the operation as a slice, even if no lower bounds are
	 * mentioned.  Otherwise, we have to prescan the indirection list to
	 * see if there are any double subscripts.
	 */
	if (!isSlice)
	{
		foreach(idx, indirection)
		{
			A_Indices  *ai = (A_Indices *) lfirst(idx);

			if (ai->lidx != NULL)
			{
				isSlice = true;
				break;
			}
		}
	}

	/*
	 * The type represented by the subscript expression is the element
	 * type if we are fetching a single element, but it is the same as the
	 * array type if we are fetching a slice or storing.
	 */
	if (isSlice || assignFrom != NULL)
		typeresult = typearray;
	else
		typeresult = typeelement;

	/*
	 * Transform the subscript expressions.
	 */
	foreach(idx, indirection)
	{
		A_Indices  *ai = (A_Indices *) lfirst(idx);
		Node	   *subexpr;

		if (isSlice)
		{
			if (ai->lidx)
			{
				subexpr = transformExpr(pstate, ai->lidx, EXPR_COLUMN_FIRST);
				/* If it's not int4 already, try to coerce */
				subexpr = CoerceTargetExpr(pstate, subexpr, exprType(subexpr),
										   INT4OID, -1);
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
											 true,		/* pass by value */
											 false,
											 false);
			}
			lowerIndexpr = lappend(lowerIndexpr, subexpr);
		}
		subexpr = transformExpr(pstate, ai->uidx, EXPR_COLUMN_FIRST);
		/* If it's not int4 already, try to coerce */
		subexpr = CoerceTargetExpr(pstate, subexpr, exprType(subexpr),
								   INT4OID, -1);
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
				/* XXX fixme: need to get the array's atttypmod? */
				assignFrom = CoerceTargetExpr(pstate, assignFrom,
											  typesource, typeneeded,
											  -1);
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
	aref->refelemtype = typeresult;		/* XXX should save element type
										 * too */
	aref->refelembyval = type_struct_element->typbyval;
	aref->refupperindexpr = upperIndexpr;
	aref->reflowerindexpr = lowerIndexpr;
	aref->refexpr = arrayBase;
	aref->refassgnexpr = assignFrom;

	return aref;
}

/*
 * make_const
 *
 *	Convert a Value node (as returned by the grammar) to a Const node
 *	of the "natural" type for the constant.  Note that this routine is
 *	only used when there is no explicit cast for the constant, so we
 *	have to guess what type is wanted.
 *
 *	For string literals we produce a constant of type UNKNOWN ---- whose
 *	representation is the same as text, but it indicates to later type
 *	resolution that we're not sure that it should be considered text.
 *	Explicit "NULL" constants are also typed as UNKNOWN.
 *
 *	For integers and floats we produce int4, float8, or numeric depending
 *	on the value of the number.  XXX In some cases it would be nice to take
 *	context into account when determining the type to convert to, but in
 *	other cases we can't delay the type choice.  One possibility is to invent
 *	a dummy type "UNKNOWNNUMERIC" that's treated similarly to UNKNOWN;
 *	that would allow us to do the right thing in examples like a simple
 *	INSERT INTO table (numericcolumn) VALUES (1.234), since we wouldn't
 *	have to resolve the unknown type until we knew the destination column
 *	type.  On the other hand UNKNOWN has considerable problems of its own.
 *	We would not like "SELECT 1.2 + 3.4" to claim it can't choose a type.
 */
Const *
make_const(Value *value)
{
	Datum		val;
	Oid			typeid;
	int			typelen;
	bool		typebyval;
	Const	   *con;

	switch (nodeTag(value))
	{
		case T_Integer:
			val = Int32GetDatum(intVal(value));

			typeid = INT4OID;
			typelen = sizeof(int32);
			typebyval = true;
			break;

		case T_Float:
			if (fitsInFloat(value))
			{
				val = Float8GetDatum(floatVal(value));

				typeid = FLOAT8OID;
				typelen = sizeof(float8);
				typebyval = false; /* XXX might change someday */
			}
			else
			{
				val = DirectFunctionCall3(numeric_in,
										  CStringGetDatum(strVal(value)),
										  ObjectIdGetDatum(InvalidOid),
										  Int32GetDatum(-1));

				typeid = NUMERICOID;
				typelen = -1;	/* variable len */
				typebyval = false;
			}
			break;

		case T_String:
			val = DirectFunctionCall1(textin, CStringGetDatum(strVal(value)));

			typeid = UNKNOWNOID;/* will be coerced later */
			typelen = -1;		/* variable len */
			typebyval = false;
			break;

		default:
			elog(NOTICE, "make_const: unknown type %d", nodeTag(value));
			/* FALLTHROUGH */

		case T_Null:
			/* return a null const */
			con = makeConst(UNKNOWNOID,
							-1,
							(Datum) NULL,
							true,
							false,
							false,
							false);
			return con;
	}

	con = makeConst(typeid,
					typelen,
					val,
					false,
					typebyval,
					false,		/* not a set */
					false);		/* not coerced */

	return con;
}

/*
 * Decide whether a T_Float value fits in float8, or must be treated as
 * type "numeric".	We check the number of digits and check for overflow/
 * underflow.  (With standard compilation options, Postgres' NUMERIC type
 * can handle decimal exponents up to 1000, considerably more than most
 * implementations of float8, so this is a sensible test.)
 */
static bool
fitsInFloat(Value *value)
{
	const char *ptr;
	int			ndigits;
	char	   *endptr;

	/*
	 * Count digits, ignoring leading zeroes (but not trailing zeroes).
	 * DBL_DIG is the maximum safe number of digits for "double".
	 */
	ptr = strVal(value);
	while (*ptr == '+' || *ptr == '-' || *ptr == '0' || *ptr == '.')
		ptr++;
	ndigits = 0;
	for (; *ptr; ptr++)
	{
		if (isdigit((int) *ptr))
			ndigits++;
		else if (*ptr == 'e' || *ptr == 'E')
			break;				/* don't count digits in exponent */
	}
	if (ndigits > DBL_DIG)
		return false;

	/*
	 * Use strtod() to check for overflow/underflow.
	 */
	errno = 0;
	(void) strtod(strVal(value), &endptr);
	if (*endptr != '\0' || errno != 0)
		return false;

	return true;
}
