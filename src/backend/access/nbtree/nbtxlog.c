/*-------------------------------------------------------------------------
 *
 * nbtxlog.c
 *	  WAL replay logic for btrees.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/nbtree/nbtxlog.c,v 1.7.2.1 2006/03/28 21:17:58 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/nbtree.h"
#include "access/xlogutils.h"


/*
 * We must keep track of expected insertions due to page splits, and apply
 * them manually if they are not seen in the WAL log during replay.  This
 * makes it safe for page insertion to be a multiple-WAL-action process.
 *
 * The data structure is a simple linked list --- this should be good enough,
 * since we don't expect a page split to remain incomplete for long.
 */
typedef struct bt_incomplete_split
{
	RelFileNode node;			/* the index */
	BlockNumber leftblk;		/* left half of split */
	BlockNumber rightblk;		/* right half of split */
	bool		is_root;		/* we split the root */
} bt_incomplete_split;

static List *incomplete_splits;


static void
log_incomplete_split(RelFileNode node, BlockNumber leftblk,
					 BlockNumber rightblk, bool is_root)
{
	bt_incomplete_split *split = palloc(sizeof(bt_incomplete_split));

	split->node = node;
	split->leftblk = leftblk;
	split->rightblk = rightblk;
	split->is_root = is_root;
	incomplete_splits = lappend(incomplete_splits, split);
}

