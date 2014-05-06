/*-------------------------------------------------------------------------
 *
 * ginfast.c
 *	  Fast insert routines for the Postgres inverted index access method.
 *	  Pending entries are stored in linear list of pages.  Later on
 *	  (typically during VACUUM), ginInsertCleanup() will be invoked to
 *	  transfer pending entries into the regular index structure.  This
 *	  wins because bulk insertion is much more efficient than retail.
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			$PostgreSQL: pgsql/src/backend/access/gin/ginfast.c,v 1.3.2.2 2009/10/02 21:14:11 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/gin.h"
#include "catalog/index.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"


#define GIN_PAGE_FREESIZE \
	( BLCKSZ - MAXALIGN(SizeOfPageHeaderData) - MAXALIGN(sizeof(GinPageOpaqueData)) )

typedef struct DatumArray
{
	Datum	   *values;			/* expansible array */
	int32		nvalues;		/* current number of valid entries */
	int32		maxvalues;		/* allocated size of array */
} DatumArray;


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
	char	   *workspace;
	char	   *ptr;

	/* workspace could be a local array; we use palloc for alignment */
	workspace = palloc(BLCKSZ);

	START_CRIT_SECTION();

	GinInitBuffer(buffer, GIN_LIST);

	off = FirstOffsetNumber;
	ptr = workspace;

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
	 * tail page may contain only the whole row(s) or final part of row placed
	 * on previous pages
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

	if (!index->rd_istemp)
	{
		XLogRecData rdata[2];
		ginxlogInsertListPage data;
		XLogRecPtr	recptr;

		data.node = index->rd_node;
		data.blkno = BufferGetBlockNumber(buffer);
		data.rightlink = rightlink;
		data.ntuples = ntuples;

		rdata[0].buffer = InvalidBuffer;
		rdata[0].data = (char *) &data;
		rdata[0].len = sizeof(ginxlogInsertListPage);
		rdata[0].next = rdata + 1;

		rdata[1].buffer = InvalidBuffer;
		rdata[1].data = workspace;
		rdata[1].len = size;
		rdata[1].next = NULL;

		recptr = XLogInsert(RM_GIN_ID, XLOG_GIN_INSERT_LISTPAGE, rdata);
		PageSetLSN(page, recptr);
		PageSetTLI(page, ThisTimeLineID);
	}

	/* get free space before releasing buffer */
	freesize = PageGetExactFreeSpace(page);

	UnlockReleaseBuffer(buffer);

	END_CRIT_SECTION();

	pfree(workspace);

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
 * Inserts collected values during normal insertion. Function guarantees
 * that all values of heap will be stored sequentially, preserving order
 */
void
ginHeapTupleFastInsert(Relation index, GinState *ginstate,
					   GinTupleCollector *collector)
{
	Buffer		metabuffer;
	Page		metapage;
	GinMetaPageData *metadata = NULL;
	XLogRecData rdata[2];
	Buffer		buffer = InvalidBuffer;
	Page		page = NULL;
	ginxlogUpdateMeta data;
	bool		separateList = false;
	bool		needCleanup = false;

	if (collector->ntuples == 0)
		return;

	data.node = index->rd_node;
	data.ntuples = 0;
	data.newRightlink = data.prevTail = InvalidBlockNumber;

	rdata[0].buffer = InvalidBuffer;
	rdata[0].data = (char *) &data;
	rdata[0].len = sizeof(ginxlogUpdateMeta);
	rdata[0].next = NULL;

	metabuffer = ReadBuffer(index, GIN_METAPAGE_BLKNO);
	metapage = BufferGetPage(metabuffer);

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

		if (metadata->head == InvalidBlockNumber)
		{
			/*
			 * Main list is empty, so just copy sublist into main list
			 */
			START_CRIT_SECTION();

			memcpy(metadata, &sublist, sizeof(GinMetaPageData));
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

		buffer = ReadBuffer(index, metadata->tail);
		LockBuffer(buffer, GIN_EXCLUSIVE);
		page = BufferGetPage(buffer);

		off = (PageIsEmpty(page)) ? FirstOffsetNumber :
			OffsetNumberNext(PageGetMaxOffsetNumber(page));

		rdata[0].next = rdata + 1;

		rdata[1].buffer = buffer;
		rdata[1].buffer_std = true;
		ptr = rdata[1].data = (char *) palloc(collector->sumsize);
		rdata[1].len = collector->sumsize;
		rdata[1].next = NULL;

		data.ntuples = collector->ntuples;

		START_CRIT_SECTION();

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

		Assert((ptr - rdata[1].data) <= collector->sumsize);

		metadata->tailFreeSize = PageGetExactFreeSpace(page);

		MarkBufferDirty(buffer);
	}

	/*
	 * Write metabuffer, make xlog entry
	 */
	MarkBufferDirty(metabuffer);

	if (!index->rd_istemp)
	{
		XLogRecPtr	recptr;

		memcpy(&data.metadata, metadata, sizeof(GinMetaPageData));

		recptr = XLogInsert(RM_GIN_ID, XLOG_GIN_UPDATE_META_PAGE, rdata);
		PageSetLSN(metapage, recptr);
		PageSetTLI(metapage, ThisTimeLineID);

		if (buffer != InvalidBuffer)
		{
			PageSetLSN(page, recptr);
			PageSetTLI(page, ThisTimeLineID);
		}
	}

	if (buffer != InvalidBuffer)
		UnlockReleaseBuffer(buffer);

	/*
	 * Force pending list cleanup when it becomes too long. And,
	 * ginInsertCleanup could take significant amount of time, so we prefer to
	 * call it when it can do all the work in a single collection cycle. In
	 * non-vacuum mode, it shouldn't require maintenance_work_mem, so fire it
	 * while pending list is still small enough to fit into work_mem.
	 *
	 * ginInsertCleanup() should not be called inside our CRIT_SECTION.
	 */
	if (metadata->nPendingPages * GIN_PAGE_FREESIZE > work_mem * 1024L)
		needCleanup = true;

	UnlockReleaseBuffer(metabuffer);

	END_CRIT_SECTION();

	if (needCleanup)
		ginInsertCleanup(index, ginstate, false, NULL);
}

