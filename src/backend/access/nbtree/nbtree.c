/*-------------------------------------------------------------------------
 *
 * nbtree.c
 *	  Implementation of Lehman and Yao's btree management algorithm for
 *	  Postgres.
 *
 * NOTES
 *	  This file contains only the public interface routines.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/nbtree/nbtree.c,v 1.89 2002/05/20 23:51:41 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/nbtree.h"
#include "catalog/index.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "storage/sinval.h"
#include "access/xlogutils.h"


/* Working state for btbuild and its callback */
typedef struct
{
	bool		usefast;
	bool		isUnique;
	bool		haveDead;
	Relation	heapRel;
	BTSpool    *spool;

	/*
	 * spool2 is needed only when the index is an unique index. Dead
	 * tuples are put into spool2 instead of spool in order to avoid
	 * uniqueness check.
	 */
	BTSpool    *spool2;
	double		indtuples;
} BTBuildState;


bool		BuildingBtree = false;		/* see comment in btbuild() */
bool		FastBuild = true;	/* use SORT instead of insertion build */

/*
 * TEMPORARY FLAG FOR TESTING NEW FIX TREE
 * CODE WITHOUT AFFECTING ANYONE ELSE
 */
bool		FixBTree = true;

static void _bt_restscan(IndexScanDesc scan);
static void btbuildCallback(Relation index,
				HeapTuple htup,
				Datum *attdata,
				char *nulls,
				bool tupleIsAlive,
				void *state);


/*
 * AtEOXact_nbtree() --- clean up nbtree subsystem at xact abort or commit.
 */
void
AtEOXact_nbtree(void)
{
	/*
	 * Note: these actions should only be necessary during xact abort; but
	 * they can't hurt during a commit.
	 */

	/* If we were building a btree, we ain't anymore. */
	BuildingBtree = false;
}


/*
 *	btbuild() -- build a new btree index.
 *
 *		We use a global variable to record the fact that we're creating
 *		a new index.  This is used to avoid high-concurrency locking,
 *		since the index won't be visible until this transaction commits
 *		and since building is guaranteed to be single-threaded.
 */
Datum
btbuild(PG_FUNCTION_ARGS)
{
	Relation	heap = (Relation) PG_GETARG_POINTER(0);
	Relation	index = (Relation) PG_GETARG_POINTER(1);
	IndexInfo  *indexInfo = (IndexInfo *) PG_GETARG_POINTER(2);
	double		reltuples;
	BTBuildState buildstate;

	/* set flag to disable locking */
	BuildingBtree = true;

	/*
	 * bootstrap processing does something strange, so don't use
	 * sort/build for initial catalog indices.	at some point i need to
	 * look harder at this.  (there is some kind of incremental processing
	 * going on there.) -- pma 08/29/95
	 */
	buildstate.usefast = (FastBuild && IsNormalProcessingMode());
	buildstate.isUnique = indexInfo->ii_Unique;
	buildstate.haveDead = false;
	buildstate.heapRel = heap;
	buildstate.spool = NULL;
	buildstate.spool2 = NULL;
	buildstate.indtuples = 0;

#ifdef BTREE_BUILD_STATS
	if (Show_btree_build_stats)
		ResetUsage();
#endif   /* BTREE_BUILD_STATS */

	/*
	 * We expect to be called exactly once for any index relation. If
	 * that's not the case, big trouble's what we have.
	 */
	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "%s already contains data",
			 RelationGetRelationName(index));

	/* initialize the btree index metadata page */
	_bt_metapinit(index);

	if (buildstate.usefast)
	{
		buildstate.spool = _bt_spoolinit(index, indexInfo->ii_Unique);

		/*
		 * Different from spool, the uniqueness isn't checked for spool2.
		 */
		if (indexInfo->ii_Unique)
			buildstate.spool2 = _bt_spoolinit(index, false);
	}

	/* do the heap scan */
	reltuples = IndexBuildHeapScan(heap, index, indexInfo,
								   btbuildCallback, (void *) &buildstate);

	/* okay, all heap tuples are indexed */
	if (buildstate.spool2 && !buildstate.haveDead)
	{
		/* spool2 turns out to be unnecessary */
		_bt_spooldestroy(buildstate.spool2);
		buildstate.spool2 = NULL;
	}

	/*
	 * if we are doing bottom-up btree build, finish the build by (1)
	 * completing the sort of the spool file, (2) inserting the sorted
	 * tuples into btree pages and (3) building the upper levels.
	 */
	if (buildstate.usefast)
	{
		_bt_leafbuild(buildstate.spool, buildstate.spool2);
		_bt_spooldestroy(buildstate.spool);
		if (buildstate.spool2)
			_bt_spooldestroy(buildstate.spool2);
	}

#ifdef BTREE_BUILD_STATS
	if (Show_btree_build_stats)
	{
		ShowUsage("BTREE BUILD STATS");
		ResetUsage();
	}