static void
forget_matching_split(Relation reln, RelFileNode node,
					  BlockNumber insertblk, OffsetNumber offnum,
					  bool is_root)
{
	Buffer		buffer;
	Page		page;
	BTItem		btitem;
	BlockNumber rightblk;
	List	   *l;

	/* Get downlink TID from page */
	buffer = XLogReadBuffer(false, reln, insertblk);
	if (!BufferIsValid(buffer))
		elog(PANIC, "forget_matching_split: block unfound");
	page = (Page) BufferGetPage(buffer);
	btitem = (BTItem) PageGetItem(page, PageGetItemId(page, offnum));
	rightblk = ItemPointerGetBlockNumber(&(btitem->bti_itup.t_tid));
	Assert(ItemPointerGetOffsetNumber(&(btitem->bti_itup.t_tid)) == P_HIKEY);
	UnlockAndReleaseBuffer(buffer);

	foreach(l, incomplete_splits)
	{
		bt_incomplete_split *split = (bt_incomplete_split *) lfirst(l);

		if (RelFileNodeEquals(node, split->node) &&
			rightblk == split->rightblk)
		{
			if (is_root != split->is_root)
				elog(LOG, "forget_matching_split: fishy is_root data");
			incomplete_splits = lremove(split, incomplete_splits);
			break;				/* need not look further */
		}
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
_bt_restore_meta(Relation reln, XLogRecPtr lsn,
				 BlockNumber root, uint32 level,
				 BlockNumber fastroot, uint32 fastlevel,
				 bool markvalid)
{
	Buffer		metabuf;
	Page		metapg;
	BTMetaPageData *md;
	BTPageOpaque pageop;

	metabuf = XLogReadBuffer(true, reln, BTREE_METAPAGE);
	if (!BufferIsValid(metabuf))
		elog(PANIC, "_bt_restore_meta: no metapage");

	metapg = BufferGetPage(metabuf);
	_bt_pageinit(metapg, BufferGetPageSize(metabuf));

	md = BTPageGetMeta(metapg);
	md->btm_magic = markvalid ? BTREE_MAGIC : 0;
	md->btm_version = BTREE_VERSION;
	md->btm_root = root;
	md->btm_level = level;
	md->btm_fastroot = fastroot;
	md->btm_fastlevel = fastlevel;

	pageop = (BTPageOpaque) PageGetSpecialPointer(metapg);
	pageop->btpo_flags = BTP_META;

	PageSetLSN(metapg, lsn);
	PageSetSUI(metapg, ThisStartUpID);
	UnlockAndWriteBuffer(metabuf);
}

static void
btree_xlog_insert(bool redo, bool isleaf, bool ismeta,
				  XLogRecPtr lsn, XLogRecord *record)
{
	xl_btree_insert *xlrec = (xl_btree_insert *) XLogRecGetData(record);
	Relation	reln;
	Buffer		buffer;
	Page		page;
	BTPageOpaque pageop;
	char	   *datapos;
	int			datalen;
	xl_btree_metadata md;

	datapos = (char *) xlrec + SizeOfBtreeInsert;
	datalen = record->xl_len - SizeOfBtreeInsert;
	if (ismeta)
	{
		memcpy(&md, datapos, sizeof(xl_btree_metadata));
		datapos += sizeof(xl_btree_metadata);
		datalen -= sizeof(xl_btree_metadata);
	}

	if (redo && (record->xl_info & XLR_BKP_BLOCK_1) && !ismeta &&
		incomplete_splits == NIL)
		return;					/* nothing to do */

	reln = XLogOpenRelation(redo, RM_BTREE_ID, xlrec->target.node);
	if (!RelationIsValid(reln))
		return;

	if (!redo || !(record->xl_info & XLR_BKP_BLOCK_1))
	{
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
				UnlockAndReleaseBuffer(buffer);
			else
			{
				if (PageAddItem(page, (Item) datapos, datalen,
						ItemPointerGetOffsetNumber(&(xlrec->target.tid)),
								LP_USED) == InvalidOffsetNumber)
					elog(PANIC, "btree_insert_redo: failed to add item");

				PageSetLSN(page, lsn);
				PageSetSUI(page, ThisStartUpID);
				UnlockAndWriteBuffer(buffer);
			}
		}
		else
		{
			if (XLByteLT(PageGetLSN(page), lsn))
				elog(PANIC, "btree_insert_undo: bad page LSN");

			if (!P_ISLEAF(pageop))
				UnlockAndReleaseBuffer(buffer);
			else
				elog(PANIC, "btree_insert_undo: unimplemented");
		}
	}

	if (redo)					/* metapage changes not undoable */
	{
		if (ismeta)
			_bt_restore_meta(reln, lsn,
							 md.root, md.level,
							 md.fastroot, md.fastlevel,
							 true);
	}

	/* Forget any split this insertion completes */
	if (redo && !isleaf && incomplete_splits != NIL)
	{
		forget_matching_split(reln, xlrec->target.node,
						 ItemPointerGetBlockNumber(&(xlrec->target.tid)),
						ItemPointerGetOffsetNumber(&(xlrec->target.tid)),
							  false);
	}
}

static void
btree_xlog_split(bool redo, bool onleft, bool isroot,
				 XLogRecPtr lsn, XLogRecord *record)
{
	xl_btree_split *xlrec = (xl_btree_split *) XLogRecGetData(record);
	Relation	reln;
	BlockNumber targetblk;
	BlockNumber leftsib;
	BlockNumber rightsib;
	Buffer		buffer;
	Page		page;
	BTPageOpaque pageop;
	char	   *op = (redo) ? "redo" : "undo";

	reln = XLogOpenRelation(redo, RM_BTREE_ID, xlrec->target.node);
	if (!RelationIsValid(reln))
		return;

	targetblk = ItemPointerGetBlockNumber(&(xlrec->target.tid));
	leftsib = (onleft) ? targetblk : xlrec->otherblk;
	rightsib = (onleft) ? xlrec->otherblk : targetblk;

	/* Left (original) sibling */
	buffer = XLogReadBuffer(true, reln, leftsib);
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
		pageop->btpo_prev = xlrec->leftblk;
		pageop->btpo_next = rightsib;
		pageop->btpo.level = xlrec->level;
		pageop->btpo_flags = (xlrec->level == 0) ? BTP_LEAF : 0;

		_bt_restore_page(page,
						 (char *) xlrec + SizeOfBtreeSplit,
						 xlrec->leftlen);

		PageSetLSN(page, lsn);
		PageSetSUI(page, ThisStartUpID);
		UnlockAndWriteBuffer(buffer);
	}
	else
	{
		/* undo */
		if (XLByteLT(PageGetLSN(page), lsn))
			elog(PANIC, "btree_split_undo: bad left sibling LSN");
		elog(PANIC, "btree_split_undo: unimplemented");
	}

	/* Right (new) sibling */
	buffer = XLogReadBuffer((redo) ? true : false, reln, rightsib);
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
		pageop->btpo_prev = leftsib;
		pageop->btpo_next = xlrec->rightblk;
		pageop->btpo.level = xlrec->level;
		pageop->btpo_flags = (xlrec->level == 0) ? BTP_LEAF : 0;

		_bt_restore_page(page,
					  (char *) xlrec + SizeOfBtreeSplit + xlrec->leftlen,
					 record->xl_len - SizeOfBtreeSplit - xlrec->leftlen);

		PageSetLSN(page, lsn);
		PageSetSUI(page, ThisStartUpID);
		UnlockAndWriteBuffer(buffer);
	}
	else
	{
		/* undo */
		if (XLByteLT(PageGetLSN(page), lsn))
			elog(PANIC, "btree_split_undo: bad right sibling LSN");
		elog(PANIC, "btree_split_undo: unimplemented");
	}

	/* Fix left-link of right (next) page */
	if (redo && !(record->xl_info & XLR_BKP_BLOCK_1))
	{
		if (xlrec->rightblk != P_NONE)
		{
			buffer = XLogReadBuffer(false, reln, xlrec->rightblk);
			if (!BufferIsValid(buffer))
				elog(PANIC, "btree_split_redo: lost next right page");

			page = (Page) BufferGetPage(buffer);
			if (PageIsNew((PageHeader) page))
				elog(PANIC, "btree_split_redo: uninitialized next right page");

			if (XLByteLE(lsn, PageGetLSN(page)))
				UnlockAndReleaseBuffer(buffer);
			else
			{
				pageop = (BTPageOpaque) PageGetSpecialPointer(page);
				pageop->btpo_prev = rightsib;

				PageSetLSN(page, lsn);
				PageSetSUI(page, ThisStartUpID);
				UnlockAndWriteBuffer(buffer);
			}
		}
	}

	/* Forget any split this insertion completes */
	if (redo && xlrec->level > 0 && incomplete_splits != NIL)
	{
		forget_matching_split(reln, xlrec->target.node,
						 ItemPointerGetBlockNumber(&(xlrec->target.tid)),
						ItemPointerGetOffsetNumber(&(xlrec->target.tid)),
							  false);
	}

	/* The job ain't done till the parent link is inserted... */
	log_incomplete_split(xlrec->target.node,
						 leftsib, rightsib, isroot);
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
	reln = XLogOpenRelation(redo, RM_BTREE_ID, xlrec->node);
	if (!RelationIsValid(reln))
		return;
	buffer = XLogReadBuffer(false, reln, xlrec->block);
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

	if (record->xl_len > SizeOfBtreeDelete)
	{
		OffsetNumber *unused;
		OffsetNumber *unend;

		unused = (OffsetNumber *) ((char *) xlrec + SizeOfBtreeDelete);
		unend = (OffsetNumber *) ((char *) xlrec + record->xl_len);

		/* be careful to delete from back to front */
		while (unused < unend)
		{
			unend--;
			PageIndexTupleDelete(page, *unend);
		}
	}

	PageSetLSN(page, lsn);
	PageSetSUI(page, ThisStartUpID);
	UnlockAndWriteBuffer(buffer);
}

