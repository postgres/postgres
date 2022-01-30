/*-------------------------------------------------------------------------
 *
 * pg_visibility.c
 *	  display visibility map information and page-level visibility bits
 *
 * Copyright (c) 2016-2022, PostgreSQL Global Development Group
 *
 *	  contrib/pg_visibility/pg_visibility.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/visibilitymap.h"
#include "access/xloginsert.h"
#include "catalog/pg_type.h"
#include "catalog/storage_xlog.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

PG_MODULE_MAGIC;

typedef struct vbits
{
	BlockNumber next;
	BlockNumber count;
	uint8		bits[FLEXIBLE_ARRAY_MEMBER];
} vbits;

typedef struct corrupt_items
{
	BlockNumber next;
	BlockNumber count;
	ItemPointer tids;
} corrupt_items;

PG_FUNCTION_INFO_V1(pg_visibility_map);
PG_FUNCTION_INFO_V1(pg_visibility_map_rel);
PG_FUNCTION_INFO_V1(pg_visibility);
PG_FUNCTION_INFO_V1(pg_visibility_rel);
PG_FUNCTION_INFO_V1(pg_visibility_map_summary);
PG_FUNCTION_INFO_V1(pg_check_frozen);
PG_FUNCTION_INFO_V1(pg_check_visible);
PG_FUNCTION_INFO_V1(pg_truncate_visibility_map);

static TupleDesc pg_visibility_tupdesc(bool include_blkno, bool include_pd);
static vbits *collect_visibility_data(Oid relid, bool include_pd);
static corrupt_items *collect_corrupt_items(Oid relid, bool all_visible,
											bool all_frozen);
static void record_corrupt_item(corrupt_items *items, ItemPointer tid);
static bool tuple_all_visible(HeapTuple tup, TransactionId OldestXmin,
							  Buffer buffer);
static void check_relation_relkind(Relation rel);

/*
 * Visibility map information for a single block of a relation.
 *
 * Note: the VM code will silently return zeroes for pages past the end
 * of the map, so we allow probes up to MaxBlockNumber regardless of the
 * actual relation size.
 */
