/*-------------------------------------------------------------------------
 *
 * parse_coerce.c
 *		handle type coersions/conversions for parser
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_coerce.c,v 2.27 2000/01/10 17:14:36 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"


#include "optimizer/clauses.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_target.h"
#include "utils/builtins.h"
#include "utils/syscache.h"

Oid			DemoteType(Oid inType);
Oid			PromoteTypeToNext(Oid inType);

static Oid	PreferredType(CATEGORY category, Oid type);


/* coerce_type()
 * Convert a function argument to a different type.
 */
Node *
coerce_type(ParseState *pstate, Node *node, Oid inputTypeId, Oid targetTypeId,
			int32 atttypmod)
{
	Node	   *result = NULL;

	if (targetTypeId == InvalidOid ||
		targetTypeId == inputTypeId)
	{
		/* no conversion needed */
		result = node;
	}
	else if (IS_BINARY_COMPATIBLE(inputTypeId, targetTypeId))
	{
		/* no work if one of the known-good transparent conversions */
		result = node;
	}
	else if (inputTypeId == UNKNOWNOID && IsA(node, Const))
	{
		/*
		 * Input is a string constant with previously undetermined type.
		 * Apply the target type's typinput function to it to produce
		 * a constant of the target type.
		 *
		 * NOTE: this case cannot be folded together with the other
		 * constant-input case, since the typinput function does not
		 * necessarily behave the same as a type conversion function.
		 * For example, int4's typinput function will reject "1.2",
		 * whereas float-to-int type conversion will round to integer.
		 *
		 * XXX if the typinput function is not cachable, we really ought
		 * to postpone evaluation of the function call until runtime.
		 * But there is no way to represent a typinput function call as
		 * an expression tree, because C-string values are not Datums.
		 */
		Const	   *con = (Const *) node;
		Const	   *newcon = makeNode(Const);
		Type		targetType = typeidType(targetTypeId);

		newcon->consttype = targetTypeId;
		newcon->constlen = typeLen(targetType);
		newcon->constbyval = typeByVal(targetType);
		newcon->constisnull = con->constisnull;
		newcon->constisset = false;

		if (! con->constisnull)
		{
			/* We know the source constant is really of type 'text' */
			char	   *val = textout((text *) con->constvalue);
			newcon->constvalue = stringTypeDatum(targetType, val, atttypmod);
			pfree(val);
		}

		result = (Node *) newcon;
	}
	else
	{
		/*
		 * Otherwise, find the appropriate type conversion function
		 * (caller should have determined that there is one), and
		 * generate an expression tree representing run-time
		 * application of the conversion function.
		 */
		FuncCall   *n = makeNode(FuncCall);
		Type		targetType = typeidType(targetTypeId);

		n->funcname = typeTypeName(targetType);
		n->args = lcons(node, NIL);
		n->agg_star = false;
		n->agg_distinct = false;

		result = transformExpr(pstate, (Node *) n, EXPR_COLUMN_FIRST);

		/*
		 * If the input is a constant, apply the type conversion function
		 * now instead of delaying to runtime.  (We could, of course,
		 * just leave this to be done during planning/optimization;
		 * but it's a very frequent special case, and we save cycles
		 * in the rewriter if we fold the expression now.)
		 *
		 * Note that no folding will occur if the conversion function is
		 * not marked 'iscachable'.
		 */
		if (IsA(node, Const))
			result = eval_const_expressions(result);
	}

	return result;
}


/* can_coerce_type()
 * Can input_typeids be coerced to func_typeids?
 *
 * There are a few types which are known apriori to be convertible.
 * We will check for those cases first, and then look for possible
 *	conversion functions.
 *
 * Notes:
 * This uses the same mechanism as the CAST() SQL construct in gram.y.
 * We should also check the function return type on candidate conversion
 *	routines just to be safe but we do not do that yet...
 * We need to have a zero-filled OID array here, otherwise the cache lookup fails.
 * - thomas 1998-03-31
 */
