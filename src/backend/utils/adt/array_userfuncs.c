/*-------------------------------------------------------------------------
 *
 * array_userfuncs.c
 *	  Misc user-visible array support functions
 *
 * Copyright (c) 2003-2014, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/array_userfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"


/*-----------------------------------------------------------------------------
 * array_push :
 *		push an element onto either end of a one-dimensional array
 *----------------------------------------------------------------------------
 */
Datum
array_push(PG_FUNCTION_ARGS)
{
	ArrayType  *v;
	Datum		newelem;
	bool		isNull;
	int		   *dimv,
			   *lb;
	ArrayType  *result;
	int			indx;
	Oid			element_type;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	Oid			arg0_typeid = get_fn_expr_argtype(fcinfo->flinfo, 0);
	Oid			arg1_typeid = get_fn_expr_argtype(fcinfo->flinfo, 1);
	Oid			arg0_elemid;
	Oid			arg1_elemid;
	ArrayMetaState *my_extra;

	if (arg0_typeid == InvalidOid || arg1_typeid == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("could not determine input data types")));

	arg0_elemid = get_element_type(arg0_typeid);
	arg1_elemid = get_element_type(arg1_typeid);

	if (arg0_elemid != InvalidOid)
	{
		if (PG_ARGISNULL(0))
			v = construct_empty_array(arg0_elemid);
		else
			v = PG_GETARG_ARRAYTYPE_P(0);
		isNull = PG_ARGISNULL(1);
		if (isNull)
			newelem = (Datum) 0;
		else
			newelem = PG_GETARG_DATUM(1);
	}
	else if (arg1_elemid != InvalidOid)
	{
		if (PG_ARGISNULL(1))
			v = construct_empty_array(arg1_elemid);
		else
			v = PG_GETARG_ARRAYTYPE_P(1);
		isNull = PG_ARGISNULL(0);
		if (isNull)
			newelem = (Datum) 0;
		else
			newelem = PG_GETARG_DATUM(0);
	}
	else
	{
		/* Shouldn't get here given proper type checking in parser */
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("neither input type is an array")));
		PG_RETURN_NULL();		/* keep compiler quiet */
	}

	element_type = ARR_ELEMTYPE(v);

	if (ARR_NDIM(v) == 1)
	{
		lb = ARR_LBOUND(v);
		dimv = ARR_DIMS(v);

		if (arg0_elemid != InvalidOid)
		{
			/* append newelem */
			int			ub = dimv[0] + lb[0] - 1;

			indx = ub + 1;
			/* overflow? */
			if (indx < ub)
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("integer out of range")));
		}
		else
		{
			/* prepend newelem */
			indx = lb[0] - 1;
			/* overflow? */
			if (indx > lb[0])
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("integer out of range")));
		}
	}
	else if (ARR_NDIM(v) == 0)
		indx = 1;
	else
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("argument must be empty or one-dimensional array")));

	/*
	 * We arrange to look up info about element type only once per series of
	 * calls, assuming the element type doesn't change underneath us.
	 */
	my_extra = (ArrayMetaState *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL)
	{
		fcinfo->flinfo->fn_extra = MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
													  sizeof(ArrayMetaState));
		my_extra = (ArrayMetaState *) fcinfo->flinfo->fn_extra;
		my_extra->element_type = ~element_type;
	}

	if (my_extra->element_type != element_type)
	{
		/* Get info about element type */
		get_typlenbyvalalign(element_type,
							 &my_extra->typlen,
							 &my_extra->typbyval,
							 &my_extra->typalign);
		my_extra->element_type = element_type;
	}
	typlen = my_extra->typlen;
	typbyval = my_extra->typbyval;
	typalign = my_extra->typalign;

	result = array_set(v, 1, &indx, newelem, isNull,
					   -1, typlen, typbyval, typalign);

	/*
	 * Readjust result's LB to match the input's.  This does nothing in the
	 * append case, but it's the simplest way to implement the prepend case.
	 */
	if (ARR_NDIM(v) == 1)
		ARR_LBOUND(result)[0] = ARR_LBOUND(v)[0];

	PG_RETURN_ARRAYTYPE_P(result);
}