static void
btree_xlog_delete_page(bool redo, bool ismeta,
					   XLogRecPtr lsn, XLogRecord *record)
{
	xl_btree_delete_page *xlrec = (xl_btree_delete_page *) XLogRecGetData(record);
	Relation	reln;
	BlockNumber parent;
	BlockNumber target;
	BlockNumber leftsib;
	BlockNumber rightsib;
	Buffer		buffer;
	Page		page;
	BTPageOpaque pageop;
	char	   *op = (redo) ? "redo" : "undo";

	reln = XLogOpenRelation(redo, RM_BTREE_ID, xlrec->target.node);
	if (!RelationIsValid(reln))
		return;

	parent = ItemPointerGetBlockNumber(&(xlrec->target.tid));
	target = xlrec->deadblk;
	leftsib = xlrec->leftblk;
	rightsib = xlrec->rightblk;

	/* parent page */
	if (redo && !(record->xl_info & XLR_BKP_BLOCK_1))
	{
		buffer = XLogReadBuffer(false, reln, parent);
		if (!BufferIsValid(buffer))
			elog(PANIC, "btree_delete_page_redo: parent block unfound");
		page = (Page) BufferGetPage(buffer);
		pageop = (BTPageOpaque) PageGetSpecialPointer(page);
		if (PageIsNew((PageHeader) page))
			elog(PANIC, "btree_delete_page_redo: uninitialized parent page");
		if (XLByteLE(lsn, PageGetLSN(page)))
			UnlockAndReleaseBuffer(buffer);
		else
		{
			OffsetNumber poffset;

			poffset = ItemPointerGetOffsetNumber(&(xlrec->target.tid));
			if (poffset >= PageGetMaxOffsetNumber(page))
			{
				Assert(poffset == P_FIRSTDATAKEY(pageop));
				PageIndexTupleDelete(page, poffset);
				pageop->btpo_flags |= BTP_HALF_DEAD;
			}
			else
			{
				ItemId		itemid;
				BTItem		btitem;
				OffsetNumber nextoffset;

				itemid = PageGetItemId(page, poffset);
				btitem = (BTItem) PageGetItem(page, itemid);
				ItemPointerSet(&(btitem->bti_itup.t_tid), rightsib, P_HIKEY);
				nextoffset = OffsetNumberNext(poffset);
				PageIndexTupleDelete(page, nextoffset);
			}

			PageSetLSN(page, lsn);
			PageSetSUI(page, ThisStartUpID);
			UnlockAndWriteBuffer(buffer);
		}
	}

	/* Fix left-link of right sibling */
	if (redo && !(record->xl_info & XLR_BKP_BLOCK_2))
	{
		buffer = XLogReadBuffer(false, reln, rightsib);
		if (!BufferIsValid(buffer))
			elog(PANIC, "btree_delete_page_redo: lost right sibling");
		page = (Page) BufferGetPage(buffer);
		if (PageIsNew((PageHeader) page))
			elog(PANIC, "btree_delete_page_redo: uninitialized right sibling");
		if (XLByteLE(lsn, PageGetLSN(page)))
			UnlockAndReleaseBuffer(buffer);
		else
		{
			pageop = (BTPageOpaque) PageGetSpecialPointer(page);
			pageop->btpo_prev = leftsib;

			PageSetLSN(page, lsn);
			PageSetSUI(page, ThisStartUpID);
			UnlockAndWriteBuffer(buffer);
		}
	}

	/* Fix right-link of left sibling, if any */
	if (redo && !(record->xl_info & XLR_BKP_BLOCK_3))
	{
		if (leftsib != P_NONE)
		{
			buffer = XLogReadBuffer(false, reln, leftsib);
			if (!BufferIsValid(buffer))
				elog(PANIC, "btree_delete_page_redo: lost left sibling");
			page = (Page) BufferGetPage(buffer);
			if (PageIsNew((PageHeader) page))
				elog(PANIC, "btree_delete_page_redo: uninitialized left sibling");
			if (XLByteLE(lsn, PageGetLSN(page)))
				UnlockAndReleaseBuffer(buffer);
			else
			{
				pageop = (BTPageOpaque) PageGetSpecialPointer(page);
				pageop->btpo_next = rightsib;

				PageSetLSN(page, lsn);
				PageSetSUI(page, ThisStartUpID);
				UnlockAndWriteBuffer(buffer);
			}
		}
	}

	/* Rewrite target page as empty deleted page */
	buffer = XLogReadBuffer(true, reln, target);
	if (!BufferIsValid(buffer))
		elog(PANIC, "btree_delete_page_%s: lost target page", op);
	page = (Page) BufferGetPage(buffer);
	if (redo)
		_bt_pageinit(page, BufferGetPageSize(buffer));
	else if (PageIsNew((PageHeader) page))
		elog(PANIC, "btree_delete_page_undo: uninitialized target page");
	pageop = (BTPageOpaque) PageGetSpecialPointer(page);

	if (redo)
	{
		pageop->btpo_prev = leftsib;
		pageop->btpo_next = rightsib;
		pageop->btpo.xact = FrozenTransactionId;
		pageop->btpo_flags = BTP_DELETED;

		PageSetLSN(page, lsn);
		PageSetSUI(page, ThisStartUpID);
		UnlockAndWriteBuffer(buffer);
	}
	else
	{
		/* undo */
		if (XLByteLT(PageGetLSN(page), lsn))
			elog(PANIC, "btree_delete_page_undo: bad left sibling LSN");
		elog(PANIC, "btree_delete_page_undo: unimplemented");
	}

	/* Update metapage if needed */
	if (redo)					/* metapage changes not undoable */
	{
		if (ismeta)
		{
			xl_btree_metadata md;

			memcpy(&md, (char *) xlrec + SizeOfBtreeDeletePage,
				   sizeof(xl_btree_metadata));
			_bt_restore_meta(reln, lsn,
							 md.root, md.level,
							 md.fastroot, md.fastlevel,
							 true);
		}
	}
}

