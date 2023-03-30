/*-------------------------------------------------------------------------
 *
 * execTuples.c
 *	  Routines dealing with TupleTableSlots.  These are used for resource
 *	  management associated with tuples (eg, releasing buffer pins for
 *	  tuples in disk buffers, or freeing the memory occupied by transient
 *	  tuples).  Slots also provide access abstraction that lets us implement
 *	  "virtual" tuples to reduce data-copying overhead.
 *
 *	  Routines dealing with the type information for tuples. Currently,
 *	  the type information for a tuple is an array of FormData_pg_attribute.
 *	  This information is needed by routines manipulating tuples
 *	  (getattribute, formtuple, etc.).
 *
 *
 *	 EXAMPLE OF HOW TABLE ROUTINES WORK
 *		Suppose we have a query such as SELECT emp.name FROM emp and we have
 *		a single SeqScan node in the query plan.
 *
 *		At ExecutorStart()
 *		----------------
 *
 *		- ExecInitSeqScan() calls ExecInitScanTupleSlot() to construct a
 *		  TupleTableSlots for the tuples returned by the access method, and
 *		  ExecInitResultTypeTL() to define the node's return
 *		  type. ExecAssignScanProjectionInfo() will, if necessary, create
 *		  another TupleTableSlot for the tuples resulting from performing
 *		  target list projections.
 *
 *		During ExecutorRun()
 *		----------------
 *		- SeqNext() calls ExecStoreBufferHeapTuple() to place the tuple
 *		  returned by the access method into the scan tuple slot.
 *
 *		- ExecSeqScan() (via ExecScan), if necessary, calls ExecProject(),
 *		  putting the result of the projection in the result tuple slot. If
 *		  not necessary, it directly returns the slot returned by SeqNext().
 *
 *		- ExecutePlan() calls the output function.
 *
 *		The important thing to watch in the executor code is how pointers
 *		to the slots containing tuples are passed instead of the tuples
 *		themselves.  This facilitates the communication of related information
 *		(such as whether or not a tuple should be pfreed, what buffer contains
 *		this tuple, the tuple's tuple descriptor, etc).  It also allows us
 *		to avoid physically constructing projection tuples in many cases.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/execTuples.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heaptoast.h"
#include "access/htup_details.h"
#include "access/tupdesc_details.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "nodes/nodeFuncs.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"
#include "utils/expandeddatum.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"

static TupleDesc ExecTypeFromTLInternal(List *targetList,
										bool skipjunk);
static pg_attribute_always_inline void slot_deform_heap_tuple(TupleTableSlot *slot, HeapTuple tuple, uint32 *offp,
															  int natts);
static inline void tts_buffer_heap_store_tuple(TupleTableSlot *slot,
											   HeapTuple tuple,
											   Buffer buffer,
											   bool transfer_pin);
static void tts_heap_store_tuple(TupleTableSlot *slot, HeapTuple tuple, bool shouldFree);


const TupleTableSlotOps TTSOpsVirtual;
const TupleTableSlotOps TTSOpsHeapTuple;
const TupleTableSlotOps TTSOpsMinimalTuple;
const TupleTableSlotOps TTSOpsBufferHeapTuple;


/*
 * TupleTableSlotOps implementations.
 */

/*
 * TupleTableSlotOps implementation for VirtualTupleTableSlot.
 */
static void
tts_virtual_init(TupleTableSlot *slot)
{
}

static void
tts_virtual_release(TupleTableSlot *slot)
{
}

static void
tts_virtual_clear(TupleTableSlot *slot)
{
	if (unlikely(TTS_SHOULDFREE(slot)))
	{
		VirtualTupleTableSlot *vslot = (VirtualTupleTableSlot *) slot;

		pfree(vslot->data);
		vslot->data = NULL;

		slot->tts_flags &= ~TTS_FLAG_SHOULDFREE;
	}

	slot->tts_nvalid = 0;
	slot->tts_flags |= TTS_FLAG_EMPTY;
	ItemPointerSetInvalid(&slot->tts_tid);
}

/*
 * VirtualTupleTableSlots always have fully populated tts_values and
 * tts_isnull arrays.  So this function should never be called.
 */
static void
tts_virtual_getsomeattrs(TupleTableSlot *slot, int natts)
{
	elog(ERROR, "getsomeattrs is not required to be called on a virtual tuple table slot");
}

/*
 * VirtualTupleTableSlots never provide system attributes (except those
 * handled generically, such as tableoid).  We generally shouldn't get
 * here, but provide a user-friendly message if we do.
 */
static Datum
tts_virtual_getsysattr(TupleTableSlot *slot, int attnum, bool *isnull)
{
	Assert(!TTS_EMPTY(slot));

	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot retrieve a system column in this context")));

	return 0;					/* silence compiler warnings */
}

/*
 * To materialize a virtual slot all the datums that aren't passed by value
 * have to be copied into the slot's memory context.  To do so, compute the
 * required size, and allocate enough memory to store all attributes.  That's
 * good for cache hit ratio, but more importantly requires only memory
 * allocation/deallocation.
 */
static void
tts_virtual_materialize(TupleTableSlot *slot)
{
	VirtualTupleTableSlot *vslot = (VirtualTupleTableSlot *) slot;
	TupleDesc	desc = slot->tts_tupleDescriptor;
	Size		sz = 0;
	char	   *data;

	/* already materialized */
	if (TTS_SHOULDFREE(slot))
		return;

	/* compute size of memory required */
	for (int natt = 0; natt < desc->natts; natt++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, natt);
		Datum		val;

		if (att->attbyval || slot->tts_isnull[natt])
			continue;

		val = slot->tts_values[natt];

		if (att->attlen == -1 &&
			VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(val)))
		{
			/*
			 * We want to flatten the expanded value so that the materialized
			 * slot doesn't depend on it.
			 */
			sz = att_align_nominal(sz, att->attalign);
			sz += EOH_get_flat_size(DatumGetEOHP(val));
		}
		else
		{
			sz = att_align_nominal(sz, att->attalign);
			sz = att_addlength_datum(sz, att->attlen, val);
		}
	}

	/* all data is byval */
	if (sz == 0)
		return;

	/* allocate memory */
	vslot->data = data = MemoryContextAlloc(slot->tts_mcxt, sz);
	slot->tts_flags |= TTS_FLAG_SHOULDFREE;

	/* and copy all attributes into the pre-allocated space */
	for (int natt = 0; natt < desc->natts; natt++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, natt);
		Datum		val;

		if (att->attbyval || slot->tts_isnull[natt])
			continue;

		val = slot->tts_values[natt];

		if (att->attlen == -1 &&
			VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(val)))
		{
			Size		data_length;

			/*
			 * We want to flatten the expanded value so that the materialized
			 * slot doesn't depend on it.
			 */
			ExpandedObjectHeader *eoh = DatumGetEOHP(val);

			data = (char *) att_align_nominal(data,
											  att->attalign);
			data_length = EOH_get_flat_size(eoh);
			EOH_flatten_into(eoh, data, data_length);

			slot->tts_values[natt] = PointerGetDatum(data);
			data += data_length;
		}
		else
		{
			Size		data_length = 0;

			data = (char *) att_align_nominal(data, att->attalign);
			data_length = att_addlength_datum(data_length, att->attlen, val);

			memcpy(data, DatumGetPointer(val), data_length);

			slot->tts_values[natt] = PointerGetDatum(data);
			data += data_length;
		}
	}
}

static void
tts_virtual_copyslot(TupleTableSlot *dstslot, TupleTableSlot *srcslot)
{
	TupleDesc	srcdesc = srcslot->tts_tupleDescriptor;

	Assert(srcdesc->natts <= dstslot->tts_tupleDescriptor->natts);

	tts_virtual_clear(dstslot);

	slot_getallattrs(srcslot);

	for (int natt = 0; natt < srcdesc->natts; natt++)
	{
		dstslot->tts_values[natt] = srcslot->tts_values[natt];
		dstslot->tts_isnull[natt] = srcslot->tts_isnull[natt];
	}

	dstslot->tts_nvalid = srcdesc->natts;
	dstslot->tts_flags &= ~TTS_FLAG_EMPTY;

	/* make sure storage doesn't depend on external memory */
	tts_virtual_materialize(dstslot);
}

static HeapTuple
tts_virtual_copy_heap_tuple(TupleTableSlot *slot)
{
	Assert(!TTS_EMPTY(slot));

	return heap_form_tuple(slot->tts_tupleDescriptor,
						   slot->tts_values,
						   slot->tts_isnull);
}

static MinimalTuple
tts_virtual_copy_minimal_tuple(TupleTableSlot *slot)
{
	Assert(!TTS_EMPTY(slot));

	return heap_form_minimal_tuple(slot->tts_tupleDescriptor,
								   slot->tts_values,
								   slot->tts_isnull);
}


/*
 * TupleTableSlotOps implementation for HeapTupleTableSlot.
 */

static void
tts_heap_init(TupleTableSlot *slot)
{
}

static void
tts_heap_release(TupleTableSlot *slot)
{
}