#endif   /* BTREE_BUILD_STATS */

	/* all done */
	BuildingBtree = false;

	/*
	 * Since we just counted the tuples in the heap, we update its stats
	 * in pg_class to guarantee that the planner takes advantage of the
	 * index we just created.  But, only update statistics during normal
	 * index definitions, not for indices on system catalogs created
	 * during bootstrap processing.  We must close the relations before
	 * updating statistics to guarantee that the relcache entries are
	 * flushed when we increment the command counter in UpdateStats(). But
	 * we do not release any locks on the relations; those will be held
	 * until end of transaction.
	 */
	if (IsNormalProcessingMode())
	{
		Oid			hrelid = RelationGetRelid(heap);
		Oid			irelid = RelationGetRelid(index);

		heap_close(heap, NoLock);
		index_close(index);
		UpdateStats(hrelid, reltuples);
		UpdateStats(irelid, buildstate.indtuples);
	}

	PG_RETURN_VOID();
}

/*
 * Per-tuple callback from IndexBuildHeapScan
 */
static void
btbuildCallback(Relation index,
				HeapTuple htup,
				Datum *attdata,
				char *nulls,
				bool tupleIsAlive,
				void *state)
{
	BTBuildState *buildstate = (BTBuildState *) state;
	IndexTuple	itup;
	BTItem		btitem;
	InsertIndexResult res;

	/* form an index tuple and point it at the heap tuple */
	itup = index_formtuple(RelationGetDescr(index), attdata, nulls);
	itup->t_tid = htup->t_self;

	btitem = _bt_formitem(itup);

	/*
	 * if we are doing bottom-up btree build, we insert the index into a
	 * spool file for subsequent processing.  otherwise, we insert into
	 * the btree.
	 */
	if (buildstate->usefast)
	{
		if (tupleIsAlive || buildstate->spool2 == NULL)
			_bt_spool(btitem, buildstate->spool);
		else
		{
			/* dead tuples are put into spool2 */
			buildstate->haveDead = true;
			_bt_spool(btitem, buildstate->spool2);
		}
	}
	else
	{
		res = _bt_doinsert(index, btitem,
						   buildstate->isUnique, buildstate->heapRel);
		if (res)
			pfree(res);
	}

	buildstate->indtuples += 1;

	pfree(btitem);
	pfree(itup);
}

/*
 *	btinsert() -- insert an index tuple into a btree.
 *
 *		Descend the tree recursively, find the appropriate location for our
 *		new tuple, put it there, set its unique OID as appropriate, and
 *		return an InsertIndexResult to the caller.
 */
Datum
btinsert(PG_FUNCTION_ARGS)
{
	Relation	rel = (Relation) PG_GETARG_POINTER(0);
	Datum	   *datum = (Datum *) PG_GETARG_POINTER(1);
	char	   *nulls = (char *) PG_GETARG_POINTER(2);
	ItemPointer ht_ctid = (ItemPointer) PG_GETARG_POINTER(3);
	Relation	heapRel = (Relation) PG_GETARG_POINTER(4);
	InsertIndexResult res;
	BTItem		btitem;
	IndexTuple	itup;

	/* generate an index tuple */
	itup = index_formtuple(RelationGetDescr(rel), datum, nulls);
	itup->t_tid = *ht_ctid;
	btitem = _bt_formitem(itup);

	res = _bt_doinsert(rel, btitem, rel->rd_uniqueindex, heapRel);

	pfree(btitem);
	pfree(itup);

	PG_RETURN_POINTER(res);
}

/*
 *	btgettuple() -- Get the next tuple in the scan.
 */
Datum
btgettuple(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ScanDirection dir = (ScanDirection) PG_GETARG_INT32(1);
	bool res;

	/*
	 * If we've already initialized this scan, we can just advance it in
	 * the appropriate direction.  If we haven't done so yet, we call a
	 * routine to get the first item in the scan.
	 */

	if (ItemPointerIsValid(&(scan->currentItemData)))
	{
		/*
		 * Restore scan position using heap TID returned by previous call
		 * to btgettuple(). _bt_restscan() re-grabs the read lock on the
		 * buffer, too.
		 */
		_bt_restscan(scan);
		res = _bt_next(scan, dir);
	}
	else
		res = _bt_first(scan, dir);

	/*
	 * Save heap TID to use it in _bt_restscan.  Then release the read
	 * lock on the buffer so that we aren't blocking other backends.
	 *
	 * NOTE: we do keep the pin on the buffer!	This is essential to ensure
	 * that someone else doesn't delete the index entry we are stopped on.
	 */
	if (res)
	{
		((BTScanOpaque) scan->opaque)->curHeapIptr = scan->xs_ctup.t_self;
		LockBuffer(((BTScanOpaque) scan->opaque)->btso_curbuf,
				   BUFFER_LOCK_UNLOCK);
	}

	PG_RETURN_BOOL(res);
}

/*
 *	btbeginscan() -- start a scan on a btree index
 */
