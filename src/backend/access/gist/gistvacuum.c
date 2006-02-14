/*-------------------------------------------------------------------------
 *
 * gistvacuum.c
 *	  interface routines for the postgres GiST index access method.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/gist/gistvacuum.c,v 1.9.2.1 2006/02/14 16:39:36 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/gist_private.h"
#include "access/gistscan.h"
#include "access/heapam.h"
#include "catalog/index.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "utils/memutils.h"
#include "storage/freespace.h"
#include "storage/smgr.h"

/* filled by gistbulkdelete, cleared by gistvacuumpcleanup */
static bool needFullVacuum = false;


typedef struct
{
	GISTSTATE	giststate;
	Relation	index;
	MemoryContext opCtx;
	IndexBulkDeleteResult *result;
} GistVacuum;

typedef struct
{
	IndexTuple *itup;
	int			ituplen;
	bool		emptypage;
} ArrayTuple;

static ArrayTuple
gistVacuumUpdate(GistVacuum *gv, BlockNumber blkno, bool needunion)
{
	ArrayTuple	res = {NULL, 0, false};
	Buffer		buffer;
	Page		page;
	OffsetNumber i,
				maxoff;
	ItemId		iid;
	int			lenaddon = 4,
				curlenaddon = 0,
				ntodelete = 0;
	IndexTuple	idxtuple,
			   *addon = NULL;
	bool		needwrite = false;
	OffsetNumber todelete[MaxOffsetNumber];
	ItemPointerData *completed = NULL;
	int			ncompleted = 0,
				lencompleted = 16;

	vacuum_delay_point();

	buffer = ReadBuffer(gv->index, blkno);
	page = (Page) BufferGetPage(buffer);
	maxoff = PageGetMaxOffsetNumber(page);

	if (GistPageIsLeaf(page))
	{
		if (GistTuplesDeleted(page))
		{
			needunion = needwrite = true;
			GistClearTuplesDeleted(page);
		}
	}
	else
	{
		completed = (ItemPointerData *) palloc(sizeof(ItemPointerData) * lencompleted);
		addon = (IndexTuple *) palloc(sizeof(IndexTuple) * lenaddon);

		for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
		{
			ArrayTuple	chldtuple;
			bool		needchildunion;

			iid = PageGetItemId(page, i);
			idxtuple = (IndexTuple) PageGetItem(page, iid);
			needchildunion = (GistTupleIsInvalid(idxtuple)) ? true : false;

			if (needchildunion)
				elog(DEBUG2, "gistVacuumUpdate: need union for block %u",
					 ItemPointerGetBlockNumber(&(idxtuple->t_tid)));

			chldtuple = gistVacuumUpdate(gv, ItemPointerGetBlockNumber(&(idxtuple->t_tid)),
										 needchildunion);
			if (chldtuple.ituplen || chldtuple.emptypage)
			{
				PageIndexTupleDelete(page, i);
				todelete[ntodelete++] = i;
				i--;
				maxoff--;
				needwrite = needunion = true;

				if (chldtuple.ituplen)
				{
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
						 * child was splitted, so we need mark completion
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

		if (curlenaddon)
		{
			/* insert updated tuples */
			if (gistnospace(page, addon, curlenaddon))
			{
				/* there is no space on page to insert tuples */
				IndexTuple *vec;
				SplitedPageLayout *dist = NULL,
						   *ptr;
				int			i;
				MemoryContext oldCtx = MemoryContextSwitchTo(gv->opCtx);

				vec = gistextractbuffer(buffer, &(res.ituplen));
				vec = gistjoinvector(vec, &(res.ituplen), addon, curlenaddon);
				res.itup = gistSplit(gv->index, buffer, vec, &(res.ituplen), &dist, &(gv->giststate));
				MemoryContextSwitchTo(oldCtx);

				vec = (IndexTuple *) palloc(sizeof(IndexTuple) * res.ituplen);
				for (i = 0; i < res.ituplen; i++)
				{
					vec[i] = (IndexTuple) palloc(IndexTupleSize(res.itup[i]));
					memcpy(vec[i], res.itup[i], IndexTupleSize(res.itup[i]));
				}
				res.itup = vec;

				if (!gv->index->rd_istemp)
				{
					XLogRecPtr	recptr;
					XLogRecData *rdata;
					ItemPointerData key;		/* set key for incomplete
												 * insert */
					char	   *xlinfo;

					ItemPointerSet(&key, blkno, TUPLE_IS_VALID);

					rdata = formSplitRdata(gv->index->rd_node, blkno,
										   &key, dist);
					xlinfo = rdata->data;

					START_CRIT_SECTION();

					recptr = XLogInsert(RM_GIST_ID, XLOG_GIST_PAGE_SPLIT, rdata);
					ptr = dist;
					while (ptr)
					{
						PageSetLSN(BufferGetPage(ptr->buffer), recptr);
						PageSetTLI(BufferGetPage(ptr->buffer), ThisTimeLineID);
						ptr = ptr->next;
					}

					END_CRIT_SECTION();
					pfree(xlinfo);
					pfree(rdata);
				}
				else
				{
					ptr = dist;
					while (ptr)
					{
						PageSetLSN(BufferGetPage(ptr->buffer), XLogRecPtrForTemp);
						ptr = ptr->next;
					}
				}

				ptr = dist;
				while (ptr)
				{
					if (BufferGetBlockNumber(ptr->buffer) != blkno)
						LockBuffer(ptr->buffer, GIST_UNLOCK);
					WriteBuffer(ptr->buffer);
					ptr = ptr->next;
				}

				if (blkno == GIST_ROOT_BLKNO)
				{
					ItemPointerData key;		/* set key for incomplete
												 * insert */

					ItemPointerSet(&key, blkno, TUPLE_IS_VALID);

					oldCtx = MemoryContextSwitchTo(gv->opCtx);
					gistnewroot(gv->index, buffer, res.itup, res.ituplen, &key);
					MemoryContextSwitchTo(oldCtx);

					WriteNoReleaseBuffer(buffer);
				}

				needwrite = false;

				MemoryContextReset(gv->opCtx);

				needunion = false;		/* gistSplit already forms unions */
			}
			else
			{
				/* enough free space */
				gistfillbuffer(gv->index, page, addon, curlenaddon, InvalidOffsetNumber);
			}
		}
	}

	if (needunion)
	{
		/* forms union for page  or check empty */
		if (PageIsEmpty(page))
		{
			if (blkno == GIST_ROOT_BLKNO)
			{
				needwrite = true;
				GistPageSetLeaf(page);
			}
			else
			{
				needwrite = true;
				res.emptypage = true;
				GistPageSetDeleted(page);
				gv->result->pages_deleted++;
			}
		}
		else
		{
			IndexTuple *vec,
						tmp;
			int			veclen = 0;
			MemoryContext oldCtx = MemoryContextSwitchTo(gv->opCtx);

			vec = gistextractbuffer(buffer, &veclen);
			tmp = gistunion(gv->index, vec, veclen, &(gv->giststate));
			MemoryContextSwitchTo(oldCtx);

			res.itup = (IndexTuple *) palloc(sizeof(IndexTuple));
			res.ituplen = 1;
			res.itup[0] = (IndexTuple) palloc(IndexTupleSize(tmp));
			memcpy(res.itup[0], tmp, IndexTupleSize(tmp));

			ItemPointerSetBlockNumber(&(res.itup[0]->t_tid), blkno);
			GistTupleSetValid(res.itup[0]);

			MemoryContextReset(gv->opCtx);
		}
	}

	if (needwrite)
	{
		if (!gv->index->rd_istemp)
		{
			XLogRecData *rdata;
			XLogRecPtr	recptr;
			char	   *xlinfo;

			rdata = formUpdateRdata(gv->index->rd_node, blkno, todelete, ntodelete,
									res.emptypage, addon, curlenaddon, NULL);
			xlinfo = rdata->data;

			START_CRIT_SECTION();
			recptr = XLogInsert(RM_GIST_ID, XLOG_GIST_ENTRY_UPDATE, rdata);
			PageSetLSN(page, recptr);
			PageSetTLI(page, ThisTimeLineID);
			END_CRIT_SECTION();

			pfree(xlinfo);
			pfree(rdata);
		}
		else
			PageSetLSN(page, XLogRecPtrForTemp);
		WriteBuffer(buffer);
	}
	else
		ReleaseBuffer(buffer);

	if (ncompleted && !gv->index->rd_istemp)
		gistxlogInsertCompletion(gv->index->rd_node, completed, ncompleted);

	for (i = 0; i < curlenaddon; i++)
		pfree(addon[i]);
	if (addon)
		pfree(addon);
	if (completed)
		pfree(completed);
	return res;
}

/*
 * For usial vacuum just update FSM, for full vacuum
 * reforms parent tuples if some of childs was deleted or changed,
 * update invalid tuples (they can exsist from last crash recovery only),
 * tries to get smaller index
 */

Datum
gistvacuumcleanup(PG_FUNCTION_ARGS)
{
	Relation	rel = (Relation) PG_GETARG_POINTER(0);
	IndexVacuumCleanupInfo *info = (IndexVacuumCleanupInfo *) PG_GETARG_POINTER(1);
	IndexBulkDeleteResult *stats = (IndexBulkDeleteResult *) PG_GETARG_POINTER(2);
	BlockNumber npages,
				blkno;
	BlockNumber nFreePages,
			   *freePages,
				maxFreePages;
	BlockNumber lastBlock = GIST_ROOT_BLKNO,
				lastFilledBlock = GIST_ROOT_BLKNO;
	bool		needLock;

	/* gistVacuumUpdate may cause hard work */
	if (info->vacuum_full)
	{
		GistVacuum	gv;
		ArrayTuple	res;

		LockRelation(rel, AccessExclusiveLock);

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
	else if (needFullVacuum)
		ereport(NOTICE,
				(errmsg("index \"%s\" needs VACUUM FULL or REINDEX to finish crash recovery",
						RelationGetRelationName(rel))));

	needFullVacuum = false;

	needLock = !RELATION_IS_LOCAL(rel);
	if (info->vacuum_full)
		needLock = false;		/* relation locked with AccessExclusiveLock */

	/* try to find deleted pages */
	if (needLock)
		LockRelationForExtension(rel, ExclusiveLock);
	npages = RelationGetNumberOfBlocks(rel);
	if (needLock)
		UnlockRelationForExtension(rel, ExclusiveLock);

	maxFreePages = npages;
	if (maxFreePages > MaxFSMPages)
		maxFreePages = MaxFSMPages;

	nFreePages = 0;
	freePages = (BlockNumber *) palloc(sizeof(BlockNumber) * maxFreePages);
	for (blkno = GIST_ROOT_BLKNO + 1; blkno < npages; blkno++)
	{
		Buffer		buffer;
		Page		page;

		vacuum_delay_point();

		buffer = ReadBuffer(rel, blkno);
		LockBuffer(buffer, GIST_SHARE);
		page = (Page) BufferGetPage(buffer);

		if (GistPageIsDeleted(page))
		{
			if (nFreePages < maxFreePages)
			{
				freePages[nFreePages] = blkno;
				nFreePages++;
			}
		}
		else
			lastFilledBlock = blkno;
		LockBuffer(buffer, GIST_UNLOCK);
		ReleaseBuffer(buffer);
	}
	lastBlock = npages - 1;

	if (info->vacuum_full && nFreePages > 0)
	{							/* try to truncate index */
		int			i;

		for (i = 0; i < nFreePages; i++)
			if (freePages[i] >= lastFilledBlock)
			{
				nFreePages = i;
				break;
			}

		if (lastBlock > lastFilledBlock)
			RelationTruncate(rel, lastFilledBlock + 1);
		stats->pages_removed = lastBlock - lastFilledBlock;
	}

	RecordIndexFreeSpace(&rel->rd_node, nFreePages, freePages);
	pfree(freePages);

	/* return statistics */
	stats->pages_free = nFreePages;
	if (needLock)
		LockRelationForExtension(rel, ExclusiveLock);
	stats->num_pages = RelationGetNumberOfBlocks(rel);
	if (needLock)
		UnlockRelationForExtension(rel, ExclusiveLock);

	if (info->vacuum_full)
		UnlockRelation(rel, AccessExclusiveLock);

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
	Relation	rel = (Relation) PG_GETARG_POINTER(0);
	IndexBulkDeleteCallback callback = (IndexBulkDeleteCallback) PG_GETARG_POINTER(1);
	void	   *callback_state = (void *) PG_GETARG_POINTER(2);
	IndexBulkDeleteResult *result = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
	GistBDItem *stack,
			   *ptr;
	bool		needLock;

	stack = (GistBDItem *) palloc0(sizeof(GistBDItem));

	stack->blkno = GIST_ROOT_BLKNO;
	needFullVacuum = false;

	while (stack)
	{
		Buffer		buffer = ReadBuffer(rel, stack->blkno);
		Page		page;
		OffsetNumber i,
					maxoff;
		IndexTuple	idxtuple;
		ItemId		iid;

		LockBuffer(buffer, GIST_SHARE);
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
				LockBuffer(buffer, GIST_UNLOCK);
				ReleaseBuffer(buffer);
				/* one more check */
				continue;
			}

			/*
			 * check for split proceeded after look at parent, we should check
			 * it after relock
			 */
			pushStackIfSplited(page, stack);

			maxoff = PageGetMaxOffsetNumber(page);

			for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
			{
				iid = PageGetItemId(page, i);
				idxtuple = (IndexTuple) PageGetItem(page, iid);

				if (callback(&(idxtuple->t_tid), callback_state))
				{
					PageIndexTupleDelete(page, i);
					todelete[ntodelete] = i;
					i--;
					maxoff--;
					ntodelete++;
					result->tuples_removed += 1;
					Assert(maxoff == PageGetMaxOffsetNumber(page));
				}
				else
					result->num_index_tuples += 1;
			}

			if (ntodelete)
			{
				GistMarkTuplesDeleted(page);

				if (!rel->rd_istemp)
				{
					XLogRecData *rdata;
					XLogRecPtr	recptr;
					gistxlogEntryUpdate *xlinfo;

					rdata = formUpdateRdata(rel->rd_node, stack->blkno, todelete, ntodelete,
											false, NULL, 0, NULL);
					xlinfo = (gistxlogEntryUpdate *) rdata->data;

					START_CRIT_SECTION();
					recptr = XLogInsert(RM_GIST_ID, XLOG_GIST_ENTRY_UPDATE, rdata);
					PageSetLSN(page, recptr);
					PageSetTLI(page, ThisTimeLineID);
					END_CRIT_SECTION();

					pfree(xlinfo);
					pfree(rdata);
				}
				else
					PageSetLSN(page, XLogRecPtrForTemp);
				WriteNoReleaseBuffer(buffer);
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
					needFullVacuum = true;
			}
		}

		LockBuffer(buffer, GIST_UNLOCK);
		ReleaseBuffer(buffer);

		ptr = stack->next;
		pfree(stack);
		stack = ptr;

		vacuum_delay_point();
	}

	needLock = !RELATION_IS_LOCAL(rel);

	if (needLock)
		LockRelationForExtension(rel, ExclusiveLock);
	result->num_pages = RelationGetNumberOfBlocks(rel);
	if (needLock)
		UnlockRelationForExtension(rel, ExclusiveLock);

	PG_RETURN_POINTER(result);
}
