/*-------------------------------------------------------------------------
 *
 * gistxlog.c
 *	  WAL replay logic for GiST.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			 $PostgreSQL: pgsql/src/backend/access/gist/gistxlog.c,v 1.24 2006/10/04 00:29:48 momjian Exp $
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/gist_private.h"
#include "access/heapam.h"
#include "miscadmin.h"
#include "utils/memutils.h"


typedef struct
{
	gistxlogPageUpdate *data;
	int			len;
	IndexTuple *itup;
	OffsetNumber *todelete;
} PageUpdateRecord;

typedef struct
{
	gistxlogPage *header;
	IndexTuple *itup;
} NewPage;

typedef struct
{
	gistxlogPageSplit *data;
	NewPage    *page;
} PageSplitRecord;

/* track for incomplete inserts, idea was taken from nbtxlog.c */

typedef struct gistIncompleteInsert
{
	RelFileNode node;
	BlockNumber origblkno;		/* for splits */
	ItemPointerData key;
	int			lenblk;
	BlockNumber *blkno;
	XLogRecPtr	lsn;
	BlockNumber *path;
	int			pathlen;
} gistIncompleteInsert;


static MemoryContext opCtx;		/* working memory for operations */
static MemoryContext insertCtx; /* holds incomplete_inserts list */
static List *incomplete_inserts;


#define ItemPointerEQ(a, b) \
	( ItemPointerGetOffsetNumber(a) == ItemPointerGetOffsetNumber(b) && \
	  ItemPointerGetBlockNumber (a) == ItemPointerGetBlockNumber(b) )


static void
pushIncompleteInsert(RelFileNode node, XLogRecPtr lsn, ItemPointerData key,
					 BlockNumber *blkno, int lenblk,
					 PageSplitRecord *xlinfo /* to extract blkno info */ )
{
	MemoryContext oldCxt;
	gistIncompleteInsert *ninsert;

	if (!ItemPointerIsValid(&key))

		/*
		 * if key is null then we should not store insertion as incomplete,
		 * because it's a vacuum operation..
		 */
		return;

	oldCxt = MemoryContextSwitchTo(insertCtx);
	ninsert = (gistIncompleteInsert *) palloc(sizeof(gistIncompleteInsert));

	ninsert->node = node;
	ninsert->key = key;
	ninsert->lsn = lsn;

	if (lenblk && blkno)
	{
		ninsert->lenblk = lenblk;
		ninsert->blkno = (BlockNumber *) palloc(sizeof(BlockNumber) * ninsert->lenblk);
		memcpy(ninsert->blkno, blkno, sizeof(BlockNumber) * ninsert->lenblk);
		ninsert->origblkno = *blkno;
	}
	else
	{
		int			i;

		Assert(xlinfo);
		ninsert->lenblk = xlinfo->data->npage;
		ninsert->blkno = (BlockNumber *) palloc(sizeof(BlockNumber) * ninsert->lenblk);
		for (i = 0; i < ninsert->lenblk; i++)
			ninsert->blkno[i] = xlinfo->page[i].header->blkno;
		ninsert->origblkno = xlinfo->data->origblkno;
	}
	Assert(ninsert->lenblk > 0);

	/*
	 * Stick the new incomplete insert onto the front of the list, not the
	 * back.  This is so that gist_xlog_cleanup will process incompletions in
	 * last-in-first-out order.
	 */
	incomplete_inserts = lcons(ninsert, incomplete_inserts);

	MemoryContextSwitchTo(oldCxt);
}

static void
forgetIncompleteInsert(RelFileNode node, ItemPointerData key)
{
	ListCell   *l;

	if (!ItemPointerIsValid(&key))
		return;

	if (incomplete_inserts == NIL)
		return;

	foreach(l, incomplete_inserts)
	{
		gistIncompleteInsert *insert = (gistIncompleteInsert *) lfirst(l);

		if (RelFileNodeEquals(node, insert->node) && ItemPointerEQ(&(insert->key), &(key)))
		{
			/* found */
			incomplete_inserts = list_delete_ptr(incomplete_inserts, insert);
			pfree(insert->blkno);
			pfree(insert);
			break;
		}
	}
}