Datum
btbeginscan(PG_FUNCTION_ARGS)
{
	Relation	rel = (Relation) PG_GETARG_POINTER(0);
	int			keysz = PG_GETARG_INT32(1);
	ScanKey		scankey = (ScanKey) PG_GETARG_POINTER(2);
	IndexScanDesc scan;

	/* get the scan */
	scan = RelationGetIndexScan(rel, keysz, scankey);

	PG_RETURN_POINTER(scan);
}

/*
 *	btrescan() -- rescan an index relation
 */
Datum
btrescan(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ScanKey		scankey = (ScanKey) PG_GETARG_POINTER(1);
	ItemPointer iptr;
	BTScanOpaque so;

	so = (BTScanOpaque) scan->opaque;

	if (so == NULL)				/* if called from btbeginscan */
	{
		so = (BTScanOpaque) palloc(sizeof(BTScanOpaqueData));
		so->btso_curbuf = so->btso_mrkbuf = InvalidBuffer;
		ItemPointerSetInvalid(&(so->curHeapIptr));
		ItemPointerSetInvalid(&(so->mrkHeapIptr));
		if (scan->numberOfKeys > 0)
			so->keyData = (ScanKey) palloc(scan->numberOfKeys * sizeof(ScanKeyData));
		else
			so->keyData = (ScanKey) NULL;
		scan->opaque = so;
	}

	/* we aren't holding any read locks, but gotta drop the pins */
	if (ItemPointerIsValid(iptr = &(scan->currentItemData)))
	{
		ReleaseBuffer(so->btso_curbuf);
		so->btso_curbuf = InvalidBuffer;
		ItemPointerSetInvalid(&(so->curHeapIptr));
		ItemPointerSetInvalid(iptr);
	}

	if (ItemPointerIsValid(iptr = &(scan->currentMarkData)))
	{
		ReleaseBuffer(so->btso_mrkbuf);
		so->btso_mrkbuf = InvalidBuffer;
		ItemPointerSetInvalid(&(so->mrkHeapIptr));
		ItemPointerSetInvalid(iptr);
	}

	/*
	 * Reset the scan keys. Note that keys ordering stuff moved to
	 * _bt_first.	   - vadim 05/05/97
	 */
	so->numberOfKeys = scan->numberOfKeys;
	if (scan->numberOfKeys > 0)
	{
		memmove(scan->keyData,
				scankey,
				scan->numberOfKeys * sizeof(ScanKeyData));
		memmove(so->keyData,
				scankey,
				so->numberOfKeys * sizeof(ScanKeyData));
	}

	PG_RETURN_VOID();
}

void
btmovescan(IndexScanDesc scan, Datum v)
{
	ItemPointer iptr;
	BTScanOpaque so;

	so = (BTScanOpaque) scan->opaque;

	/* we aren't holding any read locks, but gotta drop the pin */
	if (ItemPointerIsValid(iptr = &(scan->currentItemData)))
	{
		ReleaseBuffer(so->btso_curbuf);
		so->btso_curbuf = InvalidBuffer;
		ItemPointerSetInvalid(iptr);
	}

	so->keyData[0].sk_argument = v;
}

/*
 *	btendscan() -- close down a scan
 */
Datum
btendscan(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ItemPointer iptr;
	BTScanOpaque so;

	so = (BTScanOpaque) scan->opaque;

	/* we aren't holding any read locks, but gotta drop the pins */
	if (ItemPointerIsValid(iptr = &(scan->currentItemData)))
	{
		if (BufferIsValid(so->btso_curbuf))
			ReleaseBuffer(so->btso_curbuf);
		so->btso_curbuf = InvalidBuffer;
		ItemPointerSetInvalid(iptr);
	}

	if (ItemPointerIsValid(iptr = &(scan->currentMarkData)))
	{
		if (BufferIsValid(so->btso_mrkbuf))
			ReleaseBuffer(so->btso_mrkbuf);
		so->btso_mrkbuf = InvalidBuffer;
		ItemPointerSetInvalid(iptr);
	}

	if (so->keyData != (ScanKey) NULL)
		pfree(so->keyData);
	pfree(so);

	PG_RETURN_VOID();
}

/*
 *	btmarkpos() -- save current scan position
 */
Datum
btmarkpos(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ItemPointer iptr;
	BTScanOpaque so;

	so = (BTScanOpaque) scan->opaque;

	/* we aren't holding any read locks, but gotta drop the pin */
	if (ItemPointerIsValid(iptr = &(scan->currentMarkData)))
	{
		ReleaseBuffer(so->btso_mrkbuf);
		so->btso_mrkbuf = InvalidBuffer;
		ItemPointerSetInvalid(iptr);
	}

	/* bump pin on current buffer for assignment to mark buffer */
	if (ItemPointerIsValid(&(scan->currentItemData)))
	{
		so->btso_mrkbuf = ReadBuffer(scan->indexRelation,
								  BufferGetBlockNumber(so->btso_curbuf));
		scan->currentMarkData = scan->currentItemData;
		so->mrkHeapIptr = so->curHeapIptr;
	}

	PG_RETURN_VOID();
}