/*
 * Collect values from one tuples to be indexed. All values for
 * one tuples should be written at once - to guarantee consistent state
 */
uint32
ginHeapTupleFastCollect(Relation index, GinState *ginstate,
						GinTupleCollector *collector,
						OffsetNumber attnum, Datum value, ItemPointer item)
{
	Datum	   *entries;
	int32		i,
				nentries;

	entries = extractEntriesSU(ginstate, attnum, value, &nentries);

	if (nentries == 0)
		/* nothing to insert */
		return 0;

	/*
	 * Allocate/reallocate memory for storing collected tuples
	 */
	if (collector->tuples == NULL)
	{
		collector->lentuples = nentries * index->rd_att->natts;
		collector->tuples = (IndexTuple *) palloc(sizeof(IndexTuple) * collector->lentuples);
	}

	while (collector->ntuples + nentries > collector->lentuples)
	{
		collector->lentuples *= 2;
		collector->tuples = (IndexTuple *) repalloc(collector->tuples,
								  sizeof(IndexTuple) * collector->lentuples);
	}

	/*
	 * Creates tuple's array
	 */
	for (i = 0; i < nentries; i++)
	{
		collector->tuples[collector->ntuples + i] =
			GinFormTuple(index, ginstate, attnum, entries[i], NULL, 0, true);
		collector->tuples[collector->ntuples + i]->t_tid = *item;
		collector->sumsize += IndexTupleSize(collector->tuples[collector->ntuples + i]);
	}

	collector->ntuples += nentries;

	return nentries;
}

/*
 * Deletes pending list pages up to (not including) newHead page.
 * If newHead == InvalidBlockNumber then function drops the whole list.
 *
 * metapage is pinned and exclusive-locked throughout this function.
 *
 * Returns true if another cleanup process is running concurrently
 * (if so, we can just abandon our own efforts)
 */
static bool
shiftList(Relation index, Buffer metabuffer, BlockNumber newHead,
		  IndexBulkDeleteResult *stats)
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
		XLogRecData rdata[1];
		Buffer		buffers[GIN_NDELETE_AT_ONCE];

		data.node = index->rd_node;

		rdata[0].buffer = InvalidBuffer;
		rdata[0].data = (char *) &data;
		rdata[0].len = sizeof(ginxlogDeleteListPages);
		rdata[0].next = NULL;

		data.ndeleted = 0;
		while (data.ndeleted < GIN_NDELETE_AT_ONCE && blknoToDelete != newHead)
		{
			data.toDelete[data.ndeleted] = blknoToDelete;
			buffers[data.ndeleted] = ReadBuffer(index, blknoToDelete);
			LockBuffer(buffers[data.ndeleted], GIN_EXCLUSIVE);
			page = BufferGetPage(buffers[data.ndeleted]);

			data.ndeleted++;

			if (GinPageIsDeleted(page))
			{
				/* concurrent cleanup process is detected */
				for (i = 0; i < data.ndeleted; i++)
					UnlockReleaseBuffer(buffers[i]);

				return true;
			}

			nDeletedHeapTuples += GinPageGetOpaque(page)->maxoff;
			blknoToDelete = GinPageGetOpaque(page)->rightlink;
		}

		if (stats)
			stats->pages_deleted += data.ndeleted;

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

		MarkBufferDirty(metabuffer);

		for (i = 0; i < data.ndeleted; i++)
		{
			page = BufferGetPage(buffers[i]);
			GinPageGetOpaque(page)->flags = GIN_DELETED;
			MarkBufferDirty(buffers[i]);
		}

		if (!index->rd_istemp)
		{
			XLogRecPtr	recptr;

			memcpy(&data.metadata, metadata, sizeof(GinMetaPageData));

			recptr = XLogInsert(RM_GIN_ID, XLOG_GIN_DELETE_LISTPAGE, rdata);
			PageSetLSN(metapage, recptr);
			PageSetTLI(metapage, ThisTimeLineID);

			for (i = 0; i < data.ndeleted; i++)
			{
				page = BufferGetPage(buffers[i]);
				PageSetLSN(page, recptr);
				PageSetTLI(page, ThisTimeLineID);
			}
		}

		for (i = 0; i < data.ndeleted; i++)
			UnlockReleaseBuffer(buffers[i]);

		END_CRIT_SECTION();
	} while (blknoToDelete != newHead);

	return false;
}

