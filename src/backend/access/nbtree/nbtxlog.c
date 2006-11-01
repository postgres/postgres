/*-------------------------------------------------------------------------
 *
 * nbtxlog.c
 *	  WAL replay logic for btrees.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/nbtree/nbtxlog.c,v 1.39 2006/11/01 19:43:17 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/nbtree.h"
#include "access/transam.h"

/*
 * We must keep track of expected insertions due to page splits, and apply
 * them manually if they are not seen in the WAL log during replay.  This
 * makes it safe for page insertion to be a multiple-WAL-action process.
 *
 * Similarly, deletion of an only child page and deletion of its parent page
 * form multiple WAL log entries, and we have to be prepared to follow through
 * with the deletion if the log ends between.
 *
 * The data structure is a simple linked list --- this should be good enough,
 * since we don't expect a page split or multi deletion to remain incomplete
 * for long.  In any case we need to respect the order of operations.
 */
typedef struct bt_incomplete_action
{
	RelFileNode node;			/* the index */
	bool		is_split;		/* T = pending split, F = pending delete */
	/* these fields are for a split: */
	bool		is_root;		/* we split the root */
	BlockNumber leftblk;		/* left half of split */
	BlockNumber rightblk;		/* right half of split */
	/* these fields are for a delete: */
	BlockNumber delblk;			/* parent block to be deleted */
} bt_incomplete_action;

static List *incomplete_actions;


static void
log_incomplete_split(RelFileNode node, BlockNumber leftblk,
					 BlockNumber rightblk, bool is_root)
{
	bt_incomplete_action *action = palloc(sizeof(bt_incomplete_action));

	action->node = node;
	action->is_split = true;
	action->is_root = is_root;
	action->leftblk = leftblk;
	action->rightblk = rightblk;
	incomplete_actions = lappend(incomplete_actions, action);
}

static void
forget_matching_split(RelFileNode node, BlockNumber downlink, bool is_root)
{
	ListCell   *l;

	foreach(l, incomplete_actions)
	{
		bt_incomplete_action *action = (bt_incomplete_action *) lfirst(l);

		if (RelFileNodeEquals(node, action->node) &&
			action->is_split &&
			downlink == action->rightblk)
		{
			if (is_root != action->is_root)
				elog(LOG, "forget_matching_split: fishy is_root data (expected %d, got %d)",
					 action->is_root, is_root);
			incomplete_actions = list_delete_ptr(incomplete_actions, action);
			pfree(action);
			break;				/* need not look further */
		}
	}
}

static void
log_incomplete_deletion(RelFileNode node, BlockNumber delblk)
{
	bt_incomplete_action *action = palloc(sizeof(bt_incomplete_action));

	action->node = node;
	action->is_split = false;
	action->delblk = delblk;
	incomplete_actions = lappend(incomplete_actions, action);
}

static void
forget_matching_deletion(RelFileNode node, BlockNumber delblk)
{
	ListCell   *l;

	foreach(l, incomplete_actions)
	{
		bt_incomplete_action *action = (bt_incomplete_action *) lfirst(l);

		if (RelFileNodeEquals(node, action->node) &&
			!action->is_split &&
			delblk == action->delblk)
		{
			incomplete_actions = list_delete_ptr(incomplete_actions, action);
			pfree(action);
			break;				/* need not look further */
		}
	}
}

/*
 * _bt_restore_page -- re-enter all the index tuples on a page
 *
 * The page is freshly init'd, and *from (length len) is a copy of what
 * had been its upper part (pd_upper to pd_special).  We assume that the
 * tuples had been added to the page in item-number order, and therefore
 * the one with highest item number appears first (lowest on the page).
 *
 * NOTE: the way this routine is coded, the rebuilt page will have the items
 * in correct itemno sequence, but physically the opposite order from the
 * original, because we insert them in the opposite of itemno order.  This
 * does not matter in any current btree code, but it's something to keep an
 * eye on.	Is it worth changing just on general principles?
 */