/*
 *	btrestrpos() -- restore scan to last saved position
 */
Datum
btrestrpos(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ItemPointer iptr;
	BTScanOpaque so;

	so = (BTScanOpaque) scan->opaque;

	/* we aren't holding any read locks, but gotta drop the pin */
	if (ItemPointerIsValid(iptr = &(scan->currentItemData)))
	{
		ReleaseBuffer(so->btso_curbuf);
		so->btso_curbuf = InvalidBuffer;
		ItemPointerSetInvalid(iptr);
	}

	/* bump pin on marked buffer */
	if (ItemPointerIsValid(&(scan->currentMarkData)))
	{
		so->btso_curbuf = ReadBuffer(scan->indexRelation,
								  BufferGetBlockNumber(so->btso_mrkbuf));
		scan->currentItemData = scan->currentMarkData;
		so->curHeapIptr = so->mrkHeapIptr;
	}

	PG_RETURN_VOID();
}

/*
 * Bulk deletion of all index entries pointing to a set of heap tuples.
 * The set of target tuples is specified via a callback routine that tells
 * whether any given heap tuple (identified by ItemPointer) is being deleted.
 *
 * Result: a palloc'd struct containing statistical info for VACUUM displays.
 */
Datum
btbulkdelete(PG_FUNCTION_ARGS)
{
	Relation	rel = (Relation) PG_GETARG_POINTER(0);
	IndexBulkDeleteCallback callback = (IndexBulkDeleteCallback) PG_GETARG_POINTER(1);
	void	   *callback_state = (void *) PG_GETARG_POINTER(2);
	IndexBulkDeleteResult *result;
	BlockNumber num_pages;
	double		tuples_removed;
	double		num_index_tuples;
	IndexScanDesc scan;
	BTScanOpaque so;
	ItemPointer current;

	tuples_removed = 0;
	num_index_tuples = 0;

	/*
	 * We use a standard IndexScanDesc scan object, but to speed up the
	 * loop, we skip most of the wrapper layers of index_getnext and
	 * instead call _bt_step directly.	This implies holding buffer lock
	 * on a target page throughout the loop over the page's tuples.
	 * Initially, we have a read lock acquired by _bt_step when we stepped
	 * onto the page.  If we find a tuple we need to delete, we trade in
	 * the read lock for an exclusive write lock; after that, we hold the
	 * write lock until we step off the page (fortunately, _bt_relbuf
	 * doesn't care which kind of lock it's releasing).  This should
	 * minimize the amount of work needed per page.
	 */
	scan = index_beginscan(NULL, rel, SnapshotAny, 0, (ScanKey) NULL);
	so = (BTScanOpaque) scan->opaque;
	current = &(scan->currentItemData);

	/* Use _bt_first to get started, then _bt_step to remaining tuples */
	if (_bt_first(scan, ForwardScanDirection))
	{
		Buffer		buf;
		BlockNumber lockedBlock = InvalidBlockNumber;

		/* we have the buffer pinned and locked */
		buf = so->btso_curbuf;
		Assert(BufferIsValid(buf));

		do
		{
			Page		page;
			BlockNumber blkno;
			OffsetNumber offnum;
			BTItem		btitem;
			BTPageOpaque opaque;
			IndexTuple	itup;
			ItemPointer htup;

			CHECK_FOR_INTERRUPTS();

			/* current is the next index tuple */
			blkno = ItemPointerGetBlockNumber(current);
			offnum = ItemPointerGetOffsetNumber(current);
			page = BufferGetPage(buf);
			btitem = (BTItem) PageGetItem(page, PageGetItemId(page, offnum));
			itup = &btitem->bti_itup;
			htup = &(itup->t_tid);

			if (callback(htup, callback_state))
			{
				/*
				 * If this is first deletion on this page, trade in read
				 * lock for a really-exclusive write lock.	Then, step
				 * back one and re-examine the item, because other backends
				 * might have inserted item(s) while we weren't holding
				 * the lock!
				 *
				 * We assume that only concurrent insertions, not deletions,
				 * can occur while we're not holding the page lock (the caller
				 * should hold a suitable relation lock to ensure this).
				 * Therefore, the item we want to delete is either in the
				 * same slot as before, or some slot to its right.
				 * Rechecking the same slot is necessary and sufficient to
				 * get back in sync after any insertions.
				 */
				if (blkno != lockedBlock)
				{
					LockBuffer(buf, BUFFER_LOCK_UNLOCK);
					LockBufferForCleanup(buf);
					lockedBlock = blkno;
				}
				else
				{
					/* Okay to delete the item from the page */
					_bt_itemdel(rel, buf, current);

					/* Mark buffer dirty, but keep the lock and pin */
					WriteNoReleaseBuffer(buf);

					tuples_removed += 1;
				}

				/*
				 * In either case, we now need to back up the scan one item,
				 * so that the next cycle will re-examine the same offnum on
				 * this page.
				 *
				 * For now, just hack the current-item index.  Will need to
				 * be smarter when deletion includes removal of empty
				 * index pages.
				 *
				 * We must decrement ip_posid in all cases but one: if the
				 * page was formerly rightmost but was split while we didn't
				 * hold the lock, and ip_posid is pointing to item 1, then
				 * ip_posid now points at the high key not a valid data item.
				 * In this case we do want to step forward.
				 */
				opaque = (BTPageOpaque) PageGetSpecialPointer(page);
				if (current->ip_posid >= P_FIRSTDATAKEY(opaque))
					current->ip_posid--;
			}
			else
				num_index_tuples += 1;
		} while (_bt_step(scan, &buf, ForwardScanDirection));
	}

	index_endscan(scan);

	/* return statistics */
	num_pages = RelationGetNumberOfBlocks(rel);

	result = (IndexBulkDeleteResult *) palloc(sizeof(IndexBulkDeleteResult));
	result->num_pages = num_pages;
	result->tuples_removed = tuples_removed;
	result->num_index_tuples = num_index_tuples;

	PG_RETURN_POINTER(result);
}