Datum
pg_visibility_map(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		blkno = PG_GETARG_INT64(1);
	int32		mapbits;
	Relation	rel;
	Buffer		vmbuffer = InvalidBuffer;
	TupleDesc	tupdesc;
	Datum		values[2];
	bool		nulls[2];

	rel = relation_open(relid, AccessShareLock);

	/* Only some relkinds have a visibility map */
	check_relation_relkind(rel);

	if (blkno < 0 || blkno > MaxBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid block number")));

	tupdesc = pg_visibility_tupdesc(false, false);
	MemSet(nulls, 0, sizeof(nulls));

	mapbits = (int32) visibilitymap_get_status(rel, blkno, &vmbuffer);
	if (vmbuffer != InvalidBuffer)
		ReleaseBuffer(vmbuffer);
	values[0] = BoolGetDatum((mapbits & VISIBILITYMAP_ALL_VISIBLE) != 0);
	values[1] = BoolGetDatum((mapbits & VISIBILITYMAP_ALL_FROZEN) != 0);

	relation_close(rel, AccessShareLock);

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * Visibility map information for a single block of a relation, plus the
 * page-level information for the same block.
 */
Datum
pg_visibility(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		blkno = PG_GETARG_INT64(1);
	int32		mapbits;
	Relation	rel;
	Buffer		vmbuffer = InvalidBuffer;
	Buffer		buffer;
	Page		page;
	TupleDesc	tupdesc;
	Datum		values[3];
	bool		nulls[3];

	rel = relation_open(relid, AccessShareLock);

	/* Only some relkinds have a visibility map */
	check_relation_relkind(rel);

	if (blkno < 0 || blkno > MaxBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid block number")));

	tupdesc = pg_visibility_tupdesc(false, true);
	MemSet(nulls, 0, sizeof(nulls));

	mapbits = (int32) visibilitymap_get_status(rel, blkno, &vmbuffer);
	if (vmbuffer != InvalidBuffer)
		ReleaseBuffer(vmbuffer);
	values[0] = BoolGetDatum((mapbits & VISIBILITYMAP_ALL_VISIBLE) != 0);
	values[1] = BoolGetDatum((mapbits & VISIBILITYMAP_ALL_FROZEN) != 0);

	/* Here we have to explicitly check rel size ... */
	if (blkno < RelationGetNumberOfBlocks(rel))
	{
		buffer = ReadBuffer(rel, blkno);
		LockBuffer(buffer, BUFFER_LOCK_SHARE);

		page = BufferGetPage(buffer);
		values[2] = BoolGetDatum(PageIsAllVisible(page));

		UnlockReleaseBuffer(buffer);
	}
	else
	{
		/* As with the vismap, silently return 0 for pages past EOF */
		values[2] = BoolGetDatum(false);
	}

	relation_close(rel, AccessShareLock);

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * Visibility map information for every block in a relation.
 */
Datum
pg_visibility_map_rel(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	vbits	   *info;

	if (SRF_IS_FIRSTCALL())
	{
		Oid			relid = PG_GETARG_OID(0);
		MemoryContext oldcontext;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		funcctx->tuple_desc = pg_visibility_tupdesc(true, false);
		/* collect_visibility_data will verify the relkind */
		funcctx->user_fctx = collect_visibility_data(relid, false);
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	info = (vbits *) funcctx->user_fctx;

	if (info->next < info->count)
	{
		Datum		values[3];
		bool		nulls[3];
		HeapTuple	tuple;

		MemSet(nulls, 0, sizeof(nulls));
		values[0] = Int64GetDatum(info->next);
		values[1] = BoolGetDatum((info->bits[info->next] & (1 << 0)) != 0);
		values[2] = BoolGetDatum((info->bits[info->next] & (1 << 1)) != 0);
		info->next++;

		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}

	SRF_RETURN_DONE(funcctx);
}

/*
 * Visibility map information for every block in a relation, plus the page
 * level information for each block.
 */
Datum
pg_visibility_rel(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	vbits	   *info;

	if (SRF_IS_FIRSTCALL())
	{
		Oid			relid = PG_GETARG_OID(0);
		MemoryContext oldcontext;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		funcctx->tuple_desc = pg_visibility_tupdesc(true, true);
		/* collect_visibility_data will verify the relkind */
		funcctx->user_fctx = collect_visibility_data(relid, true);
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	info = (vbits *) funcctx->user_fctx;

	if (info->next < info->count)
	{
		Datum		values[4];
		bool		nulls[4];
		HeapTuple	tuple;

		MemSet(nulls, 0, sizeof(nulls));
		values[0] = Int64GetDatum(info->next);
		values[1] = BoolGetDatum((info->bits[info->next] & (1 << 0)) != 0);
		values[2] = BoolGetDatum((info->bits[info->next] & (1 << 1)) != 0);
		values[3] = BoolGetDatum((info->bits[info->next] & (1 << 2)) != 0);
		info->next++;

		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}

	SRF_RETURN_DONE(funcctx);
}

/*
 * Count the number of all-visible and all-frozen pages in the visibility
 * map for a particular relation.
 */
Datum
pg_visibility_map_summary(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel;
	BlockNumber nblocks;
	BlockNumber blkno;
	Buffer		vmbuffer = InvalidBuffer;
	int64		all_visible = 0;
	int64		all_frozen = 0;
	TupleDesc	tupdesc;
	Datum		values[2];
	bool		nulls[2];

	rel = relation_open(relid, AccessShareLock);

	/* Only some relkinds have a visibility map */
	check_relation_relkind(rel);

	nblocks = RelationGetNumberOfBlocks(rel);

	for (blkno = 0; blkno < nblocks; ++blkno)
	{
		int32		mapbits;

		/* Make sure we are interruptible. */
		CHECK_FOR_INTERRUPTS();

		/* Get map info. */
		mapbits = (int32) visibilitymap_get_status(rel, blkno, &vmbuffer);
		if ((mapbits & VISIBILITYMAP_ALL_VISIBLE) != 0)
			++all_visible;
		if ((mapbits & VISIBILITYMAP_ALL_FROZEN) != 0)
			++all_frozen;
	}

	/* Clean up. */
	if (vmbuffer != InvalidBuffer)
		ReleaseBuffer(vmbuffer);
	relation_close(rel, AccessShareLock);

	tupdesc = CreateTemplateTupleDesc(2);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "all_visible", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "all_frozen", INT8OID, -1, 0);
	tupdesc = BlessTupleDesc(tupdesc);

	MemSet(nulls, 0, sizeof(nulls));
	values[0] = Int64GetDatum(all_visible);
	values[1] = Int64GetDatum(all_frozen);

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * Return the TIDs of non-frozen tuples present in pages marked all-frozen
 * in the visibility map.  We hope no one will ever find any, but there could
 * be bugs, database corruption, etc.
 */
Datum
pg_check_frozen(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	corrupt_items *items;

	if (SRF_IS_FIRSTCALL())
	{
		Oid			relid = PG_GETARG_OID(0);
		MemoryContext oldcontext;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		/* collect_corrupt_items will verify the relkind */
		funcctx->user_fctx = collect_corrupt_items(relid, false, true);
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	items = (corrupt_items *) funcctx->user_fctx;

	if (items->next < items->count)
		SRF_RETURN_NEXT(funcctx, PointerGetDatum(&items->tids[items->next++]));

	SRF_RETURN_DONE(funcctx);
}

/*
 * Return the TIDs of not-all-visible tuples in pages marked all-visible
 * in the visibility map.  We hope no one will ever find any, but there could
 * be bugs, database corruption, etc.
 */
Datum
pg_check_visible(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	corrupt_items *items;

	if (SRF_IS_FIRSTCALL())
	{
		Oid			relid = PG_GETARG_OID(0);
		MemoryContext oldcontext;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		/* collect_corrupt_items will verify the relkind */
		funcctx->user_fctx = collect_corrupt_items(relid, true, false);
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	items = (corrupt_items *) funcctx->user_fctx;

	if (items->next < items->count)
		SRF_RETURN_NEXT(funcctx, PointerGetDatum(&items->tids[items->next++]));

	SRF_RETURN_DONE(funcctx);
}

/*
 * Remove the visibility map fork for a relation.  If there turn out to be
 * any bugs in the visibility map code that require rebuilding the VM, this
 * provides users with a way to do it that is cleaner than shutting down the
 * server and removing files by hand.
 *
 * This is a cut-down version of RelationTruncate.
 */
Datum
pg_truncate_visibility_map(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel;
	ForkNumber	fork;
	BlockNumber block;

	rel = relation_open(relid, AccessExclusiveLock);

	/* Only some relkinds have a visibility map */
	check_relation_relkind(rel);

	/* Forcibly reset cached file size */
	RelationGetSmgr(rel)->smgr_cached_nblocks[VISIBILITYMAP_FORKNUM] = InvalidBlockNumber;

	block = visibilitymap_prepare_truncate(rel, 0);
	if (BlockNumberIsValid(block))
	{
		fork = VISIBILITYMAP_FORKNUM;
		smgrtruncate(RelationGetSmgr(rel), &fork, 1, &block);
	}

	if (RelationNeedsWAL(rel))
	{
		xl_smgr_truncate xlrec;

		xlrec.blkno = 0;
		xlrec.rnode = rel->rd_node;
		xlrec.flags = SMGR_TRUNCATE_VM;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, sizeof(xlrec));

		XLogInsert(RM_SMGR_ID, XLOG_SMGR_TRUNCATE | XLR_SPECIAL_REL_UPDATE);
	}

	/*
	 * Release the lock right away, not at commit time.
	 *
	 * It would be a problem to release the lock prior to commit if this
	 * truncate operation sends any transactional invalidation messages. Other
	 * backends would potentially be able to lock the relation without
	 * processing them in the window of time between when we release the lock
	 * here and when we sent the messages at our eventual commit.  However,
	 * we're currently only sending a non-transactional smgr invalidation,
	 * which will have been posted to shared memory immediately from within
	 * smgr_truncate.  Therefore, there should be no race here.
	 *
	 * The reason why it's desirable to release the lock early here is because
	 * of the possibility that someone will need to use this to blow away many
	 * visibility map forks at once.  If we can't release the lock until
	 * commit time, the transaction doing this will accumulate
	 * AccessExclusiveLocks on all of those relations at the same time, which
	 * is undesirable. However, if this turns out to be unsafe we may have no
	 * choice...
	 */
	relation_close(rel, AccessExclusiveLock);

	/* Nothing to return. */
	PG_RETURN_VOID();
}

/*
 * Helper function to construct whichever TupleDesc we need for a particular
 * call.
 */
static TupleDesc
pg_visibility_tupdesc(bool include_blkno, bool include_pd)
{
	TupleDesc	tupdesc;
	AttrNumber	maxattr = 2;
	AttrNumber	a = 0;

	if (include_blkno)
		++maxattr;
	if (include_pd)
		++maxattr;
	tupdesc = CreateTemplateTupleDesc(maxattr);
	if (include_blkno)
		TupleDescInitEntry(tupdesc, ++a, "blkno", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, ++a, "all_visible", BOOLOID, -1, 0);
	TupleDescInitEntry(tupdesc, ++a, "all_frozen", BOOLOID, -1, 0);
	if (include_pd)
		TupleDescInitEntry(tupdesc, ++a, "pd_all_visible", BOOLOID, -1, 0);
	Assert(a == maxattr);

	return BlessTupleDesc(tupdesc);
}

/*
 * Collect visibility data about a relation.
 *
 * Checks relkind of relid and will throw an error if the relation does not
 * have a VM.
 */
static vbits *
collect_visibility_data(Oid relid, bool include_pd)
{
	Relation	rel;
	BlockNumber nblocks;
	vbits	   *info;
	BlockNumber blkno;
	Buffer		vmbuffer = InvalidBuffer;
	BufferAccessStrategy bstrategy = GetAccessStrategy(BAS_BULKREAD);

	rel = relation_open(relid, AccessShareLock);

	/* Only some relkinds have a visibility map */
	check_relation_relkind(rel);

	nblocks = RelationGetNumberOfBlocks(rel);
	info = palloc0(offsetof(vbits, bits) + nblocks);
	info->next = 0;
	info->count = nblocks;

	for (blkno = 0; blkno < nblocks; ++blkno)
	{
		int32		mapbits;

		/* Make sure we are interruptible. */
		CHECK_FOR_INTERRUPTS();

		/* Get map info. */
		mapbits = (int32) visibilitymap_get_status(rel, blkno, &vmbuffer);
		if ((mapbits & VISIBILITYMAP_ALL_VISIBLE) != 0)
			info->bits[blkno] |= (1 << 0);
		if ((mapbits & VISIBILITYMAP_ALL_FROZEN) != 0)
			info->bits[blkno] |= (1 << 1);

		/*
		 * Page-level data requires reading every block, so only get it if the
		 * caller needs it.  Use a buffer access strategy, too, to prevent
		 * cache-trashing.
		 */
		if (include_pd)
		{
			Buffer		buffer;
			Page		page;

			buffer = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL,
										bstrategy);
			LockBuffer(buffer, BUFFER_LOCK_SHARE);

			page = BufferGetPage(buffer);
			if (PageIsAllVisible(page))
				info->bits[blkno] |= (1 << 2);

			UnlockReleaseBuffer(buffer);
		}
	}

	/* Clean up. */
	if (vmbuffer != InvalidBuffer)
		ReleaseBuffer(vmbuffer);
	relation_close(rel, AccessShareLock);

	return info;
}

/*
 * Returns a list of items whose visibility map information does not match
 * the status of the tuples on the page.
 *
 * If all_visible is passed as true, this will include all items which are
 * on pages marked as all-visible in the visibility map but which do not
 * seem to in fact be all-visible.
 *
 * If all_frozen is passed as true, this will include all items which are
 * on pages marked as all-frozen but which do not seem to in fact be frozen.
 *
 * Checks relkind of relid and will throw an error if the relation does not
 * have a VM.
 */
static corrupt_items *
collect_corrupt_items(Oid relid, bool all_visible, bool all_frozen)
{
	Relation	rel;
	BlockNumber nblocks;
	corrupt_items *items;
	BlockNumber blkno;
	Buffer		vmbuffer = InvalidBuffer;
	BufferAccessStrategy bstrategy = GetAccessStrategy(BAS_BULKREAD);
	TransactionId OldestXmin = InvalidTransactionId;

	rel = relation_open(relid, AccessShareLock);

	/* Only some relkinds have a visibility map */
	check_relation_relkind(rel);

	if (all_visible)
		OldestXmin = GetOldestNonRemovableTransactionId(rel);

	nblocks = RelationGetNumberOfBlocks(rel);

	/*
	 * Guess an initial array size. We don't expect many corrupted tuples, so
	 * start with a small array.  This function uses the "next" field to track
	 * the next offset where we can store an item (which is the same thing as
	 * the number of items found so far) and the "count" field to track the
	 * number of entries allocated.  We'll repurpose these fields before
	 * returning.
	 */
	items = palloc0(sizeof(corrupt_items));
	items->next = 0;
	items->count = 64;
	items->tids = palloc(items->count * sizeof(ItemPointerData));

	/* Loop over every block in the relation. */
	for (blkno = 0; blkno < nblocks; ++blkno)
	{
		bool		check_frozen = false;
		bool		check_visible = false;
		Buffer		buffer;
		Page		page;
		OffsetNumber offnum,
					maxoff;

		/* Make sure we are interruptible. */
		CHECK_FOR_INTERRUPTS();

		/* Use the visibility map to decide whether to check this page. */
		if (all_frozen && VM_ALL_FROZEN(rel, blkno, &vmbuffer))
			check_frozen = true;
		if (all_visible && VM_ALL_VISIBLE(rel, blkno, &vmbuffer))
			check_visible = true;
		if (!check_visible && !check_frozen)
			continue;

		/* Read and lock the page. */
		buffer = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL,
									bstrategy);
		LockBuffer(buffer, BUFFER_LOCK_SHARE);

		page = BufferGetPage(buffer);
		maxoff = PageGetMaxOffsetNumber(page);

		/*
		 * The visibility map bits might have changed while we were acquiring
		 * the page lock.  Recheck to avoid returning spurious results.
		 */
		if (check_frozen && !VM_ALL_FROZEN(rel, blkno, &vmbuffer))
			check_frozen = false;
		if (check_visible && !VM_ALL_VISIBLE(rel, blkno, &vmbuffer))
			check_visible = false;
		if (!check_visible && !check_frozen)
		{
			UnlockReleaseBuffer(buffer);
			continue;
		}

		/* Iterate over each tuple on the page. */
		for (offnum = FirstOffsetNumber;
			 offnum <= maxoff;
			 offnum = OffsetNumberNext(offnum))
		{
			HeapTupleData tuple;
			ItemId		itemid;

			itemid = PageGetItemId(page, offnum);

			/* Unused or redirect line pointers are of no interest. */
			if (!ItemIdIsUsed(itemid) || ItemIdIsRedirected(itemid))
				continue;

			/* Dead line pointers are neither all-visible nor frozen. */
			if (ItemIdIsDead(itemid))
			{
				ItemPointerSet(&(tuple.t_self), blkno, offnum);
				record_corrupt_item(items, &tuple.t_self);
				continue;
			}

			/* Initialize a HeapTupleData structure for checks below. */
			ItemPointerSet(&(tuple.t_self), blkno, offnum);
			tuple.t_data = (HeapTupleHeader) PageGetItem(page, itemid);
			tuple.t_len = ItemIdGetLength(itemid);
			tuple.t_tableOid = relid;

			/*
			 * If we're checking whether the page is all-visible, we expect
			 * the tuple to be all-visible.
			 */
			if (check_visible &&
				!tuple_all_visible(&tuple, OldestXmin, buffer))
			{
				TransactionId RecomputedOldestXmin;

				/*
				 * Time has passed since we computed OldestXmin, so it's
				 * possible that this tuple is all-visible in reality even
				 * though it doesn't appear so based on our
				 * previously-computed value.  Let's compute a new value so we
				 * can be certain whether there is a problem.
				 *
				 * From a concurrency point of view, it sort of sucks to
				 * retake ProcArrayLock here while we're holding the buffer
				 * exclusively locked, but it should be safe against
				 * deadlocks, because surely
				 * GetOldestNonRemovableTransactionId() should never take a
				 * buffer lock. And this shouldn't happen often, so it's worth
				 * being careful so as to avoid false positives.
				 */
				RecomputedOldestXmin = GetOldestNonRemovableTransactionId(rel);

				if (!TransactionIdPrecedes(OldestXmin, RecomputedOldestXmin))
					record_corrupt_item(items, &tuple.t_self);
				else
				{
					OldestXmin = RecomputedOldestXmin;
					if (!tuple_all_visible(&tuple, OldestXmin, buffer))
						record_corrupt_item(items, &tuple.t_self);
				}
			}

			/*
			 * If we're checking whether the page is all-frozen, we expect the
			 * tuple to be in a state where it will never need freezing.
			 */
			if (check_frozen)
			{
				if (heap_tuple_needs_eventual_freeze(tuple.t_data))
					record_corrupt_item(items, &tuple.t_self);
			}
		}

		UnlockReleaseBuffer(buffer);
	}

	/* Clean up. */
	if (vmbuffer != InvalidBuffer)
		ReleaseBuffer(vmbuffer);
	relation_close(rel, AccessShareLock);

	/*
	 * Before returning, repurpose the fields to match caller's expectations.
	 * next is now the next item that should be read (rather than written) and
	 * count is now the number of items we wrote (rather than the number we
	 * allocated).
	 */
	items->count = items->next;
	items->next = 0;

	return items;
}

/*
 * Remember one corrupt item.
 */
static void
record_corrupt_item(corrupt_items *items, ItemPointer tid)
{
	/* enlarge output array if needed. */
	if (items->next >= items->count)
	{
		items->count *= 2;
		items->tids = repalloc(items->tids,
							   items->count * sizeof(ItemPointerData));
	}
	/* and add the new item */
	items->tids[items->next++] = *tid;
}

/*
 * Check whether a tuple is all-visible relative to a given OldestXmin value.
 * The buffer should contain the tuple and should be locked and pinned.
 */
static bool
tuple_all_visible(HeapTuple tup, TransactionId OldestXmin, Buffer buffer)
{
	HTSV_Result state;
	TransactionId xmin;

	state = HeapTupleSatisfiesVacuum(tup, OldestXmin, buffer);
	if (state != HEAPTUPLE_LIVE)
		return false;			/* all-visible implies live */

	/*
	 * Neither lazy_scan_heap nor heap_page_is_all_visible will mark a page
	 * all-visible unless every tuple is hinted committed. However, those hint
	 * bits could be lost after a crash, so we can't be certain that they'll
	 * be set here.  So just check the xmin.
	 */

	xmin = HeapTupleHeaderGetXmin(tup->t_data);
	if (!TransactionIdPrecedes(xmin, OldestXmin))
		return false;			/* xmin not old enough for all to see */

	return true;
}

/*
 * check_relation_relkind - convenience routine to check that relation
 * is of the relkind supported by the callers
 */
static void
check_relation_relkind(Relation rel)
{
	if (!RELKIND_HAS_TABLE_AM(rel->rd_rel->relkind))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" is of wrong relation kind",
						RelationGetRelationName(rel)),
				 errdetail_relkind_not_supported(rel->rd_rel->relkind)));
}