static void
tts_heap_clear(TupleTableSlot *slot)
{
	HeapTupleTableSlot *hslot = (HeapTupleTableSlot *) slot;

	/* Free the memory for the heap tuple if it's allowed. */
	if (TTS_SHOULDFREE(slot))
	{
		heap_freetuple(hslot->tuple);
		slot->tts_flags &= ~TTS_FLAG_SHOULDFREE;
	}

	slot->tts_nvalid = 0;
	slot->tts_flags |= TTS_FLAG_EMPTY;
	ItemPointerSetInvalid(&slot->tts_tid);
	hslot->off = 0;
	hslot->tuple = NULL;
}

static void
tts_heap_getsomeattrs(TupleTableSlot *slot, int natts)
{
	HeapTupleTableSlot *hslot = (HeapTupleTableSlot *) slot;

	Assert(!TTS_EMPTY(slot));

	slot_deform_heap_tuple(slot, hslot->tuple, &hslot->off, natts);
}

static Datum
tts_heap_getsysattr(TupleTableSlot *slot, int attnum, bool *isnull)
{
	HeapTupleTableSlot *hslot = (HeapTupleTableSlot *) slot;

	Assert(!TTS_EMPTY(slot));

	/*
	 * In some code paths it's possible to get here with a non-materialized
	 * slot, in which case we can't retrieve system columns.
	 */
	if (!hslot->tuple)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot retrieve a system column in this context")));

	return heap_getsysattr(hslot->tuple, attnum,
						   slot->tts_tupleDescriptor, isnull);
}

static void
tts_heap_materialize(TupleTableSlot *slot)
{
	HeapTupleTableSlot *hslot = (HeapTupleTableSlot *) slot;
	MemoryContext oldContext;

	Assert(!TTS_EMPTY(slot));

	/* If slot has its tuple already materialized, nothing to do. */
	if (TTS_SHOULDFREE(slot))
		return;

	oldContext = MemoryContextSwitchTo(slot->tts_mcxt);

	/*
	 * Have to deform from scratch, otherwise tts_values[] entries could point
	 * into the non-materialized tuple (which might be gone when accessed).
	 */
	slot->tts_nvalid = 0;
	hslot->off = 0;

	if (!hslot->tuple)
		hslot->tuple = heap_form_tuple(slot->tts_tupleDescriptor,
									   slot->tts_values,
									   slot->tts_isnull);
	else
	{
		/*
		 * The tuple contained in this slot is not allocated in the memory
		 * context of the given slot (else it would have TTS_FLAG_SHOULDFREE
		 * set).  Copy the tuple into the given slot's memory context.
		 */
		hslot->tuple = heap_copytuple(hslot->tuple);
	}

	slot->tts_flags |= TTS_FLAG_SHOULDFREE;

	MemoryContextSwitchTo(oldContext);
}

static void
tts_heap_copyslot(TupleTableSlot *dstslot, TupleTableSlot *srcslot)
{
	HeapTuple	tuple;
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(dstslot->tts_mcxt);
	tuple = ExecCopySlotHeapTuple(srcslot);
	MemoryContextSwitchTo(oldcontext);

	ExecStoreHeapTuple(tuple, dstslot, true);
}

static HeapTuple
tts_heap_get_heap_tuple(TupleTableSlot *slot)
{
	HeapTupleTableSlot *hslot = (HeapTupleTableSlot *) slot;

	Assert(!TTS_EMPTY(slot));
	if (!hslot->tuple)
		tts_heap_materialize(slot);

	return hslot->tuple;
}

static HeapTuple
tts_heap_copy_heap_tuple(TupleTableSlot *slot)
{
	HeapTupleTableSlot *hslot = (HeapTupleTableSlot *) slot;

	Assert(!TTS_EMPTY(slot));
	if (!hslot->tuple)
		tts_heap_materialize(slot);

	return heap_copytuple(hslot->tuple);
}

static MinimalTuple
tts_heap_copy_minimal_tuple(TupleTableSlot *slot)
{
	HeapTupleTableSlot *hslot = (HeapTupleTableSlot *) slot;

	if (!hslot->tuple)
		tts_heap_materialize(slot);

	return minimal_tuple_from_heap_tuple(hslot->tuple);
}

static void
tts_heap_store_tuple(TupleTableSlot *slot, HeapTuple tuple, bool shouldFree)
{
	HeapTupleTableSlot *hslot = (HeapTupleTableSlot *) slot;

	tts_heap_clear(slot);

	slot->tts_nvalid = 0;
	hslot->tuple = tuple;
	hslot->off = 0;
	slot->tts_flags &= ~(TTS_FLAG_EMPTY | TTS_FLAG_SHOULDFREE);
	slot->tts_tid = tuple->t_self;

	if (shouldFree)
		slot->tts_flags |= TTS_FLAG_SHOULDFREE;
}


/*
 * TupleTableSlotOps implementation for MinimalTupleTableSlot.
 */

static void
tts_minimal_init(TupleTableSlot *slot)
{
	MinimalTupleTableSlot *mslot = (MinimalTupleTableSlot *) slot;

	/*
	 * Initialize the heap tuple pointer to access attributes of the minimal
	 * tuple contained in the slot as if its a heap tuple.
	 */
	mslot->tuple = &mslot->minhdr;
}

static void
tts_minimal_release(TupleTableSlot *slot)
{
}

static void
tts_minimal_clear(TupleTableSlot *slot)
{
	MinimalTupleTableSlot *mslot = (MinimalTupleTableSlot *) slot;

	if (TTS_SHOULDFREE(slot))
	{
		heap_free_minimal_tuple(mslot->mintuple);
		slot->tts_flags &= ~TTS_FLAG_SHOULDFREE;
	}

	slot->tts_nvalid = 0;
	slot->tts_flags |= TTS_FLAG_EMPTY;
	ItemPointerSetInvalid(&slot->tts_tid);
	mslot->off = 0;
	mslot->mintuple = NULL;
}

static void
tts_minimal_getsomeattrs(TupleTableSlot *slot, int natts)
{
	MinimalTupleTableSlot *mslot = (MinimalTupleTableSlot *) slot;

	Assert(!TTS_EMPTY(slot));

	slot_deform_heap_tuple(slot, mslot->tuple, &mslot->off, natts);
}

static Datum
tts_minimal_getsysattr(TupleTableSlot *slot, int attnum, bool *isnull)
{
	Assert(!TTS_EMPTY(slot));

	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot retrieve a system column in this context")));

	return 0;					/* silence compiler warnings */
}

static void
tts_minimal_materialize(TupleTableSlot *slot)
{
	MinimalTupleTableSlot *mslot = (MinimalTupleTableSlot *) slot;
	MemoryContext oldContext;

	Assert(!TTS_EMPTY(slot));

	/* If slot has its tuple already materialized, nothing to do. */
	if (TTS_SHOULDFREE(slot))
		return;

	oldContext = MemoryContextSwitchTo(slot->tts_mcxt);

	/*
	 * Have to deform from scratch, otherwise tts_values[] entries could point
	 * into the non-materialized tuple (which might be gone when accessed).
	 */
	slot->tts_nvalid = 0;
	mslot->off = 0;

	if (!mslot->mintuple)
	{
		mslot->mintuple = heap_form_minimal_tuple(slot->tts_tupleDescriptor,
												  slot->tts_values,
												  slot->tts_isnull);
	}
	else
	{
		/*
		 * The minimal tuple contained in this slot is not allocated in the
		 * memory context of the given slot (else it would have
		 * TTS_FLAG_SHOULDFREE set).  Copy the minimal tuple into the given
		 * slot's memory context.
		 */
		mslot->mintuple = heap_copy_minimal_tuple(mslot->mintuple);
	}

	slot->tts_flags |= TTS_FLAG_SHOULDFREE;

	Assert(mslot->tuple == &mslot->minhdr);

	mslot->minhdr.t_len = mslot->mintuple->t_len + MINIMAL_TUPLE_OFFSET;
	mslot->minhdr.t_data = (HeapTupleHeader) ((char *) mslot->mintuple - MINIMAL_TUPLE_OFFSET);

	MemoryContextSwitchTo(oldContext);
}

static void
tts_minimal_copyslot(TupleTableSlot *dstslot, TupleTableSlot *srcslot)
{
	MemoryContext oldcontext;
	MinimalTuple mintuple;

	oldcontext = MemoryContextSwitchTo(dstslot->tts_mcxt);
	mintuple = ExecCopySlotMinimalTuple(srcslot);
	MemoryContextSwitchTo(oldcontext);

	ExecStoreMinimalTuple(mintuple, dstslot, true);
}

static MinimalTuple
tts_minimal_get_minimal_tuple(TupleTableSlot *slot)
{
	MinimalTupleTableSlot *mslot = (MinimalTupleTableSlot *) slot;

	if (!mslot->mintuple)
		tts_minimal_materialize(slot);

	return mslot->mintuple;
}

static HeapTuple
tts_minimal_copy_heap_tuple(TupleTableSlot *slot)
{
	MinimalTupleTableSlot *mslot = (MinimalTupleTableSlot *) slot;

	if (!mslot->mintuple)
		tts_minimal_materialize(slot);

	return heap_tuple_from_minimal_tuple(mslot->mintuple);
}

static MinimalTuple
tts_minimal_copy_minimal_tuple(TupleTableSlot *slot)
{
	MinimalTupleTableSlot *mslot = (MinimalTupleTableSlot *) slot;

	if (!mslot->mintuple)
		tts_minimal_materialize(slot);

	return heap_copy_minimal_tuple(mslot->mintuple);
}