/* Add datum to DatumArray, resizing if needed */
static void
addDatum(DatumArray *datums, Datum datum)
{
	if (datums->nvalues >= datums->maxvalues)
	{
		datums->maxvalues *= 2;
		datums->values = (Datum *) repalloc(datums->values,
										  sizeof(Datum) * datums->maxvalues);
	}

	datums->values[datums->nvalues++] = datum;
}

/*
 * Go through all tuples >= startoff on page and collect values in memory
 *
 * Note that da is just workspace --- it does not carry any state across
 * calls.
 */
static void
processPendingPage(BuildAccumulator *accum, DatumArray *da,
				   Page page, OffsetNumber startoff)
{
	ItemPointerData heapptr;
	OffsetNumber i,
				maxoff;
	OffsetNumber attrnum,
				curattnum;

	/* reset *da to empty */
	da->nvalues = 0;

	maxoff = PageGetMaxOffsetNumber(page);
	Assert(maxoff >= FirstOffsetNumber);
	ItemPointerSetInvalid(&heapptr);
	attrnum = 0;

	for (i = startoff; i <= maxoff; i = OffsetNumberNext(i))
	{
		IndexTuple	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, i));

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
			 * We can insert several datums per call, but only for one heap
			 * tuple and one column.
			 */
			ginInsertRecordBA(accum, &heapptr, attrnum, da->values, da->nvalues);
			da->nvalues = 0;
			heapptr = itup->t_tid;
			attrnum = curattnum;
		}
		addDatum(da, gin_index_getattr(accum->ginstate, itup));
	}

	ginInsertRecordBA(accum, &heapptr, attrnum, da->values, da->nvalues);
}

/*
 * Move tuples from pending pages into regular GIN structure.
 *
 * This can be called concurrently by multiple backends, so it must cope.
 * On first glance it looks completely not concurrent-safe and not crash-safe
 * either.  The reason it's okay is that multiple insertion of the same entry
 * is detected and treated as a no-op by gininsert.c.  If we crash after
 * posting entries to the main index and before removing them from the
 * pending list, it's okay because when we redo the posting later on, nothing
 * bad will happen.  Likewise, if two backends simultaneously try to post
 * a pending entry into the main index, one will succeed and one will do
 * nothing.  We try to notice when someone else is a little bit ahead of
 * us in the process, but that's just to avoid wasting cycles.  Only the
 * action of removing a page from the pending list really needs exclusive
 * lock.
 *
 * vac_delay indicates that ginInsertCleanup is called from vacuum process,
 * so call vacuum_delay_point() periodically.
 * If stats isn't null, we count deleted pending pages into the counts.
 */