static void
decodePageUpdateRecord(PageUpdateRecord *decoded, XLogRecord *record)
{
	char	   *begin = XLogRecGetData(record),
			   *ptr;
	int			i = 0,
				addpath = 0;

	decoded->data = (gistxlogPageUpdate *) begin;

	if (decoded->data->ntodelete)
	{
		decoded->todelete = (OffsetNumber *) (begin + sizeof(gistxlogPageUpdate) + addpath);
		addpath = MAXALIGN(sizeof(OffsetNumber) * decoded->data->ntodelete);
	}
	else
		decoded->todelete = NULL;

	decoded->len = 0;
	ptr = begin + sizeof(gistxlogPageUpdate) + addpath;
	while (ptr - begin < record->xl_len)
	{
		decoded->len++;
		ptr += IndexTupleSize((IndexTuple) ptr);
	}

	decoded->itup = (IndexTuple *) palloc(sizeof(IndexTuple) * decoded->len);

	ptr = begin + sizeof(gistxlogPageUpdate) + addpath;
	while (ptr - begin < record->xl_len)
	{
		decoded->itup[i] = (IndexTuple) ptr;
		ptr += IndexTupleSize(decoded->itup[i]);
		i++;
	}
}

/*
 * redo any page update (except page split)
 */
static void
gistRedoPageUpdateRecord(XLogRecPtr lsn, XLogRecord *record, bool isnewroot)
{
	gistxlogPageUpdate *xldata = (gistxlogPageUpdate *) XLogRecGetData(record);
	PageUpdateRecord xlrec;
	Relation	reln;
	Buffer		buffer;
	Page		page;

	/* we must fix incomplete_inserts list even if XLR_BKP_BLOCK_1 is set */
	forgetIncompleteInsert(xldata->node, xldata->key);

	if (!isnewroot && xldata->blkno != GIST_ROOT_BLKNO)
		/* operation with root always finalizes insertion */
		pushIncompleteInsert(xldata->node, lsn, xldata->key,
							 &(xldata->blkno), 1,
							 NULL);

	/* nothing else to do if page was backed up (and no info to do it with) */
	if (record->xl_info & XLR_BKP_BLOCK_1)
		return;

	decodePageUpdateRecord(&xlrec, record);

	reln = XLogOpenRelation(xlrec.data->node);
	buffer = XLogReadBuffer(reln, xlrec.data->blkno, false);
	if (!BufferIsValid(buffer))
		return;
	page = (Page) BufferGetPage(buffer);

	if (XLByteLE(lsn, PageGetLSN(page)))
	{
		UnlockReleaseBuffer(buffer);
		return;
	}

	if (isnewroot)
		GISTInitBuffer(buffer, 0);
	else if (xlrec.data->ntodelete)
	{
		int			i;

		for (i = 0; i < xlrec.data->ntodelete; i++)
			PageIndexTupleDelete(page, xlrec.todelete[i]);
		if (GistPageIsLeaf(page))
			GistMarkTuplesDeleted(page);
	}

	/* add tuples */
	if (xlrec.len > 0)
		gistfillbuffer(reln, page, xlrec.itup, xlrec.len, InvalidOffsetNumber);

	/*
	 * special case: leafpage, nothing to insert, nothing to delete, then
	 * vacuum marks page
	 */
	if (GistPageIsLeaf(page) && xlrec.len == 0 && xlrec.data->ntodelete == 0)
		GistClearTuplesDeleted(page);

	if (!GistPageIsLeaf(page) && PageGetMaxOffsetNumber(page) == InvalidOffsetNumber && xldata->blkno == GIST_ROOT_BLKNO)

		/*
		 * all links on non-leaf root page was deleted by vacuum full, so root
		 * page becomes a leaf
		 */
		GistPageSetLeaf(page);

	GistPageGetOpaque(page)->rightlink = InvalidBlockNumber;
	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);
	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);
}

