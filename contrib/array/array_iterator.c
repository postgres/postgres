/*
 * array_iterator.c --
 *
 * This file defines a new class of operators which take an
 * array and a scalar value, iterate a scalar operator over the
 * elements of the array and the value and compute a result as
 * the logical OR or AND of the iteration results.
 *
 * Copyright (C) 1999, Massimo Dal Zotto <dz@cs.unitn.it>
 * ported to postgreSQL 6.3.2,added oid_functions, 18.1.1999,
 * Tobias Gabele <gabele@wiz.uni-kassel.de>
 *
 * This software is distributed under the GNU General Public License
 * either version 2, or (at your option) any later version.
 */

#include "postgres.h"

#include <ctype.h>
#include <stdio.h>
#include <sys/types.h>
#include <string.h>

#include "access/tupmacs.h"
#include "access/xact.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/lsyscache.h"

#include "array_iterator.h"


static int32
array_iterator(Oid proc, int and, ArrayType *array, Datum value)
{
	Oid			elemtype;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	int			nitems,
				i;
	Datum		result;
	int			ndim,
			   *dim;
	char	   *p;
	FmgrInfo	finfo;

	/* Sanity checks */
	if (array == (ArrayType *) NULL)
	{
		/* elog(WARNING, "array_iterator: array is null"); */
		return (0);
	}

	/* detoast input if necessary */
	array = DatumGetArrayTypeP(PointerGetDatum(array));

	ndim = ARR_NDIM(array);
	dim = ARR_DIMS(array);
	nitems = ArrayGetNItems(ndim, dim);
	if (nitems == 0)
		return (0);

	/* Lookup element type information */
	elemtype = ARR_ELEMTYPE(array);
	get_typlenbyvalalign(elemtype, &typlen, &typbyval, &typalign);

	/* Lookup the function entry point */
	fmgr_info(proc, &finfo);
	if (finfo.fn_nargs != 2)
	{
		elog(ERROR, "array_iterator: proc %u does not take 2 args", proc);
		return (0);
	}

	/* Scan the array and apply the operator to each element */
	result = BoolGetDatum(false);
	p = ARR_DATA_PTR(array);
	for (i = 0; i < nitems; i++)
	{
		Datum		itemvalue;

		itemvalue = fetch_att(p, typbyval, typlen);

		p = att_addlength(p, typlen, PointerGetDatum(p));
		p = (char *) att_align(p, typalign);

		result = FunctionCall2(&finfo, itemvalue, value);

		if (DatumGetBool(result))
		{
			if (!and)
				return (1);
		}
		else
		{
			if (and)
				return (0);
		}
	}

	if (and && DatumGetBool(result))
		return (1);
	else
		return (0);
}

/*
 * Iterator functions for type _text
 */

int32
array_texteq(ArrayType *array, void *value)
{
	return array_iterator(F_TEXTEQ,
						  0,	/* logical or */
						  array, (Datum) value);
}

int32
array_all_texteq(ArrayType *array, void *value)
{
	return array_iterator(F_TEXTEQ,
						  1,	/* logical and */
						  array, (Datum) value);
}

int32
array_textregexeq(ArrayType *array, void *value)
{
	return array_iterator(F_TEXTREGEXEQ,
						  0,	/* logical or */
						  array, (Datum) value);
}

int32
array_all_textregexeq(ArrayType *array, void *value)
{
	return array_iterator(F_TEXTREGEXEQ,
						  1,	/* logical and */
						  array, (Datum) value);
}

/*
 * Iterator functions for type _bpchar. Note that the regexp
 * operators take the second argument of type text.
 */

int32
array_bpchareq(ArrayType *array, void *value)
{
	return array_iterator(F_BPCHAREQ,
						  0,	/* logical or */
						  array, (Datum) value);
}

int32
array_all_bpchareq(ArrayType *array, void *value)
{
	return array_iterator(F_BPCHAREQ,
						  1,	/* logical and */
						  array, (Datum) value);
}

int32
array_bpcharregexeq(ArrayType *array, void *value)
{
	return array_iterator(F_TEXTREGEXEQ,
						  0,	/* logical or */
						  array, (Datum) value);
}

int32
array_all_bpcharregexeq(ArrayType *array, void *value)
{
	return array_iterator(F_TEXTREGEXEQ,
						  1,	/* logical and */
						  array, (Datum) value);
}

/*
 * Iterator functions for type _int4
 */

int32
array_int4eq(ArrayType *array, int4 value)
{
	return array_iterator(F_INT4EQ,
						  0,	/* logical or */
						  array, (Datum) value);
}

int32
array_all_int4eq(ArrayType *array, int4 value)
{
	return array_iterator(F_INT4EQ,
						  1,	/* logical and */
						  array, (Datum) value);
}

int32
array_int4ne(ArrayType *array, int4 value)
{
	return array_iterator(F_INT4NE,
						  0,	/* logical or */
						  array, (Datum) value);
}

int32
array_all_int4ne(ArrayType *array, int4 value)
{
	return array_iterator(F_INT4NE,
						  1,	/* logical and */
						  array, (Datum) value);
}

int32
array_int4gt(ArrayType *array, int4 value)
{
	return array_iterator(F_INT4GT,
						  0,	/* logical or */
						  array, (Datum) value);
}

int32
array_all_int4gt(ArrayType *array, int4 value)
{
	return array_iterator(F_INT4GT,
						  1,	/* logical and */
						  array, (Datum) value);
}

int32
array_int4ge(ArrayType *array, int4 value)
{
	return array_iterator(F_INT4GE,
						  0,	/* logical or */
						  array, (Datum) value);
}

int32
array_all_int4ge(ArrayType *array, int4 value)
{
	return array_iterator(F_INT4GE,
						  1,	/* logical and */
						  array, (Datum) value);
}

int32
array_int4lt(ArrayType *array, int4 value)
{
	return array_iterator(F_INT4LT,
						  0,	/* logical or */
						  array, (Datum) value);
}

int32
array_all_int4lt(ArrayType *array, int4 value)
{
	return array_iterator(F_INT4LT,
						  1,	/* logical and */
						  array, (Datum) value);
}

int32
array_int4le(ArrayType *array, int4 value)
{
	return array_iterator(F_INT4LE,
						  0,	/* logical or */
						  array, (Datum) value);
}

int32
array_all_int4le(ArrayType *array, int4 value)
{
	return array_iterator(F_INT4LE,
						  1,	/* logical and */
						  array, (Datum) value);
}

/* new tobias gabele 1999 */

int32
array_oideq(ArrayType *array, Oid value)
{
	return array_iterator(F_OIDEQ,
						  0,	/* logical or */
						  array, (Datum) value);
}

int32
array_all_oidne(ArrayType *array, Oid value)
{
	return array_iterator(F_OIDNE,
						  1,	/* logical and */
						  array, (Datum) value);
}

int32
array_ineteq(ArrayType *array, void *value)
{
	return array_iterator(F_NETWORK_EQ,
						  0,	/* logical or */
						  array, (Datum) value);
}

int32
array_all_ineteq(ArrayType *array, void *value)
{
	return array_iterator(F_NETWORK_EQ,
						  1,	/* logical and */
						  array, (Datum) value);
}

int32
array_inetne(ArrayType *array, void *value)
{
	return array_iterator(F_NETWORK_NE,
						  0,	/* logical and */
						  array, (Datum) value);
}

int32
array_all_inetne(ArrayType *array, void *value)
{
	return array_iterator(F_NETWORK_NE,
						  1,	/* logical and */
						  array, (Datum) value);
}