static void
_bt_restore_page(Page page, char *from, int len)
{
	IndexTupleData itupdata;
	Size		itemsz;
	char	   *end = from + len;

	for (; from < end;)
	{
		/* Need to copy tuple header due to alignment considerations */
		memcpy(&itupdata, from, sizeof(IndexTupleData));
		itemsz = IndexTupleDSize(itupdata);
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
				 BlockNumber fastroot, uint32 fastlevel)
{
	Buffer		metabuf;
	Page		metapg;
	BTMetaPageData *md;
	BTPageOpaque pageop;

	metabuf = XLogReadBuffer(reln, BTREE_METAPAGE, true);
	Assert(BufferIsValid(metabuf));
	metapg = BufferGetPage(metabuf);

	_bt_pageinit(metapg, BufferGetPageSize(metabuf));

	md = BTPageGetMeta(metapg);
	md->btm_magic = BTREE_MAGIC;
	md->btm_version = BTREE_VERSION;
	md->btm_root = root;
	md->btm_level = level;
	md->btm_fastroot = fastroot;
	md->btm_fastlevel = fastlevel;

	pageop = (BTPageOpaque) PageGetSpecialPointer(metapg);
	pageop->btpo_flags = BTP_META;

	/*
	 * Set pd_lower just past the end of the metadata.	This is not essential
	 * but it makes the page look compressible to xlog.c.
	 */
	((PageHeader) metapg)->pd_lower =
		((char *) md + sizeof(BTMetaPageData)) - (char *) metapg;

	PageSetLSN(metapg, lsn);
	PageSetTLI(metapg, ThisTimeLineID);
	MarkBufferDirty(metabuf);
	UnlockReleaseBuffer(metabuf);
}

static void
btree_xlog_insert(bool isleaf, bool ismeta,
				  XLogRecPtr lsn, XLogRecord *record)
{
	xl_btree_insert *xlrec = (xl_btree_insert *) XLogRecGetData(record);
	Relation	reln;
	Buffer		buffer;
	Page		page;
	char	   *datapos;
	int			datalen;
	xl_btree_metadata md;
	BlockNumber downlink = 0;

	datapos = (char *) xlrec + SizeOfBtreeInsert;
	datalen = record->xl_len - SizeOfBtreeInsert;
	if (!isleaf)
	{
		memcpy(&downlink, datapos, sizeof(BlockNumber));
		datapos += sizeof(BlockNumber);
		datalen -= sizeof(BlockNumber);
	}
	if (ismeta)
	{
		memcpy(&md, datapos, sizeof(xl_btree_metadata));
		datapos += sizeof(xl_btree_metadata);
		datalen -= sizeof(xl_btree_metadata);
	}

	if ((record->xl_info & XLR_BKP_BLOCK_1) && !ismeta && isleaf)
		return;					/* nothing to do */

	reln = XLogOpenRelation(xlrec->target.node);

	if (!(record->xl_info & XLR_BKP_BLOCK_1))
	{
		buffer = XLogReadBuffer(reln,
							 ItemPointerGetBlockNumber(&(xlrec->target.tid)),
								false);
		if (BufferIsValid(buffer))
		{
			page = (Page) BufferGetPage(buffer);

			if (XLByteLE(lsn, PageGetLSN(page)))
			{
				UnlockReleaseBuffer(buffer);
			}
			else
			{
				if (PageAddItem(page, (Item) datapos, datalen,
							ItemPointerGetOffsetNumber(&(xlrec->target.tid)),
								LP_USED) == InvalidOffsetNumber)
					elog(PANIC, "btree_insert_redo: failed to add item");

				PageSetLSN(page, lsn);
				PageSetTLI(page, ThisTimeLineID);
				MarkBufferDirty(buffer);
				UnlockReleaseBuffer(buffer);
			}
		}
	}

	if (ismeta)
		_bt_restore_meta(reln, lsn,
						 md.root, md.level,
						 md.fastroot, md.fastlevel);

	/* Forget any split this insertion completes */
	if (!isleaf)
		forget_matching_split(xlrec->target.node, downlink, false);
}

static void
btree_xlog_split(bool onleft, bool isroot,
				 XLogRecPtr lsn, XLogRecord *record)
{
	xl_btree_split *xlrec = (xl_btree_split *) XLogRecGetData(record);
	Relation	reln;
	BlockNumber targetblk;
	OffsetNumber targetoff;
	BlockNumber leftsib;
	BlockNumber rightsib;
	BlockNumber downlink = 0;
	Buffer		buffer;
	Page		page;
	BTPageOpaque pageop;

	reln = XLogOpenRelation(xlrec->target.node);
	targetblk = ItemPointerGetBlockNumber(&(xlrec->target.tid));
	targetoff = ItemPointerGetOffsetNumber(&(xlrec->target.tid));
	leftsib = (onleft) ? targetblk : xlrec->otherblk;
	rightsib = (onleft) ? xlrec->otherblk : targetblk;

	/* Left (original) sibling */
	buffer = XLogReadBuffer(reln, leftsib, true);
	Assert(BufferIsValid(buffer));
	page = (Page) BufferGetPage(buffer);

	_bt_pageinit(page, BufferGetPageSize(buffer));
	pageop = (BTPageOpaque) PageGetSpecialPointer(page);

	pageop->btpo_prev = xlrec->leftblk;
	pageop->btpo_next = rightsib;
	pageop->btpo.level = xlrec->level;
	pageop->btpo_flags = (xlrec->level == 0) ? BTP_LEAF : 0;
	pageop->btpo_cycleid = 0;

	_bt_restore_page(page,
					 (char *) xlrec + SizeOfBtreeSplit,
					 xlrec->leftlen);

	if (onleft && xlrec->level > 0)
	{
		IndexTuple	itup;

		/* extract downlink in the target tuple */
		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, targetoff));
		downlink = ItemPointerGetBlockNumber(&(itup->t_tid));
		Assert(ItemPointerGetOffsetNumber(&(itup->t_tid)) == P_HIKEY);
	}

	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);
	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);

	/* Right (new) sibling */
	buffer = XLogReadBuffer(reln, rightsib, true);
	Assert(BufferIsValid(buffer));
	page = (Page) BufferGetPage(buffer);

	_bt_pageinit(page, BufferGetPageSize(buffer));
	pageop = (BTPageOpaque) PageGetSpecialPointer(page);

	pageop->btpo_prev = leftsib;
	pageop->btpo_next = xlrec->rightblk;
	pageop->btpo.level = xlrec->level;
	pageop->btpo_flags = (xlrec->level == 0) ? BTP_LEAF : 0;
	pageop->btpo_cycleid = 0;

	_bt_restore_page(page,
					 (char *) xlrec + SizeOfBtreeSplit + xlrec->leftlen,
					 record->xl_len - SizeOfBtreeSplit - xlrec->leftlen);

	if (!onleft && xlrec->level > 0)
	{
		IndexTuple	itup;

		/* extract downlink in the target tuple */
		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, targetoff));
		downlink = ItemPointerGetBlockNumber(&(itup->t_tid));
		Assert(ItemPointerGetOffsetNumber(&(itup->t_tid)) == P_HIKEY);
	}

	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);
	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);

	/* Fix left-link of right (next) page */
	if (!(record->xl_info & XLR_BKP_BLOCK_1))
	{
		if (xlrec->rightblk != P_NONE)
		{
			buffer = XLogReadBuffer(reln, xlrec->rightblk, false);
			if (BufferIsValid(buffer))
			{
				page = (Page) BufferGetPage(buffer);

				if (XLByteLE(lsn, PageGetLSN(page)))
				{
					UnlockReleaseBuffer(buffer);
				}
				else
				{
					pageop = (BTPageOpaque) PageGetSpecialPointer(page);
					pageop->btpo_prev = rightsib;

					PageSetLSN(page, lsn);
					PageSetTLI(page, ThisTimeLineID);
					MarkBufferDirty(buffer);
					UnlockReleaseBuffer(buffer);
				}
			}
		}
	}

	/* Forget any split this insertion completes */
	if (xlrec->level > 0)
		forget_matching_split(xlrec->target.node, downlink, false);

	/* The job ain't done till the parent link is inserted... */
	log_incomplete_split(xlrec->target.node,
						 leftsib, rightsib, isroot);
}

