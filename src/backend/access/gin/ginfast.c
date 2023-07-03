/*-------------------------------------------------------------------------
 *
 * ginfast.c
 *	  Fast insert routines for the Postgres inverted index access method.
 *	  Pending entries are stored in linear list of pages.  Later on
 *	  (typically during VACUUM), ginInsertCleanup() will be invoked to
 *	  transfer pending entries into the regular index structure.  This
 *	  wins because bulk insertion is much more efficient than retail.
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/gin/ginfast.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gin_private.h"
#include "access/ginxlog.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/pg_am.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "postmaster/autovacuum.h"
#include "storage/indexfsm.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/* GUC parameter */
int			gin_pending_list_limit = 0;

#define GIN_PAGE_FREESIZE \
	( BLCKSZ - MAXALIGN(SizeOfPageHeaderData) - MAXALIGN(sizeof(GinPageOpaqueData)) )

typedef struct KeyArray
{
	Datum	   *keys;			/* expansible array */
	GinNullCategory *categories;	/* another expansible array */
	int32		nvalues;		/* current number of valid entries */
	int32		maxvalues;		/* allocated size of arrays */
} KeyArray;


/*
 * Build a pending-list page from the given array of tuples, and write it out.
 *
 * Returns amount of free space left on the page.
 */
static int32
writeListPage(Relation index, Buffer buffer,
			  IndexTuple *tuples, int32 ntuples, BlockNumber rightlink)
{
	Page		page = BufferGetPage(buffer);
	int32		i,
				freesize,
				size = 0;
	OffsetNumber l,
				off;
	PGAlignedBlock workspace;
	char	   *ptr;

	START_CRIT_SECTION();

	GinInitBuffer(buffer, GIN_LIST);

	off = FirstOffsetNumber;
	ptr = workspace.data;

	for (i = 0; i < ntuples; i++)
	{
		int			this_size = IndexTupleSize(tuples[i]);

		memcpy(ptr, tuples[i], this_size);
		ptr += this_size;
		size += this_size;

		l = PageAddItem(page, (Item) tuples[i], this_size, off, false, false);

		if (l == InvalidOffsetNumber)
			elog(ERROR, "failed to add item to index page in \"%s\"",
				 RelationGetRelationName(index));

		off++;
	}

	Assert(size <= BLCKSZ);		/* else we overran workspace */

	GinPageGetOpaque(page)->rightlink = rightlink;

	/*
	 * tail page may contain only whole row(s) or final part of row placed on
	 * previous pages (a "row" here meaning all the index tuples generated for
	 * one heap tuple)
	 */
	if (rightlink == InvalidBlockNumber)
	{
		GinPageSetFullRow(page);
		GinPageGetOpaque(page)->maxoff = 1;
	}
	else
	{
		GinPageGetOpaque(page)->maxoff = 0;
	}

	MarkBufferDirty(buffer);

	if (RelationNeedsWAL(index))
	{
		ginxlogInsertListPage data;
		XLogRecPtr	recptr;

		data.rightlink = rightlink;
		data.ntuples = ntuples;

		XLogBeginInsert();
		XLogRegisterData((char *) &data, sizeof(ginxlogInsertListPage));

		XLogRegisterBuffer(0, buffer, REGBUF_WILL_INIT);
		XLogRegisterBufData(0, workspace.data, size);

		recptr = XLogInsert(RM_GIN_ID, XLOG_GIN_INSERT_LISTPAGE);
		PageSetLSN(page, recptr);
	}

	/* get free space before releasing buffer */
	freesize = PageGetExactFreeSpace(page);

	UnlockReleaseBuffer(buffer);

	END_CRIT_SECTION();

	return freesize;
}

