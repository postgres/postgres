/*-------------------------------------------------------------------------
 *
 * parse_coerce.c
 *		handle type coersions/conversions for parser
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_coerce.c,v 2.2 1998/05/29 14:00:20 thomas Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include "postgres.h"
#include "utils/builtins.h"
#include "fmgr.h"
#include "nodes/makefuncs.h"

#include "parser/parse_expr.h"

#include "catalog/pg_type.h"
#include "parser/parse_type.h"
#include "parser/parse_target.h"
#include "parser/parse_coerce.h"
#include "utils/syscache.h"

Oid DemoteType(Oid inType);
Oid PromoteTypeToNext(Oid inType);


/* coerce_type()
 * Convert a function argument to a different type.
 */
Node *
coerce_type(ParseState *pstate, Node *node, Oid inputTypeId, Oid targetTypeId)
{
	Node   *result = NULL;
	Oid		infunc;
	Datum	val;

#ifdef PARSEDEBUG
printf("coerce_type: argument types are %d -> %d\n",
 inputTypeId, targetTypeId);
#endif

	if (targetTypeId == InvalidOid)
	{
#ifdef PARSEDEBUG
printf("coerce_type: apparent NULL target argument; suppress type conversion\n");
#endif
		result = node;
	}
	else if (inputTypeId != targetTypeId)
	{
		/* one of the known-good transparent conversions? then drop through... */
		if (IS_BINARY_COMPATIBLE(inputTypeId, targetTypeId))
		{
#ifdef PARSEDEBUG
printf("coerce_type: argument type %s is known to be convertible to type %s\n",
 typeidTypeName(inputTypeId), typeidTypeName(targetTypeId));
#endif
			result = node;
		}

		/* if not unknown input type, try for explicit conversion using functions... */
		else if (inputTypeId != UNKNOWNOID)
		{
			/* We already know there is a function which will do this, so let's use it */
			FuncCall *n = makeNode(FuncCall);
			n->funcname = typeidTypeName(targetTypeId);
			n->args = lcons(node, NIL);

#ifdef PARSEDEBUG
printf("coerce_type: construct function %s(%s)\n",
 typeidTypeName(targetTypeId), typeidTypeName(inputTypeId));
#endif

			result = transformExpr(pstate, (Node *) n, EXPR_COLUMN_FIRST);
		}
		else
		{
#ifdef PARSEDEBUG
printf("coerce_type: node is UNKNOWN type\n");
#endif
            if (nodeTag(node) == T_Const)
            {
				Const *con = (Const *) node;

				val = (Datum) textout((struct varlena *)
									   con->constvalue);
				infunc = typeidInfunc(targetTypeId);
				con = makeNode(Const);
				con->consttype = targetTypeId;
				con->constlen = typeLen(typeidType(targetTypeId));

				/* use "-1" for varchar() type */
				con->constvalue = (Datum) fmgr(infunc,
											   val,
											   typeidTypElem(targetTypeId),
											   -1);
				con->constisnull = false;
				con->constbyval = true;
				con->constisset = false;
				result = (Node *) con;
			}
			else
			{
#ifdef PARSEDEBUG
printf("coerce_type: should never get here!\n");
#endif
				result = node;
			}
		}
	}
	else
	{
#ifdef PARSEDEBUG
printf("coerce_type: argument type IDs %d match\n", inputTypeId);
#endif

		result = node;
	}

	return result;
} /* coerce_type() */


/* can_coerce_type()
 * Can input_typeids be coerced to func_typeids?
 *
 * There are a few types which are known apriori to be convertible.
 * We will check for those cases first, and then look for possible
 *  conversion functions.
 *
 * Notes:
 * This uses the same mechanism as the CAST() SQL construct in gram.y.
 * We should also check the function return type on candidate conversion
 *  routines just to be safe but we do not do that yet...
 * We need to have a zero-filled OID array here, otherwise the cache lookup fails.
 * - thomas 1998-03-31
 */