static void
btree_xlog_delete(XLogRecPtr lsn, XLogRecord *record)
{
	xl_btree_delete *xlrec;
	Relation	reln;
	Buffer		buffer;
	Page		page;
	BTPageOpaque opaque;

	if (record->xl_info & XLR_BKP_BLOCK_1)
		return;

	xlrec = (xl_btree_delete *) XLogRecGetData(record);
	reln = XLogOpenRelation(xlrec->node);
	buffer = XLogReadBuffer(reln, xlrec->block, false);
	if (!BufferIsValid(buffer))
		return;
	page = (Page) BufferGetPage(buffer);

	if (XLByteLE(lsn, PageGetLSN(page)))
	{
		UnlockReleaseBuffer(buffer);
		return;
	}

	if (record->xl_len > SizeOfBtreeDelete)
	{
		OffsetNumber *unused;
		OffsetNumber *unend;

		unused = (OffsetNumber *) ((char *) xlrec + SizeOfBtreeDelete);
		unend = (OffsetNumber *) ((char *) xlrec + record->xl_len);

		PageIndexMultiDelete(page, unused, unend - unused);
	}

	/*
	 * Mark the page as not containing any LP_DELETE items --- see comments in
	 * _bt_delitems().
	 */
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	opaque->btpo_flags &= ~BTP_HAS_GARBAGE;

	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);
	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);
}