static void
tts_minimal_store_tuple(TupleTableSlot *slot, MinimalTuple mtup, bool shouldFree)
{
	MinimalTupleTableSlot *mslot = (MinimalTupleTableSlot *) slot;

	tts_minimal_clear(slot);

	Assert(!TTS_SHOULDFREE(slot));
	Assert(TTS_EMPTY(slot));

	slot->tts_flags &= ~TTS_FLAG_EMPTY;
	slot->tts_nvalid = 0;
	mslot->off = 0;

	mslot->mintuple = mtup;
	Assert(mslot->tuple == &mslot->minhdr);
	mslot->minhdr.t_len = mtup->t_len + MINIMAL_TUPLE_OFFSET;
	mslot->minhdr.t_data = (HeapTupleHeader) ((char *) mtup - MINIMAL_TUPLE_OFFSET);
	/* no need to set t_self or t_tableOid since we won't allow access */

	if (shouldFree)
		slot->tts_flags |= TTS_FLAG_SHOULDFREE;
}


/*
 * TupleTableSlotOps implementation for BufferHeapTupleTableSlot.
 */

static void
tts_buffer_heap_init(TupleTableSlot *slot)
{
}

static void
tts_buffer_heap_release(TupleTableSlot *slot)
{
}

static void
tts_buffer_heap_clear(TupleTableSlot *slot)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;

	/*
	 * Free the memory for heap tuple if allowed. A tuple coming from buffer
	 * can never be freed. But we may have materialized a tuple from buffer.
	 * Such a tuple can be freed.
	 */
	if (TTS_SHOULDFREE(slot))
	{
		/* We should have unpinned the buffer while materializing the tuple. */
		Assert(!BufferIsValid(bslot->buffer));

		heap_freetuple(bslot->base.tuple);
		slot->tts_flags &= ~TTS_FLAG_SHOULDFREE;
	}

	if (BufferIsValid(bslot->buffer))
		ReleaseBuffer(bslot->buffer);

	slot->tts_nvalid = 0;
	slot->tts_flags |= TTS_FLAG_EMPTY;
	ItemPointerSetInvalid(&slot->tts_tid);
	bslot->base.tuple = NULL;
	bslot->base.off = 0;
	bslot->buffer = InvalidBuffer;
}

static void
tts_buffer_heap_getsomeattrs(TupleTableSlot *slot, int natts)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;

	Assert(!TTS_EMPTY(slot));

	slot_deform_heap_tuple(slot, bslot->base.tuple, &bslot->base.off, natts);
}

static Datum
tts_buffer_heap_getsysattr(TupleTableSlot *slot, int attnum, bool *isnull)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;

	Assert(!TTS_EMPTY(slot));

	/*
	 * In some code paths it's possible to get here with a non-materialized
	 * slot, in which case we can't retrieve system columns.
	 */
	if (!bslot->base.tuple)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot retrieve a system column in this context")));

	return heap_getsysattr(bslot->base.tuple, attnum,
						   slot->tts_tupleDescriptor, isnull);
}

static void
tts_buffer_heap_materialize(TupleTableSlot *slot)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;
	MemoryContext oldContext;

	Assert(!TTS_EMPTY(slot));

	/* If slot has its tuple already materialized, nothing to do. */
	if (TTS_SHOULDFREE(slot))
		return;

	oldContext = MemoryContextSwitchTo(slot->tts_mcxt);

	/*
	 * Have to deform from scratch, otherwise tts_values[] entries could point
	 * into the non-materialized tuple (which might be gone when accessed).
	 */
	bslot->base.off = 0;
	slot->tts_nvalid = 0;

	if (!bslot->base.tuple)
	{
		/*
		 * Normally BufferHeapTupleTableSlot should have a tuple + buffer
		 * associated with it, unless it's materialized (which would've
		 * returned above). But when it's useful to allow storing virtual
		 * tuples in a buffer slot, which then also needs to be
		 * materializable.
		 */
		bslot->base.tuple = heap_form_tuple(slot->tts_tupleDescriptor,
											slot->tts_values,
											slot->tts_isnull);
	}
	else
	{
		bslot->base.tuple = heap_copytuple(bslot->base.tuple);

		/*
		 * A heap tuple stored in a BufferHeapTupleTableSlot should have a
		 * buffer associated with it, unless it's materialized or virtual.
		 */
		if (likely(BufferIsValid(bslot->buffer)))
			ReleaseBuffer(bslot->buffer);
		bslot->buffer = InvalidBuffer;
	}

	/*
	 * We don't set TTS_FLAG_SHOULDFREE until after releasing the buffer, if
	 * any.  This avoids having a transient state that would fall foul of our
	 * assertions that a slot with TTS_FLAG_SHOULDFREE doesn't own a buffer.
	 * In the unlikely event that ReleaseBuffer() above errors out, we'd
	 * effectively leak the copied tuple, but that seems fairly harmless.
	 */
	slot->tts_flags |= TTS_FLAG_SHOULDFREE;

	MemoryContextSwitchTo(oldContext);
}

static void
tts_buffer_heap_copyslot(TupleTableSlot *dstslot, TupleTableSlot *srcslot)
{
	BufferHeapTupleTableSlot *bsrcslot = (BufferHeapTupleTableSlot *) srcslot;
	BufferHeapTupleTableSlot *bdstslot = (BufferHeapTupleTableSlot *) dstslot;

	/*
	 * If the source slot is of a different kind, or is a buffer slot that has
	 * been materialized / is virtual, make a new copy of the tuple. Otherwise
	 * make a new reference to the in-buffer tuple.
	 */
	if (dstslot->tts_ops != srcslot->tts_ops ||
		TTS_SHOULDFREE(srcslot) ||
		!bsrcslot->base.tuple)
	{
		MemoryContext oldContext;

		ExecClearTuple(dstslot);
		dstslot->tts_flags &= ~TTS_FLAG_EMPTY;
		oldContext = MemoryContextSwitchTo(dstslot->tts_mcxt);
		bdstslot->base.tuple = ExecCopySlotHeapTuple(srcslot);
		dstslot->tts_flags |= TTS_FLAG_SHOULDFREE;
		MemoryContextSwitchTo(oldContext);
	}
	else
	{
		Assert(BufferIsValid(bsrcslot->buffer));

		tts_buffer_heap_store_tuple(dstslot, bsrcslot->base.tuple,
									bsrcslot->buffer, false);

		/*
		 * The HeapTupleData portion of the source tuple might be shorter
		 * lived than the destination slot. Therefore copy the HeapTuple into
		 * our slot's tupdata, which is guaranteed to live long enough (but
		 * will still point into the buffer).
		 */
		memcpy(&bdstslot->base.tupdata, bdstslot->base.tuple, sizeof(HeapTupleData));
		bdstslot->base.tuple = &bdstslot->base.tupdata;
	}
}

static HeapTuple
tts_buffer_heap_get_heap_tuple(TupleTableSlot *slot)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;

	Assert(!TTS_EMPTY(slot));

	if (!bslot->base.tuple)
		tts_buffer_heap_materialize(slot);

	return bslot->base.tuple;
}

static HeapTuple
tts_buffer_heap_copy_heap_tuple(TupleTableSlot *slot)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;

	Assert(!TTS_EMPTY(slot));

	if (!bslot->base.tuple)
		tts_buffer_heap_materialize(slot);

	return heap_copytuple(bslot->base.tuple);
}

static MinimalTuple
tts_buffer_heap_copy_minimal_tuple(TupleTableSlot *slot)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;

	Assert(!TTS_EMPTY(slot));

	if (!bslot->base.tuple)
		tts_buffer_heap_materialize(slot);

	return minimal_tuple_from_heap_tuple(bslot->base.tuple);
}

static inline void
tts_buffer_heap_store_tuple(TupleTableSlot *slot, HeapTuple tuple,
							Buffer buffer, bool transfer_pin)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;

	if (TTS_SHOULDFREE(slot))
	{
		/* materialized slot shouldn't have a buffer to release */
		Assert(!BufferIsValid(bslot->buffer));

		heap_freetuple(bslot->base.tuple);
		slot->tts_flags &= ~TTS_FLAG_SHOULDFREE;
	}

	slot->tts_flags &= ~TTS_FLAG_EMPTY;
	slot->tts_nvalid = 0;
	bslot->base.tuple = tuple;
	bslot->base.off = 0;
	slot->tts_tid = tuple->t_self;

	/*
	 * If tuple is on a disk page, keep the page pinned as long as we hold a
	 * pointer into it.  We assume the caller already has such a pin.  If
	 * transfer_pin is true, we'll transfer that pin to this slot, if not
	 * we'll pin it again ourselves.
	 *
	 * This is coded to optimize the case where the slot previously held a
	 * tuple on the same disk page: in that case releasing and re-acquiring
	 * the pin is a waste of cycles.  This is a common situation during
	 * seqscans, so it's worth troubling over.
	 */
	if (bslot->buffer != buffer)
	{
		if (BufferIsValid(bslot->buffer))
			ReleaseBuffer(bslot->buffer);

		bslot->buffer = buffer;

		if (!transfer_pin && BufferIsValid(buffer))
			IncrBufferRefCount(buffer);
	}
	else if (transfer_pin && BufferIsValid(buffer))
	{
		/*
		 * In transfer_pin mode the caller won't know about the same-page
		 * optimization, so we gotta release its pin.
		 */
		ReleaseBuffer(buffer);
	}
}