static void
makeSublist(Relation index, IndexTuple *tuples, int32 ntuples,
			GinMetaPageData *res)
{
	Buffer		curBuffer = InvalidBuffer;
	Buffer		prevBuffer = InvalidBuffer;
	int			i,
				size = 0,
				tupsize;
	int			startTuple = 0;

	Assert(ntuples > 0);

	/*
	 * Split tuples into pages
	 */
	for (i = 0; i < ntuples; i++)
	{
		if (curBuffer == InvalidBuffer)
		{
			curBuffer = GinNewBuffer(index);

			if (prevBuffer != InvalidBuffer)
			{
				res->nPendingPages++;
				writeListPage(index, prevBuffer,
							  tuples + startTuple,
							  i - startTuple,
							  BufferGetBlockNumber(curBuffer));
			}
			else
			{
				res->head = BufferGetBlockNumber(curBuffer);
			}

			prevBuffer = curBuffer;
			startTuple = i;
			size = 0;
		}

		tupsize = MAXALIGN(IndexTupleSize(tuples[i])) + sizeof(ItemIdData);

		if (size + tupsize > GinListPageSize)
		{
			/* won't fit, force a new page and reprocess */
			i--;
			curBuffer = InvalidBuffer;
		}
		else
		{
			size += tupsize;
		}
	}

	/*
	 * Write last page
	 */
	res->tail = BufferGetBlockNumber(curBuffer);
	res->tailFreeSize = writeListPage(index, curBuffer,
									  tuples + startTuple,
									  ntuples - startTuple,
									  InvalidBlockNumber);
	res->nPendingPages++;
	/* that was only one heap tuple */
	res->nPendingHeapTuples = 1;
}

/*
 * Write the index tuples contained in *collector into the index's
 * pending list.
 *
 * Function guarantees that all these tuples will be inserted consecutively,
 * preserving order
 */
