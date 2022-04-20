/*-------------------------------------------------------------------------
 *
 * expandeddatum.c
 *	  Support functions for "expanded" value representations.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/expandeddatum.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/expandeddatum.h"
#include "utils/memutils.h"

/*
 * DatumGetEOHP
 *
 * Given a Datum that is an expanded-object reference, extract the pointer.
 *
 * This is a bit tedious since the pointer may not be properly aligned;
 * compare VARATT_EXTERNAL_GET_POINTER().
 */
ExpandedObjectHeader *
DatumGetEOHP(Datum d)
{
	varattrib_1b_e *datum = (varattrib_1b_e *) DatumGetPointer(d);
	varatt_expanded ptr;

	Assert(VARATT_IS_EXTERNAL_EXPANDED(datum));
	memcpy(&ptr, VARDATA_EXTERNAL(datum), sizeof(ptr));
	Assert(VARATT_IS_EXPANDED_HEADER(ptr.eohptr));
	return ptr.eohptr;
}

/*
 * EOH_init_header
 *
 * Initialize the common header of an expanded object.
 *
 * The main thing this encapsulates is initializing the TOAST pointers.
 */
void
EOH_init_header(ExpandedObjectHeader *eohptr,
				const ExpandedObjectMethods *methods,
				MemoryContext obj_context)
{
	varatt_expanded ptr;

	eohptr->vl_len_ = EOH_HEADER_MAGIC;
	eohptr->eoh_methods = methods;
	eohptr->eoh_context = obj_context;

	ptr.eohptr = eohptr;

	SET_VARTAG_EXTERNAL(eohptr->eoh_rw_ptr, VARTAG_EXPANDED_RW);
	memcpy(VARDATA_EXTERNAL(eohptr->eoh_rw_ptr), &ptr, sizeof(ptr));

	SET_VARTAG_EXTERNAL(eohptr->eoh_ro_ptr, VARTAG_EXPANDED_RO);
	memcpy(VARDATA_EXTERNAL(eohptr->eoh_ro_ptr), &ptr, sizeof(ptr));
}

/*
 * EOH_get_flat_size
 * EOH_flatten_into
 *
 * Convenience functions for invoking the "methods" of an expanded object.
 */

Size
EOH_get_flat_size(ExpandedObjectHeader *eohptr)
{
	return eohptr->eoh_methods->get_flat_size(eohptr);
}

void
EOH_flatten_into(ExpandedObjectHeader *eohptr,
				 void *result, Size allocated_size)
{
	eohptr->eoh_methods->flatten_into(eohptr, result, allocated_size);
}

/*
 * If the Datum represents a R/W expanded object, change it to R/O.
 * Otherwise return the original Datum.
 *
 * Caller must ensure that the datum is a non-null varlena value.  Typically
 * this is invoked via MakeExpandedObjectReadOnly(), which checks that.
 */
Datum
MakeExpandedObjectReadOnlyInternal(Datum d)
{
	ExpandedObjectHeader *eohptr;

	/* Nothing to do if not a read-write expanded-object pointer */
	if (!VARATT_IS_EXTERNAL_EXPANDED_RW(DatumGetPointer(d)))
		return d;

	/* Now safe to extract the object pointer */
	eohptr = DatumGetEOHP(d);

	/* Return the built-in read-only pointer instead of given pointer */
	return EOHPGetRODatum(eohptr);
}

/*
 * Transfer ownership of an expanded object to a new parent memory context.
 * The object must be referenced by a R/W pointer, and what we return is
 * always its "standard" R/W pointer, which is certain to have the same
 * lifespan as the object itself.  (The passed-in pointer might not, and
 * in any case wouldn't provide a unique identifier if it's not that one.)
 */
Datum
TransferExpandedObject(Datum d, MemoryContext new_parent)
{
	ExpandedObjectHeader *eohptr = DatumGetEOHP(d);

	/* Assert caller gave a R/W pointer */
	Assert(VARATT_IS_EXTERNAL_EXPANDED_RW(DatumGetPointer(d)));

	/* Transfer ownership */
	MemoryContextSetParent(eohptr->eoh_context, new_parent);

	/* Return the object's standard read-write pointer */
	return EOHPGetRWDatum(eohptr);
}

/*
 * Delete an expanded object (must be referenced by a R/W pointer).
 */
void
DeleteExpandedObject(Datum d)
{
	ExpandedObjectHeader *eohptr = DatumGetEOHP(d);

	/* Assert caller gave a R/W pointer */
	Assert(VARATT_IS_EXTERNAL_EXPANDED_RW(DatumGetPointer(d)));

	/* Kill it */
	MemoryContextDelete(eohptr->eoh_context);
}