static void
btree_xlog_delete_page(uint8 info, XLogRecPtr lsn, XLogRecord *record)
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

	reln = XLogOpenRelation(xlrec->target.node);
	parent = ItemPointerGetBlockNumber(&(xlrec->target.tid));
	target = xlrec->deadblk;
	leftsib = xlrec->leftblk;
	rightsib = xlrec->rightblk;

	/* parent page */
	if (!(record->xl_info & XLR_BKP_BLOCK_1))
	{
		buffer = XLogReadBuffer(reln, parent, false);
		if (BufferIsValid(buffer))
		{
			page = (Page) BufferGetPage(buffer);
			pageop = (BTPageOpaque) PageGetSpecialPointer(page);
			if (XLByteLE(lsn, PageGetLSN(page)))
			{
				UnlockReleaseBuffer(buffer);
			}
			else
			{
				OffsetNumber poffset;

				poffset = ItemPointerGetOffsetNumber(&(xlrec->target.tid));
				if (poffset >= PageGetMaxOffsetNumber(page))
				{
					Assert(info == XLOG_BTREE_DELETE_PAGE_HALF);
					Assert(poffset == P_FIRSTDATAKEY(pageop));
					PageIndexTupleDelete(page, poffset);
					pageop->btpo_flags |= BTP_HALF_DEAD;
				}
				else
				{
					ItemId		itemid;
					IndexTuple	itup;
					OffsetNumber nextoffset;

					Assert(info != XLOG_BTREE_DELETE_PAGE_HALF);
					itemid = PageGetItemId(page, poffset);
					itup = (IndexTuple) PageGetItem(page, itemid);
					ItemPointerSet(&(itup->t_tid), rightsib, P_HIKEY);
					nextoffset = OffsetNumberNext(poffset);
					PageIndexTupleDelete(page, nextoffset);
				}

				PageSetLSN(page, lsn);
				PageSetTLI(page, ThisTimeLineID);
				MarkBufferDirty(buffer);
				UnlockReleaseBuffer(buffer);
			}
		}
	}

	/* Fix left-link of right sibling */
	if (!(record->xl_info & XLR_BKP_BLOCK_2))
	{
		buffer = XLogReadBuffer(reln, rightsib, false);
		if (BufferIsValid(buffer))
		{
			page = (Page) BufferGetPage(buffer);
			if (XLByteLE(lsn, PageGetLSN(page)))
			{
				UnlockReleaseBuffer(buffer);
			}
			else
			{
				pageop = (BTPageOpaque) PageGetSpecialPointer(page);
				pageop->btpo_prev = leftsib;

				PageSetLSN(page, lsn);
				PageSetTLI(page, ThisTimeLineID);
				MarkBufferDirty(buffer);
				UnlockReleaseBuffer(buffer);
			}
		}
	}

	/* Fix right-link of left sibling, if any */
	if (!(record->xl_info & XLR_BKP_BLOCK_3))
	{
		if (leftsib != P_NONE)
		{
			buffer = XLogReadBuffer(reln, leftsib, false);
			if (BufferIsValid(buffer))
			{
				page = (Page) BufferGetPage(buffer);
				if (XLByteLE(lsn, PageGetLSN(page)))
				{
					UnlockReleaseBuffer(buffer);
				}
				else
				{
					pageop = (BTPageOpaque) PageGetSpecialPointer(page);
					pageop->btpo_next = rightsib;

					PageSetLSN(page, lsn);
					PageSetTLI(page, ThisTimeLineID);
					MarkBufferDirty(buffer);
					UnlockReleaseBuffer(buffer);
				}
			}
		}
	}

	/* Rewrite target page as empty deleted page */
	buffer = XLogReadBuffer(reln, target, true);
	Assert(BufferIsValid(buffer));
	page = (Page) BufferGetPage(buffer);

	_bt_pageinit(page, BufferGetPageSize(buffer));
	pageop = (BTPageOpaque) PageGetSpecialPointer(page);

	pageop->btpo_prev = leftsib;
	pageop->btpo_next = rightsib;
	pageop->btpo.xact = FrozenTransactionId;
	pageop->btpo_flags = BTP_DELETED;
	pageop->btpo_cycleid = 0;

	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);
	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);

	/* Update metapage if needed */
	if (info == XLOG_BTREE_DELETE_PAGE_META)
	{
		xl_btree_metadata md;

		memcpy(&md, (char *) xlrec + SizeOfBtreeDeletePage,
			   sizeof(xl_btree_metadata));
		_bt_restore_meta(reln, lsn,
						 md.root, md.level,
						 md.fastroot, md.fastlevel);
	}

	/* Forget any completed deletion */
	forget_matching_deletion(xlrec->target.node, target);

	/* If parent became half-dead, remember it for deletion */
	if (info == XLOG_BTREE_DELETE_PAGE_HALF)
		log_incomplete_deletion(xlrec->target.node, parent);
}