static void
btree_xlog_newroot(bool redo, XLogRecPtr lsn, XLogRecord *record)
{
	xl_btree_newroot *xlrec = (xl_btree_newroot *) XLogRecGetData(record);
	Relation	reln;
	Buffer		buffer;
	Page		page;
	BTPageOpaque pageop;

	if (!redo)
		return;					/* not undoable */

	reln = XLogOpenRelation(redo, RM_BTREE_ID, xlrec->node);
	if (!RelationIsValid(reln))
		return;
	buffer = XLogReadBuffer(true, reln, xlrec->rootblk);
	if (!BufferIsValid(buffer))
		elog(PANIC, "btree_newroot_redo: no root page");

	page = (Page) BufferGetPage(buffer);
	_bt_pageinit(page, BufferGetPageSize(buffer));
	pageop = (BTPageOpaque) PageGetSpecialPointer(page);

	pageop->btpo_flags = BTP_ROOT;
	pageop->btpo_prev = pageop->btpo_next = P_NONE;
	pageop->btpo.level = xlrec->level;
	if (xlrec->level == 0)
		pageop->btpo_flags |= BTP_LEAF;

	if (record->xl_len > SizeOfBtreeNewroot)
		_bt_restore_page(page,
						 (char *) xlrec + SizeOfBtreeNewroot,
						 record->xl_len - SizeOfBtreeNewroot);

	PageSetLSN(page, lsn);
	PageSetSUI(page, ThisStartUpID);
	UnlockAndWriteBuffer(buffer);

	_bt_restore_meta(reln, lsn,
					 xlrec->rootblk, xlrec->level,
					 xlrec->rootblk, xlrec->level,
					 true);

	/* Check to see if this satisfies any incomplete insertions */
	if (record->xl_len > SizeOfBtreeNewroot &&
		incomplete_splits != NIL)
	{
		forget_matching_split(reln, xlrec->node,
							  xlrec->rootblk,
							  P_FIRSTKEY,
							  true);
	}
}