static void
gistRedoPageDeleteRecord(XLogRecPtr lsn, XLogRecord *record)
{
	gistxlogPageDelete *xldata = (gistxlogPageDelete *) XLogRecGetData(record);
	Relation	reln;
	Buffer		buffer;
	Page		page;

	/* nothing else to do if page was backed up (and no info to do it with) */
	if (record->xl_info & XLR_BKP_BLOCK_1)
		return;

	reln = XLogOpenRelation(xldata->node);
	buffer = XLogReadBuffer(reln, xldata->blkno, false);
	if (!BufferIsValid(buffer))
		return;

	page = (Page) BufferGetPage(buffer);
	GistPageSetDeleted(page);

	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);
	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);
}

static void
decodePageSplitRecord(PageSplitRecord *decoded, XLogRecord *record)
{
	char	   *begin = XLogRecGetData(record),
			   *ptr;
	int			j,
				i = 0;

	decoded->data = (gistxlogPageSplit *) begin;
	decoded->page = (NewPage *) palloc(sizeof(NewPage) * decoded->data->npage);

	ptr = begin + sizeof(gistxlogPageSplit);
	for (i = 0; i < decoded->data->npage; i++)
	{
		Assert(ptr - begin < record->xl_len);
		decoded->page[i].header = (gistxlogPage *) ptr;
		ptr += sizeof(gistxlogPage);

		decoded->page[i].itup = (IndexTuple *)
			palloc(sizeof(IndexTuple) * decoded->page[i].header->num);
		j = 0;
		while (j < decoded->page[i].header->num)
		{
			Assert(ptr - begin < record->xl_len);
			decoded->page[i].itup[j] = (IndexTuple) ptr;
			ptr += IndexTupleSize((IndexTuple) ptr);
			j++;
		}
	}
}

static void
gistRedoPageSplitRecord(XLogRecPtr lsn, XLogRecord *record)
{
	PageSplitRecord xlrec;
	Relation	reln;
	Buffer		buffer;
	Page		page;
	int			i;
	int			flags;

	decodePageSplitRecord(&xlrec, record);
	reln = XLogOpenRelation(xlrec.data->node);
	flags = xlrec.data->origleaf ? F_LEAF : 0;

	/* loop around all pages */
	for (i = 0; i < xlrec.data->npage; i++)
	{
		NewPage    *newpage = xlrec.page + i;

		buffer = XLogReadBuffer(reln, newpage->header->blkno, true);
		Assert(BufferIsValid(buffer));
		page = (Page) BufferGetPage(buffer);

		/* ok, clear buffer */
		GISTInitBuffer(buffer, flags);

		/* and fill it */
		gistfillbuffer(reln, page, newpage->itup, newpage->header->num, FirstOffsetNumber);

		PageSetLSN(page, lsn);
		PageSetTLI(page, ThisTimeLineID);
		MarkBufferDirty(buffer);
		UnlockReleaseBuffer(buffer);
	}

	forgetIncompleteInsert(xlrec.data->node, xlrec.data->key);

	pushIncompleteInsert(xlrec.data->node, lsn, xlrec.data->key,
						 NULL, 0,
						 &xlrec);
}

static void
gistRedoCreateIndex(XLogRecPtr lsn, XLogRecord *record)
{
	RelFileNode *node = (RelFileNode *) XLogRecGetData(record);
	Relation	reln;
	Buffer		buffer;
	Page		page;

	reln = XLogOpenRelation(*node);
	buffer = XLogReadBuffer(reln, GIST_ROOT_BLKNO, true);
	Assert(BufferIsValid(buffer));
	page = (Page) BufferGetPage(buffer);

	GISTInitBuffer(buffer, F_LEAF);

	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);

	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);
}

static void
gistRedoCompleteInsert(XLogRecPtr lsn, XLogRecord *record)
{
	char	   *begin = XLogRecGetData(record),
			   *ptr;
	gistxlogInsertComplete *xlrec;

	xlrec = (gistxlogInsertComplete *) begin;

	ptr = begin + sizeof(gistxlogInsertComplete);
	while (ptr - begin < record->xl_len)
	{
		Assert(record->xl_len - (ptr - begin) >= sizeof(ItemPointerData));
		forgetIncompleteInsert(xlrec->node, *((ItemPointerData *) ptr));
		ptr += sizeof(ItemPointerData);
	}
}

