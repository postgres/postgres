/*-------------------------------------------------------------------------
 *
 * parse_node.c
 *	  various routines that make nodes for querytrees
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_node.c,v 1.81 2003/08/04 02:40:02 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "parser/parsetree.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_node.h"
#include "parser/parse_relation.h"
#include "utils/builtins.h"
#include "utils/int8.h"
#include "utils/syscache.h"
#include "utils/varbit.h"


/* make_parsestate()
 * Allocate and initialize a new ParseState.
 * The CALLER is responsible for freeing the ParseState* returned.
 */
ParseState *
make_parsestate(ParseState *parentParseState)
{
	ParseState *pstate;

	pstate = palloc0(sizeof(ParseState));

	pstate->parentParseState = parentParseState;

	/* Fill in fields that don't start at null/false/zero */
	pstate->p_next_resno = 1;

	if (parentParseState)
		pstate->p_variableparams = parentParseState->p_variableparams;

	return pstate;
}


/*
 * make_var
 *		Build a Var node for an attribute identified by RTE and attrno
 */
Var *
make_var(ParseState *pstate, RangeTblEntry *rte, int attrno)
{
	int			vnum,
				sublevels_up;
	Oid			vartypeid;
	int32		type_mod;

	vnum = RTERangeTablePosn(pstate, rte, &sublevels_up);
	get_rte_attribute_type(rte, attrno, &vartypeid, &type_mod);
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
 *				(may be NULL if we are handling an INSERT)
 * arrayType	OID of array's datatype
 * arrayTypMod	typmod to be applied to array elements
 * indirection	Untransformed list of subscripts (must not be NIL)
 * forceSlice	If true, treat subscript as array slice in all cases
 * assignFrom	NULL for array fetch, else transformed expression for source.
 */
ArrayRef *
transformArraySubscripts(ParseState *pstate,
						 Node *arrayBase,
						 Oid arrayType,
						 int32 arrayTypMod,
						 List *indirection,
						 bool forceSlice,
						 Node *assignFrom)
{
	Oid			elementType,
				resultType;
	HeapTuple	type_tuple_array;
	Form_pg_type type_struct_array;
	bool		isSlice = forceSlice;
	List	   *upperIndexpr = NIL;
	List	   *lowerIndexpr = NIL;
	List	   *idx;
	ArrayRef   *aref;

	/* Get the type tuple for the array */
	type_tuple_array = SearchSysCache(TYPEOID,
									  ObjectIdGetDatum(arrayType),
									  0, 0, 0);
	if (!HeapTupleIsValid(type_tuple_array))
		elog(ERROR, "cache lookup failed for type %u", arrayType);
	type_struct_array = (Form_pg_type) GETSTRUCT(type_tuple_array);

	elementType = type_struct_array->typelem;
	if (elementType == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
			errmsg("cannot subscript type %s because it is not an array",
				   format_type_be(arrayType))));

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
		resultType = arrayType;
	else
		resultType = elementType;

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
				subexpr = transformExpr(pstate, ai->lidx);
				/* If it's not int4 already, try to coerce */
				subexpr = coerce_to_target_type(pstate,
											  subexpr, exprType(subexpr),
												INT4OID, -1,
												COERCION_ASSIGNMENT,
												COERCE_IMPLICIT_CAST);
				if (subexpr == NULL)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
					  errmsg("array subscript must have type integer")));
			}
			else
			{
				/* Make a constant 1 */
				subexpr = (Node *) makeConst(INT4OID,
											 sizeof(int32),
											 Int32GetDatum(1),
											 false,
											 true);		/* pass by value */
			}
			lowerIndexpr = lappend(lowerIndexpr, subexpr);
		}
		subexpr = transformExpr(pstate, ai->uidx);
		/* If it's not int4 already, try to coerce */
		subexpr = coerce_to_target_type(pstate,
										subexpr, exprType(subexpr),
										INT4OID, -1,
										COERCION_ASSIGNMENT,
										COERCE_IMPLICIT_CAST);
		if (subexpr == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("array subscript must have type integer")));
		upperIndexpr = lappend(upperIndexpr, subexpr);
	}

	/*
	 * If doing an array store, coerce the source value to the right type.
	 */
	if (assignFrom != NULL)
	{
		Oid			typesource = exprType(assignFrom);
		Oid			typeneeded = isSlice ? arrayType : elementType;

		if (typesource != InvalidOid)
		{
			assignFrom = coerce_to_target_type(pstate,
											   assignFrom, typesource,
											   typeneeded, arrayTypMod,
											   COERCION_ASSIGNMENT,
											   COERCE_IMPLICIT_CAST);
			if (assignFrom == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("array assignment requires type %s"
								" but expression is of type %s",
								format_type_be(typeneeded),
								format_type_be(typesource)),
						 errhint("You will need to rewrite or cast the expression.")));
		}
	}

	/*
	 * Ready to build the ArrayRef node.
	 */
	aref = makeNode(ArrayRef);
	aref->refrestype = resultType;
	aref->refarraytype = arrayType;
	aref->refelemtype = elementType;
	aref->refupperindexpr = upperIndexpr;
	aref->reflowerindexpr = lowerIndexpr;
	aref->refexpr = (Expr *) arrayBase;
	aref->refassgnexpr = (Expr *) assignFrom;

	ReleaseSysCache(type_tuple_array);

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
 *	For integers and floats we produce int4, int8, or numeric depending
 *	on the value of the number.  XXX This should include int2 as well,
 *	but additional cleanup is needed before we can do that; else cases
 *	like "WHERE int4var = 42" will fail to be indexable.
 */
Const *
make_const(Value *value)
{
	Datum		val;
	int64		val64;
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
			/* could be an oversize integer as well as a float ... */
			if (scanint8(strVal(value), true, &val64))
			{
				val = Int64GetDatum(val64);

				typeid = INT8OID;
				typelen = sizeof(int64);
				typebyval = false;		/* XXX might change someday */
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
			val = DirectFunctionCall1(unknownin,
									  CStringGetDatum(strVal(value)));

			typeid = UNKNOWNOID;	/* will be coerced later */
			typelen = -1;		/* variable len */
			typebyval = false;
			break;

		case T_BitString:
			val = DirectFunctionCall3(bit_in,
									  CStringGetDatum(strVal(value)),
									  ObjectIdGetDatum(InvalidOid),
									  Int32GetDatum(-1));
			typeid = BITOID;
			typelen = -1;
			typebyval = false;
			break;

		case T_Null:
			/* return a null const */
			con = makeConst(UNKNOWNOID,
							-1,
							(Datum) 0,
							true,
							false);
			return con;

		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(value));
			return NULL;		/* keep compiler quiet */
	}

	con = makeConst(typeid,
					typelen,
					val,
					false,
					typebyval);

	return con;
}
