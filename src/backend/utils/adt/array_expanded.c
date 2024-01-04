/*-------------------------------------------------------------------------
 *
 * array_expanded.c
 *	  Basic functions for manipulating expanded arrays.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/array_expanded.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tupmacs.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"


/* "Methods" required for an expanded object */
static Size EA_get_flat_size(ExpandedObjectHeader *eohptr);
static void EA_flatten_into(ExpandedObjectHeader *eohptr,
							void *result, Size allocated_size);

static const ExpandedObjectMethods EA_methods =
{
	EA_get_flat_size,
	EA_flatten_into
};

/* Other local functions */
static void copy_byval_expanded_array(ExpandedArrayHeader *eah,
									  ExpandedArrayHeader *oldeah);


/*
 * expand_array: convert an array Datum into an expanded array
 *
 * The expanded object will be a child of parentcontext.
 *
 * Some callers can provide cache space to avoid repeated lookups of element
 * type data across calls; if so, pass a metacache pointer, making sure that
 * metacache->element_type is initialized to InvalidOid before first call.
 * If no cross-call caching is required, pass NULL for metacache.
 */
Datum
expand_array(Datum arraydatum, MemoryContext parentcontext,
			 ArrayMetaState *metacache)
{
	ArrayType  *array;
	ExpandedArrayHeader *eah;
	MemoryContext objcxt;
	MemoryContext oldcxt;
	ArrayMetaState fakecache;

	/*
	 * Allocate private context for expanded object.  We start by assuming
	 * that the array won't be very large; but if it does grow a lot, don't
	 * constrain aset.c's large-context behavior.
	 */
	objcxt = AllocSetContextCreate(parentcontext,
								   "expanded array",
								   ALLOCSET_START_SMALL_SIZES);

	/* Set up expanded array header */
	eah = (ExpandedArrayHeader *)
		MemoryContextAlloc(objcxt, sizeof(ExpandedArrayHeader));

	EOH_init_header(&eah->hdr, &EA_methods, objcxt);
	eah->ea_magic = EA_MAGIC;

	/* If the source is an expanded array, we may be able to optimize */
	if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(arraydatum)))
	{
		ExpandedArrayHeader *oldeah = (ExpandedArrayHeader *) DatumGetEOHP(arraydatum);

		Assert(oldeah->ea_magic == EA_MAGIC);

		/*
		 * Update caller's cache if provided; we don't need it this time, but
		 * next call might be for a non-expanded source array.  Furthermore,
		 * if the caller didn't provide a cache area, use some local storage
		 * to cache anyway, thereby avoiding a catalog lookup in the case
		 * where we fall through to the flat-copy code path.
		 */
		if (metacache == NULL)
			metacache = &fakecache;
		metacache->element_type = oldeah->element_type;
		metacache->typlen = oldeah->typlen;
		metacache->typbyval = oldeah->typbyval;
		metacache->typalign = oldeah->typalign;

		/*
		 * If element type is pass-by-value and we have a Datum-array
		 * representation, just copy the source's metadata and Datum/isnull
		 * arrays.  The original flat array, if present at all, adds no
		 * additional information so we need not copy it.
		 */
		if (oldeah->typbyval && oldeah->dvalues != NULL)
		{
			copy_byval_expanded_array(eah, oldeah);
			/* return a R/W pointer to the expanded array */
			return EOHPGetRWDatum(&eah->hdr);
		}

		/*
		 * Otherwise, either we have only a flat representation or the
		 * elements are pass-by-reference.  In either case, the best thing
		 * seems to be to copy the source as a flat representation and then
		 * deconstruct that later if necessary.  For the pass-by-ref case, we
		 * could perhaps save some cycles with custom code that generates the
		 * deconstructed representation in parallel with copying the values,
		 * but it would be a lot of extra code for fairly marginal gain.  So,
		 * fall through into the flat-source code path.
		 */
	}

	/*
	 * Detoast and copy source array into private context, as a flat array.
	 *
	 * Note that this coding risks leaking some memory in the private context
	 * if we have to fetch data from a TOAST table; however, experimentation
	 * says that the leak is minimal.  Doing it this way saves a copy step,
	 * which seems worthwhile, especially if the array is large enough to need
	 * external storage.
	 */
	oldcxt = MemoryContextSwitchTo(objcxt);
	array = DatumGetArrayTypePCopy(arraydatum);
	MemoryContextSwitchTo(oldcxt);

	eah->ndims = ARR_NDIM(array);
	/* note these pointers point into the fvalue header! */
	eah->dims = ARR_DIMS(array);
	eah->lbound = ARR_LBOUND(array);

	/* Save array's element-type data for possible use later */
	eah->element_type = ARR_ELEMTYPE(array);
	if (metacache && metacache->element_type == eah->element_type)
	{
		/* We have a valid cache of representational data */
		eah->typlen = metacache->typlen;
		eah->typbyval = metacache->typbyval;
		eah->typalign = metacache->typalign;
	}
	else
	{
		/* No, so look it up */
		get_typlenbyvalalign(eah->element_type,
							 &eah->typlen,
							 &eah->typbyval,
							 &eah->typalign);
		/* Update cache if provided */
		if (metacache)
		{
			metacache->element_type = eah->element_type;
			metacache->typlen = eah->typlen;
			metacache->typbyval = eah->typbyval;
			metacache->typalign = eah->typalign;
		}
	}

	/* we don't make a deconstructed representation now */
	eah->dvalues = NULL;
	eah->dnulls = NULL;
	eah->dvalueslen = 0;
	eah->nelems = 0;
	eah->flat_size = 0;

	/* remember we have a flat representation */
	eah->fvalue = array;
	eah->fstartptr = ARR_DATA_PTR(array);
	eah->fendptr = ((char *) array) + ARR_SIZE(array);

	/* return a R/W pointer to the expanded array */
	return EOHPGetRWDatum(&eah->hdr);
}

