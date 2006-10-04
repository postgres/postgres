/*-------------------------------------------------------------------------
 *
 * gistvacuum.c
 *	  interface routines for the postgres GiST index access method.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/gist/gistvacuum.c,v 1.28 2006/10/04 00:29:48 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/gist_private.h"
#include "access/heapam.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "storage/freespace.h"
#include "utils/memutils.h"


typedef struct GistBulkDeleteResult
{
	IndexBulkDeleteResult std;	/* common state */
	bool		needFullVacuum;
} GistBulkDeleteResult;

typedef struct
{
	GISTSTATE	giststate;
	Relation	index;
	MemoryContext opCtx;
	GistBulkDeleteResult *result;
} GistVacuum;

typedef struct
{
	IndexTuple *itup;
	int			ituplen;
	bool		emptypage;
} ArrayTuple;

/*
 * Make union of keys on page
 */
static IndexTuple
PageMakeUnionKey(GistVacuum *gv, Buffer buffer)
{
	Page		page = BufferGetPage(buffer);
	IndexTuple *vec,
				tmp,
				res;
	int			veclen = 0;
	MemoryContext oldCtx = MemoryContextSwitchTo(gv->opCtx);

	vec = gistextractpage(page, &veclen);

	/*
	 * we call gistunion() in temprorary context because user-defined
	 * functions called in gistunion() may do not free all memory
	 */
	tmp = gistunion(gv->index, vec, veclen, &(gv->giststate));
	MemoryContextSwitchTo(oldCtx);

	res = (IndexTuple) palloc(IndexTupleSize(tmp));
	memcpy(res, tmp, IndexTupleSize(tmp));

	ItemPointerSetBlockNumber(&(res->t_tid), BufferGetBlockNumber(buffer));
	GistTupleSetValid(res);

	MemoryContextReset(gv->opCtx);

	return res;
}

static void
gistDeleteSubtree(GistVacuum *gv, BlockNumber blkno)
{
	Buffer		buffer;
	Page		page;

	buffer = ReadBuffer(gv->index, blkno);
	LockBuffer(buffer, GIST_EXCLUSIVE);
	page = (Page) BufferGetPage(buffer);

	if (!GistPageIsLeaf(page))
	{
		int			i;

		for (i = FirstOffsetNumber; i <= PageGetMaxOffsetNumber(page); i = OffsetNumberNext(i))
		{
			ItemId		iid = PageGetItemId(page, i);
			IndexTuple	idxtuple = (IndexTuple) PageGetItem(page, iid);

			gistDeleteSubtree(gv, ItemPointerGetBlockNumber(&(idxtuple->t_tid)));
		}
	}

	START_CRIT_SECTION();

	MarkBufferDirty(buffer);

	page = (Page) BufferGetPage(buffer);
	GistPageSetDeleted(page);
	gv->result->std.pages_deleted++;

	if (!gv->index->rd_istemp)
	{
		XLogRecData rdata[2];
		XLogRecPtr	recptr;
		gistxlogPageDelete xlrec;

		xlrec.node = gv->index->rd_node;
		xlrec.blkno = blkno;

		rdata[0].buffer = buffer;
		rdata[0].buffer_std = true;
		rdata[0].data = NULL;
		rdata[0].len = 0;
		rdata[0].next = &(rdata[1]);

		rdata[1].buffer = InvalidBuffer;
		rdata[1].data = (char *) &xlrec;
		rdata[1].len = sizeof(gistxlogPageDelete);
		rdata[1].next = NULL;

		recptr = XLogInsert(RM_GIST_ID, XLOG_GIST_PAGE_DELETE, rdata);
		PageSetLSN(page, recptr);
		PageSetTLI(page, ThisTimeLineID);
	}
	else
		PageSetLSN(page, XLogRecPtrForTemp);

	END_CRIT_SECTION();

	UnlockReleaseBuffer(buffer);
}

static Page
GistPageGetCopyPage(Page page)
{
	Size		pageSize = PageGetPageSize(page);
	Page		tmppage;

	tmppage = (Page) palloc(pageSize);
	memcpy(tmppage, page, pageSize);

	return tmppage;
}