void
gist_redo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;

	MemoryContext oldCxt;

	oldCxt = MemoryContextSwitchTo(opCtx);
	switch (info)
	{
		case XLOG_GIST_PAGE_UPDATE:
			gistRedoPageUpdateRecord(lsn, record, false);
			break;
		case XLOG_GIST_PAGE_DELETE:
			gistRedoPageDeleteRecord(lsn, record);
			break;
		case XLOG_GIST_NEW_ROOT:
			gistRedoPageUpdateRecord(lsn, record, true);
			break;
		case XLOG_GIST_PAGE_SPLIT:
			gistRedoPageSplitRecord(lsn, record);
			break;
		case XLOG_GIST_CREATE_INDEX:
			gistRedoCreateIndex(lsn, record);
			break;
		case XLOG_GIST_INSERT_COMPLETE:
			gistRedoCompleteInsert(lsn, record);
			break;
		default:
			elog(PANIC, "gist_redo: unknown op code %u", info);
	}

	MemoryContextSwitchTo(oldCxt);
	MemoryContextReset(opCtx);
}

static void
out_target(StringInfo buf, RelFileNode node, ItemPointerData key)
{
	appendStringInfo(buf, "rel %u/%u/%u",
					 node.spcNode, node.dbNode, node.relNode);
	if (ItemPointerIsValid(&key))
		appendStringInfo(buf, "; tid %u/%u",
						 ItemPointerGetBlockNumber(&key),
						 ItemPointerGetOffsetNumber(&key));
}

static void
out_gistxlogPageUpdate(StringInfo buf, gistxlogPageUpdate *xlrec)
{
	out_target(buf, xlrec->node, xlrec->key);
	appendStringInfo(buf, "; block number %u", xlrec->blkno);
}

static void
out_gistxlogPageDelete(StringInfo buf, gistxlogPageDelete *xlrec)
{
	appendStringInfo(buf, "page_delete: rel %u/%u/%u; blkno %u",
				xlrec->node.spcNode, xlrec->node.dbNode, xlrec->node.relNode,
					 xlrec->blkno);
}

static void
out_gistxlogPageSplit(StringInfo buf, gistxlogPageSplit *xlrec)
{
	appendStringInfo(buf, "page_split: ");
	out_target(buf, xlrec->node, xlrec->key);
	appendStringInfo(buf, "; block number %u splits to %d pages",
					 xlrec->origblkno, xlrec->npage);
}

void
gist_desc(StringInfo buf, uint8 xl_info, char *rec)
{
	uint8		info = xl_info & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_GIST_PAGE_UPDATE:
			appendStringInfo(buf, "page_update: ");
			out_gistxlogPageUpdate(buf, (gistxlogPageUpdate *) rec);
			break;
		case XLOG_GIST_PAGE_DELETE:
			out_gistxlogPageDelete(buf, (gistxlogPageDelete *) rec);
			break;
		case XLOG_GIST_NEW_ROOT:
			appendStringInfo(buf, "new_root: ");
			out_target(buf, ((gistxlogPageUpdate *) rec)->node, ((gistxlogPageUpdate *) rec)->key);
			break;
		case XLOG_GIST_PAGE_SPLIT:
			out_gistxlogPageSplit(buf, (gistxlogPageSplit *) rec);
			break;
		case XLOG_GIST_CREATE_INDEX:
			appendStringInfo(buf, "create_index: rel %u/%u/%u",
							 ((RelFileNode *) rec)->spcNode,
							 ((RelFileNode *) rec)->dbNode,
							 ((RelFileNode *) rec)->relNode);
			break;
		case XLOG_GIST_INSERT_COMPLETE:
			appendStringInfo(buf, "complete_insert: rel %u/%u/%u",
							 ((gistxlogInsertComplete *) rec)->node.spcNode,
							 ((gistxlogInsertComplete *) rec)->node.dbNode,
							 ((gistxlogInsertComplete *) rec)->node.relNode);
			break;
		default:
			appendStringInfo(buf, "unknown gist op code %u", info);
			break;
	}
}