static void
btree_xlog_newroot(XLogRecPtr lsn, XLogRecord *record)
{
	xl_btree_newroot *xlrec = (xl_btree_newroot *) XLogRecGetData(record);
	Relation	reln;
	Buffer		buffer;
	Page		page;
	BTPageOpaque pageop;
	BlockNumber downlink = 0;

	reln = XLogOpenRelation(xlrec->node);
	buffer = XLogReadBuffer(reln, xlrec->rootblk, true);
	Assert(BufferIsValid(buffer));
	page = (Page) BufferGetPage(buffer);

	_bt_pageinit(page, BufferGetPageSize(buffer));
	pageop = (BTPageOpaque) PageGetSpecialPointer(page);

	pageop->btpo_flags = BTP_ROOT;
	pageop->btpo_prev = pageop->btpo_next = P_NONE;
	pageop->btpo.level = xlrec->level;
	if (xlrec->level == 0)
		pageop->btpo_flags |= BTP_LEAF;
	pageop->btpo_cycleid = 0;

	if (record->xl_len > SizeOfBtreeNewroot)
	{
		IndexTuple	itup;

		_bt_restore_page(page,
						 (char *) xlrec + SizeOfBtreeNewroot,
						 record->xl_len - SizeOfBtreeNewroot);
		/* extract downlink to the right-hand split page */
		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, P_FIRSTKEY));
		downlink = ItemPointerGetBlockNumber(&(itup->t_tid));
		Assert(ItemPointerGetOffsetNumber(&(itup->t_tid)) == P_HIKEY);
	}

	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);
	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);

	_bt_restore_meta(reln, lsn,
					 xlrec->rootblk, xlrec->level,
					 xlrec->rootblk, xlrec->level);

	/* Check to see if this satisfies any incomplete insertions */
	if (record->xl_len > SizeOfBtreeNewroot)
		forget_matching_split(xlrec->node, downlink, true);
}