static void
btree_xlog_newmeta(bool redo, XLogRecPtr lsn, XLogRecord *record,
				   bool markvalid)
{
	xl_btree_newmeta *xlrec = (xl_btree_newmeta *) XLogRecGetData(record);
	Relation	reln;

	if (!redo)
		return;					/* not undoable */

	reln = XLogOpenRelation(redo, RM_BTREE_ID, xlrec->node);
	if (!RelationIsValid(reln))
		return;

	_bt_restore_meta(reln, lsn,
					 xlrec->meta.root, xlrec->meta.level,
					 xlrec->meta.fastroot, xlrec->meta.fastlevel,
					 markvalid);
}

static void
btree_xlog_newpage(bool redo, XLogRecPtr lsn, XLogRecord *record)
{
	xl_btree_newpage *xlrec = (xl_btree_newpage *) XLogRecGetData(record);
	Relation	reln;
	Buffer		buffer;
	Page		page;

	if (!redo || (record->xl_info & XLR_BKP_BLOCK_1))
		return;

	reln = XLogOpenRelation(redo, RM_BTREE_ID, xlrec->node);
	if (!RelationIsValid(reln))
		return;
	buffer = XLogReadBuffer(true, reln, xlrec->blkno);
	if (!BufferIsValid(buffer))
		elog(PANIC, "btree_newpage_redo: block unfound");
	page = (Page) BufferGetPage(buffer);

	Assert(record->xl_len == SizeOfBtreeNewpage + BLCKSZ);
	memcpy(page, (char *) xlrec + SizeOfBtreeNewpage, BLCKSZ);

	PageSetLSN(page, lsn);
	PageSetSUI(page, ThisStartUpID);
	UnlockAndWriteBuffer(buffer);
}