/*
 * helper for expand_array(): copy pass-by-value Datum-array representation
 */
static void
copy_byval_expanded_array(ExpandedArrayHeader *eah,
						  ExpandedArrayHeader *oldeah)
{
	MemoryContext objcxt = eah->hdr.eoh_context;
	int			ndims = oldeah->ndims;
	int			dvalueslen = oldeah->dvalueslen;

	/* Copy array dimensionality information */
	eah->ndims = ndims;
	/* We can alloc both dimensionality arrays with one palloc */
	eah->dims = (int *) MemoryContextAlloc(objcxt, ndims * 2 * sizeof(int));
	eah->lbound = eah->dims + ndims;
	/* .. but don't assume the source's arrays are contiguous */
	memcpy(eah->dims, oldeah->dims, ndims * sizeof(int));
	memcpy(eah->lbound, oldeah->lbound, ndims * sizeof(int));

	/* Copy element-type data */
	eah->element_type = oldeah->element_type;
	eah->typlen = oldeah->typlen;
	eah->typbyval = oldeah->typbyval;
	eah->typalign = oldeah->typalign;

	/* Copy the deconstructed representation */
	eah->dvalues = (Datum *) MemoryContextAlloc(objcxt,
												dvalueslen * sizeof(Datum));
	memcpy(eah->dvalues, oldeah->dvalues, dvalueslen * sizeof(Datum));
	if (oldeah->dnulls)
	{
		eah->dnulls = (bool *) MemoryContextAlloc(objcxt,
												  dvalueslen * sizeof(bool));
		memcpy(eah->dnulls, oldeah->dnulls, dvalueslen * sizeof(bool));
	}
	else
		eah->dnulls = NULL;
	eah->dvalueslen = dvalueslen;
	eah->nelems = oldeah->nelems;
	eah->flat_size = oldeah->flat_size;

	/* we don't make a flat representation */
	eah->fvalue = NULL;
	eah->fstartptr = NULL;
	eah->fendptr = NULL;
}

/*
 * get_flat_size method for expanded arrays
 */
static Size
EA_get_flat_size(ExpandedObjectHeader *eohptr)
{
	ExpandedArrayHeader *eah = (ExpandedArrayHeader *) eohptr;
	int			nelems;
	int			ndims;
	Datum	   *dvalues;
	bool	   *dnulls;
	Size		nbytes;
	int			i;

	Assert(eah->ea_magic == EA_MAGIC);

	/* Easy if we have a valid flattened value */
	if (eah->fvalue)
		return ARR_SIZE(eah->fvalue);

	/* If we have a cached size value, believe that */
	if (eah->flat_size)
		return eah->flat_size;

	/*
	 * Compute space needed by examining dvalues/dnulls.  Note that the result
	 * array will have a nulls bitmap if dnulls isn't NULL, even if the array
	 * doesn't actually contain any nulls now.
	 */
	nelems = eah->nelems;
	ndims = eah->ndims;
	Assert(nelems == ArrayGetNItems(ndims, eah->dims));
	dvalues = eah->dvalues;
	dnulls = eah->dnulls;
	nbytes = 0;
	for (i = 0; i < nelems; i++)
	{
		if (dnulls && dnulls[i])
			continue;
		nbytes = att_addlength_datum(nbytes, eah->typlen, dvalues[i]);
		nbytes = att_align_nominal(nbytes, eah->typalign);
		/* check for overflow of total request */
		if (!AllocSizeIsValid(nbytes))
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("array size exceeds the maximum allowed (%d)",
							(int) MaxAllocSize)));
	}

	if (dnulls)
		nbytes += ARR_OVERHEAD_WITHNULLS(ndims, nelems);
	else
		nbytes += ARR_OVERHEAD_NONULLS(ndims);

	/* cache for next time */
	eah->flat_size = nbytes;

	return nbytes;
}

/*
 * flatten_into method for expanded arrays
 */