static ArrayTuple
vacuumSplitPage(GistVacuum *gv, Page tempPage, Buffer buffer, IndexTuple *addon, int curlenaddon)
{
	ArrayTuple	res = {NULL, 0, false};
	IndexTuple *vec;
	SplitedPageLayout *dist = NULL,
			   *ptr;
	int			i,
				veclen = 0;
	BlockNumber blkno = BufferGetBlockNumber(buffer);
	MemoryContext oldCtx = MemoryContextSwitchTo(gv->opCtx);

	vec = gistextractpage(tempPage, &veclen);
	vec = gistjoinvector(vec, &veclen, addon, curlenaddon);
	dist = gistSplit(gv->index, tempPage, vec, veclen, &(gv->giststate));

	MemoryContextSwitchTo(oldCtx);

	if (blkno != GIST_ROOT_BLKNO)
	{
		/* if non-root split then we should not allocate new buffer */
		dist->buffer = buffer;
		dist->page = tempPage;
		/* during vacuum we never split leaf page */
		GistPageGetOpaque(dist->page)->flags = 0;
	}
	else
		pfree(tempPage);

	res.itup = (IndexTuple *) palloc(sizeof(IndexTuple) * veclen);
	res.ituplen = 0;

	/* make new pages and fills them */
	for (ptr = dist; ptr; ptr = ptr->next)
	{
		char	   *data;

		if (ptr->buffer == InvalidBuffer)
		{
			ptr->buffer = gistNewBuffer(gv->index);
			GISTInitBuffer(ptr->buffer, 0);
			ptr->page = BufferGetPage(ptr->buffer);
		}
		ptr->block.blkno = BufferGetBlockNumber(ptr->buffer);

		data = (char *) (ptr->list);
		for (i = 0; i < ptr->block.num; i++)
		{
			if (PageAddItem(ptr->page, (Item) data, IndexTupleSize((IndexTuple) data), i + FirstOffsetNumber, LP_USED) == InvalidOffsetNumber)
				elog(ERROR, "failed to add item to index page in \"%s\"", RelationGetRelationName(gv->index));
			data += IndexTupleSize((IndexTuple) data);
		}

		ItemPointerSetBlockNumber(&(ptr->itup->t_tid), ptr->block.blkno);
		res.itup[res.ituplen] = (IndexTuple) palloc(IndexTupleSize(ptr->itup));
		memcpy(res.itup[res.ituplen], ptr->itup, IndexTupleSize(ptr->itup));
		res.ituplen++;
	}

	START_CRIT_SECTION();

	for (ptr = dist; ptr; ptr = ptr->next)
	{
		MarkBufferDirty(ptr->buffer);
		GistPageGetOpaque(ptr->page)->rightlink = InvalidBlockNumber;
	}

	/* restore splitted non-root page */
	if (blkno != GIST_ROOT_BLKNO)
	{
		PageRestoreTempPage(dist->page, BufferGetPage(dist->buffer));
		dist->page = BufferGetPage(dist->buffer);
	}

	if (!gv->index->rd_istemp)
	{
		XLogRecPtr	recptr;
		XLogRecData *rdata;
		ItemPointerData key;	/* set key for incomplete insert */
		char	   *xlinfo;

		ItemPointerSet(&key, blkno, TUPLE_IS_VALID);

		rdata = formSplitRdata(gv->index->rd_node, blkno,
							   false, &key, dist);
		xlinfo = rdata->data;

		recptr = XLogInsert(RM_GIST_ID, XLOG_GIST_PAGE_SPLIT, rdata);
		for (ptr = dist; ptr; ptr = ptr->next)
		{
			PageSetLSN(BufferGetPage(ptr->buffer), recptr);
			PageSetTLI(BufferGetPage(ptr->buffer), ThisTimeLineID);
		}

		pfree(xlinfo);
		pfree(rdata);
	}
	else
	{
		for (ptr = dist; ptr; ptr = ptr->next)
			PageSetLSN(BufferGetPage(ptr->buffer), XLogRecPtrForTemp);
	}

	for (ptr = dist; ptr; ptr = ptr->next)
	{
		/* we must keep the buffer pin on the head page */
		if (BufferGetBlockNumber(ptr->buffer) != blkno)
			UnlockReleaseBuffer(ptr->buffer);
	}

	if (blkno == GIST_ROOT_BLKNO)
	{
		ItemPointerData key;	/* set key for incomplete insert */

		ItemPointerSet(&key, blkno, TUPLE_IS_VALID);

		gistnewroot(gv->index, buffer, res.itup, res.ituplen, &key);
	}

	END_CRIT_SECTION();

	MemoryContextReset(gv->opCtx);

	return res;
}