IndexTuple
gist_form_invalid_tuple(BlockNumber blkno)
{
	/*
	 * we don't alloc space for null's bitmap, this is invalid tuple, be
	 * carefull in read and write code
	 */
	Size		size = IndexInfoFindDataOffset(0);
	IndexTuple	tuple = (IndexTuple) palloc0(size);

	tuple->t_info |= size;

	ItemPointerSetBlockNumber(&(tuple->t_tid), blkno);
	GistTupleSetInvalid(tuple);

	return tuple;
}


static void
gistxlogFindPath(Relation index, gistIncompleteInsert *insert)
{
	GISTInsertStack *top;

	insert->pathlen = 0;
	insert->path = NULL;

	if ((top = gistFindPath(index, insert->origblkno)) != NULL)
	{
		int			i;
		GISTInsertStack *ptr;

		for (ptr = top; ptr; ptr = ptr->parent)
			insert->pathlen++;

		insert->path = (BlockNumber *) palloc(sizeof(BlockNumber) * insert->pathlen);

		i = 0;
		for (ptr = top; ptr; ptr = ptr->parent)
			insert->path[i++] = ptr->blkno;
	}
	else
		elog(ERROR, "lost parent for block %u", insert->origblkno);
}

static SplitedPageLayout *
gistMakePageLayout(Buffer *buffers, int nbuffers)
{
	SplitedPageLayout *res = NULL,
			   *resptr;

	while (nbuffers-- > 0)
	{
		Page		page = BufferGetPage(buffers[nbuffers]);
		IndexTuple *vec;
		int			veclen;

		resptr = (SplitedPageLayout *) palloc0(sizeof(SplitedPageLayout));

		resptr->block.blkno = BufferGetBlockNumber(buffers[nbuffers]);
		resptr->block.num = PageGetMaxOffsetNumber(page);

		vec = gistextractpage(page, &veclen);
		resptr->list = gistfillitupvec(vec, veclen, &(resptr->lenlist));

		resptr->next = res;
		res = resptr;
	}

	return res;
}

/*
 * Continue insert after crash.  In normal situations, there aren't any
 * incomplete inserts, but if a crash occurs partway through an insertion
 * sequence, we'll need to finish making the index valid at the end of WAL
 * replay.
 *
 * Note that we assume the index is now in a valid state, except for the
 * unfinished insertion.  In particular it's safe to invoke gistFindPath();
 * there shouldn't be any garbage pages for it to run into.
 *
 * To complete insert we can't use basic insertion algorithm because
 * during insertion we can't call user-defined support functions of opclass.
 * So, we insert 'invalid' tuples without real key and do it by separate algorithm.
 * 'invalid' tuple should be updated by vacuum full.
 */