bool
can_coerce_type(int nargs, Oid *input_typeids, Oid *func_typeids)
{
	HeapTuple	ftup;
	int			i;
	Type		tp;
	Oid			oid_array[8];

	/* run through argument list... */
	for (i = 0; i < nargs; i++)
	{
#ifdef PARSEDEBUG
printf("can_coerce_type: argument #%d types are %d -> %d\n",
 i, input_typeids[i], func_typeids[i]);
#endif
		if (input_typeids[i] != func_typeids[i])
		{
			/* one of the known-good transparent conversions? then drop through... */
			if (IS_BINARY_COMPATIBLE(input_typeids[i], func_typeids[i]))
			{
#ifdef PARSEDEBUG
printf("can_coerce_type: argument #%d type %s is known to be convertible to type %s\n",
 i, typeidTypeName(input_typeids[i]), typeidTypeName(func_typeids[i]));
#endif
			}

			/* don't know what to do for the output type? then quit... */
			else if (func_typeids[i] == InvalidOid)
			{
#ifdef PARSEDEBUG
printf("can_coerce_type: output OID func_typeids[%d] is zero\n", i);
#endif
				return false;
			}

			/* don't know what to do for the input type? then quit... */
			else if (input_typeids[i] == InvalidOid)
			{
#ifdef PARSEDEBUG
printf("can_coerce_type: input OID input_typeids[%d] is zero\n", i);
#endif
				return false;
			}

			/* if not unknown input type, try for explicit conversion using functions... */
			else if (input_typeids[i] != UNKNOWNOID)
			{
				MemSet(&oid_array[0], 0, 8 * sizeof(Oid));
				oid_array[0] = input_typeids[i];

				/* look for a single-argument function named with the target type name */
				ftup = SearchSysCacheTuple(PRONAME,
										   PointerGetDatum(typeidTypeName(func_typeids[i])),
										   Int32GetDatum(1),
										   PointerGetDatum(oid_array),
										   0);

				/* should also check the function return type just to be safe... */
				if (HeapTupleIsValid(ftup))
				{
#ifdef PARSEDEBUG
printf("can_coerce_type: found function %s(%s) to convert argument #%d\n",
 typeidTypeName(func_typeids[i]), typeidTypeName(input_typeids[i]), i);
#endif
				}
				else
				{
#ifdef PARSEDEBUG
printf("can_coerce_type: did not find function %s(%s) to convert argument #%d\n",
 typeidTypeName(func_typeids[i]), typeidTypeName(input_typeids[i]), i);
#endif
					return false;
				}
			}
			else
			{
#ifdef PARSEDEBUG
printf("can_coerce_type: argument #%d type is %d (UNKNOWN)\n",
 i, input_typeids[i]);
#endif
			}

			tp = typeidType(input_typeids[i]);
			if (typeTypeFlag(tp) == 'c')
			{
#ifdef PARSEDEBUG
printf("can_coerce_type: typeTypeFlag for %s is 'c'\n",
 typeidTypeName(input_typeids[i]));
#endif
				return false;
			}

#ifdef PARSEDEBUG
printf("can_coerce_type: conversion from %s to %s is possible\n",
 typeidTypeName(input_typeids[i]), typeidTypeName(func_typeids[i]));
#endif
		}
		else
		{
#ifdef PARSEDEBUG
printf("can_coerce_type: argument #%d type IDs %d match\n",
 i, input_typeids[i]);
#endif
		}
	}

	return true;
} /* can_coerce_type() */


/* TypeCategory()
 * Assign a category to the specified OID.
 */
CATEGORY
TypeCategory(Oid inType)
{
	CATEGORY result;

	switch (inType)
	{
		case (BOOLOID):
			result = BOOLEAN_TYPE;
			break;

		case (CHAROID):
		case (BPCHAROID):
		case (VARCHAROID):
		case (TEXTOID):
			result = STRING_TYPE;
			break;

		case (INT2OID):
		case (INT4OID):
		case (FLOAT4OID):
		case (FLOAT8OID):
		case (CASHOID):
			result = NUMERIC_TYPE;
			break;

		case (ABSTIMEOID):
		case (TIMESTAMPOID):
		case (DATETIMEOID):
			result = DATETIME_TYPE;
			break;

		case (RELTIMEOID):
		case (TIMESPANOID):
			result = TIMESPAN_TYPE;
			break;

		case (POINTOID):
		case (LSEGOID):
		case (LINEOID):
		case (BOXOID):
		case (PATHOID):
		case (CIRCLEOID):
		case (POLYGONOID):
			result = GEOMETRIC_TYPE;
			break;

		default:
			result = USER_TYPE;
			break;
	}
	return (result);
} /* TypeCategory() */


/* IsPreferredType()
 * Assign a category to the specified OID.
 */
bool
IsPreferredType(CATEGORY category, Oid type)
{
	return (type == PreferredType(category, type));
} /* IsPreferredType() */


/* PreferredType()
 * Return the preferred type OID for the specified category.
 */