static void
EA_flatten_into(ExpandedObjectHeader *eohptr,
				void *result, Size allocated_size)
{
	ExpandedArrayHeader *eah = (ExpandedArrayHeader *) eohptr;
	ArrayType  *aresult = (ArrayType *) result;
	int			nelems;
	int			ndims;
	int32		dataoffset;

	Assert(eah->ea_magic == EA_MAGIC);

	/* Easy if we have a valid flattened value */
	if (eah->fvalue)
	{
		Assert(allocated_size == ARR_SIZE(eah->fvalue));
		memcpy(result, eah->fvalue, allocated_size);
		return;
	}

	/* Else allocation should match previous get_flat_size result */
	Assert(allocated_size == eah->flat_size);

	/* Fill result array from dvalues/dnulls */
	nelems = eah->nelems;
	ndims = eah->ndims;

	if (eah->dnulls)
		dataoffset = ARR_OVERHEAD_WITHNULLS(ndims, nelems);
	else
		dataoffset = 0;			/* marker for no null bitmap */

	/* We must ensure that any pad space is zero-filled */
	memset(aresult, 0, allocated_size);

	SET_VARSIZE(aresult, allocated_size);
	aresult->ndim = ndims;
	aresult->dataoffset = dataoffset;
	aresult->elemtype = eah->element_type;
	memcpy(ARR_DIMS(aresult), eah->dims, ndims * sizeof(int));
	memcpy(ARR_LBOUND(aresult), eah->lbound, ndims * sizeof(int));

	CopyArrayEls(aresult,
				 eah->dvalues, eah->dnulls, nelems,
				 eah->typlen, eah->typbyval, eah->typalign,
				 false);
}

/*
 * Argument fetching support code
 */

/*
 * DatumGetExpandedArray: get a writable expanded array from an input argument
 *
 * Caution: if the input is a read/write pointer, this returns the input
 * argument; so callers must be sure that their changes are "safe", that is
 * they cannot leave the array in a corrupt state.
 */
ExpandedArrayHeader *
DatumGetExpandedArray(Datum d)
{
	/* If it's a writable expanded array already, just return it */
	if (VARATT_IS_EXTERNAL_EXPANDED_RW(DatumGetPointer(d)))
	{
		ExpandedArrayHeader *eah = (ExpandedArrayHeader *) DatumGetEOHP(d);

		Assert(eah->ea_magic == EA_MAGIC);
		return eah;
	}

	/* Else expand the hard way */
	d = expand_array(d, CurrentMemoryContext, NULL);
	return (ExpandedArrayHeader *) DatumGetEOHP(d);
}

/*
 * As above, when caller has the ability to cache element type info
 */
ExpandedArrayHeader *
DatumGetExpandedArrayX(Datum d, ArrayMetaState *metacache)
{
	/* If it's a writable expanded array already, just return it */
	if (VARATT_IS_EXTERNAL_EXPANDED_RW(DatumGetPointer(d)))
	{
		ExpandedArrayHeader *eah = (ExpandedArrayHeader *) DatumGetEOHP(d);

		Assert(eah->ea_magic == EA_MAGIC);
		/* Update cache if provided */
		if (metacache)
		{
			metacache->element_type = eah->element_type;
			metacache->typlen = eah->typlen;
			metacache->typbyval = eah->typbyval;
			metacache->typalign = eah->typalign;
		}
		return eah;
	}

	/* Else expand using caller's cache if any */
	d = expand_array(d, CurrentMemoryContext, metacache);
	return (ExpandedArrayHeader *) DatumGetEOHP(d);
}

/*
 * DatumGetAnyArrayP: return either an expanded array or a detoasted varlena
 * array.  The result must not be modified in-place.
 */
AnyArrayType *
DatumGetAnyArrayP(Datum d)
{
	ExpandedArrayHeader *eah;

	/*
	 * If it's an expanded array (RW or RO), return the header pointer.
	 */
	if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(d)))
	{
		eah = (ExpandedArrayHeader *) DatumGetEOHP(d);
		Assert(eah->ea_magic == EA_MAGIC);
		return (AnyArrayType *) eah;
	}

	/* Else do regular detoasting as needed */
	return (AnyArrayType *) PG_DETOAST_DATUM(d);
}

/*
 * Create the Datum/isnull representation of an expanded array object
 * if we didn't do so previously
 */
void
deconstruct_expanded_array(ExpandedArrayHeader *eah)
{
	if (eah->dvalues == NULL)
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(eah->hdr.eoh_context);
		Datum	   *dvalues;
		bool	   *dnulls;
		int			nelems;

		dnulls = NULL;
		deconstruct_array(eah->fvalue,
						  eah->element_type,
						  eah->typlen, eah->typbyval, eah->typalign,
						  &dvalues,
						  ARR_HASNULL(eah->fvalue) ? &dnulls : NULL,
						  &nelems);

		/*
		 * Update header only after successful completion of this step.  If
		 * deconstruct_array fails partway through, worst consequence is some
		 * leaked memory in the object's context.  If the caller fails at a
		 * later point, that's fine, since the deconstructed representation is
		 * valid anyhow.
		 */
		eah->dvalues = dvalues;
		eah->dnulls = dnulls;
		eah->dvalueslen = eah->nelems = nelems;
		MemoryContextSwitchTo(oldcxt);
	}
}