void
btree_redo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_BTREE_INSERT_LEAF:
			btree_xlog_insert(true, true, false, lsn, record);
			break;
		case XLOG_BTREE_INSERT_UPPER:
			btree_xlog_insert(true, false, false, lsn, record);
			break;
		case XLOG_BTREE_INSERT_META:
			btree_xlog_insert(true, false, true, lsn, record);
			break;
		case XLOG_BTREE_SPLIT_L:
			btree_xlog_split(true, true, false, lsn, record);
			break;
		case XLOG_BTREE_SPLIT_R:
			btree_xlog_split(true, false, false, lsn, record);
			break;
		case XLOG_BTREE_SPLIT_L_ROOT:
			btree_xlog_split(true, true, true, lsn, record);
			break;
		case XLOG_BTREE_SPLIT_R_ROOT:
			btree_xlog_split(true, false, true, lsn, record);
			break;
		case XLOG_BTREE_DELETE:
			btree_xlog_delete(true, lsn, record);
			break;
		case XLOG_BTREE_DELETE_PAGE:
			btree_xlog_delete_page(true, false, lsn, record);
			break;
		case XLOG_BTREE_DELETE_PAGE_META:
			btree_xlog_delete_page(true, true, lsn, record);
			break;
		case XLOG_BTREE_NEWROOT:
			btree_xlog_newroot(true, lsn, record);
			break;
		case XLOG_BTREE_NEWMETA:
			btree_xlog_newmeta(true, lsn, record, true);
			break;
		case XLOG_BTREE_NEWPAGE:
			btree_xlog_newpage(true, lsn, record);
			break;
		case XLOG_BTREE_INVALIDMETA:
			btree_xlog_newmeta(true, lsn, record, false);
			break;
		default:
			elog(PANIC, "btree_redo: unknown op code %u", info);
	}
}