void
ginHeapTupleFastInsert(GinState *ginstate, GinTupleCollector *collector)
{
	Relation	index = ginstate->index;
	Buffer		metabuffer;
	Page		metapage;
	GinMetaPageData *metadata = NULL;
	Buffer		buffer = InvalidBuffer;
	Page		page = NULL;
	ginxlogUpdateMeta data;
	bool		separateList = false;
	bool		needCleanup = false;
	int			cleanupSize;
	bool		needWal;

	if (collector->ntuples == 0)
		return;

	needWal = RelationNeedsWAL(index);

	data.node = index->rd_node;
	data.ntuples = 0;
	data.newRightlink = data.prevTail = InvalidBlockNumber;

	metabuffer = ReadBuffer(index, GIN_METAPAGE_BLKNO);
	metapage = BufferGetPage(metabuffer);

	/*
	 * An insertion to the pending list could logically belong anywhere in the
	 * tree, so it conflicts with all serializable scans.  All scans acquire a
	 * predicate lock on the metabuffer to represent that.  Therefore we'll
	 * check for conflicts in, but not until we have the page locked and are
	 * ready to modify the page.
	 */

	if (collector->sumsize + collector->ntuples * sizeof(ItemIdData) > GinListPageSize)
	{
		/*
		 * Total size is greater than one page => make sublist
		 */
		separateList = true;
	}
	else
	{
		LockBuffer(metabuffer, GIN_EXCLUSIVE);
		metadata = GinPageGetMeta(metapage);

		if (metadata->head == InvalidBlockNumber ||
			collector->sumsize + collector->ntuples * sizeof(ItemIdData) > metadata->tailFreeSize)
		{
			/*
			 * Pending list is empty or total size is greater than freespace
			 * on tail page => make sublist
			 *
			 * We unlock metabuffer to keep high concurrency
			 */
			separateList = true;
			LockBuffer(metabuffer, GIN_UNLOCK);
		}
	}

	if (separateList)
	{
		/*
		 * We should make sublist separately and append it to the tail
		 */
		GinMetaPageData sublist;

		memset(&sublist, 0, sizeof(GinMetaPageData));
		makeSublist(index, collector->tuples, collector->ntuples, &sublist);

		/*
		 * metapage was unlocked, see above
		 */
		LockBuffer(metabuffer, GIN_EXCLUSIVE);
		metadata = GinPageGetMeta(metapage);

		CheckForSerializableConflictIn(index, NULL, GIN_METAPAGE_BLKNO);

		if (metadata->head == InvalidBlockNumber)
		{
			/*
			 * Main list is empty, so just insert sublist as main list
			 */
			START_CRIT_SECTION();

			metadata->head = sublist.head;
			metadata->tail = sublist.tail;
			metadata->tailFreeSize = sublist.tailFreeSize;

			metadata->nPendingPages = sublist.nPendingPages;
			metadata->nPendingHeapTuples = sublist.nPendingHeapTuples;

			if (needWal)
				XLogBeginInsert();
		}
		else
		{
			/*
			 * Merge lists
			 */
			data.prevTail = metadata->tail;
			data.newRightlink = sublist.head;

			buffer = ReadBuffer(index, metadata->tail);
			LockBuffer(buffer, GIN_EXCLUSIVE);
			page = BufferGetPage(buffer);

			Assert(GinPageGetOpaque(page)->rightlink == InvalidBlockNumber);

			START_CRIT_SECTION();

			GinPageGetOpaque(page)->rightlink = sublist.head;

			MarkBufferDirty(buffer);

			metadata->tail = sublist.tail;
			metadata->tailFreeSize = sublist.tailFreeSize;

			metadata->nPendingPages += sublist.nPendingPages;
			metadata->nPendingHeapTuples += sublist.nPendingHeapTuples;

			if (needWal)
			{
				XLogBeginInsert();
				XLogRegisterBuffer(1, buffer, REGBUF_STANDARD);
			}
		}
	}
	else
	{
		/*
		 * Insert into tail page.  Metapage is already locked
		 */
		OffsetNumber l,
					off;
		int			i,
					tupsize;
		char	   *ptr;
		char	   *collectordata;

		CheckForSerializableConflictIn(index, NULL, GIN_METAPAGE_BLKNO);

		buffer = ReadBuffer(index, metadata->tail);
		LockBuffer(buffer, GIN_EXCLUSIVE);
		page = BufferGetPage(buffer);

		off = (PageIsEmpty(page)) ? FirstOffsetNumber :
			OffsetNumberNext(PageGetMaxOffsetNumber(page));

		collectordata = ptr = (char *) palloc(collector->sumsize);

		data.ntuples = collector->ntuples;

		START_CRIT_SECTION();

		if (needWal)
			XLogBeginInsert();

		/*
		 * Increase counter of heap tuples
		 */
		Assert(GinPageGetOpaque(page)->maxoff <= metadata->nPendingHeapTuples);
		GinPageGetOpaque(page)->maxoff++;
		metadata->nPendingHeapTuples++;

		for (i = 0; i < collector->ntuples; i++)
		{
			tupsize = IndexTupleSize(collector->tuples[i]);
			l = PageAddItem(page, (Item) collector->tuples[i], tupsize, off, false, false);

			if (l == InvalidOffsetNumber)
				elog(ERROR, "failed to add item to index page in \"%s\"",
					 RelationGetRelationName(index));

			memcpy(ptr, collector->tuples[i], tupsize);
			ptr += tupsize;

			off++;
		}

		Assert((ptr - collectordata) <= collector->sumsize);
		if (needWal)
		{
			XLogRegisterBuffer(1, buffer, REGBUF_STANDARD);
			XLogRegisterBufData(1, collectordata, collector->sumsize);
		}

		metadata->tailFreeSize = PageGetExactFreeSpace(page);

		MarkBufferDirty(buffer);
	}

	/*
	 * Set pd_lower just past the end of the metadata.  This is essential,
	 * because without doing so, metadata will be lost if xlog.c compresses
	 * the page.  (We must do this here because pre-v11 versions of PG did not
	 * set the metapage's pd_lower correctly, so a pg_upgraded index might
	 * contain the wrong value.)
	 */
	((PageHeader) metapage)->pd_lower =
		((char *) metadata + sizeof(GinMetaPageData)) - (char *) metapage;

	/*
	 * Write metabuffer, make xlog entry
	 */
	MarkBufferDirty(metabuffer);

	if (needWal)
	{
		XLogRecPtr	recptr;

		memcpy(&data.metadata, metadata, sizeof(GinMetaPageData));

		XLogRegisterBuffer(0, metabuffer, REGBUF_WILL_INIT | REGBUF_STANDARD);
		XLogRegisterData((char *) &data, sizeof(ginxlogUpdateMeta));

		recptr = XLogInsert(RM_GIN_ID, XLOG_GIN_UPDATE_META_PAGE);
		PageSetLSN(metapage, recptr);

		if (buffer != InvalidBuffer)
		{
			PageSetLSN(page, recptr);
		}
	}

	if (buffer != InvalidBuffer)
		UnlockReleaseBuffer(buffer);

	/*
	 * Force pending list cleanup when it becomes too long. And,
	 * ginInsertCleanup could take significant amount of time, so we prefer to
	 * call it when it can do all the work in a single collection cycle. In
	 * non-vacuum mode, it shouldn't require maintenance_work_mem, so fire it
	 * while pending list is still small enough to fit into
	 * gin_pending_list_limit.
	 *
	 * ginInsertCleanup() should not be called inside our CRIT_SECTION.
	 */
	cleanupSize = GinGetPendingListCleanupSize(index);
	if (metadata->nPendingPages * GIN_PAGE_FREESIZE > cleanupSize * 1024L)
		needCleanup = true;

	UnlockReleaseBuffer(metabuffer);

	END_CRIT_SECTION();

	/*
	 * Since it could contend with concurrent cleanup process we cleanup
	 * pending list not forcibly.
	 */
	if (needCleanup)
		ginInsertCleanup(ginstate, false, true, false, NULL);
}

