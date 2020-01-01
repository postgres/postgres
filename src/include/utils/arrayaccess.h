/*-------------------------------------------------------------------------
 *
 * arrayaccess.h
 *	  Declarations for element-by-element access to Postgres arrays.
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/arrayaccess.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ARRAYACCESS_H
#define ARRAYACCESS_H

#include "access/tupmacs.h"
#include "utils/array.h"


/*
 * Functions for iterating through elements of a flat or expanded array.
 * These require a state struct "array_iter iter".
 *
 * Use "array_iter_setup(&iter, arrayptr);" to prepare to iterate, and
 * "datumvar = array_iter_next(&iter, &isnullvar, index, ...);" to fetch
 * the next element into datumvar/isnullvar.
 * "index" must be the zero-origin element number; we make caller provide
 * this since caller is generally counting the elements anyway.  Despite
 * that, these functions can only fetch elements sequentially.
 */

typedef struct array_iter
{
	/* datumptr being NULL or not tells if we have flat or expanded array */

	/* Fields used when we have an expanded array */
	Datum	   *datumptr;		/* Pointer to Datum array */
	bool	   *isnullptr;		/* Pointer to isnull array */

	/* Fields used when we have a flat array */
	char	   *dataptr;		/* Current spot in the data area */
	bits8	   *bitmapptr;		/* Current byte of the nulls bitmap, or NULL */
	int			bitmask;		/* mask for current bit in nulls bitmap */
} array_iter;


static inline void
array_iter_setup(array_iter *it, AnyArrayType *a)
{
	if (VARATT_IS_EXPANDED_HEADER(a))
	{
		if (a->xpn.dvalues)
		{
			it->datumptr = a->xpn.dvalues;
			it->isnullptr = a->xpn.dnulls;
			/* we must fill all fields to prevent compiler warnings */
			it->dataptr = NULL;
			it->bitmapptr = NULL;
		}
		else
		{
			/* Work with flat array embedded in the expanded datum */
			it->datumptr = NULL;
			it->isnullptr = NULL;
			it->dataptr = ARR_DATA_PTR(a->xpn.fvalue);
			it->bitmapptr = ARR_NULLBITMAP(a->xpn.fvalue);
		}
	}
	else
	{
		it->datumptr = NULL;
		it->isnullptr = NULL;
		it->dataptr = ARR_DATA_PTR((ArrayType *) a);
		it->bitmapptr = ARR_NULLBITMAP((ArrayType *) a);
	}
	it->bitmask = 1;
}

static inline Datum
array_iter_next(array_iter *it, bool *isnull, int i,
				int elmlen, bool elmbyval, char elmalign)
{
	Datum		ret;

	if (it->datumptr)
	{
		ret = it->datumptr[i];
		*isnull = it->isnullptr ? it->isnullptr[i] : false;
	}
	else
	{
		if (it->bitmapptr && (*(it->bitmapptr) & it->bitmask) == 0)
		{
			*isnull = true;
			ret = (Datum) 0;
		}
		else
		{
			*isnull = false;
			ret = fetch_att(it->dataptr, elmbyval, elmlen);
			it->dataptr = att_addlength_pointer(it->dataptr, elmlen,
												it->dataptr);
			it->dataptr = (char *) att_align_nominal(it->dataptr, elmalign);
		}
		it->bitmask <<= 1;
		if (it->bitmask == 0x100)
		{
			if (it->bitmapptr)
				it->bitmapptr++;
			it->bitmask = 1;
		}
	}

	return ret;
}

#endif							/* ARRAYACCESS_H */