void
btree_redo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_BTREE_INSERT_LEAF:
			btree_xlog_insert(true, false, lsn, record);
			break;
		case XLOG_BTREE_INSERT_UPPER:
			btree_xlog_insert(false, false, lsn, record);
			break;
		case XLOG_BTREE_INSERT_META:
			btree_xlog_insert(false, true, lsn, record);
			break;
		case XLOG_BTREE_SPLIT_L:
			btree_xlog_split(true, false, lsn, record);
			break;
		case XLOG_BTREE_SPLIT_R:
			btree_xlog_split(false, false, lsn, record);
			break;
		case XLOG_BTREE_SPLIT_L_ROOT:
			btree_xlog_split(true, true, lsn, record);
			break;
		case XLOG_BTREE_SPLIT_R_ROOT:
			btree_xlog_split(false, true, lsn, record);
			break;
		case XLOG_BTREE_DELETE:
			btree_xlog_delete(lsn, record);
			break;
		case XLOG_BTREE_DELETE_PAGE:
		case XLOG_BTREE_DELETE_PAGE_META:
		case XLOG_BTREE_DELETE_PAGE_HALF:
			btree_xlog_delete_page(info, lsn, record);
			break;
		case XLOG_BTREE_NEWROOT:
			btree_xlog_newroot(lsn, record);
			break;
		default:
			elog(PANIC, "btree_redo: unknown op code %u", info);
	}
}

static void
out_target(StringInfo buf, xl_btreetid *target)
{
	appendStringInfo(buf, "rel %u/%u/%u; tid %u/%u",
			 target->node.spcNode, target->node.dbNode, target->node.relNode,
					 ItemPointerGetBlockNumber(&(target->tid)),
					 ItemPointerGetOffsetNumber(&(target->tid)));
}