void
ginInsertCleanup(Relation index, GinState *ginstate,
				 bool vac_delay, IndexBulkDeleteResult *stats)
{
	Buffer		metabuffer,
				buffer;
	Page		metapage,
				page;
	GinMetaPageData *metadata;
	MemoryContext opCtx,
				oldCtx;
	BuildAccumulator accum;
	DatumArray	datums;
	BlockNumber blkno;

	metabuffer = ReadBuffer(index, GIN_METAPAGE_BLKNO);
	LockBuffer(metabuffer, GIN_SHARE);
	metapage = BufferGetPage(metabuffer);
	metadata = GinPageGetMeta(metapage);

	if (metadata->head == InvalidBlockNumber)
	{
		/* Nothing to do */
		UnlockReleaseBuffer(metabuffer);
		return;
	}

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
								  ALLOCSET_DEFAULT_MINSIZE,
								  ALLOCSET_DEFAULT_INITSIZE,
								  ALLOCSET_DEFAULT_MAXSIZE);

	oldCtx = MemoryContextSwitchTo(opCtx);

	datums.maxvalues = 128;
	datums.nvalues = 0;
	datums.values = (Datum *) palloc(sizeof(Datum) * datums.maxvalues);

	ginInitBA(&accum);
	accum.ginstate = ginstate;

	/*
	 * At the top of this loop, we have pin and lock on the current page of
	 * the pending list.  However, we'll release that before exiting the loop.
	 * Note we also have pin but not lock on the metapage.
	 */
	for (;;)
	{
		if (GinPageIsDeleted(page))
		{
			/* another cleanup process is running concurrently */
			UnlockReleaseBuffer(buffer);
			break;
		}

		/*
		 * read page's datums into memory
		 */
		processPendingPage(&accum, &datums, page, FirstOffsetNumber);

		if (vac_delay)
			vacuum_delay_point();

		/*
		 * Is it time to flush memory to disk?	Flush if we are at the end of
		 * the pending list, or if we have a full row and memory is getting
		 * full.
		 *
		 * XXX using up maintenance_work_mem here is probably unreasonably
		 * much, since vacuum might already be using that much.
		 */
		if (GinPageGetOpaque(page)->rightlink == InvalidBlockNumber ||
			(GinPageHasFullRow(page) &&
			 (accum.allocatedMemory >= maintenance_work_mem * 1024L ||
			  accum.maxdepth > GIN_MAX_TREE_DEPTH)))
		{
			ItemPointerData *list;
			uint32		nlist;
			Datum		entry;
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
			while ((list = ginGetEntry(&accum, &attnum, &entry, &nlist)) != NULL)
			{
				ginEntryInsert(index, ginstate, attnum, entry, list, nlist, FALSE);
				if (vac_delay)
					vacuum_delay_point();
			}

			/*
			 * Lock the whole list to remove pages
			 */
			LockBuffer(metabuffer, GIN_EXCLUSIVE);
			LockBuffer(buffer, GIN_SHARE);

			if (GinPageIsDeleted(page))
			{
				/* another cleanup process is running concurrently */
				UnlockReleaseBuffer(buffer);
				LockBuffer(metabuffer, GIN_UNLOCK);
				break;
			}

			/*
			 * While we left the page unlocked, more stuff might have gotten
			 * added to it.  If so, process those entries immediately.  There
			 * shouldn't be very many, so we don't worry about the fact that
			 * we're doing this with exclusive lock. Insertion algorithm
			 * gurantees that inserted row(s) will not continue on next page.
			 * NOTE: intentionally no vacuum_delay_point in this loop.
			 */
			if (PageGetMaxOffsetNumber(page) != maxoff)
			{
				ginInitBA(&accum);
				processPendingPage(&accum, &datums, page, maxoff + 1);

				while ((list = ginGetEntry(&accum, &attnum, &entry, &nlist)) != NULL)
					ginEntryInsert(index, ginstate, attnum, entry, list, nlist, FALSE);
			}

			/*
			 * Remember next page - it will become the new list head
			 */
			blkno = GinPageGetOpaque(page)->rightlink;
			UnlockReleaseBuffer(buffer);		/* shiftList will do exclusive
												 * locking */

			/*
			 * remove readed pages from pending list, at this point all
			 * content of readed pages is in regular structure
			 */
			if (shiftList(index, metabuffer, blkno, stats))
			{
				/* another cleanup process is running concurrently */
				LockBuffer(metabuffer, GIN_UNLOCK);
				break;
			}

			Assert(blkno == metadata->head);
			LockBuffer(metabuffer, GIN_UNLOCK);

			/*
			 * if we removed the whole pending list just exit
			 */
			if (blkno == InvalidBlockNumber)
				break;

			/*
			 * release memory used so far and reinit state
			 */
			MemoryContextReset(opCtx);
			ginInitBA(&accum);
			datums.nvalues = 0;
			datums.values = (Datum *) palloc(sizeof(Datum) * datums.maxvalues);
		}
		else
		{
			blkno = GinPageGetOpaque(page)->rightlink;
			UnlockReleaseBuffer(buffer);
		}

		/*
		 * Read next page in pending list
		 */
		CHECK_FOR_INTERRUPTS();
		buffer = ReadBuffer(index, blkno);
		LockBuffer(buffer, GIN_SHARE);
		page = BufferGetPage(buffer);
	}

	ReleaseBuffer(metabuffer);

	/* Clean up temporary space */
	MemoryContextSwitchTo(oldCtx);
	MemoryContextDelete(opCtx);
}