/*
 * Create temporary index tuples for a single indexable item (one index column
 * for the heap tuple specified by ht_ctid), and append them to the array
 * in *collector.  They will subsequently be written out using
 * ginHeapTupleFastInsert.  Note that to guarantee consistent state, all
 * temp tuples for a given heap tuple must be written in one call to
 * ginHeapTupleFastInsert.
 */
void
ginHeapTupleFastCollect(GinState *ginstate,
						GinTupleCollector *collector,
						OffsetNumber attnum, Datum value, bool isNull,
						ItemPointer ht_ctid)
{
	Datum	   *entries;
	GinNullCategory *categories;
	int32		i,
				nentries;

	/*
	 * Extract the key values that need to be inserted in the index
	 */
	entries = ginExtractEntries(ginstate, attnum, value, isNull,
								&nentries, &categories);

	/*
	 * Protect against integer overflow in allocation calculations
	 */
	if (nentries < 0 ||
		collector->ntuples + nentries > MaxAllocSize / sizeof(IndexTuple))
		elog(ERROR, "too many entries for GIN index");

	/*
	 * Allocate/reallocate memory for storing collected tuples
	 */
	if (collector->tuples == NULL)
	{
		/*
		 * Determine the number of elements to allocate in the tuples array
		 * initially.  Make it a power of 2 to avoid wasting memory when
		 * resizing (since palloc likes powers of 2).
		 */
		collector->lentuples = pg_nextpower2_32(Max(16, nentries));
		collector->tuples = (IndexTuple *) palloc(sizeof(IndexTuple) * collector->lentuples);
	}
	else if (collector->lentuples < collector->ntuples + nentries)
	{
		/*
		 * Advance lentuples to the next suitable power of 2.  This won't
		 * overflow, though we could get to a value that exceeds
		 * MaxAllocSize/sizeof(IndexTuple), causing an error in repalloc.
		 */
		collector->lentuples = pg_nextpower2_32(collector->ntuples + nentries);
		collector->tuples = (IndexTuple *) repalloc(collector->tuples,
													sizeof(IndexTuple) * collector->lentuples);
	}

	/*
	 * Build an index tuple for each key value, and add to array.  In pending
	 * tuples we just stick the heap TID into t_tid.
	 */
	for (i = 0; i < nentries; i++)
	{
		IndexTuple	itup;

		itup = GinFormTuple(ginstate, attnum, entries[i], categories[i],
							NULL, 0, 0, true);
		itup->t_tid = *ht_ctid;
		collector->tuples[collector->ntuples++] = itup;
		collector->sumsize += IndexTupleSize(itup);
	}
}

/*
 * Deletes pending list pages up to (not including) newHead page.
 * If newHead == InvalidBlockNumber then function drops the whole list.
 *
 * metapage is pinned and exclusive-locked throughout this function.
 */