void
btree_desc(StringInfo buf, uint8 xl_info, char *rec)
{
	uint8		info = xl_info & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_BTREE_INSERT_LEAF:
			{
				xl_btree_insert *xlrec = (xl_btree_insert *) rec;

				appendStringInfo(buf, "insert: ");
				out_target(buf, &(xlrec->target));
				break;
			}
		case XLOG_BTREE_INSERT_UPPER:
			{
				xl_btree_insert *xlrec = (xl_btree_insert *) rec;

				appendStringInfo(buf, "insert_upper: ");
				out_target(buf, &(xlrec->target));
				break;
			}
		case XLOG_BTREE_INSERT_META:
			{
				xl_btree_insert *xlrec = (xl_btree_insert *) rec;

				appendStringInfo(buf, "insert_meta: ");
				out_target(buf, &(xlrec->target));
				break;
			}
		case XLOG_BTREE_SPLIT_L:
			{
				xl_btree_split *xlrec = (xl_btree_split *) rec;

				appendStringInfo(buf, "split_l: ");
				out_target(buf, &(xlrec->target));
				appendStringInfo(buf, "; oth %u; rgh %u",
								 xlrec->otherblk, xlrec->rightblk);
				break;
			}
		case XLOG_BTREE_SPLIT_R:
			{
				xl_btree_split *xlrec = (xl_btree_split *) rec;

				appendStringInfo(buf, "split_r: ");
				out_target(buf, &(xlrec->target));
				appendStringInfo(buf, "; oth %u; rgh %u",
								 xlrec->otherblk, xlrec->rightblk);
				break;
			}
		case XLOG_BTREE_SPLIT_L_ROOT:
			{
				xl_btree_split *xlrec = (xl_btree_split *) rec;

				appendStringInfo(buf, "split_l_root: ");
				out_target(buf, &(xlrec->target));
				appendStringInfo(buf, "; oth %u; rgh %u",
								 xlrec->otherblk, xlrec->rightblk);
				break;
			}
		case XLOG_BTREE_SPLIT_R_ROOT:
			{
				xl_btree_split *xlrec = (xl_btree_split *) rec;

				appendStringInfo(buf, "split_r_root: ");
				out_target(buf, &(xlrec->target));
				appendStringInfo(buf, "; oth %u; rgh %u",
								 xlrec->otherblk, xlrec->rightblk);
				break;
			}
		case XLOG_BTREE_DELETE:
			{
				xl_btree_delete *xlrec = (xl_btree_delete *) rec;

				appendStringInfo(buf, "delete: rel %u/%u/%u; blk %u",
								 xlrec->node.spcNode, xlrec->node.dbNode,
								 xlrec->node.relNode, xlrec->block);
				break;
			}
		case XLOG_BTREE_DELETE_PAGE:
		case XLOG_BTREE_DELETE_PAGE_META:
		case XLOG_BTREE_DELETE_PAGE_HALF:
			{
				xl_btree_delete_page *xlrec = (xl_btree_delete_page *) rec;

				appendStringInfo(buf, "delete_page: ");
				out_target(buf, &(xlrec->target));
				appendStringInfo(buf, "; dead %u; left %u; right %u",
							xlrec->deadblk, xlrec->leftblk, xlrec->rightblk);
				break;
			}
		case XLOG_BTREE_NEWROOT:
			{
				xl_btree_newroot *xlrec = (xl_btree_newroot *) rec;

				appendStringInfo(buf, "newroot: rel %u/%u/%u; root %u lev %u",
								 xlrec->node.spcNode, xlrec->node.dbNode,
								 xlrec->node.relNode,
								 xlrec->rootblk, xlrec->level);
				break;
			}
		default:
			appendStringInfo(buf, "UNKNOWN");
			break;
	}
}

void
btree_xlog_startup(void)
{
	incomplete_actions = NIL;
}

void
btree_xlog_cleanup(void)
{
	ListCell   *l;

	foreach(l, incomplete_actions)
	{
		bt_incomplete_action *action = (bt_incomplete_action *) lfirst(l);
		Relation	reln;

		reln = XLogOpenRelation(action->node);
		if (action->is_split)
		{
			/* finish an incomplete split */
			Buffer		lbuf,
						rbuf;
			Page		lpage,
						rpage;
			BTPageOpaque lpageop,
						rpageop;
			bool		is_only;

			lbuf = XLogReadBuffer(reln, action->leftblk, false);
			/* failure is impossible because we wrote this page earlier */
			if (!BufferIsValid(lbuf))
				elog(PANIC, "btree_xlog_cleanup: left block unfound");
			lpage = (Page) BufferGetPage(lbuf);
			lpageop = (BTPageOpaque) PageGetSpecialPointer(lpage);
			rbuf = XLogReadBuffer(reln, action->rightblk, false);
			/* failure is impossible because we wrote this page earlier */
			if (!BufferIsValid(rbuf))
				elog(PANIC, "btree_xlog_cleanup: right block unfound");
			rpage = (Page) BufferGetPage(rbuf);
			rpageop = (BTPageOpaque) PageGetSpecialPointer(rpage);

			/* if the pages are all of their level, it's a only-page split */
			is_only = P_LEFTMOST(lpageop) && P_RIGHTMOST(rpageop);

			_bt_insert_parent(reln, lbuf, rbuf, NULL,
							  action->is_root, is_only);
		}
		else
		{
			/* finish an incomplete deletion (of a half-dead page) */
			Buffer		buf;

			buf = XLogReadBuffer(reln, action->delblk, false);
			if (BufferIsValid(buf))
				if (_bt_pagedel(reln, buf, NULL, true) == 0)
					elog(PANIC, "btree_xlog_cleanup: _bt_pagdel failed");
		}
	}
	incomplete_actions = NIL;
}

bool
btree_safe_restartpoint(void)
{
	if (incomplete_actions)
		return false;
	return true;
}