static ArrayTuple
gistVacuumUpdate(GistVacuum *gv, BlockNumber blkno, bool needunion)
{
	ArrayTuple	res = {NULL, 0, false};
	Buffer		buffer;
	Page		page,
				tempPage = NULL;
	OffsetNumber i,
				maxoff;
	ItemId		iid;
	int			lenaddon = 4,
				curlenaddon = 0,
				nOffToDelete = 0,
				nBlkToDelete = 0;
	IndexTuple	idxtuple,
			   *addon = NULL;
	bool		needwrite = false;
	OffsetNumber offToDelete[MaxOffsetNumber];
	BlockNumber blkToDelete[MaxOffsetNumber];
	ItemPointerData *completed = NULL;
	int			ncompleted = 0,
				lencompleted = 16;

	vacuum_delay_point();

	buffer = ReadBuffer(gv->index, blkno);
	LockBuffer(buffer, GIST_EXCLUSIVE);
	gistcheckpage(gv->index, buffer);
	page = (Page) BufferGetPage(buffer);
	maxoff = PageGetMaxOffsetNumber(page);

	if (GistPageIsLeaf(page))
	{
		if (GistTuplesDeleted(page))
			needunion = needwrite = true;
	}
	else
	{
		completed = (ItemPointerData *) palloc(sizeof(ItemPointerData) * lencompleted);
		addon = (IndexTuple *) palloc(sizeof(IndexTuple) * lenaddon);

		/* get copy of page to work */
		tempPage = GistPageGetCopyPage(page);

		for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
		{
			ArrayTuple	chldtuple;
			bool		needchildunion;

			iid = PageGetItemId(tempPage, i);
			idxtuple = (IndexTuple) PageGetItem(tempPage, iid);
			needchildunion = (GistTupleIsInvalid(idxtuple)) ? true : false;

			if (needchildunion)
				elog(DEBUG2, "gistVacuumUpdate: need union for block %u",
					 ItemPointerGetBlockNumber(&(idxtuple->t_tid)));

			chldtuple = gistVacuumUpdate(gv, ItemPointerGetBlockNumber(&(idxtuple->t_tid)),
										 needchildunion);
			if (chldtuple.ituplen || chldtuple.emptypage)
			{
				/* update tuple or/and inserts new */
				if (chldtuple.emptypage)
					blkToDelete[nBlkToDelete++] = ItemPointerGetBlockNumber(&(idxtuple->t_tid));
				offToDelete[nOffToDelete++] = i;
				PageIndexTupleDelete(tempPage, i);
				i--;
				maxoff--;
				needwrite = needunion = true;

				if (chldtuple.ituplen)
				{

					Assert(chldtuple.emptypage == false);
					while (curlenaddon + chldtuple.ituplen >= lenaddon)
					{
						lenaddon *= 2;
						addon = (IndexTuple *) repalloc(addon, sizeof(IndexTuple) * lenaddon);
					}

					memcpy(addon + curlenaddon, chldtuple.itup, chldtuple.ituplen * sizeof(IndexTuple));

					curlenaddon += chldtuple.ituplen;

					if (chldtuple.ituplen > 1)
					{
						/*
						 * child was split, so we need mark completion
						 * insert(split)
						 */
						int			j;

						while (ncompleted + chldtuple.ituplen > lencompleted)
						{
							lencompleted *= 2;
							completed = (ItemPointerData *) repalloc(completed, sizeof(ItemPointerData) * lencompleted);
						}
						for (j = 0; j < chldtuple.ituplen; j++)
						{
							ItemPointerCopy(&(chldtuple.itup[j]->t_tid), completed + ncompleted);
							ncompleted++;
						}
					}
					pfree(chldtuple.itup);
				}
			}
		}

		Assert(maxoff == PageGetMaxOffsetNumber(tempPage));

		if (curlenaddon)
		{
			/* insert updated tuples */
			if (gistnospace(tempPage, addon, curlenaddon, InvalidOffsetNumber, 0))
			{
				/* there is no space on page to insert tuples */
				res = vacuumSplitPage(gv, tempPage, buffer, addon, curlenaddon);
				tempPage = NULL;	/* vacuumSplitPage() free tempPage */
				needwrite = needunion = false;	/* gistSplit already forms
												 * unions and writes pages */
			}
			else
				/* enough free space */
				gistfillbuffer(gv->index, tempPage, addon, curlenaddon, InvalidOffsetNumber);
		}
	}

	/*
	 * If page is empty, we should remove pointer to it before deleting page
	 * (except root)
	 */

	if (blkno != GIST_ROOT_BLKNO && (PageIsEmpty(page) || (tempPage && PageIsEmpty(tempPage))))
	{
		/*
		 * New version of page is empty, so leave it unchanged, upper call
		 * will mark our page as deleted. In case of page split we never will
		 * be here...
		 *
		 * If page was empty it can't become non-empty during processing
		 */
		res.emptypage = true;
		UnlockReleaseBuffer(buffer);
	}
	else
	{
		/* write page and remove its childs if it need */

		START_CRIT_SECTION();

		if (tempPage && needwrite)
		{
			PageRestoreTempPage(tempPage, page);
			tempPage = NULL;
		}

		/* Empty index */
		if (PageIsEmpty(page) && blkno == GIST_ROOT_BLKNO)
		{
			needwrite = true;
			GistPageSetLeaf(page);
		}


		if (needwrite)
		{
			MarkBufferDirty(buffer);
			GistClearTuplesDeleted(page);

			if (!gv->index->rd_istemp)
			{
				XLogRecData *rdata;
				XLogRecPtr	recptr;
				char	   *xlinfo;

				rdata = formUpdateRdata(gv->index->rd_node, buffer,
										offToDelete, nOffToDelete,
										addon, curlenaddon, NULL);
				xlinfo = rdata->next->data;

				recptr = XLogInsert(RM_GIST_ID, XLOG_GIST_PAGE_UPDATE, rdata);
				PageSetLSN(page, recptr);
				PageSetTLI(page, ThisTimeLineID);

				pfree(xlinfo);
				pfree(rdata);
			}
			else
				PageSetLSN(page, XLogRecPtrForTemp);
		}

		END_CRIT_SECTION();

		if (needunion && !PageIsEmpty(page))
		{
			res.itup = (IndexTuple *) palloc(sizeof(IndexTuple));
			res.ituplen = 1;
			res.itup[0] = PageMakeUnionKey(gv, buffer);
		}

		UnlockReleaseBuffer(buffer);

		/* delete empty children, now we havn't any links to pointed subtrees */
		for (i = 0; i < nBlkToDelete; i++)
			gistDeleteSubtree(gv, blkToDelete[i]);

		if (ncompleted && !gv->index->rd_istemp)
			gistxlogInsertCompletion(gv->index->rd_node, completed, ncompleted);
	}


	for (i = 0; i < curlenaddon; i++)
		pfree(addon[i]);
	if (addon)
		pfree(addon);
	if (completed)
		pfree(completed);
	if (tempPage)
		pfree(tempPage);

	return res;
}