static void
shiftList(Relation index, Buffer metabuffer, BlockNumber newHead,
		  bool fill_fsm, IndexBulkDeleteResult *stats)
{
	Page		metapage;
	GinMetaPageData *metadata;
	BlockNumber blknoToDelete;

	metapage = BufferGetPage(metabuffer);
	metadata = GinPageGetMeta(metapage);
	blknoToDelete = metadata->head;

	do
	{
		Page		page;
		int			i;
		int64		nDeletedHeapTuples = 0;
		ginxlogDeleteListPages data;
		Buffer		buffers[GIN_NDELETE_AT_ONCE];
		BlockNumber freespace[GIN_NDELETE_AT_ONCE];

		data.ndeleted = 0;
		while (data.ndeleted < GIN_NDELETE_AT_ONCE && blknoToDelete != newHead)
		{
			freespace[data.ndeleted] = blknoToDelete;
			buffers[data.ndeleted] = ReadBuffer(index, blknoToDelete);
			LockBuffer(buffers[data.ndeleted], GIN_EXCLUSIVE);
			page = BufferGetPage(buffers[data.ndeleted]);

			data.ndeleted++;

			Assert(!GinPageIsDeleted(page));

			nDeletedHeapTuples += GinPageGetOpaque(page)->maxoff;
			blknoToDelete = GinPageGetOpaque(page)->rightlink;
		}

		if (stats)
			stats->pages_deleted += data.ndeleted;

		/*
		 * This operation touches an unusually large number of pages, so
		 * prepare the XLogInsert machinery for that before entering the
		 * critical section.
		 */
		if (RelationNeedsWAL(index))
			XLogEnsureRecordSpace(data.ndeleted, 0);

		START_CRIT_SECTION();

		metadata->head = blknoToDelete;

		Assert(metadata->nPendingPages >= data.ndeleted);
		metadata->nPendingPages -= data.ndeleted;
		Assert(metadata->nPendingHeapTuples >= nDeletedHeapTuples);
		metadata->nPendingHeapTuples -= nDeletedHeapTuples;

		if (blknoToDelete == InvalidBlockNumber)
		{
			metadata->tail = InvalidBlockNumber;
			metadata->tailFreeSize = 0;
			metadata->nPendingPages = 0;
			metadata->nPendingHeapTuples = 0;
		}

		/*
		 * Set pd_lower just past the end of the metadata.  This is essential,
		 * because without doing so, metadata will be lost if xlog.c
		 * compresses the page.  (We must do this here because pre-v11
		 * versions of PG did not set the metapage's pd_lower correctly, so a
		 * pg_upgraded index might contain the wrong value.)
		 */
		((PageHeader) metapage)->pd_lower =
			((char *) metadata + sizeof(GinMetaPageData)) - (char *) metapage;

		MarkBufferDirty(metabuffer);

		for (i = 0; i < data.ndeleted; i++)
		{
			page = BufferGetPage(buffers[i]);
			GinPageGetOpaque(page)->flags = GIN_DELETED;
			MarkBufferDirty(buffers[i]);
		}

		if (RelationNeedsWAL(index))
		{
			XLogRecPtr	recptr;

			XLogBeginInsert();
			XLogRegisterBuffer(0, metabuffer,
							   REGBUF_WILL_INIT | REGBUF_STANDARD);
			for (i = 0; i < data.ndeleted; i++)
				XLogRegisterBuffer(i + 1, buffers[i], REGBUF_WILL_INIT);

			memcpy(&data.metadata, metadata, sizeof(GinMetaPageData));

			XLogRegisterData((char *) &data,
							 sizeof(ginxlogDeleteListPages));

			recptr = XLogInsert(RM_GIN_ID, XLOG_GIN_DELETE_LISTPAGE);
			PageSetLSN(metapage, recptr);

			for (i = 0; i < data.ndeleted; i++)
			{
				page = BufferGetPage(buffers[i]);
				PageSetLSN(page, recptr);
			}
		}

		for (i = 0; i < data.ndeleted; i++)
			UnlockReleaseBuffer(buffers[i]);

		END_CRIT_SECTION();

		for (i = 0; fill_fsm && i < data.ndeleted; i++)
			RecordFreeIndexPage(index, freespace[i]);

	} while (blknoToDelete != newHead);
}

/* Initialize empty KeyArray */
static void
initKeyArray(KeyArray *keys, int32 maxvalues)
{
	keys->keys = (Datum *) palloc(sizeof(Datum) * maxvalues);
	keys->categories = (GinNullCategory *)
		palloc(sizeof(GinNullCategory) * maxvalues);
	keys->nvalues = 0;
	keys->maxvalues = maxvalues;
}

