/*-------------------------------------------------------------------------
 *
 * array_userfuncs.c
 *	  Misc user-visible array support functions
 *
 * Copyright (c) 2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/array_userfuncs.c,v 1.11.2.1 2004/12/17 21:00:07 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

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
	int		   *dimv,
			   *lb;
	ArrayType  *result;
	int			indx;
	bool		isNull;
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
		v = PG_GETARG_ARRAYTYPE_P(0);
		element_type = ARR_ELEMTYPE(v);
		newelem = PG_GETARG_DATUM(1);
	}
	else if (arg1_elemid != InvalidOid)
	{
		v = PG_GETARG_ARRAYTYPE_P(1);
		element_type = ARR_ELEMTYPE(v);
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

	if (ARR_NDIM(v) == 1)
	{
		lb = ARR_LBOUND(v);
		dimv = ARR_DIMS(v);

		if (arg0_elemid != InvalidOid)
		{
			/* append newelem */
			int			ub = dimv[0] + lb[0] - 1;

			indx = ub + 1;
		}
		else
		{
			/* prepend newelem */
			indx = lb[0] - 1;
		}
	}
	else if (ARR_NDIM(v) == 0)
		indx = 1;
	else
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("argument must be empty or one-dimensional array")));

	/*
	 * We arrange to look up info about element type only once per series
	 * of calls, assuming the element type doesn't change underneath us.
	 */
	my_extra = (ArrayMetaState *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL)
	{
		fcinfo->flinfo->fn_extra = MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
												 sizeof(ArrayMetaState));
		my_extra = (ArrayMetaState *) fcinfo->flinfo->fn_extra;
		my_extra->element_type = InvalidOid;
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

	result = array_set(v, 1, &indx, newelem, -1,
					   typlen, typbyval, typalign, &isNull);

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
	int		   *dims,
			   *lbs,
				ndims,
				ndatabytes,
				nbytes;
	int		   *dims1,
			   *lbs1,
				ndims1,
				ndatabytes1;
	int		   *dims2,
			   *lbs2,
				ndims2,
				ndatabytes2;
	int			i;
	char	   *dat1,
			   *dat2;
	Oid			element_type;
	Oid			element_type1;
	Oid			element_type2;
	ArrayType  *result;

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
	 * short circuit - if one input array is empty, and the other is not,
	 * we return the non-empty one as the result
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
	ndatabytes1 = ARR_SIZE(v1) - ARR_OVERHEAD(ndims1);
	ndatabytes2 = ARR_SIZE(v2) - ARR_OVERHEAD(ndims2);

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
		 * resulting array has the second argument as the outer array,
		 * with the first argument appended to the front of the outer
		 * dimension
		 */
		ndims = ndims2;
		dims = (int *) palloc(ndims * sizeof(int));
		lbs = (int *) palloc(ndims * sizeof(int));
		memcpy(dims, dims2, ndims * sizeof(int));
		memcpy(lbs, lbs2, ndims * sizeof(int));

		/* increment number of elements in outer array */
		dims[0] += 1;

		/* decrement outer array lower bound */
		lbs[0] -= 1;

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
		 * resulting array has the first argument as the outer array, with
		 * the second argument appended to the end of the outer dimension
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

	/* build the result array */
	ndatabytes = ndatabytes1 + ndatabytes2;
	nbytes = ndatabytes + ARR_OVERHEAD(ndims);
	result = (ArrayType *) palloc(nbytes);

	result->size = nbytes;
	result->ndim = ndims;
	result->flags = 0;
	result->elemtype = element_type;
	memcpy(ARR_DIMS(result), dims, ndims * sizeof(int));
	memcpy(ARR_LBOUND(result), lbs, ndims * sizeof(int));
	/* data area is arg1 then arg2 */
	memcpy(ARR_DATA_PTR(result), dat1, ndatabytes1);
	memcpy(ARR_DATA_PTR(result) + ndatabytes1, dat2, ndatabytes2);

	PG_RETURN_ARRAYTYPE_P(result);
}


/*
 * used by text_to_array() in varlena.c
 */
ArrayType *
create_singleton_array(FunctionCallInfo fcinfo,
					   Oid element_type,
					   Datum element,
					   int ndims)
{
	Datum		dvalues[1];
	int16		typlen;
	bool		typbyval;
	char		typalign;
	int			dims[MAXDIM];
	int			lbs[MAXDIM];
	int			i;
	ArrayMetaState *my_extra;

	if (element_type == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid array element type OID: %u", element_type)));
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

	for (i = 0; i < ndims; i++)
	{
		dims[i] = 1;
		lbs[i] = 1;
	}

	/*
	 * We arrange to look up info about element type only once per series
	 * of calls, assuming the element type doesn't change underneath us.
	 */
	my_extra = (ArrayMetaState *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL)
	{
		fcinfo->flinfo->fn_extra = MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
												 sizeof(ArrayMetaState));
		my_extra = (ArrayMetaState *) fcinfo->flinfo->fn_extra;
		my_extra->element_type = InvalidOid;
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

	return construct_md_array(dvalues, ndims, dims, lbs, element_type,
							  typlen, typbyval, typalign);
}