/*-----------------------------------------------------------------------------
 * array_cat :
 *		concatenate two nD arrays to form an nD array, or
 *		push an (n-1)D array onto the end of an nD array
 *----------------------------------------------------------------------------
 */
Datum
array_cat(PG_FUNCTION_ARGS)
{
	ArrayType  *v1,
			   *v2;
	ArrayType  *result;
	int		   *dims,
			   *lbs,
				ndims,
				nitems,
				ndatabytes,
				nbytes;
	int		   *dims1,
			   *lbs1,
				ndims1,
				nitems1,
				ndatabytes1;
	int		   *dims2,
			   *lbs2,
				ndims2,
				nitems2,
				ndatabytes2;
	int			i;
	char	   *dat1,
			   *dat2;
	bits8	   *bitmap1,
			   *bitmap2;
	Oid			element_type;
	Oid			element_type1;
	Oid			element_type2;
	int32		dataoffset;

	/* Concatenating a null array is a no-op, just return the other input */
	if (PG_ARGISNULL(0))
	{
		if (PG_ARGISNULL(1))
			PG_RETURN_NULL();
		result = PG_GETARG_ARRAYTYPE_P(1);
		PG_RETURN_ARRAYTYPE_P(result);
	}
	if (PG_ARGISNULL(1))
	{
		result = PG_GETARG_ARRAYTYPE_P(0);
		PG_RETURN_ARRAYTYPE_P(result);
	}

	v1 = PG_GETARG_ARRAYTYPE_P(0);
	v2 = PG_GETARG_ARRAYTYPE_P(1);

	element_type1 = ARR_ELEMTYPE(v1);
	element_type2 = ARR_ELEMTYPE(v2);

	/* Check we have matching element types */
	if (element_type1 != element_type2)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("cannot concatenate incompatible arrays"),
				 errdetail("Arrays with element types %s and %s are not "
						   "compatible for concatenation.",
						   format_type_be(element_type1),
						   format_type_be(element_type2))));

	/* OK, use it */
	element_type = element_type1;

	/*----------
	 * We must have one of the following combinations of inputs:
	 * 1) one empty array, and one non-empty array
	 * 2) both arrays empty
	 * 3) two arrays with ndims1 == ndims2
	 * 4) ndims1 == ndims2 - 1
	 * 5) ndims1 == ndims2 + 1
	 *----------
	 */
	ndims1 = ARR_NDIM(v1);
	ndims2 = ARR_NDIM(v2);

	/*
	 * short circuit - if one input array is empty, and the other is not, we
	 * return the non-empty one as the result
	 *
	 * if both are empty, return the first one
	 */
	if (ndims1 == 0 && ndims2 > 0)
		PG_RETURN_ARRAYTYPE_P(v2);

	if (ndims2 == 0)
		PG_RETURN_ARRAYTYPE_P(v1);

	/* the rest fall under rule 3, 4, or 5 */
	if (ndims1 != ndims2 &&
		ndims1 != ndims2 - 1 &&
		ndims1 != ndims2 + 1)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("cannot concatenate incompatible arrays"),
				 errdetail("Arrays of %d and %d dimensions are not "
						   "compatible for concatenation.",
						   ndims1, ndims2)));

	/* get argument array details */
	lbs1 = ARR_LBOUND(v1);
	lbs2 = ARR_LBOUND(v2);
	dims1 = ARR_DIMS(v1);
	dims2 = ARR_DIMS(v2);
	dat1 = ARR_DATA_PTR(v1);
	dat2 = ARR_DATA_PTR(v2);
	bitmap1 = ARR_NULLBITMAP(v1);
	bitmap2 = ARR_NULLBITMAP(v2);
	nitems1 = ArrayGetNItems(ndims1, dims1);
	nitems2 = ArrayGetNItems(ndims2, dims2);
	ndatabytes1 = ARR_SIZE(v1) - ARR_DATA_OFFSET(v1);
	ndatabytes2 = ARR_SIZE(v2) - ARR_DATA_OFFSET(v2);

	if (ndims1 == ndims2)
	{
		/*
		 * resulting array is made up of the elements (possibly arrays
		 * themselves) of the input argument arrays
		 */
		ndims = ndims1;
		dims = (int *) palloc(ndims * sizeof(int));
		lbs = (int *) palloc(ndims * sizeof(int));

		dims[0] = dims1[0] + dims2[0];
		lbs[0] = lbs1[0];

		for (i = 1; i < ndims; i++)
		{
			if (dims1[i] != dims2[i] || lbs1[i] != lbs2[i])
				ereport(ERROR,
						(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
						 errmsg("cannot concatenate incompatible arrays"),
					errdetail("Arrays with differing element dimensions are "
							  "not compatible for concatenation.")));

			dims[i] = dims1[i];
			lbs[i] = lbs1[i];
		}
	}
	else if (ndims1 == ndims2 - 1)
	{
		/*
		 * resulting array has the second argument as the outer array, with
		 * the first argument inserted at the front of the outer dimension
		 */
		ndims = ndims2;
		dims = (int *) palloc(ndims * sizeof(int));
		lbs = (int *) palloc(ndims * sizeof(int));
		memcpy(dims, dims2, ndims * sizeof(int));
		memcpy(lbs, lbs2, ndims * sizeof(int));

		/* increment number of elements in outer array */
		dims[0] += 1;

		/* make sure the added element matches our existing elements */
		for (i = 0; i < ndims1; i++)
		{
			if (dims1[i] != dims[i + 1] || lbs1[i] != lbs[i + 1])
				ereport(ERROR,
						(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
						 errmsg("cannot concatenate incompatible arrays"),
						 errdetail("Arrays with differing dimensions are not "
								   "compatible for concatenation.")));
		}
	}
	else
	{
		/*
		 * (ndims1 == ndims2 + 1)
		 *
		 * resulting array has the first argument as the outer array, with the
		 * second argument appended to the end of the outer dimension
		 */
		ndims = ndims1;
		dims = (int *) palloc(ndims * sizeof(int));
		lbs = (int *) palloc(ndims * sizeof(int));
		memcpy(dims, dims1, ndims * sizeof(int));
		memcpy(lbs, lbs1, ndims * sizeof(int));

		/* increment number of elements in outer array */
		dims[0] += 1;

		/* make sure the added element matches our existing elements */
		for (i = 0; i < ndims2; i++)
		{
			if (dims2[i] != dims[i + 1] || lbs2[i] != lbs[i + 1])
				ereport(ERROR,
						(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
						 errmsg("cannot concatenate incompatible arrays"),
						 errdetail("Arrays with differing dimensions are not "
								   "compatible for concatenation.")));
		}
	}

	/* Do this mainly for overflow checking */
	nitems = ArrayGetNItems(ndims, dims);

	/* build the result array */
	ndatabytes = ndatabytes1 + ndatabytes2;
	if (ARR_HASNULL(v1) || ARR_HASNULL(v2))
	{
		dataoffset = ARR_OVERHEAD_WITHNULLS(ndims, nitems);
		nbytes = ndatabytes + dataoffset;
	}
	else
	{
		dataoffset = 0;			/* marker for no null bitmap */
		nbytes = ndatabytes + ARR_OVERHEAD_NONULLS(ndims);
	}
	result = (ArrayType *) palloc0(nbytes);
	SET_VARSIZE(result, nbytes);
	result->ndim = ndims;
	result->dataoffset = dataoffset;
	result->elemtype = element_type;
	memcpy(ARR_DIMS(result), dims, ndims * sizeof(int));
	memcpy(ARR_LBOUND(result), lbs, ndims * sizeof(int));
	/* data area is arg1 then arg2 */
	memcpy(ARR_DATA_PTR(result), dat1, ndatabytes1);
	memcpy(ARR_DATA_PTR(result) + ndatabytes1, dat2, ndatabytes2);
	/* handle the null bitmap if needed */
	if (ARR_HASNULL(result))
	{
		array_bitmap_copy(ARR_NULLBITMAP(result), 0,
						  bitmap1, 0,
						  nitems1);
		array_bitmap_copy(ARR_NULLBITMAP(result), nitems1,
						  bitmap2, 0,
						  nitems2);
	}

	PG_RETURN_ARRAYTYPE_P(result);
}


