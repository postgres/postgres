/*-------------------------------------------------------------------------
 *
 * array_userfuncs.c
 *	  Misc user-visible array support functions
 *
 * Copyright (c) 2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/array_userfuncs.c,v 1.3 2003/06/25 21:30:32 momjian Exp $
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
 * singleton_array :
 *		Form a multi-dimensional array given one starting element.
 *
 * - first argument is the datum with which to build the array
 * - second argument is the number of dimensions the array should have;
 *     defaults to 1 if no second argument is provided
 *----------------------------------------------------------------------------
 */
Datum
singleton_array(PG_FUNCTION_ARGS)
{
	Oid			elem_type = get_fn_expr_argtype(fcinfo, 0);
	int			ndims;

	if (elem_type == InvalidOid)
		elog(ERROR, "Cannot determine input datatype");

	if (PG_NARGS() == 2)
		ndims = PG_GETARG_INT32(1);
	else
		ndims = 1;

	PG_RETURN_ARRAYTYPE_P(create_singleton_array(elem_type,
												 PG_GETARG_DATUM(0),
												 ndims));
}

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
	Oid			arg0_typeid = get_fn_expr_argtype(fcinfo, 0);
	Oid			arg1_typeid = get_fn_expr_argtype(fcinfo, 1);
	Oid			arg0_elemid;
	Oid			arg1_elemid;

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

	/* Sanity check: do we have a one-dimensional array */
	if (ARR_NDIM(v) != 1)
		elog(ERROR, "Arrays greater than one-dimension are not supported");

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

	get_typlenbyvalalign(element_type, &typlen, &typbyval, &typalign);

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
	 * 1) two arrays with ndims1 == ndims2
	 * 2) ndims1 == ndims2 - 1
	 * 3) ndims1 == ndims2 + 1
	 */
	ndims1 = ARR_NDIM(v1);
	ndims2 = ARR_NDIM(v2);

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

/*----------------------------------------------------------------------------
 * array_accum :
 *		accumulator to build a 1-D array from input values -- this can be used
 *		to create custom aggregates.
 *
 * This function is not marked strict, so we have to be careful about nulls.
 *----------------------------------------------------------------------------
 */
Datum
array_accum(PG_FUNCTION_ARGS)
{
	/* return NULL if both arguments are NULL */
	if (PG_ARGISNULL(0) && PG_ARGISNULL(1))
		PG_RETURN_NULL();

	/* create a new 1-D array from the new element if the array is NULL */
	if (PG_ARGISNULL(0))
	{
		Oid			tgt_type = get_fn_expr_rettype(fcinfo);
		Oid			tgt_elem_type;

		if (tgt_type == InvalidOid)
			elog(ERROR, "Cannot determine target array type");
		tgt_elem_type = get_element_type(tgt_type);
		if (tgt_elem_type == InvalidOid)
			elog(ERROR, "Target type is not an array");

		PG_RETURN_ARRAYTYPE_P(create_singleton_array(tgt_elem_type,
													 PG_GETARG_DATUM(1),
													 1));
	}

	/* return the array if the new element is NULL */
	if (PG_ARGISNULL(1))
		PG_RETURN_ARRAYTYPE_P(PG_GETARG_ARRAYTYPE_P_COPY(0));

	/*
	 * Otherwise this is equivalent to array_push.  We hack the call a little
	 * so that array_push can see the fn_expr information.
	 */
	return array_push(fcinfo);
}

/*-----------------------------------------------------------------------------
 * array_assign :
 *		assign an element of an array to a new value and return the
 *		redefined array
 *----------------------------------------------------------------------------
 */
Datum
array_assign(PG_FUNCTION_ARGS)
{
	ArrayType  *v;
	int			idx_to_chg;
	Datum		newelem;
	int		   *dimv,
			   *lb, ub;
	ArrayType  *result;
	bool		isNull;
	Oid			element_type;
	int16		typlen;
	bool		typbyval;
	char		typalign;

	v = PG_GETARG_ARRAYTYPE_P(0);
	idx_to_chg = PG_GETARG_INT32(1);
	newelem = PG_GETARG_DATUM(2);

	/* Sanity check: do we have a one-dimensional array */
	if (ARR_NDIM(v) != 1)
		elog(ERROR, "Arrays greater than one-dimension are not supported");

	lb = ARR_LBOUND(v);
	dimv = ARR_DIMS(v);
	ub = dimv[0] + lb[0] - 1;
	if (idx_to_chg < lb[0] || idx_to_chg > ub)
		elog(ERROR, "Cannot alter nonexistent array element: %d", idx_to_chg);

	element_type = ARR_ELEMTYPE(v);
	/* Sanity check: do we have a non-zero element type */
	if (element_type == 0)
		elog(ERROR, "Invalid array element type: %u", element_type);

	get_typlenbyvalalign(element_type, &typlen, &typbyval, &typalign);

	result = array_set(v, 1, &idx_to_chg, newelem, -1,
					   typlen, typbyval, typalign, &isNull);

	PG_RETURN_ARRAYTYPE_P(result);
}

/*-----------------------------------------------------------------------------
 * array_subscript :
 *		return specific element of an array
 *----------------------------------------------------------------------------
 */
Datum
array_subscript(PG_FUNCTION_ARGS)
{
	ArrayType  *v;
	int			idx;
	int		   *dimv,
			   *lb, ub;
	Datum		result;
	bool		isNull;
	Oid			element_type;
	int16		typlen;
	bool		typbyval;
	char		typalign;

	v = PG_GETARG_ARRAYTYPE_P(0);
	idx = PG_GETARG_INT32(1);

	/* Sanity check: do we have a one-dimensional array */
	if (ARR_NDIM(v) != 1)
		elog(ERROR, "Arrays greater than one-dimension are not supported");

	lb = ARR_LBOUND(v);
	dimv = ARR_DIMS(v);
	ub = dimv[0] + lb[0] - 1;
	if (idx < lb[0] || idx > ub)
		elog(ERROR, "Cannot return nonexistent array element: %d", idx);

	element_type = ARR_ELEMTYPE(v);
	/* Sanity check: do we have a non-zero element type */
	if (element_type == 0)
		elog(ERROR, "Invalid array element type: %u", element_type);

	get_typlenbyvalalign(element_type, &typlen, &typbyval, &typalign);

	result = array_ref(v, 1, &idx, -1, typlen, typbyval, typalign, &isNull);

	PG_RETURN_DATUM(result);
}

/*
 * actually does the work for singleton_array(), and array_accum() if it is
 * given a null input array.
 */
ArrayType *
create_singleton_array(Oid element_type, Datum element, int ndims)
{
	Datum	dvalues[1];
	int16	typlen;
	bool	typbyval;
	char	typalign;
	int		dims[MAXDIM];
	int		lbs[MAXDIM];
	int		i;

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

	get_typlenbyvalalign(element_type, &typlen, &typbyval, &typalign);

	return construct_md_array(dvalues, ndims, dims, lbs, element_type,
							  typlen, typbyval, typalign);
}