/*
 * Restore scan position when btgettuple is called to continue a scan.
 */
static void
_bt_restscan(IndexScanDesc scan)
{
	Relation	rel = scan->indexRelation;
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	Buffer		buf = so->btso_curbuf;
	Page		page;
	ItemPointer current = &(scan->currentItemData);
	OffsetNumber offnum = ItemPointerGetOffsetNumber(current),
				maxoff;
	BTPageOpaque opaque;
	ItemPointerData target = so->curHeapIptr;
	BTItem		item;
	BlockNumber blkno;

	/*
	 * Get back the read lock we were holding on the buffer. (We still
	 * have a reference-count pin on it, so need not get that.)
	 */
	LockBuffer(buf, BT_READ);

	page = BufferGetPage(buf);
	maxoff = PageGetMaxOffsetNumber(page);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);

	/*
	 * We use this as flag when first index tuple on page is deleted but
	 * we do not move left (this would slowdown vacuum) - so we set
	 * current->ip_posid before first index tuple on the current page
	 * (_bt_step will move it right)...
	 */
	if (!ItemPointerIsValid(&target))
	{
		ItemPointerSetOffsetNumber(current,
							   OffsetNumberPrev(P_FIRSTDATAKEY(opaque)));
		return;
	}

	/*
	 * The item we were on may have moved right due to insertions. Find it
	 * again.
	 */
	for (;;)
	{
		/* Check for item on this page */
		for (;
			 offnum <= maxoff;
			 offnum = OffsetNumberNext(offnum))
		{
			item = (BTItem) PageGetItem(page, PageGetItemId(page, offnum));
			if (item->bti_itup.t_tid.ip_blkid.bi_hi ==
				target.ip_blkid.bi_hi &&
				item->bti_itup.t_tid.ip_blkid.bi_lo ==
				target.ip_blkid.bi_lo &&
				item->bti_itup.t_tid.ip_posid == target.ip_posid)
			{
				current->ip_posid = offnum;
				return;
			}
		}

		/*
		 * By here, the item we're looking for moved right at least one
		 * page
		 */
		if (P_RIGHTMOST(opaque))
			elog(FATAL, "_bt_restscan: my bits moved right off the end of the world!"
				 "\n\tRecreate index %s.", RelationGetRelationName(rel));

		blkno = opaque->btpo_next;
		_bt_relbuf(rel, buf);
		buf = _bt_getbuf(rel, blkno, BT_READ);
		page = BufferGetPage(buf);
		maxoff = PageGetMaxOffsetNumber(page);
		opaque = (BTPageOpaque) PageGetSpecialPointer(page);
		offnum = P_FIRSTDATAKEY(opaque);
		ItemPointerSet(current, blkno, offnum);
		so->btso_curbuf = buf;
	}
}

static void
_bt_restore_page(Page page, char *from, int len)
{
	BTItemData	btdata;
	Size		itemsz;
	char	   *end = from + len;

	for (; from < end;)
	{
		memcpy(&btdata, from, sizeof(BTItemData));
		itemsz = IndexTupleDSize(btdata.bti_itup) +
			(sizeof(BTItemData) - sizeof(IndexTupleData));
		itemsz = MAXALIGN(itemsz);
		if (PageAddItem(page, (Item) from, itemsz,
					  FirstOffsetNumber, LP_USED) == InvalidOffsetNumber)
			elog(PANIC, "_bt_restore_page: can't add item to page");
		from += itemsz;
	}
}