/*
 * used by text_to_array() in varlena.c
 */
ArrayType *
create_singleton_array(FunctionCallInfo fcinfo,
					   Oid element_type,
					   Datum element,
					   bool isNull,
					   int ndims)
{
	Datum		dvalues[1];
	bool		nulls[1];
	int16		typlen;
	bool		typbyval;
	char		typalign;
	int			dims[MAXDIM];
	int			lbs[MAXDIM];
	int			i;
	ArrayMetaState *my_extra;

	if (ndims < 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid number of dimensions: %d", ndims)));
	if (ndims > MAXDIM)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("number of array dimensions (%d) exceeds the maximum allowed (%d)",
						ndims, MAXDIM)));

	dvalues[0] = element;
	nulls[0] = isNull;

	for (i = 0; i < ndims; i++)
	{
		dims[i] = 1;
		lbs[i] = 1;
	}

	/*
	 * We arrange to look up info about element type only once per series of
	 * calls, assuming the element type doesn't change underneath us.
	 */
	my_extra = (ArrayMetaState *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL)
	{
		fcinfo->flinfo->fn_extra = MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
													  sizeof(ArrayMetaState));
		my_extra = (ArrayMetaState *) fcinfo->flinfo->fn_extra;
		my_extra->element_type = ~element_type;
	}

	if (my_extra->element_type != element_type)
	{
		/* Get info about element type */
		get_typlenbyvalalign(element_type,
							 &my_extra->typlen,
							 &my_extra->typbyval,
							 &my_extra->typalign);
		my_extra->element_type = element_type;
	}
	typlen = my_extra->typlen;
	typbyval = my_extra->typbyval;
	typalign = my_extra->typalign;

	return construct_md_array(dvalues, nulls, ndims, dims, lbs, element_type,
							  typlen, typbyval, typalign);
}