static void
gistContinueInsert(gistIncompleteInsert *insert)
{
	IndexTuple *itup;
	int			i,
				lenitup;
	Relation	index;

	index = XLogOpenRelation(insert->node);

	/*
	 * needed vector itup never will be more than initial lenblkno+2, because
	 * during this processing Indextuple can be only smaller
	 */
	lenitup = insert->lenblk;
	itup = (IndexTuple *) palloc(sizeof(IndexTuple) * (lenitup + 2 /* guarantee root split */ ));

	for (i = 0; i < insert->lenblk; i++)
		itup[i] = gist_form_invalid_tuple(insert->blkno[i]);

	/*
	 * any insertion of itup[] should make LOG message about
	 */

	if (insert->origblkno == GIST_ROOT_BLKNO)
	{
		/*
		 * it was split root, so we should only make new root. it can't be
		 * simple insert into root, we should replace all content of root.
		 */
		Buffer		buffer = XLogReadBuffer(index, GIST_ROOT_BLKNO, true);

		gistnewroot(index, buffer, itup, lenitup, NULL);
		UnlockReleaseBuffer(buffer);
	}
	else
	{
		Buffer	   *buffers;
		Page	   *pages;
		int			numbuffer;
		OffsetNumber *todelete;

		/* construct path */
		gistxlogFindPath(index, insert);

		Assert(insert->pathlen > 0);

		buffers = (Buffer *) palloc(sizeof(Buffer) * (insert->lenblk + 2 /* guarantee root split */ ));
		pages = (Page *) palloc(sizeof(Page) * (insert->lenblk + 2 /* guarantee root split */ ));
		todelete = (OffsetNumber *) palloc(sizeof(OffsetNumber) * (insert->lenblk + 2 /* guarantee root split */ ));

		for (i = 0; i < insert->pathlen; i++)
		{
			int			j,
						k,
						pituplen = 0;
			XLogRecData *rdata;
			XLogRecPtr	recptr;
			Buffer		tempbuffer = InvalidBuffer;
			int			ntodelete = 0;

			numbuffer = 1;
			buffers[0] = ReadBuffer(index, insert->path[i]);
			LockBuffer(buffers[0], GIST_EXCLUSIVE);

			/*
			 * we check buffer, because we restored page earlier
			 */
			gistcheckpage(index, buffers[0]);

			pages[0] = BufferGetPage(buffers[0]);
			Assert(!GistPageIsLeaf(pages[0]));

			pituplen = PageGetMaxOffsetNumber(pages[0]);

			/* find remove old IndexTuples to remove */
			for (j = 0; j < pituplen && ntodelete < lenitup; j++)
			{
				BlockNumber blkno;
				ItemId		iid = PageGetItemId(pages[0], j + FirstOffsetNumber);
				IndexTuple	idxtup = (IndexTuple) PageGetItem(pages[0], iid);

				blkno = ItemPointerGetBlockNumber(&(idxtup->t_tid));

				for (k = 0; k < lenitup; k++)
					if (ItemPointerGetBlockNumber(&(itup[k]->t_tid)) == blkno)
					{
						todelete[ntodelete] = j + FirstOffsetNumber - ntodelete;
						ntodelete++;
						break;
					}
			}

			if (ntodelete == 0)
				elog(PANIC, "gistContinueInsert: can't find pointer to page(s)");

			/*
			 * we check space with subtraction only first tuple to delete,
			 * hope, that wiil be enough space....
			 */

			if (gistnospace(pages[0], itup, lenitup, *todelete, 0))
			{

				/* no space left on page, so we must split */
				buffers[numbuffer] = ReadBuffer(index, P_NEW);
				LockBuffer(buffers[numbuffer], GIST_EXCLUSIVE);
				GISTInitBuffer(buffers[numbuffer], 0);
				pages[numbuffer] = BufferGetPage(buffers[numbuffer]);
				gistfillbuffer(index, pages[numbuffer], itup, lenitup, FirstOffsetNumber);
				numbuffer++;

				if (BufferGetBlockNumber(buffers[0]) == GIST_ROOT_BLKNO)
				{
					Buffer		tmp;

					/*
					 * we split root, just copy content from root to new page
					 */

					/* sanity check */
					if (i + 1 != insert->pathlen)
						elog(PANIC, "unexpected pathlen in index \"%s\"",
							 RelationGetRelationName(index));

					/* fill new page, root will be changed later */
					tempbuffer = ReadBuffer(index, P_NEW);
					LockBuffer(tempbuffer, GIST_EXCLUSIVE);
					memcpy(BufferGetPage(tempbuffer), pages[0], BufferGetPageSize(tempbuffer));

					/* swap buffers[0] (was root) and temp buffer */
					tmp = buffers[0];
					buffers[0] = tempbuffer;
					tempbuffer = tmp;	/* now in tempbuffer GIST_ROOT_BLKNO,
										 * it is still unchanged */

					pages[0] = BufferGetPage(buffers[0]);
				}

				START_CRIT_SECTION();

				for (j = 0; j < ntodelete; j++)
					PageIndexTupleDelete(pages[0], todelete[j]);

				rdata = formSplitRdata(index->rd_node, insert->path[i],
									   false, &(insert->key),
									 gistMakePageLayout(buffers, numbuffer));

			}
			else
			{
				START_CRIT_SECTION();

				for (j = 0; j < ntodelete; j++)
					PageIndexTupleDelete(pages[0], todelete[j]);
				gistfillbuffer(index, pages[0], itup, lenitup, InvalidOffsetNumber);

				rdata = formUpdateRdata(index->rd_node, buffers[0],
										todelete, ntodelete,
										itup, lenitup, &(insert->key));
			}

			/*
			 * use insert->key as mark for completion of insert (form*Rdata()
			 * above) for following possible replays
			 */

			/* write pages, we should mark it dirty befor XLogInsert() */
			for (j = 0; j < numbuffer; j++)
			{
				GistPageGetOpaque(pages[j])->rightlink = InvalidBlockNumber;
				MarkBufferDirty(buffers[j]);
			}
			recptr = XLogInsert(RM_GIST_ID, XLOG_GIST_PAGE_UPDATE, rdata);
			for (j = 0; j < numbuffer; j++)
			{
				PageSetLSN(pages[j], recptr);
				PageSetTLI(pages[j], ThisTimeLineID);
			}

			END_CRIT_SECTION();

			lenitup = numbuffer;
			for (j = 0; j < numbuffer; j++)
			{
				itup[j] = gist_form_invalid_tuple(BufferGetBlockNumber(buffers[j]));
				UnlockReleaseBuffer(buffers[j]);
			}

			if (tempbuffer != InvalidBuffer)
			{
				/*
				 * it was a root split, so fill it by new values
				 */
				gistnewroot(index, tempbuffer, itup, lenitup, &(insert->key));
				UnlockReleaseBuffer(tempbuffer);
			}
		}
	}

	ereport(LOG,
			(errmsg("index %u/%u/%u needs VACUUM FULL or REINDEX to finish crash recovery",
			insert->node.spcNode, insert->node.dbNode, insert->node.relNode),
		   errdetail("Incomplete insertion detected during crash replay.")));
}