/*
 * slot_deform_heap_tuple
 *		Given a TupleTableSlot, extract data from the slot's physical tuple
 *		into its Datum/isnull arrays.  Data is extracted up through the
 *		natts'th column (caller must ensure this is a legal column number).
 *
 *		This is essentially an incremental version of heap_deform_tuple:
 *		on each call we extract attributes up to the one needed, without
 *		re-computing information about previously extracted attributes.
 *		slot->tts_nvalid is the number of attributes already extracted.
 *
 * This is marked as always inline, so the different offp for different types
 * of slots gets optimized away.
 */
static pg_attribute_always_inline void
slot_deform_heap_tuple(TupleTableSlot *slot, HeapTuple tuple, uint32 *offp,
					   int natts)
{
	TupleDesc	tupleDesc = slot->tts_tupleDescriptor;
	Datum	   *values = slot->tts_values;
	bool	   *isnull = slot->tts_isnull;
	HeapTupleHeader tup = tuple->t_data;
	bool		hasnulls = HeapTupleHasNulls(tuple);
	int			attnum;
	char	   *tp;				/* ptr to tuple data */
	uint32		off;			/* offset in tuple data */
	bits8	   *bp = tup->t_bits;	/* ptr to null bitmap in tuple */
	bool		slow;			/* can we use/set attcacheoff? */

	/* We can only fetch as many attributes as the tuple has. */
	natts = Min(HeapTupleHeaderGetNatts(tuple->t_data), natts);

	/*
	 * Check whether the first call for this tuple, and initialize or restore
	 * loop state.
	 */
	attnum = slot->tts_nvalid;
	if (attnum == 0)
	{
		/* Start from the first attribute */
		off = 0;
		slow = false;
	}
	else
	{
		/* Restore state from previous execution */
		off = *offp;
		slow = TTS_SLOW(slot);
	}

	tp = (char *) tup + tup->t_hoff;

	for (; attnum < natts; attnum++)
	{
		Form_pg_attribute thisatt = TupleDescAttr(tupleDesc, attnum);

		if (hasnulls && att_isnull(attnum, bp))
		{
			values[attnum] = (Datum) 0;
			isnull[attnum] = true;
			slow = true;		/* can't use attcacheoff anymore */
			continue;
		}

		isnull[attnum] = false;

		if (!slow && thisatt->attcacheoff >= 0)
			off = thisatt->attcacheoff;
		else if (thisatt->attlen == -1)
		{
			/*
			 * We can only cache the offset for a varlena attribute if the
			 * offset is already suitably aligned, so that there would be no
			 * pad bytes in any case: then the offset will be valid for either
			 * an aligned or unaligned value.
			 */
			if (!slow &&
				off == att_align_nominal(off, thisatt->attalign))
				thisatt->attcacheoff = off;
			else
			{
				off = att_align_pointer(off, thisatt->attalign, -1,
										tp + off);
				slow = true;
			}
		}
		else
		{
			/* not varlena, so safe to use att_align_nominal */
			off = att_align_nominal(off, thisatt->attalign);

			if (!slow)
				thisatt->attcacheoff = off;
		}

		values[attnum] = fetchatt(thisatt, tp + off);

		off = att_addlength_pointer(off, thisatt->attlen, tp + off);

		if (thisatt->attlen <= 0)
			slow = true;		/* can't use attcacheoff anymore */
	}

	/*
	 * Save state for next execution
	 */
	slot->tts_nvalid = attnum;
	*offp = off;
	if (slow)
		slot->tts_flags |= TTS_FLAG_SLOW;
	else
		slot->tts_flags &= ~TTS_FLAG_SLOW;
}


const TupleTableSlotOps TTSOpsVirtual = {
	.base_slot_size = sizeof(VirtualTupleTableSlot),
	.init = tts_virtual_init,
	.release = tts_virtual_release,
	.clear = tts_virtual_clear,
	.getsomeattrs = tts_virtual_getsomeattrs,
	.getsysattr = tts_virtual_getsysattr,
	.materialize = tts_virtual_materialize,
	.copyslot = tts_virtual_copyslot,

	/*
	 * A virtual tuple table slot can not "own" a heap tuple or a minimal
	 * tuple.
	 */
	.get_heap_tuple = NULL,
	.get_minimal_tuple = NULL,
	.copy_heap_tuple = tts_virtual_copy_heap_tuple,
	.copy_minimal_tuple = tts_virtual_copy_minimal_tuple
};

const TupleTableSlotOps TTSOpsHeapTuple = {
	.base_slot_size = sizeof(HeapTupleTableSlot),
	.init = tts_heap_init,
	.release = tts_heap_release,
	.clear = tts_heap_clear,
	.getsomeattrs = tts_heap_getsomeattrs,
	.getsysattr = tts_heap_getsysattr,
	.materialize = tts_heap_materialize,
	.copyslot = tts_heap_copyslot,
	.get_heap_tuple = tts_heap_get_heap_tuple,

	/* A heap tuple table slot can not "own" a minimal tuple. */
	.get_minimal_tuple = NULL,
	.copy_heap_tuple = tts_heap_copy_heap_tuple,
	.copy_minimal_tuple = tts_heap_copy_minimal_tuple
};

const TupleTableSlotOps TTSOpsMinimalTuple = {
	.base_slot_size = sizeof(MinimalTupleTableSlot),
	.init = tts_minimal_init,
	.release = tts_minimal_release,
	.clear = tts_minimal_clear,
	.getsomeattrs = tts_minimal_getsomeattrs,
	.getsysattr = tts_minimal_getsysattr,
	.materialize = tts_minimal_materialize,
	.copyslot = tts_minimal_copyslot,

	/* A minimal tuple table slot can not "own" a heap tuple. */
	.get_heap_tuple = NULL,
	.get_minimal_tuple = tts_minimal_get_minimal_tuple,
	.copy_heap_tuple = tts_minimal_copy_heap_tuple,
	.copy_minimal_tuple = tts_minimal_copy_minimal_tuple
};

const TupleTableSlotOps TTSOpsBufferHeapTuple = {
	.base_slot_size = sizeof(BufferHeapTupleTableSlot),
	.init = tts_buffer_heap_init,
	.release = tts_buffer_heap_release,
	.clear = tts_buffer_heap_clear,
	.getsomeattrs = tts_buffer_heap_getsomeattrs,
	.getsysattr = tts_buffer_heap_getsysattr,
	.materialize = tts_buffer_heap_materialize,
	.copyslot = tts_buffer_heap_copyslot,
	.get_heap_tuple = tts_buffer_heap_get_heap_tuple,

	/* A buffer heap tuple table slot can not "own" a minimal tuple. */
	.get_minimal_tuple = NULL,
	.copy_heap_tuple = tts_buffer_heap_copy_heap_tuple,
	.copy_minimal_tuple = tts_buffer_heap_copy_minimal_tuple
};


/* ----------------------------------------------------------------
 *				  tuple table create/delete functions
 * ----------------------------------------------------------------
 */

/* --------------------------------
 *		MakeTupleTableSlot
 *
 *		Basic routine to make an empty TupleTableSlot of given
 *		TupleTableSlotType. If tupleDesc is specified the slot's descriptor is
 *		fixed for its lifetime, gaining some efficiency. If that's
 *		undesirable, pass NULL.
 * --------------------------------
 */
TupleTableSlot *
MakeTupleTableSlot(TupleDesc tupleDesc,
				   const TupleTableSlotOps *tts_ops)
{
	Size		basesz,
				allocsz;
	TupleTableSlot *slot;

	basesz = tts_ops->base_slot_size;

	/*
	 * When a fixed descriptor is specified, we can reduce overhead by
	 * allocating the entire slot in one go.
	 */
	if (tupleDesc)
		allocsz = MAXALIGN(basesz) +
			MAXALIGN(tupleDesc->natts * sizeof(Datum)) +
			MAXALIGN(tupleDesc->natts * sizeof(bool));
	else
		allocsz = basesz;

	slot = palloc0(allocsz);
	/* const for optimization purposes, OK to modify at allocation time */
	*((const TupleTableSlotOps **) &slot->tts_ops) = tts_ops;
	slot->type = T_TupleTableSlot;
	slot->tts_flags |= TTS_FLAG_EMPTY;
	if (tupleDesc != NULL)
		slot->tts_flags |= TTS_FLAG_FIXED;
	slot->tts_tupleDescriptor = tupleDesc;
	slot->tts_mcxt = CurrentMemoryContext;
	slot->tts_nvalid = 0;

	if (tupleDesc != NULL)
	{
		slot->tts_values = (Datum *)
			(((char *) slot)
			 + MAXALIGN(basesz));
		slot->tts_isnull = (bool *)
			(((char *) slot)
			 + MAXALIGN(basesz)
			 + MAXALIGN(tupleDesc->natts * sizeof(Datum)));

		PinTupleDesc(tupleDesc);
	}

	/*
	 * And allow slot type specific initialization.
	 */
	slot->tts_ops->init(slot);

	return slot;
}

/* --------------------------------
 *		ExecAllocTableSlot
 *
 *		Create a tuple table slot within a tuple table (which is just a List).
 * --------------------------------
 */