void
btree_undo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_BTREE_INSERT_LEAF:
			btree_xlog_insert(false, true, false, lsn, record);
			break;
		case XLOG_BTREE_INSERT_UPPER:
			btree_xlog_insert(false, false, false, lsn, record);
			break;
		case XLOG_BTREE_INSERT_META:
			btree_xlog_insert(false, false, true, lsn, record);
			break;
		case XLOG_BTREE_SPLIT_L:
			btree_xlog_split(false, true, false, lsn, record);
			break;
		case XLOG_BTREE_SPLIT_R:
			btree_xlog_split(false, false, false, lsn, record);
			break;
		case XLOG_BTREE_SPLIT_L_ROOT:
			btree_xlog_split(false, true, true, lsn, record);
			break;
		case XLOG_BTREE_SPLIT_R_ROOT:
			btree_xlog_split(false, false, true, lsn, record);
			break;
		case XLOG_BTREE_DELETE:
			btree_xlog_delete(false, lsn, record);
			break;
		case XLOG_BTREE_DELETE_PAGE:
			btree_xlog_delete_page(false, false, lsn, record);
			break;
		case XLOG_BTREE_DELETE_PAGE_META:
			btree_xlog_delete_page(false, true, lsn, record);
			break;
		case XLOG_BTREE_NEWROOT:
			btree_xlog_newroot(false, lsn, record);
			break;
		case XLOG_BTREE_NEWMETA:
			btree_xlog_newmeta(false, lsn, record, true);
			break;
		case XLOG_BTREE_NEWPAGE:
			btree_xlog_newpage(false, lsn, record);
			break;
		case XLOG_BTREE_INVALIDMETA:
			btree_xlog_newmeta(false, lsn, record, false);
			break;
		default:
			elog(PANIC, "btree_undo: unknown op code %u", info);
	}
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

	switch (info)
	{
		case XLOG_BTREE_INSERT_LEAF:
			{
				xl_btree_insert *xlrec = (xl_btree_insert *) rec;

				strcat(buf, "insert: ");
				out_target(buf, &(xlrec->target));
				break;
			}
		case XLOG_BTREE_INSERT_UPPER:
			{
				xl_btree_insert *xlrec = (xl_btree_insert *) rec;

				strcat(buf, "insert_upper: ");
				out_target(buf, &(xlrec->target));
				break;
			}
		case XLOG_BTREE_INSERT_META:
			{
				xl_btree_insert *xlrec = (xl_btree_insert *) rec;

				strcat(buf, "insert_meta: ");
				out_target(buf, &(xlrec->target));
				break;
			}
		case XLOG_BTREE_SPLIT_L:
			{
				xl_btree_split *xlrec = (xl_btree_split *) rec;

				strcat(buf, "split_l: ");
				out_target(buf, &(xlrec->target));
				sprintf(buf + strlen(buf), "; oth %u; rgh %u",
						xlrec->otherblk, xlrec->rightblk);
				break;
			}
		case XLOG_BTREE_SPLIT_R:
			{
				xl_btree_split *xlrec = (xl_btree_split *) rec;

				strcat(buf, "split_r: ");
				out_target(buf, &(xlrec->target));
				sprintf(buf + strlen(buf), "; oth %u; rgh %u",
						xlrec->otherblk, xlrec->rightblk);
				break;
			}
		case XLOG_BTREE_SPLIT_L_ROOT:
			{
				xl_btree_split *xlrec = (xl_btree_split *) rec;

				strcat(buf, "split_l_root: ");
				out_target(buf, &(xlrec->target));
				sprintf(buf + strlen(buf), "; oth %u; rgh %u",
						xlrec->otherblk, xlrec->rightblk);
				break;
			}
		case XLOG_BTREE_SPLIT_R_ROOT:
			{
				xl_btree_split *xlrec = (xl_btree_split *) rec;

				strcat(buf, "split_r_root: ");
				out_target(buf, &(xlrec->target));
				sprintf(buf + strlen(buf), "; oth %u; rgh %u",
						xlrec->otherblk, xlrec->rightblk);
				break;
			}
		case XLOG_BTREE_DELETE:
			{
				xl_btree_delete *xlrec = (xl_btree_delete *) rec;

				sprintf(buf + strlen(buf), "delete: node %u/%u; blk %u",
				 xlrec->node.tblNode, xlrec->node.relNode, xlrec->block);
				break;
			}
		case XLOG_BTREE_DELETE_PAGE:
		case XLOG_BTREE_DELETE_PAGE_META:
			{
				xl_btree_delete_page *xlrec = (xl_btree_delete_page *) rec;

				strcat(buf, "delete_page: ");
				out_target(buf, &(xlrec->target));
				sprintf(buf + strlen(buf), "; dead %u; left %u; right %u",
						xlrec->deadblk, xlrec->leftblk, xlrec->rightblk);
				break;
			}
		case XLOG_BTREE_NEWROOT:
			{
				xl_btree_newroot *xlrec = (xl_btree_newroot *) rec;

				sprintf(buf + strlen(buf), "newroot: node %u/%u; root %u lev %u",
						xlrec->node.tblNode, xlrec->node.relNode,
						xlrec->rootblk, xlrec->level);
				break;
			}
		case XLOG_BTREE_NEWMETA:
			{
				xl_btree_newmeta *xlrec = (xl_btree_newmeta *) rec;

				sprintf(buf + strlen(buf), "newmeta: node %u/%u; root %u lev %u fast %u lev %u",
						xlrec->node.tblNode, xlrec->node.relNode,
						xlrec->meta.root, xlrec->meta.level,
						xlrec->meta.fastroot, xlrec->meta.fastlevel);
				break;
			}
		case XLOG_BTREE_NEWPAGE:
			{
				xl_btree_newpage *xlrec = (xl_btree_newpage *) rec;

				sprintf(buf + strlen(buf), "newpage: node %u/%u; page %u",
						xlrec->node.tblNode, xlrec->node.relNode,
						xlrec->blkno);
				break;
			}
		case XLOG_BTREE_INVALIDMETA:
			{
				xl_btree_newmeta *xlrec = (xl_btree_newmeta *) rec;

				sprintf(buf + strlen(buf), "invalidmeta: node %u/%u; root %u lev %u fast %u lev %u",
						xlrec->node.tblNode, xlrec->node.relNode,
						xlrec->meta.root, xlrec->meta.level,
						xlrec->meta.fastroot, xlrec->meta.fastlevel);
				break;
			}
		default:
			strcat(buf, "UNKNOWN");
			break;
	}
}