void
gist_xlog_startup(void)
{
	incomplete_inserts = NIL;
	insertCtx = AllocSetContextCreate(CurrentMemoryContext,
									  "GiST recovery temporary context",
									  ALLOCSET_DEFAULT_MINSIZE,
									  ALLOCSET_DEFAULT_INITSIZE,
									  ALLOCSET_DEFAULT_MAXSIZE);
	opCtx = createTempGistContext();
}

void
gist_xlog_cleanup(void)
{
	ListCell   *l;
	MemoryContext oldCxt;

	oldCxt = MemoryContextSwitchTo(opCtx);

	foreach(l, incomplete_inserts)
	{
		gistIncompleteInsert *insert = (gistIncompleteInsert *) lfirst(l);

		gistContinueInsert(insert);
		MemoryContextReset(opCtx);
	}
	MemoryContextSwitchTo(oldCxt);

	MemoryContextDelete(opCtx);
	MemoryContextDelete(insertCtx);
}

bool
gist_safe_restartpoint(void)
{
	if (incomplete_inserts)
		return false;
	return true;
}


XLogRecData *
formSplitRdata(RelFileNode node, BlockNumber blkno, bool page_is_leaf,
			   ItemPointer key, SplitedPageLayout *dist)
{
	XLogRecData *rdata;
	gistxlogPageSplit *xlrec = (gistxlogPageSplit *) palloc(sizeof(gistxlogPageSplit));
	SplitedPageLayout *ptr;
	int			npage = 0,
				cur = 1;

	ptr = dist;
	while (ptr)
	{
		npage++;
		ptr = ptr->next;
	}

	rdata = (XLogRecData *) palloc(sizeof(XLogRecData) * (npage * 2 + 2));

	xlrec->node = node;
	xlrec->origblkno = blkno;
	xlrec->origleaf = page_is_leaf;
	xlrec->npage = (uint16) npage;
	if (key)
		xlrec->key = *key;
	else
		ItemPointerSetInvalid(&(xlrec->key));

	rdata[0].buffer = InvalidBuffer;
	rdata[0].data = (char *) xlrec;
	rdata[0].len = sizeof(gistxlogPageSplit);
	rdata[0].next = NULL;

	ptr = dist;
	while (ptr)
	{
		rdata[cur].buffer = InvalidBuffer;
		rdata[cur].data = (char *) &(ptr->block);
		rdata[cur].len = sizeof(gistxlogPage);
		rdata[cur - 1].next = &(rdata[cur]);
		cur++;

		rdata[cur].buffer = InvalidBuffer;
		rdata[cur].data = (char *) (ptr->list);
		rdata[cur].len = ptr->lenlist;
		rdata[cur - 1].next = &(rdata[cur]);
		rdata[cur].next = NULL;
		cur++;
		ptr = ptr->next;
	}

	return rdata;
}

