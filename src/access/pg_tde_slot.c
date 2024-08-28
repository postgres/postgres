/*-------------------------------------------------------------------------
 *
 * pg_tdeam.c
 *	  pg_tde TupleTableSlot implementation code
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2024, Percona
 *
 *
 * IDENTIFICATION
 *	  contrib/pg_tde/access/pg_tde_slot.c
 *
 *
 */
#include "postgres.h"
#include "pg_tde_defines.h"
#include "access/pg_tde_slot.h"
#include "access/heaptoast.h"
#include "access/htup_details.h"
#include "access/tupdesc_details.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "nodes/nodeFuncs.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"
#include "utils/expandeddatum.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"
#include "encryption/enc_tde.h"

/*
 * TTSOpsTDEBufferHeapTuple is effectively the same as TTSOpsBufferHeapTuple slot.
 * The only difference is that it keeps the reference of the decrypted tuple
 * and free it during clear slot operation
 */

const TupleTableSlotOps TTSOpsTDEBufferHeapTuple;

static pg_attribute_always_inline void tdeheap_slot_deform_heap_tuple(TupleTableSlot *slot, HeapTuple tuple, uint32 *offp, int natts);
static inline void tdeheap_tts_buffer_heap_store_tuple(TupleTableSlot *slot,
											   HeapTuple tuple,
											   Buffer buffer,
											   bool transfer_pin);

static void
tdeheap_tts_buffer_heap_init(TupleTableSlot *slot)
{
    TDEBufferHeapTupleTableSlot *bslot = (TDEBufferHeapTupleTableSlot *) slot;
}

static void
tdeheap_tts_buffer_heap_release(TupleTableSlot *slot)
{
	TDEBufferHeapTupleTableSlot *bslot = (TDEBufferHeapTupleTableSlot *) slot;
}

static void
tdeheap_tts_buffer_heap_clear(TupleTableSlot *slot)
{
	TDEBufferHeapTupleTableSlot *bslot = (TDEBufferHeapTupleTableSlot *) slot;

	/*
	 * Free the memory for heap tuple if allowed. A tuple coming from buffer
	 * can never be freed. But we may have materialized a tuple from buffer.
	 * Such a tuple can be freed.
	 */
	if (TTS_SHOULDFREE(slot))
	{
		/* We should have unpinned the buffer while materializing the tuple. */
		Assert(!BufferIsValid(bslot->buffer));

		tdeheap_freetuple(bslot->base.tuple);
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
tdeheap_tts_buffer_heap_getsomeattrs(TupleTableSlot *slot, int natts)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;

	Assert(!TTS_EMPTY(slot));

	tdeheap_slot_deform_heap_tuple(slot, bslot->base.tuple, &bslot->base.off, natts);
}

static Datum
tdeheap_tts_buffer_heap_getsysattr(TupleTableSlot *slot, int attnum, bool *isnull)
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

	return tdeheap_getsysattr(bslot->base.tuple, attnum,
						slot->tts_tupleDescriptor, isnull);
}

static bool
tdeheap_buffer_is_current_xact_tuple(TupleTableSlot *slot)
{
        BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;
        TransactionId xmin;

        Assert(!TTS_EMPTY(slot));

        /*
         * In some code paths it's possible to get here with a non-materialized
         * slot, in which case we can't check if tuple is created by the current
         * transaction.
         */
        if (!bslot->base.tuple)
                ereport(ERROR,
                                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                                 errmsg("don't have a storage tuple in this context")));

        xmin = HeapTupleHeaderGetRawXmin(bslot->base.tuple->t_data);

        return TransactionIdIsCurrentTransactionId(xmin);
}

static void
tdeheap_tts_buffer_heap_materialize(TupleTableSlot *slot)
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
		bslot->base.tuple = tdeheap_form_tuple(slot->tts_tupleDescriptor,
											slot->tts_values,
											slot->tts_isnull);
	}
	else
	{
		bslot->base.tuple = tdeheap_copytuple(bslot->base.tuple);

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
tdeheap_tts_buffer_heap_copyslot(TupleTableSlot *dstslot, TupleTableSlot *srcslot)
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

		tdeheap_tts_buffer_heap_store_tuple(dstslot, bsrcslot->base.tuple,
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
tdeheap_tts_buffer_heap_copy_heap_tuple(TupleTableSlot *slot)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;

	Assert(!TTS_EMPTY(slot));

	if (!bslot->base.tuple)
		tdeheap_tts_buffer_heap_materialize(slot);

	return tdeheap_copytuple(bslot->base.tuple);
}

static MinimalTuple
tdeheap_tts_buffer_heap_copy_minimal_tuple(TupleTableSlot *slot)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;

	Assert(!TTS_EMPTY(slot));

	if (!bslot->base.tuple)
		tdeheap_tts_buffer_heap_materialize(slot);

	return minimal_tuple_from_heap_tuple(bslot->base.tuple);
}