/* Add datum to KeyArray, resizing if needed */
static void
addDatum(KeyArray *keys, Datum datum, GinNullCategory category)
{
	if (keys->nvalues >= keys->maxvalues)
	{
		keys->maxvalues *= 2;
		keys->keys = (Datum *)
			repalloc(keys->keys, sizeof(Datum) * keys->maxvalues);
		keys->categories = (GinNullCategory *)
			repalloc(keys->categories, sizeof(GinNullCategory) * keys->maxvalues);
	}

	keys->keys[keys->nvalues] = datum;
	keys->categories[keys->nvalues] = category;
	keys->nvalues++;
}

/*
 * Collect data from a pending-list page in preparation for insertion into
 * the main index.
 *
 * Go through all tuples >= startoff on page and collect values in accum
 *
 * Note that ka is just workspace --- it does not carry any state across
 * calls.
 */
static void
processPendingPage(BuildAccumulator *accum, KeyArray *ka,
				   Page page, OffsetNumber startoff)
{
	ItemPointerData heapptr;
	OffsetNumber i,
				maxoff;
	OffsetNumber attrnum;

	/* reset *ka to empty */
	ka->nvalues = 0;

	maxoff = PageGetMaxOffsetNumber(page);
	Assert(maxoff >= FirstOffsetNumber);
	ItemPointerSetInvalid(&heapptr);
	attrnum = 0;

	for (i = startoff; i <= maxoff; i = OffsetNumberNext(i))
	{
		IndexTuple	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, i));
		OffsetNumber curattnum;
		Datum		curkey;
		GinNullCategory curcategory;

		/* Check for change of heap TID or attnum */
		curattnum = gintuple_get_attrnum(accum->ginstate, itup);

		if (!ItemPointerIsValid(&heapptr))
		{
			heapptr = itup->t_tid;
			attrnum = curattnum;
		}
		else if (!(ItemPointerEquals(&heapptr, &itup->t_tid) &&
				   curattnum == attrnum))
		{
			/*
			 * ginInsertBAEntries can insert several datums per call, but only
			 * for one heap tuple and one column.  So call it at a boundary,
			 * and reset ka.
			 */
			ginInsertBAEntries(accum, &heapptr, attrnum,
							   ka->keys, ka->categories, ka->nvalues);
			ka->nvalues = 0;
			heapptr = itup->t_tid;
			attrnum = curattnum;
		}

		/* Add key to KeyArray */
		curkey = gintuple_get_key(accum->ginstate, itup, &curcategory);
		addDatum(ka, curkey, curcategory);
	}

	/* Dump out all remaining keys */
	ginInsertBAEntries(accum, &heapptr, attrnum,
					   ka->keys, ka->categories, ka->nvalues);
}

/*
 * Move tuples from pending pages into regular GIN structure.
 *
 * On first glance it looks completely not crash-safe. But if we crash
 * after posting entries to the main index and before removing them from the
 * pending list, it's okay because when we redo the posting later on, nothing
 * bad will happen.
 *
 * fill_fsm indicates that ginInsertCleanup should add deleted pages
 * to FSM otherwise caller is responsible to put deleted pages into
 * FSM.
 *
 * If stats isn't null, we count deleted pending pages into the counts.
 */