TupleTableSlot *
ExecAllocTableSlot(List **tupleTable, TupleDesc desc,
				   const TupleTableSlotOps *tts_ops)
{
	TupleTableSlot *slot = MakeTupleTableSlot(desc, tts_ops);

	*tupleTable = lappend(*tupleTable, slot);

	return slot;
}

/* --------------------------------
 *		ExecResetTupleTable
 *
 *		This releases any resources (buffer pins, tupdesc refcounts)
 *		held by the tuple table, and optionally releases the memory
 *		occupied by the tuple table data structure.
 *		It is expected that this routine be called by ExecEndPlan().
 * --------------------------------
 */
void
ExecResetTupleTable(List *tupleTable,	/* tuple table */
					bool shouldFree)	/* true if we should free memory */
{
	ListCell   *lc;

	foreach(lc, tupleTable)
	{
		TupleTableSlot *slot = lfirst_node(TupleTableSlot, lc);

		/* Always release resources and reset the slot to empty */
		ExecClearTuple(slot);
		slot->tts_ops->release(slot);
		if (slot->tts_tupleDescriptor)
		{
			ReleaseTupleDesc(slot->tts_tupleDescriptor);
			slot->tts_tupleDescriptor = NULL;
		}

		/* If shouldFree, release memory occupied by the slot itself */
		if (shouldFree)
		{
			if (!TTS_FIXED(slot))
			{
				if (slot->tts_values)
					pfree(slot->tts_values);
				if (slot->tts_isnull)
					pfree(slot->tts_isnull);
			}
			pfree(slot);
		}
	}

	/* If shouldFree, release the list structure */
	if (shouldFree)
		list_free(tupleTable);
}

/* --------------------------------
 *		MakeSingleTupleTableSlot
 *
 *		This is a convenience routine for operations that need a standalone
 *		TupleTableSlot not gotten from the main executor tuple table.  It makes
 *		a single slot of given TupleTableSlotType and initializes it to use the
 *		given tuple descriptor.
 * --------------------------------
 */
TupleTableSlot *
MakeSingleTupleTableSlot(TupleDesc tupdesc,
						 const TupleTableSlotOps *tts_ops)
{
	TupleTableSlot *slot = MakeTupleTableSlot(tupdesc, tts_ops);

	return slot;
}

/* --------------------------------
 *		ExecDropSingleTupleTableSlot
 *
 *		Release a TupleTableSlot made with MakeSingleTupleTableSlot.
 *		DON'T use this on a slot that's part of a tuple table list!
 * --------------------------------
 */
void
ExecDropSingleTupleTableSlot(TupleTableSlot *slot)
{
	/* This should match ExecResetTupleTable's processing of one slot */
	Assert(IsA(slot, TupleTableSlot));
	ExecClearTuple(slot);
	slot->tts_ops->release(slot);
	if (slot->tts_tupleDescriptor)
		ReleaseTupleDesc(slot->tts_tupleDescriptor);
	if (!TTS_FIXED(slot))
	{
		if (slot->tts_values)
			pfree(slot->tts_values);
		if (slot->tts_isnull)
			pfree(slot->tts_isnull);
	}
	pfree(slot);
}


/* ----------------------------------------------------------------
 *				  tuple table slot accessor functions
 * ----------------------------------------------------------------
 */

/* --------------------------------
 *		ExecSetSlotDescriptor
 *
 *		This function is used to set the tuple descriptor associated
 *		with the slot's tuple.  The passed descriptor must have lifespan
 *		at least equal to the slot's.  If it is a reference-counted descriptor
 *		then the reference count is incremented for as long as the slot holds
 *		a reference.
 * --------------------------------
 */
void
ExecSetSlotDescriptor(TupleTableSlot *slot, /* slot to change */
					  TupleDesc tupdesc)	/* new tuple descriptor */
{
	Assert(!TTS_FIXED(slot));

	/* For safety, make sure slot is empty before changing it */
	ExecClearTuple(slot);

	/*
	 * Release any old descriptor.  Also release old Datum/isnull arrays if
	 * present (we don't bother to check if they could be re-used).
	 */
	if (slot->tts_tupleDescriptor)
		ReleaseTupleDesc(slot->tts_tupleDescriptor);

	if (slot->tts_values)
		pfree(slot->tts_values);
	if (slot->tts_isnull)
		pfree(slot->tts_isnull);

	/*
	 * Install the new descriptor; if it's refcounted, bump its refcount.
	 */
	slot->tts_tupleDescriptor = tupdesc;
	PinTupleDesc(tupdesc);

	/*
	 * Allocate Datum/isnull arrays of the appropriate size.  These must have
	 * the same lifetime as the slot, so allocate in the slot's own context.
	 */
	slot->tts_values = (Datum *)
		MemoryContextAlloc(slot->tts_mcxt, tupdesc->natts * sizeof(Datum));
	slot->tts_isnull = (bool *)
		MemoryContextAlloc(slot->tts_mcxt, tupdesc->natts * sizeof(bool));
}

/* --------------------------------
 *		ExecStoreHeapTuple
 *
 *		This function is used to store an on-the-fly physical tuple into a specified
 *		slot in the tuple table.
 *
 *		tuple:	tuple to store
 *		slot:	TTSOpsHeapTuple type slot to store it in
 *		shouldFree: true if ExecClearTuple should pfree() the tuple
 *					when done with it
 *
 * shouldFree is normally set 'true' for tuples constructed on-the-fly.  But it
 * can be 'false' when the referenced tuple is held in a tuple table slot
 * belonging to a lower-level executor Proc node.  In this case the lower-level
 * slot retains ownership and responsibility for eventually releasing the
 * tuple.  When this method is used, we must be certain that the upper-level
 * Proc node will lose interest in the tuple sooner than the lower-level one
 * does!  If you're not certain, copy the lower-level tuple with heap_copytuple
 * and let the upper-level table slot assume ownership of the copy!
 *
 * Return value is just the passed-in slot pointer.
 *
 * If the target slot is not guaranteed to be TTSOpsHeapTuple type slot, use
 * the, more expensive, ExecForceStoreHeapTuple().
 * --------------------------------
 */
TupleTableSlot *
ExecStoreHeapTuple(HeapTuple tuple,
				   TupleTableSlot *slot,
				   bool shouldFree)
{
	/*
	 * sanity checks
	 */
	Assert(tuple != NULL);
	Assert(slot != NULL);
	Assert(slot->tts_tupleDescriptor != NULL);

	if (unlikely(!TTS_IS_HEAPTUPLE(slot)))
		elog(ERROR, "trying to store a heap tuple into wrong type of slot");
	tts_heap_store_tuple(slot, tuple, shouldFree);

	slot->tts_tableOid = tuple->t_tableOid;

	return slot;
}

/* --------------------------------
 *		ExecStoreBufferHeapTuple
 *
 *		This function is used to store an on-disk physical tuple from a buffer
 *		into a specified slot in the tuple table.
 *
 *		tuple:	tuple to store
 *		slot:	TTSOpsBufferHeapTuple type slot to store it in
 *		buffer: disk buffer if tuple is in a disk page, else InvalidBuffer
 *
 * The tuple table code acquires a pin on the buffer which is held until the
 * slot is cleared, so that the tuple won't go away on us.
 *
 * Return value is just the passed-in slot pointer.
 *
 * If the target slot is not guaranteed to be TTSOpsBufferHeapTuple type slot,
 * use the, more expensive, ExecForceStoreHeapTuple().
 * --------------------------------
 */
TupleTableSlot *
ExecStoreBufferHeapTuple(HeapTuple tuple,
						 TupleTableSlot *slot,
						 Buffer buffer)
{
	/*
	 * sanity checks
	 */
	Assert(tuple != NULL);
	Assert(slot != NULL);
	Assert(slot->tts_tupleDescriptor != NULL);
	Assert(BufferIsValid(buffer));

	if (unlikely(!TTS_IS_BUFFERTUPLE(slot)))
		elog(ERROR, "trying to store an on-disk heap tuple into wrong type of slot");
	tts_buffer_heap_store_tuple(slot, tuple, buffer, false);

	slot->tts_tableOid = tuple->t_tableOid;

	return slot;
}

/*
 * Like ExecStoreBufferHeapTuple, but transfer an existing pin from the caller
 * to the slot, i.e. the caller doesn't need to, and may not, release the pin.
 */
TupleTableSlot *
ExecStorePinnedBufferHeapTuple(HeapTuple tuple,
							   TupleTableSlot *slot,
							   Buffer buffer)
{
	/*
	 * sanity checks
	 */
	Assert(tuple != NULL);
	Assert(slot != NULL);
	Assert(slot->tts_tupleDescriptor != NULL);
	Assert(BufferIsValid(buffer));

	if (unlikely(!TTS_IS_BUFFERTUPLE(slot)))
		elog(ERROR, "trying to store an on-disk heap tuple into wrong type of slot");
	tts_buffer_heap_store_tuple(slot, tuple, buffer, true);

	slot->tts_tableOid = tuple->t_tableOid;

	return slot;
}

/*
 * Store a minimal tuple into TTSOpsMinimalTuple type slot.
 *
 * If the target slot is not guaranteed to be TTSOpsMinimalTuple type slot,
 * use the, more expensive, ExecForceStoreMinimalTuple().
 */