Oid
PreferredType(CATEGORY category, Oid type)
{
	Oid		result;

	switch (category)
	{
		case (BOOLEAN_TYPE):
			result = BOOLOID;
			break;

		case (STRING_TYPE):
			result = TEXTOID;
			break;

		case (NUMERIC_TYPE):
			result = FLOAT8OID;
			break;

		case (DATETIME_TYPE):
			result = DATETIMEOID;
			break;

		case (TIMESPAN_TYPE):
			result = TIMESPANOID;
			break;

		case (GEOMETRIC_TYPE):
		case (USER_TYPE):
			result = type;
			break;

		default:
			result = UNKNOWNOID;
			break;
	}
#ifdef PARSEDEBUG
printf("PreferredType- (%d) preferred type is %s\n", category, typeidTypeName(result));
#endif
	return (result);
} /* PreferredType() */


#if FALSE
Oid
PromoteTypeToNext(Oid inType)
{
	Oid result;

	switch (inType)
	{
		case (CHAROID):
		case (BPCHAROID):
			result = VARCHAROID;
			break;

		case (VARCHAROID):
			result = TEXTOID;
			break;

		case (INT2OID):
		case (CASHOID):
			result = INT4OID;
			break;

		case (INT4OID):
		case (FLOAT4OID):
			result = FLOAT8OID;
			break;

		case (DATEOID):
		case (ABSTIMEOID):
		case (TIMESTAMPOID):
			result = DATETIMEOID;
			break;

		case (TIMEOID):
		case (RELTIMEOID):
			result = TIMESPANOID;
			break;

		case (BOOLOID):
		case (TEXTOID):
		case (FLOAT8OID):
		case (DATETIMEOID):
		case (TIMESPANOID):
		default:
			result = inType;
			break;
	}
	return (result);
} /* PromoteTypeToNext() */


Oid
DemoteType(Oid inType)
{
	Oid result;

	switch (inType)
	{
		case (FLOAT4OID):
		case (FLOAT8OID):
			result = INT4OID;
			break;

		default:
			result = inType;
			break;
	}
	return (result);
} /* DemoteType() */


Oid
PromoteLesserType(Oid inType1, Oid inType2, Oid *newType1, Oid *newType2)
{
	Oid result;

	if (inType1 == inType2)
	{
		result = PromoteTypeToNext(inType1);
		inType1 = result;
		*arg2 = result;
		return (result);
	}

	kind1 = ClassifyType(inType1);
	kind2 = ClassifyType(*arg2);
	if (kind1 != kind2)
	{
		*newType1 = inType1;
		*newType2 = inType2;
		result = InvalidOid;
	}

	isBuiltIn1 = IS_BUILTIN_TYPE(inType1);
	isBuiltIn2 = IS_BUILTIN_TYPE(*arg2);

	if (isBuiltIn1 && isBuiltIn2)
	{
		switch (*arg1)
		{
			case (CHAROID):
			switch (*arg2)
			{
		case (BPCHAROID):
		case (VARCHAROID):
		case (TEXTOID):

		case (INT2OID):
		case (INT4OID):
		case (FLOAT4OID):
		case (FLOAT8OID):
		case (CASHOID):

		case (POINTOID):
		case (LSEGOID):
		case (LINEOID):
		case (BOXOID):
		case (PATHOID):
		case (CIRCLEOID):
		case (POLYGONOID):

		case (InvalidOid):
		case (UNKNOWNOID):
		case (BOOLOID):
		default:
			*arg1 = InvalidOid;
			*arg2 = InvalidOid;
			result = InvalidOid;
	}
	}
	else if (isBuiltIn1 && !isBuiltIn2)
	{
		if ((promotedType = PromoteBuiltInType(*arg1)) != *arg1)
		{
			*arg1 = promotedType;
			return (promotedType);
		}
		else if (CanCoerceType(*arg1, *arg2))
		{
			*arg1 = *arg2;
			return (*arg2);
		}
	}
	else if (!isBuiltIn1 && isBuiltIn2)
	{
		if ((promotedType = PromoteBuiltInType(*arg2)) != *arg2)
		{
			*arg2 = promotedType;
			return (promotedType);
		}
		else if (CanCoerceType(*arg2, *arg1))
		{
			*arg2 = *arg1;
			return (*arg1);
		}
	}


	if (*arg2 == InvalidOid)
		return InvalidOid;

	switch (*arg1)
	{
		case (CHAROID):
			switch (*arg2)
			{
		case (BPCHAROID):
		case (VARCHAROID):
		case (TEXTOID):

		case (INT2OID):
		case (INT4OID):
		case (FLOAT4OID):
		case (FLOAT8OID):
		case (CASHOID):

		case (POINTOID):
		case (LSEGOID):
		case (LINEOID):
		case (BOXOID):
		case (PATHOID):
		case (CIRCLEOID):
		case (POLYGONOID):

		case (InvalidOid):
		case (UNKNOWNOID):
		case (BOOLOID):
		default:
			*arg1 = InvalidOid;
			*arg2 = InvalidOid;
			result = InvalidOid;
	}
}
#endif