/*
 * ARRAY_AGG aggregate function
 */
Datum
array_agg_transfn(PG_FUNCTION_ARGS)
{
	Oid			arg1_typeid = get_fn_expr_argtype(fcinfo->flinfo, 1);
	MemoryContext aggcontext;
	ArrayBuildState *state;
	Datum		elem;

	if (arg1_typeid == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("could not determine input data type")));

	if (!AggCheckCallContext(fcinfo, &aggcontext))
	{
		/* cannot be called directly because of internal-type argument */
		elog(ERROR, "array_agg_transfn called in non-aggregate context");
	}

	state = PG_ARGISNULL(0) ? NULL : (ArrayBuildState *) PG_GETARG_POINTER(0);
	elem = PG_ARGISNULL(1) ? (Datum) 0 : PG_GETARG_DATUM(1);
	state = accumArrayResult(state,
							 elem,
							 PG_ARGISNULL(1),
							 arg1_typeid,
							 aggcontext);

	/*
	 * The transition type for array_agg() is declared to be "internal", which
	 * is a pass-by-value type the same size as a pointer.  So we can safely
	 * pass the ArrayBuildState pointer through nodeAgg.c's machinations.
	 */
	PG_RETURN_POINTER(state);
}

Datum
array_agg_finalfn(PG_FUNCTION_ARGS)
{
	Datum		result;
	ArrayBuildState *state;
	int			dims[1];
	int			lbs[1];

	/*
	 * Test for null before Asserting we are in right context.  This is to
	 * avoid possible Assert failure in 8.4beta installations, where it is
	 * possible for users to create NULL constants of type internal.
	 */
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();		/* returns null iff no input values */

	/* cannot be called directly because of internal-type argument */
	Assert(AggCheckCallContext(fcinfo, NULL));

	state = (ArrayBuildState *) PG_GETARG_POINTER(0);

	dims[0] = state->nelems;
	lbs[0] = 1;

	/*
	 * Make the result.  We cannot release the ArrayBuildState because
	 * sometimes aggregate final functions are re-executed.  Rather, it is
	 * nodeAgg.c's responsibility to reset the aggcontext when it's safe to do
	 * so.
	 */
	result = makeMdArrayResult(state, 1, dims, lbs,
							   CurrentMemoryContext,
							   false);

	PG_RETURN_DATUM(result);
}