TupleTableSlot *
ExecStoreMinimalTuple(MinimalTuple mtup,
					  TupleTableSlot *slot,
					  bool shouldFree)
{
	/*
	 * sanity checks
	 */
	Assert(mtup != NULL);
	Assert(slot != NULL);
	Assert(slot->tts_tupleDescriptor != NULL);

	if (unlikely(!TTS_IS_MINIMALTUPLE(slot)))
		elog(ERROR, "trying to store a minimal tuple into wrong type of slot");
	tts_minimal_store_tuple(slot, mtup, shouldFree);

	return slot;
}

/*
 * Store a HeapTuple into any kind of slot, performing conversion if
 * necessary.
 */
void
ExecForceStoreHeapTuple(HeapTuple tuple,
						TupleTableSlot *slot,
						bool shouldFree)
{
	if (TTS_IS_HEAPTUPLE(slot))
	{
		ExecStoreHeapTuple(tuple, slot, shouldFree);
	}
	else if (TTS_IS_BUFFERTUPLE(slot))
	{
		MemoryContext oldContext;
		BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;

		ExecClearTuple(slot);
		slot->tts_flags &= ~TTS_FLAG_EMPTY;
		oldContext = MemoryContextSwitchTo(slot->tts_mcxt);
		bslot->base.tuple = heap_copytuple(tuple);
		slot->tts_flags |= TTS_FLAG_SHOULDFREE;
		MemoryContextSwitchTo(oldContext);

		if (shouldFree)
			pfree(tuple);
	}
	else
	{
		ExecClearTuple(slot);
		heap_deform_tuple(tuple, slot->tts_tupleDescriptor,
						  slot->tts_values, slot->tts_isnull);
		ExecStoreVirtualTuple(slot);

		if (shouldFree)
		{
			ExecMaterializeSlot(slot);
			pfree(tuple);
		}
	}
}

/*
 * Store a MinimalTuple into any kind of slot, performing conversion if
 * necessary.
 */
void
ExecForceStoreMinimalTuple(MinimalTuple mtup,
						   TupleTableSlot *slot,
						   bool shouldFree)
{
	if (TTS_IS_MINIMALTUPLE(slot))
	{
		tts_minimal_store_tuple(slot, mtup, shouldFree);
	}
	else
	{
		HeapTupleData htup;

		ExecClearTuple(slot);

		htup.t_len = mtup->t_len + MINIMAL_TUPLE_OFFSET;
		htup.t_data = (HeapTupleHeader) ((char *) mtup - MINIMAL_TUPLE_OFFSET);
		heap_deform_tuple(&htup, slot->tts_tupleDescriptor,
						  slot->tts_values, slot->tts_isnull);
		ExecStoreVirtualTuple(slot);

		if (shouldFree)
		{
			ExecMaterializeSlot(slot);
			pfree(mtup);
		}
	}
}

/* --------------------------------
 *		ExecStoreVirtualTuple
 *			Mark a slot as containing a virtual tuple.
 *
 * The protocol for loading a slot with virtual tuple data is:
 *		* Call ExecClearTuple to mark the slot empty.
 *		* Store data into the Datum/isnull arrays.
 *		* Call ExecStoreVirtualTuple to mark the slot valid.
 * This is a bit unclean but it avoids one round of data copying.
 * --------------------------------
 */
TupleTableSlot *
ExecStoreVirtualTuple(TupleTableSlot *slot)
{
	/*
	 * sanity checks
	 */
	Assert(slot != NULL);
	Assert(slot->tts_tupleDescriptor != NULL);
	Assert(TTS_EMPTY(slot));

	slot->tts_flags &= ~TTS_FLAG_EMPTY;
	slot->tts_nvalid = slot->tts_tupleDescriptor->natts;

	return slot;
}

/* --------------------------------
 *		ExecStoreAllNullTuple
 *			Set up the slot to contain a null in every column.
 *
 * At first glance this might sound just like ExecClearTuple, but it's
 * entirely different: the slot ends up full, not empty.
 * --------------------------------
 */
TupleTableSlot *
ExecStoreAllNullTuple(TupleTableSlot *slot)
{
	/*
	 * sanity checks
	 */
	Assert(slot != NULL);
	Assert(slot->tts_tupleDescriptor != NULL);

	/* Clear any old contents */
	ExecClearTuple(slot);

	/*
	 * Fill all the columns of the virtual tuple with nulls
	 */
	MemSet(slot->tts_values, 0,
		   slot->tts_tupleDescriptor->natts * sizeof(Datum));
	memset(slot->tts_isnull, true,
		   slot->tts_tupleDescriptor->natts * sizeof(bool));

	return ExecStoreVirtualTuple(slot);
}

/*
 * Store a HeapTuple in datum form, into a slot. That always requires
 * deforming it and storing it in virtual form.
 *
 * Until the slot is materialized, the contents of the slot depend on the
 * datum.
 */
void
ExecStoreHeapTupleDatum(Datum data, TupleTableSlot *slot)
{
	HeapTupleData tuple = {0};
	HeapTupleHeader td;

	td = DatumGetHeapTupleHeader(data);

	tuple.t_len = HeapTupleHeaderGetDatumLength(td);
	tuple.t_self = td->t_ctid;
	tuple.t_data = td;

	ExecClearTuple(slot);

	heap_deform_tuple(&tuple, slot->tts_tupleDescriptor,
					  slot->tts_values, slot->tts_isnull);
	ExecStoreVirtualTuple(slot);
}

/*
 * ExecFetchSlotHeapTuple - fetch HeapTuple representing the slot's content
 *
 * The returned HeapTuple represents the slot's content as closely as
 * possible.
 *
 * If materialize is true, the contents of the slots will be made independent
 * from the underlying storage (i.e. all buffer pins are released, memory is
 * allocated in the slot's context).
 *
 * If shouldFree is not-NULL it'll be set to true if the returned tuple has
 * been allocated in the calling memory context, and must be freed by the
 * caller (via explicit pfree() or a memory context reset).
 *
 * NB: If materialize is true, modifications of the returned tuple are
 * allowed. But it depends on the type of the slot whether such modifications
 * will also affect the slot's contents. While that is not the nicest
 * behaviour, all such modifications are in the process of being removed.
 */
HeapTuple
ExecFetchSlotHeapTuple(TupleTableSlot *slot, bool materialize, bool *shouldFree)
{
	/*
	 * sanity checks
	 */
	Assert(slot != NULL);
	Assert(!TTS_EMPTY(slot));

	/* Materialize the tuple so that the slot "owns" it, if requested. */
	if (materialize)
		slot->tts_ops->materialize(slot);

	if (slot->tts_ops->get_heap_tuple == NULL)
	{
		if (shouldFree)
			*shouldFree = true;
		return slot->tts_ops->copy_heap_tuple(slot);
	}
	else
	{
		if (shouldFree)
			*shouldFree = false;
		return slot->tts_ops->get_heap_tuple(slot);
	}
}

/* --------------------------------
 *		ExecFetchSlotMinimalTuple
 *			Fetch the slot's minimal physical tuple.
 *
 *		If the given tuple table slot can hold a minimal tuple, indicated by a
 *		non-NULL get_minimal_tuple callback, the function returns the minimal
 *		tuple returned by that callback. It assumes that the minimal tuple
 *		returned by the callback is "owned" by the slot i.e. the slot is
 *		responsible for freeing the memory consumed by the tuple. Hence it sets
 *		*shouldFree to false, indicating that the caller should not free the
 *		memory consumed by the minimal tuple. In this case the returned minimal
 *		tuple should be considered as read-only.
 *
 *		If that callback is not supported, it calls copy_minimal_tuple callback
 *		which is expected to return a copy of minimal tuple representing the
 *		contents of the slot. In this case *shouldFree is set to true,
 *		indicating the caller that it should free the memory consumed by the
 *		minimal tuple. In this case the returned minimal tuple may be written
 *		up.
 * --------------------------------
 */
MinimalTuple
ExecFetchSlotMinimalTuple(TupleTableSlot *slot,
						  bool *shouldFree)
{
	/*
	 * sanity checks
	 */
	Assert(slot != NULL);
	Assert(!TTS_EMPTY(slot));

	if (slot->tts_ops->get_minimal_tuple)
	{
		if (shouldFree)
			*shouldFree = false;
		return slot->tts_ops->get_minimal_tuple(slot);
	}
	else
	{
		if (shouldFree)
			*shouldFree = true;
		return slot->tts_ops->copy_minimal_tuple(slot);
	}
}

/* --------------------------------
 *		ExecFetchSlotHeapTupleDatum
 *			Fetch the slot's tuple as a composite-type Datum.
 *
 *		The result is always freshly palloc'd in the caller's memory context.
 * --------------------------------
 */
Datum
ExecFetchSlotHeapTupleDatum(TupleTableSlot *slot)
{
	HeapTuple	tup;
	TupleDesc	tupdesc;
	bool		shouldFree;
	Datum		ret;

	/* Fetch slot's contents in regular-physical-tuple form */
	tup = ExecFetchSlotHeapTuple(slot, false, &shouldFree);
	tupdesc = slot->tts_tupleDescriptor;

	/* Convert to Datum form */
	ret = heap_copy_tuple_as_datum(tup, tupdesc);

	if (shouldFree)
		pfree(tup);

	return ret;
}

/* ----------------------------------------------------------------
 *				convenience initialization routines
 * ----------------------------------------------------------------
 */

/* ----------------
 *		ExecInitResultTypeTL
 *
 *		Initialize result type, using the plan node's targetlist.
 * ----------------
 */
