/*-------------------------------------------------------------------------
 *
 * array_userfuncs.c
 *	  Misc user-visible array support functions
 *
 * Copyright (c) 2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/array_userfuncs.c,v 1.5 2003/07/01 00:04:38 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "utils/array.h"
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
		elog(ERROR, "array_push: cannot determine input data types");
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
		elog(ERROR, "array_push: neither input type is an array");
		PG_RETURN_NULL();		/* keep compiler quiet */
	}

	if (ARR_NDIM(v) == 1)
	{
		lb = ARR_LBOUND(v);
		dimv = ARR_DIMS(v);

		if (arg0_elemid != InvalidOid)
		{
			/* append newelem */
			int	ub = dimv[0] + lb[0] - 1;
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
		elog(ERROR, "only empty and one-dimensional arrays are supported");


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
 *		concatenate two nD arrays to form an (n+1)D array, or
 *		push an (n-1)D array onto the end of an nD array
 *----------------------------------------------------------------------------
 */
Datum
array_cat(PG_FUNCTION_ARGS)
{
	ArrayType  *v1, *v2;
	int		   *dims, *lbs, ndims, ndatabytes, nbytes;
	int		   *dims1, *lbs1, ndims1, ndatabytes1;
	int		   *dims2, *lbs2, ndims2, ndatabytes2;
	char	   *dat1, *dat2;
	Oid			element_type;
	Oid			element_type1;
	Oid			element_type2;
	ArrayType  *result;

	v1 = PG_GETARG_ARRAYTYPE_P(0);
	v2 = PG_GETARG_ARRAYTYPE_P(1);

	/*
	 * We must have one of the following combinations of inputs:
	 * 1) one empty array, and one non-empty array
	 * 2) both arrays empty
	 * 3) two arrays with ndims1 == ndims2
	 * 4) ndims1 == ndims2 - 1
	 * 5) ndims1 == ndims2 + 1
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

	/* the rest fall into combo 2, 3, or 4 */
	if (ndims1 != ndims2 && ndims1 != ndims2 - 1 && ndims1 != ndims2 + 1)
		elog(ERROR, "Cannot concatenate incompatible arrays of %d and "
					"%d dimensions", ndims1, ndims2);

	element_type1 = ARR_ELEMTYPE(v1);
	element_type2 = ARR_ELEMTYPE(v2);

	/* Do we have a matching element types */
	if (element_type1 != element_type2)
		elog(ERROR, "Cannot concatenate incompatible arrays with element "
					"type %u and %u", element_type1, element_type2);

	/* OK, use it */
	element_type = element_type1;

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
		 * resulting array has two element outer array made up of input
		 * argument arrays
		 */
		int		i;

		ndims = ndims1 + 1;
		dims = (int *) palloc(ndims * sizeof(int));
		lbs = (int *) palloc(ndims * sizeof(int));

		dims[0] = 2;	/* outer array made up of two input arrays */
		lbs[0] = 1;		/* start lower bound at 1 */

		for (i = 0; i < ndims1; i++)
		{
			if (dims1[i] != dims2[i] || lbs1[i] != lbs2[i])
				elog(ERROR, "Cannot concatenate arrays with differing dimensions");

			dims[i + 1] = dims1[i];
			lbs[i + 1] = lbs1[i];
		}
	}
	else if (ndims1 == ndims2 - 1)
	{
		/*
		 * resulting array has the second argument as the outer array,
		 * with the first argument appended to the front of the outer
		 * dimension
		 */
		int		i;

		ndims = ndims2;
		dims = dims2;
		lbs = lbs2;

		/* increment number of elements in outer array */
		dims[0] += 1;

		/* make sure the added element matches our existing elements */
		for (i = 0; i < ndims1; i++)
		{
			if (dims1[i] != dims[i + 1] || lbs1[i] != lbs[i + 1])
				elog(ERROR, "Cannot concatenate arrays with differing dimensions");
		}
	}
	else /* (ndims1 == ndims2 + 1) */
	{
		/*
		 * resulting array has the first argument as the outer array,
		 * with the second argument appended to the end of the outer
		 * dimension
		 */
		int		i;

		ndims = ndims1;
		dims = dims1;
		lbs = lbs1;

		/* increment number of elements in outer array */
		dims[0] += 1;

		/* make sure the added element matches our existing elements */
		for (i = 0; i < ndims2; i++)
		{
			if (dims2[i] != dims[i + 1] || lbs2[i] != lbs[i + 1])
				elog(ERROR, "Cannot concatenate arrays with differing dimensions");
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
	Datum	dvalues[1];
	int16	typlen;
	bool	typbyval;
	char	typalign;
	int		dims[MAXDIM];
	int		lbs[MAXDIM];
	int		i;
	ArrayMetaState *my_extra;

	if (element_type == 0)
		elog(ERROR, "Invalid array element type: %u", element_type);
	if (ndims < 1 || ndims > MAXDIM)
		elog(ERROR, "Invalid number of dimensions %d", ndims);

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