void
btree_xlog_startup(void)
{
	incomplete_splits = NIL;
}

void
btree_xlog_cleanup(void)
{
	List	   *l;

	foreach(l, incomplete_splits)
	{
		bt_incomplete_split *split = (bt_incomplete_split *) lfirst(l);
		Relation	reln;
		Buffer		lbuf,
					rbuf;
		Page		lpage,
					rpage;
		BTPageOpaque lpageop,
					rpageop;
		bool		is_only;

		reln = XLogOpenRelation(true, RM_BTREE_ID, split->node);
		if (!RelationIsValid(reln))
			continue;
		lbuf = XLogReadBuffer(false, reln, split->leftblk);
		if (!BufferIsValid(lbuf))
			elog(PANIC, "btree_xlog_cleanup: left block unfound");
		lpage = (Page) BufferGetPage(lbuf);
		lpageop = (BTPageOpaque) PageGetSpecialPointer(lpage);
		rbuf = XLogReadBuffer(false, reln, split->rightblk);
		if (!BufferIsValid(rbuf))
			elog(PANIC, "btree_xlog_cleanup: right block unfound");
		rpage = (Page) BufferGetPage(rbuf);
		rpageop = (BTPageOpaque) PageGetSpecialPointer(rpage);

		/* if the two pages are all of their level, it's a only-page split */
		is_only = P_LEFTMOST(lpageop) && P_RIGHTMOST(rpageop);

		_bt_insert_parent(reln, lbuf, rbuf, (BTStack) NULL,
						  split->is_root, is_only);
	}
	incomplete_splits = NIL;
}