static void
btree_xlog_delete(bool redo, XLogRecPtr lsn, XLogRecord *record)
{
	xl_btree_delete *xlrec;
	Relation	reln;
	Buffer		buffer;
	Page		page;

	if (!redo || (record->xl_info & XLR_BKP_BLOCK_1))
		return;

	xlrec = (xl_btree_delete *) XLogRecGetData(record);
	reln = XLogOpenRelation(redo, RM_BTREE_ID, xlrec->target.node);
	if (!RelationIsValid(reln))
		return;
	buffer = XLogReadBuffer(false, reln,
						ItemPointerGetBlockNumber(&(xlrec->target.tid)));
	if (!BufferIsValid(buffer))
		elog(PANIC, "btree_delete_redo: block unfound");
	page = (Page) BufferGetPage(buffer);
	if (PageIsNew((PageHeader) page))
		elog(PANIC, "btree_delete_redo: uninitialized page");

	if (XLByteLE(lsn, PageGetLSN(page)))
	{
		UnlockAndReleaseBuffer(buffer);
		return;
	}

	PageIndexTupleDelete(page, ItemPointerGetOffsetNumber(&(xlrec->target.tid)));

	PageSetLSN(page, lsn);
	PageSetSUI(page, ThisStartUpID);
	UnlockAndWriteBuffer(buffer);

	return;
}

static void
btree_xlog_insert(bool redo, XLogRecPtr lsn, XLogRecord *record)
{
	xl_btree_insert *xlrec;
	Relation	reln;
	Buffer		buffer;
	Page		page;
	BTPageOpaque pageop;

	if (redo && (record->xl_info & XLR_BKP_BLOCK_1))
		return;

	xlrec = (xl_btree_insert *) XLogRecGetData(record);
	reln = XLogOpenRelation(redo, RM_BTREE_ID, xlrec->target.node);
	if (!RelationIsValid(reln))
		return;
	buffer = XLogReadBuffer(false, reln,
						ItemPointerGetBlockNumber(&(xlrec->target.tid)));
	if (!BufferIsValid(buffer))
		elog(PANIC, "btree_insert_%sdo: block unfound", (redo) ? "re" : "un");
	page = (Page) BufferGetPage(buffer);
	if (PageIsNew((PageHeader) page))
		elog(PANIC, "btree_insert_%sdo: uninitialized page", (redo) ? "re" : "un");
	pageop = (BTPageOpaque) PageGetSpecialPointer(page);

	if (redo)
	{
		if (XLByteLE(lsn, PageGetLSN(page)))
		{
			UnlockAndReleaseBuffer(buffer);
			return;
		}
		if (PageAddItem(page, (Item) ((char *) xlrec + SizeOfBtreeInsert),
						record->xl_len - SizeOfBtreeInsert,
						ItemPointerGetOffsetNumber(&(xlrec->target.tid)),
						LP_USED) == InvalidOffsetNumber)
			elog(PANIC, "btree_insert_redo: failed to add item");

		PageSetLSN(page, lsn);
		PageSetSUI(page, ThisStartUpID);
		UnlockAndWriteBuffer(buffer);
	}
	else
	{
		if (XLByteLT(PageGetLSN(page), lsn))
			elog(PANIC, "btree_insert_undo: bad page LSN");

		if (!P_ISLEAF(pageop))
		{
			UnlockAndReleaseBuffer(buffer);
			return;
		}

		elog(PANIC, "btree_insert_undo: unimplemented");
	}

	return;
}