void
ExecInitResultTypeTL(PlanState *planstate)
{
	TupleDesc	tupDesc = ExecTypeFromTL(planstate->plan->targetlist);

	planstate->ps_ResultTupleDesc = tupDesc;
}

/* --------------------------------
 *		ExecInit{Result,Scan,Extra}TupleSlot[TL]
 *
 *		These are convenience routines to initialize the specified slot
 *		in nodes inheriting the appropriate state.  ExecInitExtraTupleSlot
 *		is used for initializing special-purpose slots.
 * --------------------------------
 */

/* ----------------
 *		ExecInitResultTupleSlotTL
 *
 *		Initialize result tuple slot, using the tuple descriptor previously
 *		computed with ExecInitResultTypeTL().
 * ----------------
 */
void
ExecInitResultSlot(PlanState *planstate, const TupleTableSlotOps *tts_ops)
{
	TupleTableSlot *slot;

	slot = ExecAllocTableSlot(&planstate->state->es_tupleTable,
							  planstate->ps_ResultTupleDesc, tts_ops);
	planstate->ps_ResultTupleSlot = slot;

	planstate->resultopsfixed = planstate->ps_ResultTupleDesc != NULL;
	planstate->resultops = tts_ops;
	planstate->resultopsset = true;
}

/* ----------------
 *		ExecInitResultTupleSlotTL
 *
 *		Initialize result tuple slot, using the plan node's targetlist.
 * ----------------
 */
void
ExecInitResultTupleSlotTL(PlanState *planstate,
						  const TupleTableSlotOps *tts_ops)
{
	ExecInitResultTypeTL(planstate);
	ExecInitResultSlot(planstate, tts_ops);
}

/* ----------------
 *		ExecInitScanTupleSlot
 * ----------------
 */
void
ExecInitScanTupleSlot(EState *estate, ScanState *scanstate,
					  TupleDesc tupledesc, const TupleTableSlotOps *tts_ops)
{
	scanstate->ss_ScanTupleSlot = ExecAllocTableSlot(&estate->es_tupleTable,
													 tupledesc, tts_ops);
	scanstate->ps.scandesc = tupledesc;
	scanstate->ps.scanopsfixed = tupledesc != NULL;
	scanstate->ps.scanops = tts_ops;
	scanstate->ps.scanopsset = true;
}

/* ----------------
 *		ExecInitExtraTupleSlot
 *
 * Return a newly created slot. If tupledesc is non-NULL the slot will have
 * that as its fixed tupledesc. Otherwise the caller needs to use
 * ExecSetSlotDescriptor() to set the descriptor before use.
 * ----------------
 */
TupleTableSlot *
ExecInitExtraTupleSlot(EState *estate,
					   TupleDesc tupledesc,
					   const TupleTableSlotOps *tts_ops)
{
	return ExecAllocTableSlot(&estate->es_tupleTable, tupledesc, tts_ops);
}

/* ----------------
 *		ExecInitNullTupleSlot
 *
 * Build a slot containing an all-nulls tuple of the given type.
 * This is used as a substitute for an input tuple when performing an
 * outer join.
 * ----------------
 */
TupleTableSlot *
ExecInitNullTupleSlot(EState *estate, TupleDesc tupType,
					  const TupleTableSlotOps *tts_ops)
{
	TupleTableSlot *slot = ExecInitExtraTupleSlot(estate, tupType, tts_ops);

	return ExecStoreAllNullTuple(slot);
}

/* ---------------------------------------------------------------
 *      Routines for setting/accessing attributes in a slot.
 * ---------------------------------------------------------------
 */

/*
 * Fill in missing values for a TupleTableSlot.
 *
 * This is only exposed because it's needed for JIT compiled tuple
 * deforming. That exception aside, there should be no callers outside of this
 * file.
 */
void
slot_getmissingattrs(TupleTableSlot *slot, int startAttNum, int lastAttNum)
{
	AttrMissing *attrmiss = NULL;

	if (slot->tts_tupleDescriptor->constr)
		attrmiss = slot->tts_tupleDescriptor->constr->missing;

	if (!attrmiss)
	{
		/* no missing values array at all, so just fill everything in as NULL */
		memset(slot->tts_values + startAttNum, 0,
			   (lastAttNum - startAttNum) * sizeof(Datum));
		memset(slot->tts_isnull + startAttNum, 1,
			   (lastAttNum - startAttNum) * sizeof(bool));
	}
	else
	{
		int			missattnum;

		/* if there is a missing values array we must process them one by one */
		for (missattnum = startAttNum;
			 missattnum < lastAttNum;
			 missattnum++)
		{
			slot->tts_values[missattnum] = attrmiss[missattnum].am_value;
			slot->tts_isnull[missattnum] = !attrmiss[missattnum].am_present;
		}
	}
}

/*
 * slot_getsomeattrs_int - workhorse for slot_getsomeattrs()
 */
void
slot_getsomeattrs_int(TupleTableSlot *slot, int attnum)
{
	/* Check for caller errors */
	Assert(slot->tts_nvalid < attnum);	/* checked in slot_getsomeattrs */
	Assert(attnum > 0);

	if (unlikely(attnum > slot->tts_tupleDescriptor->natts))
		elog(ERROR, "invalid attribute number %d", attnum);

	/* Fetch as many attributes as possible from the underlying tuple. */
	slot->tts_ops->getsomeattrs(slot, attnum);

	/*
	 * If the underlying tuple doesn't have enough attributes, tuple
	 * descriptor must have the missing attributes.
	 */
	if (unlikely(slot->tts_nvalid < attnum))
	{
		slot_getmissingattrs(slot, slot->tts_nvalid, attnum);
		slot->tts_nvalid = attnum;
	}
}

/* ----------------------------------------------------------------
 *		ExecTypeFromTL
 *
 *		Generate a tuple descriptor for the result tuple of a targetlist.
 *		(A parse/plan tlist must be passed, not an ExprState tlist.)
 *		Note that resjunk columns, if any, are included in the result.
 *
 *		Currently there are about 4 different places where we create
 *		TupleDescriptors.  They should all be merged, or perhaps
 *		be rewritten to call BuildDesc().
 * ----------------------------------------------------------------
 */
TupleDesc
ExecTypeFromTL(List *targetList)
{
	return ExecTypeFromTLInternal(targetList, false);
}

/* ----------------------------------------------------------------
 *		ExecCleanTypeFromTL
 *
 *		Same as above, but resjunk columns are omitted from the result.
 * ----------------------------------------------------------------
 */
TupleDesc
ExecCleanTypeFromTL(List *targetList)
{
	return ExecTypeFromTLInternal(targetList, true);
}

static TupleDesc
ExecTypeFromTLInternal(List *targetList, bool skipjunk)
{
	TupleDesc	typeInfo;
	ListCell   *l;
	int			len;
	int			cur_resno = 1;

	if (skipjunk)
		len = ExecCleanTargetListLength(targetList);
	else
		len = ExecTargetListLength(targetList);
	typeInfo = CreateTemplateTupleDesc(len);

	foreach(l, targetList)
	{
		TargetEntry *tle = lfirst(l);

		if (skipjunk && tle->resjunk)
			continue;
		TupleDescInitEntry(typeInfo,
						   cur_resno,
						   tle->resname,
						   exprType((Node *) tle->expr),
						   exprTypmod((Node *) tle->expr),
						   0);
		TupleDescInitEntryCollation(typeInfo,
									cur_resno,
									exprCollation((Node *) tle->expr));
		cur_resno++;
	}

	return typeInfo;
}

/*
 * ExecTypeFromExprList - build a tuple descriptor from a list of Exprs
 *
 * This is roughly like ExecTypeFromTL, but we work from bare expressions
 * not TargetEntrys.  No names are attached to the tupledesc's columns.
 */
TupleDesc
ExecTypeFromExprList(List *exprList)
{
	TupleDesc	typeInfo;
	ListCell   *lc;
	int			cur_resno = 1;

	typeInfo = CreateTemplateTupleDesc(list_length(exprList));

	foreach(lc, exprList)
	{
		Node	   *e = lfirst(lc);

		TupleDescInitEntry(typeInfo,
						   cur_resno,
						   NULL,
						   exprType(e),
						   exprTypmod(e),
						   0);
		TupleDescInitEntryCollation(typeInfo,
									cur_resno,
									exprCollation(e));
		cur_resno++;
	}

	return typeInfo;
}

/*
 * ExecTypeSetColNames - set column names in a RECORD TupleDesc
 *
 * Column names must be provided as an alias list (list of String nodes).
 */
void
ExecTypeSetColNames(TupleDesc typeInfo, List *namesList)
{
	int			colno = 0;
	ListCell   *lc;

	/* It's only OK to change col names in a not-yet-blessed RECORD type */
	Assert(typeInfo->tdtypeid == RECORDOID);
	Assert(typeInfo->tdtypmod < 0);

	foreach(lc, namesList)
	{
		char	   *cname = strVal(lfirst(lc));
		Form_pg_attribute attr;

		/* Guard against too-long names list (probably can't happen) */
		if (colno >= typeInfo->natts)
			break;
		attr = TupleDescAttr(typeInfo, colno);
		colno++;

		/*
		 * Do nothing for empty aliases or dropped columns (these cases
		 * probably can't arise in RECORD types, either)
		 */
		if (cname[0] == '\0' || attr->attisdropped)
			continue;

		/* OK, assign the column name */
		namestrcpy(&(attr->attname), cname);
	}
}