void
ginInsertCleanup(GinState *ginstate, bool full_clean,
				 bool fill_fsm, bool forceCleanup,
				 IndexBulkDeleteResult *stats)
{
	Relation	index = ginstate->index;
	Buffer		metabuffer,
				buffer;
	Page		metapage,
				page;
	GinMetaPageData *metadata;
	MemoryContext opCtx,
				oldCtx;
	BuildAccumulator accum;
	KeyArray	datums;
	BlockNumber blkno,
				blknoFinish;
	bool		cleanupFinish = false;
	bool		fsm_vac = false;
	Size		workMemory;

	/*
	 * We would like to prevent concurrent cleanup process. For that we will
	 * lock metapage in exclusive mode using LockPage() call. Nobody other
	 * will use that lock for metapage, so we keep possibility of concurrent
	 * insertion into pending list
	 */

	if (forceCleanup)
	{
		/*
		 * We are called from [auto]vacuum/analyze or gin_clean_pending_list()
		 * and we would like to wait concurrent cleanup to finish.
		 */
		LockPage(index, GIN_METAPAGE_BLKNO, ExclusiveLock);
		workMemory =
			(IsAutoVacuumWorkerProcess() && autovacuum_work_mem != -1) ?
			autovacuum_work_mem : maintenance_work_mem;
	}
	else
	{
		/*
		 * We are called from regular insert and if we see concurrent cleanup
		 * just exit in hope that concurrent process will clean up pending
		 * list.
		 */
		if (!ConditionalLockPage(index, GIN_METAPAGE_BLKNO, ExclusiveLock))
			return;
		workMemory = work_mem;
	}

	metabuffer = ReadBuffer(index, GIN_METAPAGE_BLKNO);
	LockBuffer(metabuffer, GIN_SHARE);
	metapage = BufferGetPage(metabuffer);
	metadata = GinPageGetMeta(metapage);

	if (metadata->head == InvalidBlockNumber)
	{
		/* Nothing to do */
		UnlockReleaseBuffer(metabuffer);
		UnlockPage(index, GIN_METAPAGE_BLKNO, ExclusiveLock);
		return;
	}

	/*
	 * Remember a tail page to prevent infinite cleanup if other backends add
	 * new tuples faster than we can cleanup.
	 */
	blknoFinish = metadata->tail;

	/*
	 * Read and lock head of pending list
	 */
	blkno = metadata->head;
	buffer = ReadBuffer(index, blkno);
	LockBuffer(buffer, GIN_SHARE);
	page = BufferGetPage(buffer);

	LockBuffer(metabuffer, GIN_UNLOCK);

	/*
	 * Initialize.  All temporary space will be in opCtx
	 */
	opCtx = AllocSetContextCreate(CurrentMemoryContext,
								  "GIN insert cleanup temporary context",
								  ALLOCSET_DEFAULT_SIZES);

	oldCtx = MemoryContextSwitchTo(opCtx);

	initKeyArray(&datums, 128);
	ginInitBA(&accum);
	accum.ginstate = ginstate;

	/*
	 * At the top of this loop, we have pin and lock on the current page of
	 * the pending list.  However, we'll release that before exiting the loop.
	 * Note we also have pin but not lock on the metapage.
	 */
	for (;;)
	{
		Assert(!GinPageIsDeleted(page));

		/*
		 * Are we walk through the page which as we remember was a tail when
		 * we start our cleanup?  But if caller asks us to clean up whole
		 * pending list then ignore old tail, we will work until list becomes
		 * empty.
		 */
		if (blkno == blknoFinish && full_clean == false)
			cleanupFinish = true;

		/*
		 * read page's datums into accum
		 */
		processPendingPage(&accum, &datums, page, FirstOffsetNumber);

		vacuum_delay_point();

		/*
		 * Is it time to flush memory to disk?	Flush if we are at the end of
		 * the pending list, or if we have a full row and memory is getting
		 * full.
		 */
		if (GinPageGetOpaque(page)->rightlink == InvalidBlockNumber ||
			(GinPageHasFullRow(page) &&
			 (accum.allocatedMemory >= workMemory * 1024L)))
		{
			ItemPointerData *list;
			uint32		nlist;
			Datum		key;
			GinNullCategory category;
			OffsetNumber maxoff,
						attnum;

			/*
			 * Unlock current page to increase performance. Changes of page
			 * will be checked later by comparing maxoff after completion of
			 * memory flush.
			 */
			maxoff = PageGetMaxOffsetNumber(page);
			LockBuffer(buffer, GIN_UNLOCK);

			/*
			 * Moving collected data into regular structure can take
			 * significant amount of time - so, run it without locking pending
			 * list.
			 */
			ginBeginBAScan(&accum);
			while ((list = ginGetBAEntry(&accum,
										 &attnum, &key, &category, &nlist)) != NULL)
			{
				ginEntryInsert(ginstate, attnum, key, category,
							   list, nlist, NULL);
				vacuum_delay_point();
			}

			/*
			 * Lock the whole list to remove pages
			 */
			LockBuffer(metabuffer, GIN_EXCLUSIVE);
			LockBuffer(buffer, GIN_SHARE);

			Assert(!GinPageIsDeleted(page));

			/*
			 * While we left the page unlocked, more stuff might have gotten
			 * added to it.  If so, process those entries immediately.  There
			 * shouldn't be very many, so we don't worry about the fact that
			 * we're doing this with exclusive lock. Insertion algorithm
			 * guarantees that inserted row(s) will not continue on next page.
			 * NOTE: intentionally no vacuum_delay_point in this loop.
			 */
			if (PageGetMaxOffsetNumber(page) != maxoff)
			{
				ginInitBA(&accum);
				processPendingPage(&accum, &datums, page, maxoff + 1);

				ginBeginBAScan(&accum);
				while ((list = ginGetBAEntry(&accum,
											 &attnum, &key, &category, &nlist)) != NULL)
					ginEntryInsert(ginstate, attnum, key, category,
								   list, nlist, NULL);
			}

			/*
			 * Remember next page - it will become the new list head
			 */
			blkno = GinPageGetOpaque(page)->rightlink;
			UnlockReleaseBuffer(buffer);	/* shiftList will do exclusive
											 * locking */

			/*
			 * remove read pages from pending list, at this point all content
			 * of read pages is in regular structure
			 */
			shiftList(index, metabuffer, blkno, fill_fsm, stats);

			/* At this point, some pending pages have been freed up */
			fsm_vac = true;

			Assert(blkno == metadata->head);
			LockBuffer(metabuffer, GIN_UNLOCK);

			/*
			 * if we removed the whole pending list or we cleanup tail (which
			 * we remembered on start our cleanup process) then just exit
			 */
			if (blkno == InvalidBlockNumber || cleanupFinish)
				break;

			/*
			 * release memory used so far and reinit state
			 */
			MemoryContextReset(opCtx);
			initKeyArray(&datums, datums.maxvalues);
			ginInitBA(&accum);
		}
		else
		{
			blkno = GinPageGetOpaque(page)->rightlink;
			UnlockReleaseBuffer(buffer);
		}

		/*
		 * Read next page in pending list
		 */
		vacuum_delay_point();
		buffer = ReadBuffer(index, blkno);
		LockBuffer(buffer, GIN_SHARE);
		page = BufferGetPage(buffer);
	}

	UnlockPage(index, GIN_METAPAGE_BLKNO, ExclusiveLock);
	ReleaseBuffer(metabuffer);

	/*
	 * As pending list pages can have a high churn rate, it is desirable to
	 * recycle them immediately to the FreeSpaceMap when ordinary backends
	 * clean the list.
	 */
	if (fsm_vac && fill_fsm)
		IndexFreeSpaceMapVacuum(index);

	/* Clean up temporary space */
	MemoryContextSwitchTo(oldCtx);
	MemoryContextDelete(opCtx);
}