static void
btree_xlog_split(bool redo, bool onleft, XLogRecPtr lsn, XLogRecord *record)
{
	xl_btree_split *xlrec = (xl_btree_split *) XLogRecGetData(record);
	Relation	reln;
	BlockNumber blkno;
	Buffer		buffer;
	Page		page;
	BTPageOpaque pageop;
	char	   *op = (redo) ? "redo" : "undo";
	bool		isleaf = (record->xl_info & XLOG_BTREE_LEAF);

	reln = XLogOpenRelation(redo, RM_BTREE_ID, xlrec->target.node);
	if (!RelationIsValid(reln))
		return;

	/* Left (original) sibling */
	blkno = (onleft) ? ItemPointerGetBlockNumber(&(xlrec->target.tid)) :
		BlockIdGetBlockNumber(&(xlrec->otherblk));
	buffer = XLogReadBuffer(false, reln, blkno);
	if (!BufferIsValid(buffer))
		elog(PANIC, "btree_split_%s: lost left sibling", op);

	page = (Page) BufferGetPage(buffer);
	if (redo)
		_bt_pageinit(page, BufferGetPageSize(buffer));
	else if (PageIsNew((PageHeader) page))
		elog(PANIC, "btree_split_undo: uninitialized left sibling");
	pageop = (BTPageOpaque) PageGetSpecialPointer(page);

	if (redo)
	{
		pageop->btpo_parent = BlockIdGetBlockNumber(&(xlrec->parentblk));
		pageop->btpo_prev = BlockIdGetBlockNumber(&(xlrec->leftblk));
		if (onleft)
			pageop->btpo_next = BlockIdGetBlockNumber(&(xlrec->otherblk));
		else
			pageop->btpo_next = ItemPointerGetBlockNumber(&(xlrec->target.tid));
		pageop->btpo_flags = (isleaf) ? BTP_LEAF : 0;

		_bt_restore_page(page, (char *) xlrec + SizeOfBtreeSplit, xlrec->leftlen);

		PageSetLSN(page, lsn);
		PageSetSUI(page, ThisStartUpID);
		UnlockAndWriteBuffer(buffer);
	}
	else
/* undo */
	{
		if (XLByteLT(PageGetLSN(page), lsn))
			elog(PANIC, "btree_split_undo: bad left sibling LSN");
		elog(PANIC, "btree_split_undo: unimplemented");
	}

	/* Right (new) sibling */
	blkno = (onleft) ? BlockIdGetBlockNumber(&(xlrec->otherblk)) :
		ItemPointerGetBlockNumber(&(xlrec->target.tid));
	buffer = XLogReadBuffer((redo) ? true : false, reln, blkno);
	if (!BufferIsValid(buffer))
		elog(PANIC, "btree_split_%s: lost right sibling", op);

	page = (Page) BufferGetPage(buffer);
	if (redo)
		_bt_pageinit(page, BufferGetPageSize(buffer));
	else if (PageIsNew((PageHeader) page))
		elog(PANIC, "btree_split_undo: uninitialized right sibling");
	pageop = (BTPageOpaque) PageGetSpecialPointer(page);

	if (redo)
	{
		pageop->btpo_parent = BlockIdGetBlockNumber(&(xlrec->parentblk));
		pageop->btpo_prev = (onleft) ?
			ItemPointerGetBlockNumber(&(xlrec->target.tid)) :
			BlockIdGetBlockNumber(&(xlrec->otherblk));
		pageop->btpo_next = BlockIdGetBlockNumber(&(xlrec->rightblk));
		pageop->btpo_flags = (isleaf) ? BTP_LEAF : 0;

		_bt_restore_page(page,
					  (char *) xlrec + SizeOfBtreeSplit + xlrec->leftlen,
					 record->xl_len - SizeOfBtreeSplit - xlrec->leftlen);

		PageSetLSN(page, lsn);
		PageSetSUI(page, ThisStartUpID);
		UnlockAndWriteBuffer(buffer);
	}
	else
/* undo */
	{
		if (XLByteLT(PageGetLSN(page), lsn))
			elog(PANIC, "btree_split_undo: bad right sibling LSN");
		elog(PANIC, "btree_split_undo: unimplemented");
	}

	if (!redo || (record->xl_info & XLR_BKP_BLOCK_1))
		return;

	/* Right (next) page */
	blkno = BlockIdGetBlockNumber(&(xlrec->rightblk));
	if (blkno == P_NONE)
		return;

	buffer = XLogReadBuffer(false, reln, blkno);
	if (!BufferIsValid(buffer))
		elog(PANIC, "btree_split_redo: lost next right page");

	page = (Page) BufferGetPage(buffer);
	if (PageIsNew((PageHeader) page))
		elog(PANIC, "btree_split_redo: uninitialized next right page");

	if (XLByteLE(lsn, PageGetLSN(page)))
	{
		UnlockAndReleaseBuffer(buffer);
		return;
	}
	pageop = (BTPageOpaque) PageGetSpecialPointer(page);
	pageop->btpo_prev = (onleft) ?
		BlockIdGetBlockNumber(&(xlrec->otherblk)) :
		ItemPointerGetBlockNumber(&(xlrec->target.tid));

	PageSetLSN(page, lsn);
	PageSetSUI(page, ThisStartUpID);
	UnlockAndWriteBuffer(buffer);
}