/*
 * For usual vacuum just update FSM, for full vacuum
 * reforms parent tuples if some of childs was deleted or changed,
 * update invalid tuples (they can exist from last crash recovery only),
 * tries to get smaller index
 */

Datum
gistvacuumcleanup(PG_FUNCTION_ARGS)
{
	IndexVacuumInfo *info = (IndexVacuumInfo *) PG_GETARG_POINTER(0);
	GistBulkDeleteResult *stats = (GistBulkDeleteResult *) PG_GETARG_POINTER(1);
	Relation	rel = info->index;
	BlockNumber npages,
				blkno;
	BlockNumber totFreePages,
				nFreePages,
			   *freePages,
				maxFreePages;
	BlockNumber lastBlock = GIST_ROOT_BLKNO,
				lastFilledBlock = GIST_ROOT_BLKNO;
	bool		needLock;

	/* Set up all-zero stats if gistbulkdelete wasn't called */
	if (stats == NULL)
	{
		stats = (GistBulkDeleteResult *) palloc0(sizeof(GistBulkDeleteResult));
		/* use heap's tuple count */
		Assert(info->num_heap_tuples >= 0);
		stats->std.num_index_tuples = info->num_heap_tuples;

		/*
		 * XXX the above is wrong if index is partial.	Would it be OK to just
		 * return NULL, or is there work we must do below?
		 */
	}

	/* gistVacuumUpdate may cause hard work */
	if (info->vacuum_full)
	{
		GistVacuum	gv;
		ArrayTuple	res;

		/* note: vacuum.c already acquired AccessExclusiveLock on index */

		gv.index = rel;
		initGISTstate(&(gv.giststate), rel);
		gv.opCtx = createTempGistContext();
		gv.result = stats;

		/* walk through the entire index for update tuples */
		res = gistVacuumUpdate(&gv, GIST_ROOT_BLKNO, false);
		/* cleanup */
		if (res.itup)
		{
			int			i;

			for (i = 0; i < res.ituplen; i++)
				pfree(res.itup[i]);
			pfree(res.itup);
		}
		freeGISTstate(&(gv.giststate));
		MemoryContextDelete(gv.opCtx);
	}
	else if (stats->needFullVacuum)
		ereport(NOTICE,
				(errmsg("index \"%s\" needs VACUUM FULL or REINDEX to finish crash recovery",
						RelationGetRelationName(rel))));

	/*
	 * If vacuum full, we already have exclusive lock on the index. Otherwise,
	 * need lock unless it's local to this backend.
	 */
	if (info->vacuum_full)
		needLock = false;
	else
		needLock = !RELATION_IS_LOCAL(rel);

	/* try to find deleted pages */
	if (needLock)
		LockRelationForExtension(rel, ExclusiveLock);
	npages = RelationGetNumberOfBlocks(rel);
	if (needLock)
		UnlockRelationForExtension(rel, ExclusiveLock);

	maxFreePages = npages;
	if (maxFreePages > MaxFSMPages)
		maxFreePages = MaxFSMPages;

	totFreePages = nFreePages = 0;
	freePages = (BlockNumber *) palloc(sizeof(BlockNumber) * maxFreePages);

	for (blkno = GIST_ROOT_BLKNO + 1; blkno < npages; blkno++)
	{
		Buffer		buffer;
		Page		page;

		vacuum_delay_point();

		buffer = ReadBuffer(rel, blkno);
		LockBuffer(buffer, GIST_SHARE);
		page = (Page) BufferGetPage(buffer);

		if (PageIsNew(page) || GistPageIsDeleted(page))
		{
			if (nFreePages < maxFreePages)
				freePages[nFreePages++] = blkno;
			totFreePages++;
		}
		else
			lastFilledBlock = blkno;
		UnlockReleaseBuffer(buffer);
	}
	lastBlock = npages - 1;

	if (info->vacuum_full && nFreePages > 0)
	{							/* try to truncate index */
		int			i;

		for (i = 0; i < nFreePages; i++)
			if (freePages[i] >= lastFilledBlock)
			{
				totFreePages = nFreePages = i;
				break;
			}

		if (lastBlock > lastFilledBlock)
			RelationTruncate(rel, lastFilledBlock + 1);
		stats->std.pages_removed = lastBlock - lastFilledBlock;
	}

	RecordIndexFreeSpace(&rel->rd_node, totFreePages, nFreePages, freePages);
	pfree(freePages);

	/* return statistics */
	stats->std.pages_free = totFreePages;
	if (needLock)
		LockRelationForExtension(rel, ExclusiveLock);
	stats->std.num_pages = RelationGetNumberOfBlocks(rel);
	if (needLock)
		UnlockRelationForExtension(rel, ExclusiveLock);

	PG_RETURN_POINTER(stats);
}