/*
 * SQL-callable function to clean the insert pending list
 */
Datum
gin_clean_pending_list(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	Relation	indexRel = index_open(indexoid, RowExclusiveLock);
	IndexBulkDeleteResult stats;
	GinState	ginstate;

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is in progress"),
				 errhint("GIN pending list cannot be cleaned up during recovery.")));

	/* Must be a GIN index */
	if (indexRel->rd_rel->relkind != RELKIND_INDEX ||
		indexRel->rd_rel->relam != GIN_AM_OID)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a GIN index",
						RelationGetRelationName(indexRel))));

	/*
	 * Reject attempts to read non-local temporary relations; we would be
	 * likely to get wrong data since we have no visibility into the owning
	 * session's local buffers.
	 */
	if (RELATION_IS_OTHER_TEMP(indexRel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot access temporary indexes of other sessions")));

	/* User must own the index (comparable to privileges needed for VACUUM) */
	if (!pg_class_ownercheck(indexoid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_INDEX,
					   RelationGetRelationName(indexRel));

	memset(&stats, 0, sizeof(stats));
	initGinState(&ginstate, indexRel);
	ginInsertCleanup(&ginstate, true, true, true, &stats);

	index_close(indexRel, RowExclusiveLock);

	PG_RETURN_INT64((int64) stats.pages_deleted);
}