bool
can_coerce_type(int nargs, Oid *input_typeids, Oid *func_typeids)
{
	HeapTuple	ftup;
	int			i;
	Type		tp;
	Oid			oid_array[FUNC_MAX_ARGS];

	/* run through argument list... */
	for (i = 0; i < nargs; i++)
	{
		if (input_typeids[i] != func_typeids[i])
		{

			/*
			 * one of the known-good transparent conversions? then drop
			 * through...
			 */
			if (IS_BINARY_COMPATIBLE(input_typeids[i], func_typeids[i]))
				;

			/* don't know what to do for the output type? then quit... */
			else if (func_typeids[i] == InvalidOid)
				return false;
			/* don't know what to do for the input type? then quit... */
			else if (input_typeids[i] == InvalidOid)
				return false;

			/*
			 * if not unknown input type, try for explicit conversion
			 * using functions...
			 */
			else if (input_typeids[i] != UNKNOWNOID)
			{
				MemSet(oid_array, 0, FUNC_MAX_ARGS * sizeof(Oid));
				oid_array[0] = input_typeids[i];

				/*
				 * look for a single-argument function named with the
				 * target type name
				 */
				ftup = SearchSysCacheTuple(PROCNAME,
						PointerGetDatum(typeidTypeName(func_typeids[i])),
										   Int32GetDatum(1),
										   PointerGetDatum(oid_array),
										   0);

				/*
				 * should also check the function return type just to be
				 * safe...
				 */
				if (!HeapTupleIsValid(ftup))
					return false;
			}

			tp = typeidType(input_typeids[i]);
			if (typeTypeFlag(tp) == 'c')
				return false;
		}
	}

	return true;
}


/* TypeCategory()
 * Assign a category to the specified OID.
 */
CATEGORY
TypeCategory(Oid inType)
{
	CATEGORY	result;

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

		case (OIDOID):
		case (INT2OID):
		case (INT4OID):
		case (INT8OID):
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

		case (INETOID):
		case (CIDROID):
			result = NETWORK_TYPE;
			break;

		default:
			result = USER_TYPE;
			break;
	}
	return result;
}	/* TypeCategory() */


/* IsPreferredType()
 * Check if this type is a preferred type.
 */
bool
IsPreferredType(CATEGORY category, Oid type)
{
	return type == PreferredType(category, type);
}	/* IsPreferredType() */


/* PreferredType()
 * Return the preferred type OID for the specified category.
 */
static Oid
PreferredType(CATEGORY category, Oid type)
{
	Oid			result;

	switch (category)
	{
		case (BOOLEAN_TYPE):
			result = BOOLOID;
			break;

		case (STRING_TYPE):
			result = TEXTOID;
			break;

		case (NUMERIC_TYPE):
			if (type == OIDOID)
				result = OIDOID;
			else
				result = FLOAT8OID;
			break;

		case (DATETIME_TYPE):
			result = DATETIMEOID;
			break;

		case (TIMESPAN_TYPE):
			result = TIMESPANOID;
			break;

		case (NETWORK_TYPE):
			result = INETOID;
			break;

		case (GEOMETRIC_TYPE):
		case (USER_TYPE):
			result = type;
			break;

		default:
			result = UNKNOWNOID;
			break;
	}
	return result;
}	/* PreferredType() */


#ifdef NOT_USED
Oid
PromoteTypeToNext(Oid inType)
{
	Oid			result;

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
		case (INT8OID):
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
	return result;
}	/* PromoteTypeToNext() */


Oid
DemoteType(Oid inType)
{
	Oid			result;

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
	return result;
}	/* DemoteType() */


Oid
PromoteLesserType(Oid inType1, Oid inType2, Oid *newType1, Oid *newType2)
{
	Oid			result;

	if (inType1 == inType2)
	{
		result = PromoteTypeToNext(inType1);
		inType1 = result;
		*arg2 = result;
		return result;
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
	}
	else if (isBuiltIn1 && !isBuiltIn2)
	{
		if ((promotedType = PromoteBuiltInType(*arg1)) != *arg1)
		{
			*arg1 = promotedType;
			return promotedType;
		}
		else if (CanCoerceType(*arg1, *arg2))
		{
			*arg1 = *arg2;
			return *arg2;
		}
	}
	else if (!isBuiltIn1 && isBuiltIn2)
	{
		if ((promotedType = PromoteBuiltInType(*arg2)) != *arg2)
		{
			*arg2 = promotedType;
			return promotedType;
		}
		else if (CanCoerceType(*arg2, *arg1))
		{
			*arg2 = *arg1;
			return *arg1;
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
}

#endif