typedef struct GistBDItem
{
	GistNSN		parentlsn;
	BlockNumber blkno;
	struct GistBDItem *next;
} GistBDItem;

static void
pushStackIfSplited(Page page, GistBDItem *stack)
{
	GISTPageOpaque opaque = GistPageGetOpaque(page);

	if (stack->blkno != GIST_ROOT_BLKNO && !XLogRecPtrIsInvalid(stack->parentlsn) &&
		XLByteLT(stack->parentlsn, opaque->nsn) &&
		opaque->rightlink != InvalidBlockNumber /* sanity check */ )
	{
		/* split page detected, install right link to the stack */

		GistBDItem *ptr = (GistBDItem *) palloc(sizeof(GistBDItem));

		ptr->blkno = opaque->rightlink;
		ptr->parentlsn = stack->parentlsn;
		ptr->next = stack->next;
		stack->next = ptr;
	}
}


/*
 * Bulk deletion of all index entries pointing to a set of heap tuples and
 * check invalid tuples after crash recovery.
 * The set of target tuples is specified via a callback routine that tells
 * whether any given heap tuple (identified by ItemPointer) is being deleted.
 *
 * Result: a palloc'd struct containing statistical info for VACUUM displays.
 */
Datum
gistbulkdelete(PG_FUNCTION_ARGS)
{
	IndexVacuumInfo *info = (IndexVacuumInfo *) PG_GETARG_POINTER(0);
	GistBulkDeleteResult *stats = (GistBulkDeleteResult *) PG_GETARG_POINTER(1);
	IndexBulkDeleteCallback callback = (IndexBulkDeleteCallback) PG_GETARG_POINTER(2);
	void	   *callback_state = (void *) PG_GETARG_POINTER(3);
	Relation	rel = info->index;
	GistBDItem *stack,
			   *ptr;

	/* first time through? */
	if (stats == NULL)
		stats = (GistBulkDeleteResult *) palloc0(sizeof(GistBulkDeleteResult));
	/* we'll re-count the tuples each time */
	stats->std.num_index_tuples = 0;

	stack = (GistBDItem *) palloc0(sizeof(GistBDItem));
	stack->blkno = GIST_ROOT_BLKNO;

	while (stack)
	{
		Buffer		buffer = ReadBuffer(rel, stack->blkno);
		Page		page;
		OffsetNumber i,
					maxoff;
		IndexTuple	idxtuple;
		ItemId		iid;

		LockBuffer(buffer, GIST_SHARE);
		gistcheckpage(rel, buffer);
		page = (Page) BufferGetPage(buffer);

		if (GistPageIsLeaf(page))
		{
			OffsetNumber todelete[MaxOffsetNumber];
			int			ntodelete = 0;

			LockBuffer(buffer, GIST_UNLOCK);
			LockBuffer(buffer, GIST_EXCLUSIVE);

			page = (Page) BufferGetPage(buffer);
			if (stack->blkno == GIST_ROOT_BLKNO && !GistPageIsLeaf(page))
			{
				/* only the root can become non-leaf during relock */
				UnlockReleaseBuffer(buffer);
				/* one more check */
				continue;
			}

			/*
			 * check for split proceeded after look at parent, we should check
			 * it after relock
			 */
			pushStackIfSplited(page, stack);

			/*
			 * Remove deletable tuples from page
			 */

			maxoff = PageGetMaxOffsetNumber(page);

			for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
			{
				iid = PageGetItemId(page, i);
				idxtuple = (IndexTuple) PageGetItem(page, iid);

				if (callback(&(idxtuple->t_tid), callback_state))
				{
					todelete[ntodelete] = i - ntodelete;
					ntodelete++;
					stats->std.tuples_removed += 1;
				}
				else
					stats->std.num_index_tuples += 1;
			}

			if (ntodelete)
			{
				START_CRIT_SECTION();

				MarkBufferDirty(buffer);

				for (i = 0; i < ntodelete; i++)
					PageIndexTupleDelete(page, todelete[i]);
				GistMarkTuplesDeleted(page);

				if (!rel->rd_istemp)
				{
					XLogRecData *rdata;
					XLogRecPtr	recptr;
					gistxlogPageUpdate *xlinfo;

					rdata = formUpdateRdata(rel->rd_node, buffer,
											todelete, ntodelete,
											NULL, 0,
											NULL);
					xlinfo = (gistxlogPageUpdate *) rdata->next->data;

					recptr = XLogInsert(RM_GIST_ID, XLOG_GIST_PAGE_UPDATE, rdata);
					PageSetLSN(page, recptr);
					PageSetTLI(page, ThisTimeLineID);

					pfree(xlinfo);
					pfree(rdata);
				}
				else
					PageSetLSN(page, XLogRecPtrForTemp);

				END_CRIT_SECTION();
			}

		}
		else
		{
			/* check for split proceeded after look at parent */
			pushStackIfSplited(page, stack);

			maxoff = PageGetMaxOffsetNumber(page);

			for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
			{
				iid = PageGetItemId(page, i);
				idxtuple = (IndexTuple) PageGetItem(page, iid);

				ptr = (GistBDItem *) palloc(sizeof(GistBDItem));
				ptr->blkno = ItemPointerGetBlockNumber(&(idxtuple->t_tid));
				ptr->parentlsn = PageGetLSN(page);
				ptr->next = stack->next;
				stack->next = ptr;

				if (GistTupleIsInvalid(idxtuple))
					stats->needFullVacuum = true;
			}
		}

		UnlockReleaseBuffer(buffer);

		ptr = stack->next;
		pfree(stack);
		stack = ptr;

		vacuum_delay_point();
	}

	PG_RETURN_POINTER(stats);
}