/*
 * Construct the rdata array for an XLOG record describing a page update
 * (deletion and/or insertion of tuples on a single index page).
 *
 * Note that both the todelete array and the tuples are marked as belonging
 * to the target buffer; they need not be stored in XLOG if XLogInsert decides
 * to log the whole buffer contents instead.  Also, we take care that there's
 * at least one rdata item referencing the buffer, even when ntodelete and
 * ituplen are both zero; this ensures that XLogInsert knows about the buffer.
 */
XLogRecData *
formUpdateRdata(RelFileNode node, Buffer buffer,
				OffsetNumber *todelete, int ntodelete,
				IndexTuple *itup, int ituplen, ItemPointer key)
{
	XLogRecData *rdata;
	gistxlogPageUpdate *xlrec;
	int			cur,
				i;

	rdata = (XLogRecData *) palloc(sizeof(XLogRecData) * (3 + ituplen));
	xlrec = (gistxlogPageUpdate *) palloc(sizeof(gistxlogPageUpdate));

	xlrec->node = node;
	xlrec->blkno = BufferGetBlockNumber(buffer);
	xlrec->ntodelete = ntodelete;

	if (key)
		xlrec->key = *key;
	else
		ItemPointerSetInvalid(&(xlrec->key));

	rdata[0].buffer = buffer;
	rdata[0].buffer_std = true;
	rdata[0].data = NULL;
	rdata[0].len = 0;
	rdata[0].next = &(rdata[1]);

	rdata[1].data = (char *) xlrec;
	rdata[1].len = sizeof(gistxlogPageUpdate);
	rdata[1].buffer = InvalidBuffer;
	rdata[1].next = &(rdata[2]);

	rdata[2].data = (char *) todelete;
	rdata[2].len = MAXALIGN(sizeof(OffsetNumber) * ntodelete);
	rdata[2].buffer = buffer;
	rdata[2].buffer_std = true;
	rdata[2].next = NULL;

	/* new tuples */
	cur = 3;
	for (i = 0; i < ituplen; i++)
	{
		rdata[cur - 1].next = &(rdata[cur]);
		rdata[cur].data = (char *) (itup[i]);
		rdata[cur].len = IndexTupleSize(itup[i]);
		rdata[cur].buffer = buffer;
		rdata[cur].buffer_std = true;
		rdata[cur].next = NULL;
		cur++;
	}

	return rdata;
}

XLogRecPtr
gistxlogInsertCompletion(RelFileNode node, ItemPointerData *keys, int len)
{
	gistxlogInsertComplete xlrec;
	XLogRecData rdata[2];
	XLogRecPtr	recptr;

	Assert(len > 0);
	xlrec.node = node;

	rdata[0].buffer = InvalidBuffer;
	rdata[0].data = (char *) &xlrec;
	rdata[0].len = sizeof(gistxlogInsertComplete);
	rdata[0].next = &(rdata[1]);

	rdata[1].buffer = InvalidBuffer;
	rdata[1].data = (char *) keys;
	rdata[1].len = sizeof(ItemPointerData) * len;
	rdata[1].next = NULL;

	START_CRIT_SECTION();

	recptr = XLogInsert(RM_GIST_ID, XLOG_GIST_INSERT_COMPLETE, rdata);

	END_CRIT_SECTION();

	return recptr;
}