/*
 * BlessTupleDesc - make a completed tuple descriptor useful for SRFs
 *
 * Rowtype Datums returned by a function must contain valid type information.
 * This happens "for free" if the tupdesc came from a relcache entry, but
 * not if we have manufactured a tupdesc for a transient RECORD datatype.
 * In that case we have to notify typcache.c of the existence of the type.
 */
TupleDesc
BlessTupleDesc(TupleDesc tupdesc)
{
	if (tupdesc->tdtypeid == RECORDOID &&
		tupdesc->tdtypmod < 0)
		assign_record_type_typmod(tupdesc);

	return tupdesc;				/* just for notational convenience */
}

/*
 * TupleDescGetAttInMetadata - Build an AttInMetadata structure based on the
 * supplied TupleDesc. AttInMetadata can be used in conjunction with C strings
 * to produce a properly formed tuple.
 */
AttInMetadata *
TupleDescGetAttInMetadata(TupleDesc tupdesc)
{
	int			natts = tupdesc->natts;
	int			i;
	Oid			atttypeid;
	Oid			attinfuncid;
	FmgrInfo   *attinfuncinfo;
	Oid		   *attioparams;
	int32	   *atttypmods;
	AttInMetadata *attinmeta;

	attinmeta = (AttInMetadata *) palloc(sizeof(AttInMetadata));

	/* "Bless" the tupledesc so that we can make rowtype datums with it */
	attinmeta->tupdesc = BlessTupleDesc(tupdesc);

	/*
	 * Gather info needed later to call the "in" function for each attribute
	 */
	attinfuncinfo = (FmgrInfo *) palloc0(natts * sizeof(FmgrInfo));
	attioparams = (Oid *) palloc0(natts * sizeof(Oid));
	atttypmods = (int32 *) palloc0(natts * sizeof(int32));

	for (i = 0; i < natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		/* Ignore dropped attributes */
		if (!att->attisdropped)
		{
			atttypeid = att->atttypid;
			getTypeInputInfo(atttypeid, &attinfuncid, &attioparams[i]);
			fmgr_info(attinfuncid, &attinfuncinfo[i]);
			atttypmods[i] = att->atttypmod;
		}
	}
	attinmeta->attinfuncs = attinfuncinfo;
	attinmeta->attioparams = attioparams;
	attinmeta->atttypmods = atttypmods;

	return attinmeta;
}

/*
 * BuildTupleFromCStrings - build a HeapTuple given user data in C string form.
 * values is an array of C strings, one for each attribute of the return tuple.
 * A NULL string pointer indicates we want to create a NULL field.
 */
HeapTuple
BuildTupleFromCStrings(AttInMetadata *attinmeta, char **values)
{
	TupleDesc	tupdesc = attinmeta->tupdesc;
	int			natts = tupdesc->natts;
	Datum	   *dvalues;
	bool	   *nulls;
	int			i;
	HeapTuple	tuple;

	dvalues = (Datum *) palloc(natts * sizeof(Datum));
	nulls = (bool *) palloc(natts * sizeof(bool));

	/*
	 * Call the "in" function for each non-dropped attribute, even for nulls,
	 * to support domains.
	 */
	for (i = 0; i < natts; i++)
	{
		if (!TupleDescAttr(tupdesc, i)->attisdropped)
		{
			/* Non-dropped attributes */
			dvalues[i] = InputFunctionCall(&attinmeta->attinfuncs[i],
										   values[i],
										   attinmeta->attioparams[i],
										   attinmeta->atttypmods[i]);
			if (values[i] != NULL)
				nulls[i] = false;
			else
				nulls[i] = true;
		}
		else
		{
			/* Handle dropped attributes by setting to NULL */
			dvalues[i] = (Datum) 0;
			nulls[i] = true;
		}
	}

	/*
	 * Form a tuple
	 */
	tuple = heap_form_tuple(tupdesc, dvalues, nulls);

	/*
	 * Release locally palloc'd space.  XXX would probably be good to pfree
	 * values of pass-by-reference datums, as well.
	 */
	pfree(dvalues);
	pfree(nulls);

	return tuple;
}

/*
 * HeapTupleHeaderGetDatum - convert a HeapTupleHeader pointer to a Datum.
 *
 * This must *not* get applied to an on-disk tuple; the tuple should be
 * freshly made by heap_form_tuple or some wrapper routine for it (such as
 * BuildTupleFromCStrings).  Be sure also that the tupledesc used to build
 * the tuple has a properly "blessed" rowtype.
 *
 * Formerly this was a macro equivalent to PointerGetDatum, relying on the
 * fact that heap_form_tuple fills in the appropriate tuple header fields
 * for a composite Datum.  However, we now require that composite Datums not
 * contain any external TOAST pointers.  We do not want heap_form_tuple itself
 * to enforce that; more specifically, the rule applies only to actual Datums
 * and not to HeapTuple structures.  Therefore, HeapTupleHeaderGetDatum is
 * now a function that detects whether there are externally-toasted fields
 * and constructs a new tuple with inlined fields if so.  We still need
 * heap_form_tuple to insert the Datum header fields, because otherwise this
 * code would have no way to obtain a tupledesc for the tuple.
 *
 * Note that if we do build a new tuple, it's palloc'd in the current
 * memory context.  Beware of code that changes context between the initial
 * heap_form_tuple/etc call and calling HeapTuple(Header)GetDatum.
 *
 * For performance-critical callers, it could be worthwhile to take extra
 * steps to ensure that there aren't TOAST pointers in the output of
 * heap_form_tuple to begin with.  It's likely however that the costs of the
 * typcache lookup and tuple disassembly/reassembly are swamped by TOAST
 * dereference costs, so that the benefits of such extra effort would be
 * minimal.
 *
 * XXX it would likely be better to create wrapper functions that produce
 * a composite Datum from the field values in one step.  However, there's
 * enough code using the existing APIs that we couldn't get rid of this
 * hack anytime soon.
 */
Datum
HeapTupleHeaderGetDatum(HeapTupleHeader tuple)
{
	Datum		result;
	TupleDesc	tupDesc;

	/* No work if there are no external TOAST pointers in the tuple */
	if (!HeapTupleHeaderHasExternal(tuple))
		return PointerGetDatum(tuple);

	/* Use the type data saved by heap_form_tuple to look up the rowtype */
	tupDesc = lookup_rowtype_tupdesc(HeapTupleHeaderGetTypeId(tuple),
									 HeapTupleHeaderGetTypMod(tuple));

	/* And do the flattening */
	result = toast_flatten_tuple_to_datum(tuple,
										  HeapTupleHeaderGetDatumLength(tuple),
										  tupDesc);

	ReleaseTupleDesc(tupDesc);

	return result;
}


/*
 * Functions for sending tuples to the frontend (or other specified destination)
 * as though it is a SELECT result. These are used by utility commands that
 * need to project directly to the destination and don't need or want full
 * table function capability. Currently used by EXPLAIN and SHOW ALL.
 */
TupOutputState *
begin_tup_output_tupdesc(DestReceiver *dest,
						 TupleDesc tupdesc,
						 const TupleTableSlotOps *tts_ops)
{
	TupOutputState *tstate;

	tstate = (TupOutputState *) palloc(sizeof(TupOutputState));

	tstate->slot = MakeSingleTupleTableSlot(tupdesc, tts_ops);
	tstate->dest = dest;

	tstate->dest->rStartup(tstate->dest, (int) CMD_SELECT, tupdesc);

	return tstate;
}

/*
 * write a single tuple
 */
void
do_tup_output(TupOutputState *tstate, Datum *values, bool *isnull)
{
	TupleTableSlot *slot = tstate->slot;
	int			natts = slot->tts_tupleDescriptor->natts;

	/* make sure the slot is clear */
	ExecClearTuple(slot);

	/* insert data */
	memcpy(slot->tts_values, values, natts * sizeof(Datum));
	memcpy(slot->tts_isnull, isnull, natts * sizeof(bool));

	/* mark slot as containing a virtual tuple */
	ExecStoreVirtualTuple(slot);

	/* send the tuple to the receiver */
	(void) tstate->dest->receiveSlot(slot, tstate->dest);

	/* clean up */
	ExecClearTuple(slot);
}

/*
 * write a chunk of text, breaking at newline characters
 *
 * Should only be used with a single-TEXT-attribute tupdesc.
 */
void
do_text_output_multiline(TupOutputState *tstate, const char *txt)
{
	Datum		values[1];
	bool		isnull[1] = {false};

	while (*txt)
	{
		const char *eol;
		int			len;

		eol = strchr(txt, '\n');
		if (eol)
		{
			len = eol - txt;
			eol++;
		}
		else
		{
			len = strlen(txt);
			eol = txt + len;
		}

		values[0] = PointerGetDatum(cstring_to_text_with_len(txt, len));
		do_tup_output(tstate, values, isnull);
		pfree(DatumGetPointer(values[0]));
		txt = eol;
	}
}

void
end_tup_output(TupOutputState *tstate)
{
	tstate->dest->rShutdown(tstate->dest);
	/* note that destroying the dest is not ours to do */
	ExecDropSingleTupleTableSlot(tstate->slot);
	pfree(tstate);
}