static inline void
tdeheap_tts_buffer_heap_store_tuple(TupleTableSlot *slot, HeapTuple tuple,
								   Buffer buffer, bool transfer_pin)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;

	if (TTS_SHOULDFREE(slot))
	{
		/* materialized slot shouldn't have a buffer to release */
		Assert(!BufferIsValid(bslot->buffer));

		tdeheap_freetuple(bslot->base.tuple);
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
 *		This is essentially an incremental version of tdeheap_deform_tuple:
 *		on each call we extract attributes up to the one needed, without
 *		re-computing information about previously extracted attributes.
 *		slot->tts_nvalid is the number of attributes already extracted.
 *
 * This is marked as always inline, so the different offp for different types
 * of slots gets optimized away.
 */
static pg_attribute_always_inline void
tdeheap_slot_deform_heap_tuple(TupleTableSlot *slot, HeapTuple tuple, uint32 *offp,
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

static HeapTuple
slot_copytuple(void* buffer, HeapTuple tuple)
{
	HeapTuple	newTuple;

	if (!HeapTupleIsValid(tuple) || tuple->t_data == NULL)
		return NULL;

	newTuple = (HeapTuple) buffer;
	newTuple->t_len = tuple->t_len;
	newTuple->t_self = tuple->t_self;
	newTuple->t_tableOid = tuple->t_tableOid;
	newTuple->t_data = (HeapTupleHeader) ((char *) newTuple + HEAPTUPLESIZE);
	// TODO: no need for this memcpy, don't decrypt in place instead!
	memcpy((char *) newTuple->t_data, (char *) tuple->t_data, tuple->t_len);
	return newTuple;
}

const TupleTableSlotOps TTSOpsTDEBufferHeapTuple = {
	.base_slot_size = sizeof(TDEBufferHeapTupleTableSlot),
	.init = tdeheap_tts_buffer_heap_init,
	.release = tdeheap_tts_buffer_heap_release,
	.clear = tdeheap_tts_buffer_heap_clear,
	.getsomeattrs = tdeheap_tts_buffer_heap_getsomeattrs,
	.getsysattr = tdeheap_tts_buffer_heap_getsysattr,
	.materialize = tdeheap_tts_buffer_heap_materialize,
#if PG_VERSION_NUM >= 170000
	.is_current_xact_tuple = tdeheap_buffer_is_current_xact_tuple,
#endif
	.copyslot = tdeheap_tts_buffer_heap_copyslot,
	.get_heap_tuple = NULL,

	/* A buffer heap tuple table slot can not "own" a minimal tuple. */
	.get_minimal_tuple = NULL,
	.copy_heap_tuple = tdeheap_tts_buffer_heap_copy_heap_tuple,
	.copy_minimal_tuple = tdeheap_tts_buffer_heap_copy_minimal_tuple};

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
PGTdeExecStoreBufferHeapTuple(Relation rel,
                         HeapTuple tuple,
						 TupleTableSlot *slot,
						 Buffer buffer)
{

	TDEBufferHeapTupleTableSlot *bslot = (TDEBufferHeapTupleTableSlot *)slot;
	/*
	 * sanity checks
	 */
	Assert(rel != NULL);
	Assert(tuple != NULL);
	Assert(slot != NULL);
	Assert(slot->tts_tupleDescriptor != NULL);
	Assert(BufferIsValid(buffer));

	if (unlikely(!TTS_IS_TDE_BUFFERTUPLE(slot)))
		elog(ERROR, "trying to store an on-disk heap tuple into wrong type of slot");

	if (rel->rd_rel->relkind != RELKIND_TOASTVALUE)
	{
		RelKeyData *key = GetRelationKey(rel->rd_locator);
		slot_copytuple(bslot->decrypted_buffer, tuple);
		PG_TDE_DECRYPT_TUPLE_EX(tuple, (HeapTuple)bslot->decrypted_buffer, key, "ExecStoreBuffer");
		tuple->t_data = ((HeapTuple)bslot->decrypted_buffer)->t_data;
	}

	tdeheap_tts_buffer_heap_store_tuple(slot, tuple, buffer, false);

	slot->tts_tableOid = tuple->t_tableOid;

	return slot;
}

/*
 * Like ExecStoreBufferHeapTuple, but transfer an existing pin from the caller
 * to the slot, i.e. the caller doesn't need to, and may not, release the pin.
 */
TupleTableSlot *
PGTdeExecStorePinnedBufferHeapTuple(Relation rel,
                             HeapTuple tuple,
                             TupleTableSlot *slot,
                             Buffer buffer)
{
	TDEBufferHeapTupleTableSlot *bslot = (TDEBufferHeapTupleTableSlot *)slot;
	/*
	 * sanity checks
	 */
	Assert(rel != NULL);
	Assert(tuple != NULL);
	Assert(slot != NULL);
	Assert(slot->tts_tupleDescriptor != NULL);
	Assert(BufferIsValid(buffer));

	if (unlikely(!TTS_IS_TDE_BUFFERTUPLE(slot)))
		elog(ERROR, "trying to store an on-disk heap tuple into wrong type of slot");

	if (rel->rd_rel->relkind != RELKIND_TOASTVALUE)
	{
		RelKeyData *key = GetRelationKey(rel->rd_locator);

		slot_copytuple(bslot->decrypted_buffer, tuple);
		PG_TDE_DECRYPT_TUPLE_EX(tuple, (HeapTuple)bslot->decrypted_buffer, key, "ExecStorePinnedBuffer");
		/* TODO: revisit this */
		tuple->t_data = ((HeapTuple)bslot->decrypted_buffer)->t_data;
	}

	tdeheap_tts_buffer_heap_store_tuple(slot, tuple, buffer, true);

	slot->tts_tableOid = tuple->t_tableOid;

	return slot;
}