static void
btree_xlog_newroot(bool redo, XLogRecPtr lsn, XLogRecord *record)
{
	xl_btree_newroot *xlrec = (xl_btree_newroot *) XLogRecGetData(record);
	Relation	reln;
	Buffer		buffer;
	Page		page;
	BTPageOpaque pageop;
	Buffer		metabuf;
	Page		metapg;
	BTMetaPageData md;

	if (!redo)
		return;

	reln = XLogOpenRelation(redo, RM_BTREE_ID, xlrec->node);
	if (!RelationIsValid(reln))
		return;
	buffer = XLogReadBuffer(true, reln, BlockIdGetBlockNumber(&(xlrec->rootblk)));
	if (!BufferIsValid(buffer))
		elog(PANIC, "btree_newroot_redo: no root page");
	metabuf = XLogReadBuffer(false, reln, BTREE_METAPAGE);
	if (!BufferIsValid(buffer))
		elog(PANIC, "btree_newroot_redo: no metapage");
	page = (Page) BufferGetPage(buffer);
	_bt_pageinit(page, BufferGetPageSize(buffer));
	pageop = (BTPageOpaque) PageGetSpecialPointer(page);

	pageop->btpo_flags |= BTP_ROOT;
	pageop->btpo_prev = pageop->btpo_next = P_NONE;
	pageop->btpo_parent = BTREE_METAPAGE;

	if (record->xl_info & XLOG_BTREE_LEAF)
		pageop->btpo_flags |= BTP_LEAF;

	if (record->xl_len > SizeOfBtreeNewroot)
		_bt_restore_page(page,
						 (char *) xlrec + SizeOfBtreeNewroot,
						 record->xl_len - SizeOfBtreeNewroot);

	PageSetLSN(page, lsn);
	PageSetSUI(page, ThisStartUpID);
	UnlockAndWriteBuffer(buffer);

	metapg = BufferGetPage(metabuf);
	_bt_pageinit(metapg, BufferGetPageSize(metabuf));
	md.btm_magic = BTREE_MAGIC;
	md.btm_version = BTREE_VERSION;
	md.btm_root = BlockIdGetBlockNumber(&(xlrec->rootblk));
	md.btm_level = xlrec->level;
	memcpy((char *) BTPageGetMeta(metapg), (char *) &md, sizeof(md));

	pageop = (BTPageOpaque) PageGetSpecialPointer(metapg);
	pageop->btpo_flags = BTP_META;

	PageSetLSN(metapg, lsn);
	PageSetSUI(metapg, ThisStartUpID);
	UnlockAndWriteBuffer(metabuf);
}

void
btree_redo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;

	info &= ~XLOG_BTREE_LEAF;
	if (info == XLOG_BTREE_DELETE)
		btree_xlog_delete(true, lsn, record);
	else if (info == XLOG_BTREE_INSERT)
		btree_xlog_insert(true, lsn, record);
	else if (info == XLOG_BTREE_SPLIT)
		btree_xlog_split(true, false, lsn, record);		/* new item on the right */
	else if (info == XLOG_BTREE_SPLEFT)
		btree_xlog_split(true, true, lsn, record);		/* new item on the left */
	else if (info == XLOG_BTREE_NEWROOT)
		btree_xlog_newroot(true, lsn, record);
	else
		elog(PANIC, "btree_redo: unknown op code %u", info);
}

void
btree_undo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;

	info &= ~XLOG_BTREE_LEAF;
	if (info == XLOG_BTREE_DELETE)
		btree_xlog_delete(false, lsn, record);
	else if (info == XLOG_BTREE_INSERT)
		btree_xlog_insert(false, lsn, record);
	else if (info == XLOG_BTREE_SPLIT)
		btree_xlog_split(false, false, lsn, record);	/* new item on the right */
	else if (info == XLOG_BTREE_SPLEFT)
		btree_xlog_split(false, true, lsn, record);		/* new item on the left */
	else if (info == XLOG_BTREE_NEWROOT)
		btree_xlog_newroot(false, lsn, record);
	else
		elog(PANIC, "btree_undo: unknown op code %u", info);
}

static void
out_target(char *buf, xl_btreetid *target)
{
	sprintf(buf + strlen(buf), "node %u/%u; tid %u/%u",
			target->node.tblNode, target->node.relNode,
			ItemPointerGetBlockNumber(&(target->tid)),
			ItemPointerGetOffsetNumber(&(target->tid)));
}

void
btree_desc(char *buf, uint8 xl_info, char *rec)
{
	uint8		info = xl_info & ~XLR_INFO_MASK;

	info &= ~XLOG_BTREE_LEAF;
	if (info == XLOG_BTREE_INSERT)
	{
		xl_btree_insert *xlrec = (xl_btree_insert *) rec;

		strcat(buf, "insert: ");
		out_target(buf, &(xlrec->target));
	}
	else if (info == XLOG_BTREE_DELETE)
	{
		xl_btree_delete *xlrec = (xl_btree_delete *) rec;

		strcat(buf, "delete: ");
		out_target(buf, &(xlrec->target));
	}
	else if (info == XLOG_BTREE_SPLIT || info == XLOG_BTREE_SPLEFT)
	{
		xl_btree_split *xlrec = (xl_btree_split *) rec;

		sprintf(buf + strlen(buf), "split(%s): ",
				(info == XLOG_BTREE_SPLIT) ? "right" : "left");
		out_target(buf, &(xlrec->target));
		sprintf(buf + strlen(buf), "; oth %u; rgh %u",
				BlockIdGetBlockNumber(&xlrec->otherblk),
				BlockIdGetBlockNumber(&xlrec->rightblk));
	}
	else if (info == XLOG_BTREE_NEWROOT)
	{
		xl_btree_newroot *xlrec = (xl_btree_newroot *) rec;

		sprintf(buf + strlen(buf), "root: node %u/%u; blk %u",
				xlrec->node.tblNode, xlrec->node.relNode,
				BlockIdGetBlockNumber(&xlrec->rootblk));
	}
	else
		strcat(buf, "UNKNOWN");
}
