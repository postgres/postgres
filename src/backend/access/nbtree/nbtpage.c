/*-------------------------------------------------------------------------
 *
 * nbtpage.c
 *	  BTree-specific page management code for the Postgres btree access
 *	  method.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/nbtree/nbtpage.c,v 1.72.2.1 2005/05/07 21:33:21 tgl Exp $
 *
 *	NOTES
 *	   Postgres btree pages look like ordinary relation pages.	The opaque
 *	   data at high addresses includes pointers to left and right siblings
 *	   and flag data describing page state.  The first page in a btree, page
 *	   zero, is special -- it stores meta-information describing the tree.
 *	   Pages one and higher store the actual tree data.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/nbtree.h"
#include "miscadmin.h"
#include "storage/freespace.h"
#include "storage/lmgr.h"


/*
 *	_bt_metapinit() -- Initialize the metadata page of a new btree.
 *
 * If markvalid is true, the index is immediately marked valid, else it
 * will be invalid until _bt_metaproot() is called.
 *
 * Note: there's no real need for any locking here.  Since the transaction
 * creating the index hasn't committed yet, no one else can even see the index
 * much less be trying to use it.  (In a REINDEX-in-place scenario, that's
 * not true, but we assume the caller holds sufficient locks on the index.)
 */
void
_bt_metapinit(Relation rel, bool markvalid)
{
	Buffer		buf;
	Page		pg;
	BTMetaPageData *metad;
	BTPageOpaque op;

	if (RelationGetNumberOfBlocks(rel) != 0)
		elog(ERROR, "cannot initialize non-empty btree index \"%s\"",
			 RelationGetRelationName(rel));

	buf = ReadBuffer(rel, P_NEW);
	Assert(BufferGetBlockNumber(buf) == BTREE_METAPAGE);
	pg = BufferGetPage(buf);

	/* NO ELOG(ERROR) from here till newmeta op is logged */
	START_CRIT_SECTION();

	_bt_pageinit(pg, BufferGetPageSize(buf));

	metad = BTPageGetMeta(pg);
	metad->btm_magic = markvalid ? BTREE_MAGIC : 0;
	metad->btm_version = BTREE_VERSION;
	metad->btm_root = P_NONE;
	metad->btm_level = 0;
	metad->btm_fastroot = P_NONE;
	metad->btm_fastlevel = 0;

	op = (BTPageOpaque) PageGetSpecialPointer(pg);
	op->btpo_flags = BTP_META;

	/* XLOG stuff */
	if (!rel->rd_istemp)
	{
		xl_btree_newmeta xlrec;
		XLogRecPtr	recptr;
		XLogRecData rdata[1];

		xlrec.node = rel->rd_node;
		xlrec.meta.root = metad->btm_root;
		xlrec.meta.level = metad->btm_level;
		xlrec.meta.fastroot = metad->btm_fastroot;
		xlrec.meta.fastlevel = metad->btm_fastlevel;

		rdata[0].buffer = InvalidBuffer;
		rdata[0].data = (char *) &xlrec;
		rdata[0].len = SizeOfBtreeNewmeta;
		rdata[0].next = NULL;

		recptr = XLogInsert(RM_BTREE_ID,
							markvalid ? XLOG_BTREE_NEWMETA : XLOG_BTREE_INVALIDMETA,
							rdata);

		PageSetLSN(pg, recptr);
		PageSetSUI(pg, ThisStartUpID);
	}

	END_CRIT_SECTION();

	WriteBuffer(buf);
}

/*
 *	_bt_getroot() -- Get the root page of the btree.
 *
 *		Since the root page can move around the btree file, we have to read
 *		its location from the metadata page, and then read the root page
 *		itself.  If no root page exists yet, we have to create one.  The
 *		standard class of race conditions exists here; I think I covered
 *		them all in the Hopi Indian rain dance of lock requests below.
 *
 *		The access type parameter (BT_READ or BT_WRITE) controls whether
 *		a new root page will be created or not.  If access = BT_READ,
 *		and no root page exists, we just return InvalidBuffer.	For
 *		BT_WRITE, we try to create the root page if it doesn't exist.
 *		NOTE that the returned root page will have only a read lock set
 *		on it even if access = BT_WRITE!
 *
 *		The returned page is not necessarily the true root --- it could be
 *		a "fast root" (a page that is alone in its level due to deletions).
 *		Also, if the root page is split while we are "in flight" to it,
 *		what we will return is the old root, which is now just the leftmost
 *		page on a probably-not-very-wide level.  For most purposes this is
 *		as good as or better than the true root, so we do not bother to
 *		insist on finding the true root.  We do, however, guarantee to
 *		return a live (not deleted or half-dead) page.
 *
 *		On successful return, the root page is pinned and read-locked.
 *		The metadata page is not locked or pinned on exit.
 */
Buffer
_bt_getroot(Relation rel, int access)
{
	Buffer		metabuf;
	Page		metapg;
	BTPageOpaque metaopaque;
	Buffer		rootbuf;
	Page		rootpage;
	BTPageOpaque rootopaque;
	BlockNumber rootblkno;
	uint32		rootlevel;
	BTMetaPageData *metad;

	metabuf = _bt_getbuf(rel, BTREE_METAPAGE, BT_READ);
	metapg = BufferGetPage(metabuf);
	metaopaque = (BTPageOpaque) PageGetSpecialPointer(metapg);
	metad = BTPageGetMeta(metapg);

	/* sanity-check the metapage */
	if (!(metaopaque->btpo_flags & BTP_META) ||
		metad->btm_magic != BTREE_MAGIC)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" is not a btree",
						RelationGetRelationName(rel))));

	if (metad->btm_version != BTREE_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("version mismatch in index \"%s\": file version %d, code version %d",
						RelationGetRelationName(rel),
						metad->btm_version, BTREE_VERSION)));

	/* if no root page initialized yet, do it */
	if (metad->btm_root == P_NONE)
	{
		/* If access = BT_READ, caller doesn't want us to create root yet */
		if (access == BT_READ)
		{
			_bt_relbuf(rel, metabuf);
			return InvalidBuffer;
		}

		/* trade in our read lock for a write lock */
		LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
		LockBuffer(metabuf, BT_WRITE);

		/*
		 * Race condition:	if someone else initialized the metadata
		 * between the time we released the read lock and acquired the
		 * write lock, we must avoid doing it again.
		 */
		if (metad->btm_root != P_NONE)
		{
			/*
			 * Metadata initialized by someone else.  In order to
			 * guarantee no deadlocks, we have to release the metadata
			 * page and start all over again.  (Is that really true? But
			 * it's hardly worth trying to optimize this case.)
			 */
			_bt_relbuf(rel, metabuf);
			return _bt_getroot(rel, access);
		}

		/*
		 * Get, initialize, write, and leave a lock of the appropriate
		 * type on the new root page.  Since this is the first page in the
		 * tree, it's a leaf as well as the root.
		 */
		rootbuf = _bt_getbuf(rel, P_NEW, BT_WRITE);
		rootblkno = BufferGetBlockNumber(rootbuf);
		rootpage = BufferGetPage(rootbuf);

		_bt_pageinit(rootpage, BufferGetPageSize(rootbuf));
		rootopaque = (BTPageOpaque) PageGetSpecialPointer(rootpage);
		rootopaque->btpo_prev = rootopaque->btpo_next = P_NONE;
		rootopaque->btpo_flags = (BTP_LEAF | BTP_ROOT);
		rootopaque->btpo.level = 0;

		/* NO ELOG(ERROR) till meta is updated */
		START_CRIT_SECTION();

		metad->btm_root = rootblkno;
		metad->btm_level = 0;
		metad->btm_fastroot = rootblkno;
		metad->btm_fastlevel = 0;

		/* XLOG stuff */
		if (!rel->rd_istemp)
		{
			xl_btree_newroot xlrec;
			XLogRecPtr	recptr;
			XLogRecData rdata;

			xlrec.node = rel->rd_node;
			xlrec.rootblk = rootblkno;
			xlrec.level = 0;

			rdata.buffer = InvalidBuffer;
			rdata.data = (char *) &xlrec;
			rdata.len = SizeOfBtreeNewroot;
			rdata.next = NULL;

			recptr = XLogInsert(RM_BTREE_ID, XLOG_BTREE_NEWROOT, &rdata);

			PageSetLSN(rootpage, recptr);
			PageSetSUI(rootpage, ThisStartUpID);
			PageSetLSN(metapg, recptr);
			PageSetSUI(metapg, ThisStartUpID);
		}

		END_CRIT_SECTION();

		_bt_wrtnorelbuf(rel, rootbuf);

		/*
		 * swap root write lock for read lock.	There is no danger of
		 * anyone else accessing the new root page while it's unlocked,
		 * since no one else knows where it is yet.
		 */
		LockBuffer(rootbuf, BUFFER_LOCK_UNLOCK);
		LockBuffer(rootbuf, BT_READ);

		/* okay, metadata is correct, write and release it */
		_bt_wrtbuf(rel, metabuf);
	}
	else
	{
		rootblkno = metad->btm_fastroot;
		Assert(rootblkno != P_NONE);
		rootlevel = metad->btm_fastlevel;

		_bt_relbuf(rel, metabuf);		/* done with the meta page */

		for (;;)
		{
			rootbuf = _bt_getbuf(rel, rootblkno, BT_READ);
			rootpage = BufferGetPage(rootbuf);
			rootopaque = (BTPageOpaque) PageGetSpecialPointer(rootpage);

			if (!P_IGNORE(rootopaque))
				break;

			/* it's dead, Jim.  step right one page */
			if (P_RIGHTMOST(rootopaque))
				elog(ERROR, "no live root page found in \"%s\"",
					 RelationGetRelationName(rel));
			rootblkno = rootopaque->btpo_next;

			_bt_relbuf(rel, rootbuf);
		}

		/* Note: can't check btpo.level on deleted pages */
		if (rootopaque->btpo.level != rootlevel)
			elog(ERROR, "root page %u of \"%s\" has level %u, expected %u",
				 rootblkno, RelationGetRelationName(rel),
				 rootopaque->btpo.level, rootlevel);
	}

	/*
	 * By here, we have a pin and read lock on the root page, and no lock
	 * set on the metadata page.  Return the root page's buffer.
	 */
	return rootbuf;
}

/*
 *	_bt_gettrueroot() -- Get the true root page of the btree.
 *
 *		This is the same as the BT_READ case of _bt_getroot(), except
 *		we follow the true-root link not the fast-root link.
 *
 * By the time we acquire lock on the root page, it might have been split and
 * not be the true root anymore.  This is okay for the present uses of this
 * routine; we only really need to be able to move up at least one tree level
 * from whatever non-root page we were at.	If we ever do need to lock the
 * one true root page, we could loop here, re-reading the metapage on each
 * failure.  (Note that it wouldn't do to hold the lock on the metapage while
 * moving to the root --- that'd deadlock against any concurrent root split.)
 */
Buffer
_bt_gettrueroot(Relation rel)
{
	Buffer		metabuf;
	Page		metapg;
	BTPageOpaque metaopaque;
	Buffer		rootbuf;
	Page		rootpage;
	BTPageOpaque rootopaque;
	BlockNumber rootblkno;
	uint32		rootlevel;
	BTMetaPageData *metad;

	metabuf = _bt_getbuf(rel, BTREE_METAPAGE, BT_READ);
	metapg = BufferGetPage(metabuf);
	metaopaque = (BTPageOpaque) PageGetSpecialPointer(metapg);
	metad = BTPageGetMeta(metapg);

	if (!(metaopaque->btpo_flags & BTP_META) ||
		metad->btm_magic != BTREE_MAGIC)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" is not a btree",
						RelationGetRelationName(rel))));

	if (metad->btm_version != BTREE_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("version mismatch in index \"%s\": file version %d, code version %d",
						RelationGetRelationName(rel),
						metad->btm_version, BTREE_VERSION)));

	/* if no root page initialized yet, fail */
	if (metad->btm_root == P_NONE)
	{
		_bt_relbuf(rel, metabuf);
		return InvalidBuffer;
	}

	rootblkno = metad->btm_root;
	rootlevel = metad->btm_level;

	_bt_relbuf(rel, metabuf);	/* done with the meta page */

	for (;;)
	{
		rootbuf = _bt_getbuf(rel, rootblkno, BT_READ);
		rootpage = BufferGetPage(rootbuf);
		rootopaque = (BTPageOpaque) PageGetSpecialPointer(rootpage);

		if (!P_IGNORE(rootopaque))
			break;

		/* it's dead, Jim.  step right one page */
		if (P_RIGHTMOST(rootopaque))
			elog(ERROR, "no live root page found in \"%s\"",
				 RelationGetRelationName(rel));
		rootblkno = rootopaque->btpo_next;

		_bt_relbuf(rel, rootbuf);
	}

	/* Note: can't check btpo.level on deleted pages */
	if (rootopaque->btpo.level != rootlevel)
		elog(ERROR, "root page %u of \"%s\" has level %u, expected %u",
			 rootblkno, RelationGetRelationName(rel),
			 rootopaque->btpo.level, rootlevel);

	return rootbuf;
}

/*
 *	_bt_getbuf() -- Get a buffer by block number for read or write.
 *
 *		blkno == P_NEW means to get an unallocated index page.
 *
 *		When this routine returns, the appropriate lock is set on the
 *		requested buffer and its reference count has been incremented
 *		(ie, the buffer is "locked and pinned").
 */
Buffer
_bt_getbuf(Relation rel, BlockNumber blkno, int access)
{
	Buffer		buf;

	if (blkno != P_NEW)
	{
		/* Read an existing block of the relation */
		buf = ReadBuffer(rel, blkno);
		LockBuffer(buf, access);
	}
	else
	{
		bool		needLock;
		Page		page;

		Assert(access == BT_WRITE);

		/*
		 * First see if the FSM knows of any free pages.
		 *
		 * We can't trust the FSM's report unreservedly; we have to check
		 * that the page is still free.  (For example, an already-free
		 * page could have been re-used between the time the last VACUUM
		 * scanned it and the time the VACUUM made its FSM updates.)
		 *
		 * In fact, it's worse than that: we can't even assume that it's
		 * safe to take a lock on the reported page.  If somebody else
		 * has a lock on it, or even worse our own caller does, we could
		 * deadlock.  (The own-caller scenario is actually not improbable.
		 * Consider an index on a serial or timestamp column.  Nearly all
		 * splits will be at the rightmost page, so it's entirely likely
		 * that _bt_split will call us while holding a lock on the page most
		 * recently acquired from FSM.  A VACUUM running concurrently with
		 * the previous split could well have placed that page back in FSM.)
		 *
		 * To get around that, we ask for only a conditional lock on the
		 * reported page.  If we fail, then someone else is using the page,
		 * and we may reasonably assume it's not free.  (If we happen to be
		 * wrong, the worst consequence is the page will be lost to use till
		 * the next VACUUM, which is no big problem.)
		 */
		for (;;)
		{
			blkno = GetFreeIndexPage(&rel->rd_node);
			if (blkno == InvalidBlockNumber)
				break;
			buf = ReadBuffer(rel, blkno);
			if (ConditionalLockBuffer(buf))
			{
				page = BufferGetPage(buf);
				if (_bt_page_recyclable(page))
				{
					/* Okay to use page.  Re-initialize and return it */
					_bt_pageinit(page, BufferGetPageSize(buf));
					return buf;
				}
				elog(DEBUG2, "FSM returned nonrecyclable page");
				_bt_relbuf(rel, buf);
			}
			else
			{
				elog(DEBUG2, "FSM returned nonlockable page");
				/* couldn't get lock, so just drop pin */
				ReleaseBuffer(buf);
			}
		}

		/*
		 * Extend the relation by one page.
		 *
		 * We have to use a lock to ensure no one else is extending the rel
		 * at the same time, else we will both try to initialize the same
		 * new page.  We can skip locking for new or temp relations,
		 * however, since no one else could be accessing them.
		 */
		needLock = !(rel->rd_isnew || rel->rd_istemp);

		if (needLock)
			LockPage(rel, 0, ExclusiveLock);

		buf = ReadBuffer(rel, P_NEW);

		/* Acquire buffer lock on new page */
		LockBuffer(buf, BT_WRITE);

		/*
		 * Release the file-extension lock; it's now OK for someone else to
		 * extend the relation some more.  Note that we cannot release this
		 * lock before we have buffer lock on the new page, or we risk a
		 * race condition against btvacuumcleanup --- see comments therein.
		 */
		if (needLock)
			UnlockPage(rel, 0, ExclusiveLock);

		/* Initialize the new page before returning it */
		page = BufferGetPage(buf);
		Assert(PageIsNew((PageHeader) page));
		_bt_pageinit(page, BufferGetPageSize(buf));
	}

	/* ref count and lock type are correct */
	return buf;
}

/*
 *	_bt_relbuf() -- release a locked buffer.
 *
 * Lock and pin (refcount) are both dropped.  Note that either read or
 * write lock can be dropped this way, but if we modified the buffer,
 * this is NOT the right way to release a write lock.
 */
void
_bt_relbuf(Relation rel, Buffer buf)
{
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(buf);
}

/*
 *	_bt_wrtbuf() -- write a btree page to disk.
 *
 *		This routine releases the lock held on the buffer and our refcount
 *		for it.  It is an error to call _bt_wrtbuf() without a write lock
 *		and a pin on the buffer.
 *
 * NOTE: actually, the buffer manager just marks the shared buffer page
 * dirty here; the real I/O happens later.	This is okay since we are not
 * relying on write ordering anyway.  The WAL mechanism is responsible for
 * guaranteeing correctness after a crash.
 */
void
_bt_wrtbuf(Relation rel, Buffer buf)
{
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	WriteBuffer(buf);
}

/*
 *	_bt_wrtnorelbuf() -- write a btree page to disk, but do not release
 *						 our reference or lock.
 *
 *		It is an error to call _bt_wrtnorelbuf() without a write lock
 *		and a pin on the buffer.
 *
 * See above NOTE.
 */
void
_bt_wrtnorelbuf(Relation rel, Buffer buf)
{
	WriteNoReleaseBuffer(buf);
}

/*
 *	_bt_pageinit() -- Initialize a new page.
 *
 * On return, the page header is initialized; data space is empty;
 * special space is zeroed out.
 */
void
_bt_pageinit(Page page, Size size)
{
	PageInit(page, size, sizeof(BTPageOpaqueData));
}

/*
 *	_bt_page_recyclable() -- Is an existing page recyclable?
 *
 * This exists to make sure _bt_getbuf and btvacuumcleanup have the same
 * policy about whether a page is safe to re-use.
 */
bool
_bt_page_recyclable(Page page)
{
	BTPageOpaque opaque;

	/*
	 * It's possible to find an all-zeroes page in an index --- for
	 * example, a backend might successfully extend the relation one page
	 * and then crash before it is able to make a WAL entry for adding the
	 * page. If we find a zeroed page then reclaim it.
	 */
	if (PageIsNew(page))
		return true;

	/*
	 * Otherwise, recycle if deleted and too old to have any processes
	 * interested in it.
	 */
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	if (P_ISDELETED(opaque) &&
		TransactionIdPrecedesOrEquals(opaque->btpo.xact, RecentXmin))
		return true;
	return false;
}

/*
 *	_bt_metaproot() -- Change the root page of the btree.
 *
 *		Lehman and Yao require that the root page move around in order to
 *		guarantee deadlock-free short-term, fine-granularity locking.  When
 *		we split the root page, we record the new parent in the metadata page
 *		for the relation.  This routine does the work.
 *
 *		No direct preconditions, but if you don't have the write lock on
 *		at least the old root page when you call this, you're making a big
 *		mistake.  On exit, metapage data is correct and we no longer have
 *		a pin or lock on the metapage.
 *
 * Actually this is not used for splitting on-the-fly anymore.	It's only used
 * in nbtsort.c at the completion of btree building, where we know we have
 * sole access to the index anyway.
 */
void
_bt_metaproot(Relation rel, BlockNumber rootbknum, uint32 level)
{
	Buffer		metabuf;
	Page		metap;
	BTPageOpaque metaopaque;
	BTMetaPageData *metad;

	metabuf = _bt_getbuf(rel, BTREE_METAPAGE, BT_WRITE);
	metap = BufferGetPage(metabuf);
	metaopaque = (BTPageOpaque) PageGetSpecialPointer(metap);
	Assert(metaopaque->btpo_flags & BTP_META);

	/* NO ELOG(ERROR) from here till newmeta op is logged */
	START_CRIT_SECTION();

	metad = BTPageGetMeta(metap);
	Assert(metad->btm_magic == BTREE_MAGIC || metad->btm_magic == 0);
	metad->btm_magic = BTREE_MAGIC;		/* it's valid now for sure */
	metad->btm_root = rootbknum;
	metad->btm_level = level;
	metad->btm_fastroot = rootbknum;
	metad->btm_fastlevel = level;

	/* XLOG stuff */
	if (!rel->rd_istemp)
	{
		xl_btree_newmeta xlrec;
		XLogRecPtr	recptr;
		XLogRecData rdata[1];

		xlrec.node = rel->rd_node;
		xlrec.meta.root = metad->btm_root;
		xlrec.meta.level = metad->btm_level;
		xlrec.meta.fastroot = metad->btm_fastroot;
		xlrec.meta.fastlevel = metad->btm_fastlevel;

		rdata[0].buffer = InvalidBuffer;
		rdata[0].data = (char *) &xlrec;
		rdata[0].len = SizeOfBtreeNewmeta;
		rdata[0].next = NULL;

		recptr = XLogInsert(RM_BTREE_ID, XLOG_BTREE_NEWMETA, rdata);

		PageSetLSN(metap, recptr);
		PageSetSUI(metap, ThisStartUpID);
	}

	END_CRIT_SECTION();

	_bt_wrtbuf(rel, metabuf);
}

/*
 * Delete item(s) from a btree page.
 *
 * This must only be used for deleting leaf items.	Deleting an item on a
 * non-leaf page has to be done as part of an atomic action that includes
 * deleting the page it points to.
 *
 * This routine assumes that the caller has pinned and locked the buffer,
 * and will write the buffer afterwards.  Also, the given itemnos *must*
 * appear in increasing order in the array.
 */
void
_bt_delitems(Relation rel, Buffer buf,
			 OffsetNumber *itemnos, int nitems)
{
	Page		page = BufferGetPage(buf);
	int			i;

	/* No ereport(ERROR) until changes are logged */
	START_CRIT_SECTION();

	/*
	 * Delete the items in reverse order so we don't have to think about
	 * adjusting item numbers for previous deletions.
	 */
	for (i = nitems - 1; i >= 0; i--)
		PageIndexTupleDelete(page, itemnos[i]);

	/* XLOG stuff */
	if (!rel->rd_istemp)
	{
		xl_btree_delete xlrec;
		XLogRecPtr	recptr;
		XLogRecData rdata[2];

		xlrec.node = rel->rd_node;
		xlrec.block = BufferGetBlockNumber(buf);

		rdata[0].buffer = InvalidBuffer;
		rdata[0].data = (char *) &xlrec;
		rdata[0].len = SizeOfBtreeDelete;
		rdata[0].next = &(rdata[1]);

		/*
		 * The target-offsets array is not in the buffer, but pretend that
		 * it is.  When XLogInsert stores the whole buffer, the offsets
		 * array need not be stored too.
		 */
		rdata[1].buffer = buf;
		if (nitems > 0)
		{
			rdata[1].data = (char *) itemnos;
			rdata[1].len = nitems * sizeof(OffsetNumber);
		}
		else
		{
			rdata[1].data = NULL;
			rdata[1].len = 0;
		}
		rdata[1].next = NULL;

		recptr = XLogInsert(RM_BTREE_ID, XLOG_BTREE_DELETE, rdata);

		PageSetLSN(page, recptr);
		PageSetSUI(page, ThisStartUpID);
	}

	END_CRIT_SECTION();
}

/*
 * _bt_pagedel() -- Delete a page from the b-tree.
 *
 * This action unlinks the page from the b-tree structure, removing all
 * pointers leading to it --- but not touching its own left and right links.
 * The page cannot be physically reclaimed right away, since other processes
 * may currently be trying to follow links leading to the page; they have to
 * be allowed to use its right-link to recover.  See nbtree/README.
 *
 * On entry, the target buffer must be pinned and read-locked.	This lock and
 * pin will be dropped before exiting.
 *
 * Returns the number of pages successfully deleted (zero on failure; could
 * be more than one if parent blocks were deleted).
 *
 * NOTE: this leaks memory.  Rather than trying to clean up everything
 * carefully, it's better to run it in a temp context that can be reset
 * frequently.
 */
int
_bt_pagedel(Relation rel, Buffer buf, bool vacuum_full)
{
	BlockNumber target,
				leftsib,
				rightsib,
				parent;
	OffsetNumber poffset,
				maxoff;
	uint32		targetlevel,
				ilevel;
	ItemId		itemid;
	BTItem		targetkey,
				btitem;
	ScanKey		itup_scankey;
	BTStack		stack;
	Buffer		lbuf,
				rbuf,
				pbuf;
	bool		parent_half_dead;
	bool		parent_one_child;
	bool		rightsib_empty;
	Buffer		metabuf = InvalidBuffer;
	Page		metapg = NULL;
	BTMetaPageData *metad = NULL;
	Page		page;
	BTPageOpaque opaque;

	/*
	 * We can never delete rightmost pages nor root pages.	While at it,
	 * check that page is not already deleted and is empty.
	 */
	page = BufferGetPage(buf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	if (P_RIGHTMOST(opaque) || P_ISROOT(opaque) || P_ISDELETED(opaque) ||
		P_FIRSTDATAKEY(opaque) <= PageGetMaxOffsetNumber(page))
	{
		_bt_relbuf(rel, buf);
		return 0;
	}

	/*
	 * Save info about page, including a copy of its high key (it must
	 * have one, being non-rightmost).
	 */
	target = BufferGetBlockNumber(buf);
	targetlevel = opaque->btpo.level;
	leftsib = opaque->btpo_prev;
	itemid = PageGetItemId(page, P_HIKEY);
	targetkey = CopyBTItem((BTItem) PageGetItem(page, itemid));

	/*
	 * We need to get an approximate pointer to the page's parent page.
	 * Use the standard search mechanism to search for the page's high
	 * key; this will give us a link to either the current parent or
	 * someplace to its left (if there are multiple equal high keys).  To
	 * avoid deadlocks, we'd better drop the target page lock first.
	 */
	_bt_relbuf(rel, buf);
	/* we need a scan key to do our search, so build one */
	itup_scankey = _bt_mkscankey(rel, &(targetkey->bti_itup));
	/* find the leftmost leaf page containing this key */
	stack = _bt_search(rel, rel->rd_rel->relnatts, itup_scankey,
					   &lbuf, BT_READ);
	/* don't need a pin on that either */
	_bt_relbuf(rel, lbuf);

	/*
	 * If we are trying to delete an interior page, _bt_search did more
	 * than we needed.	Locate the stack item pointing to our parent
	 * level.
	 */
	ilevel = 0;
	for (;;)
	{
		if (stack == NULL)
			elog(ERROR, "not enough stack items");
		if (ilevel == targetlevel)
			break;
		stack = stack->bts_parent;
		ilevel++;
	}

	/*
	 * We have to lock the pages we need to modify in the standard order:
	 * moving right, then up.  Else we will deadlock against other
	 * writers.
	 *
	 * So, we need to find and write-lock the current left sibling of the
	 * target page.  The sibling that was current a moment ago could have
	 * split, so we may have to move right.  This search could fail if
	 * either the sibling or the target page was deleted by someone else
	 * meanwhile; if so, give up.  (Right now, that should never happen,
	 * since page deletion is only done in VACUUM and there shouldn't be
	 * multiple VACUUMs concurrently on the same table.)
	 */
	if (leftsib != P_NONE)
	{
		lbuf = _bt_getbuf(rel, leftsib, BT_WRITE);
		page = BufferGetPage(lbuf);
		opaque = (BTPageOpaque) PageGetSpecialPointer(page);
		while (P_ISDELETED(opaque) || opaque->btpo_next != target)
		{
			/* step right one page */
			leftsib = opaque->btpo_next;
			_bt_relbuf(rel, lbuf);
			if (leftsib == P_NONE)
			{
				elog(LOG, "no left sibling (concurrent deletion?)");
				return 0;
			}
			lbuf = _bt_getbuf(rel, leftsib, BT_WRITE);
			page = BufferGetPage(lbuf);
			opaque = (BTPageOpaque) PageGetSpecialPointer(page);
		}
	}
	else
		lbuf = InvalidBuffer;

	/*
	 * Next write-lock the target page itself.	It should be okay to take
	 * just a write lock not a superexclusive lock, since no scans would
	 * stop on an empty page.
	 */
	buf = _bt_getbuf(rel, target, BT_WRITE);
	page = BufferGetPage(buf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);

	/*
	 * Check page is still empty etc, else abandon deletion.  The empty
	 * check is necessary since someone else might have inserted into it
	 * while we didn't have it locked; the others are just for paranoia's
	 * sake.
	 */
	if (P_RIGHTMOST(opaque) || P_ISROOT(opaque) || P_ISDELETED(opaque) ||
		P_FIRSTDATAKEY(opaque) <= PageGetMaxOffsetNumber(page))
	{
		_bt_relbuf(rel, buf);
		if (BufferIsValid(lbuf))
			_bt_relbuf(rel, lbuf);
		return 0;
	}
	if (opaque->btpo_prev != leftsib)
		elog(ERROR, "left link changed unexpectedly");

	/*
	 * And next write-lock the (current) right sibling.
	 */
	rightsib = opaque->btpo_next;
	rbuf = _bt_getbuf(rel, rightsib, BT_WRITE);

	/*
	 * Next find and write-lock the current parent of the target page.
	 * This is essentially the same as the corresponding step of
	 * splitting.
	 */
	ItemPointerSet(&(stack->bts_btitem.bti_itup.t_tid),
				   target, P_HIKEY);
	pbuf = _bt_getstackbuf(rel, stack, BT_WRITE);
	if (pbuf == InvalidBuffer)
		elog(ERROR, "failed to re-find parent key in \"%s\"",
			 RelationGetRelationName(rel));
	parent = stack->bts_blkno;
	poffset = stack->bts_offset;

	/*
	 * If the target is the rightmost child of its parent, then we can't
	 * delete, unless it's also the only child --- in which case the
	 * parent changes to half-dead status.
	 */
	page = BufferGetPage(pbuf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	maxoff = PageGetMaxOffsetNumber(page);
	parent_half_dead = false;
	parent_one_child = false;
	if (poffset >= maxoff)
	{
		if (poffset == P_FIRSTDATAKEY(opaque))
			parent_half_dead = true;
		else
		{
			_bt_relbuf(rel, pbuf);
			_bt_relbuf(rel, rbuf);
			_bt_relbuf(rel, buf);
			if (BufferIsValid(lbuf))
				_bt_relbuf(rel, lbuf);
			return 0;
		}
	}
	else
	{
		/* Will there be exactly one child left in this parent? */
		if (OffsetNumberNext(P_FIRSTDATAKEY(opaque)) == maxoff)
			parent_one_child = true;
	}

	/*
	 * If we are deleting the next-to-last page on the target's level,
	 * then the rightsib is a candidate to become the new fast root. (In
	 * theory, it might be possible to push the fast root even further
	 * down, but the odds of doing so are slim, and the locking
	 * considerations daunting.)
	 *
	 * We can safely acquire a lock on the metapage here --- see comments for
	 * _bt_newroot().
	 */
	if (leftsib == P_NONE)
	{
		page = BufferGetPage(rbuf);
		opaque = (BTPageOpaque) PageGetSpecialPointer(page);
		Assert(opaque->btpo.level == targetlevel);
		if (P_RIGHTMOST(opaque))
		{
			/* rightsib will be the only one left on the level */
			metabuf = _bt_getbuf(rel, BTREE_METAPAGE, BT_WRITE);
			metapg = BufferGetPage(metabuf);
			metad = BTPageGetMeta(metapg);

			/*
			 * The expected case here is btm_fastlevel == targetlevel+1;
			 * if the fastlevel is <= targetlevel, something is wrong, and
			 * we choose to overwrite it to fix it.
			 */
			if (metad->btm_fastlevel > targetlevel + 1)
			{
				/* no update wanted */
				_bt_relbuf(rel, metabuf);
				metabuf = InvalidBuffer;
			}
		}
	}

	/*
	 * Here we begin doing the deletion.
	 */

	/* No ereport(ERROR) until changes are logged */
	START_CRIT_SECTION();

	/*
	 * Update parent.  The normal case is a tad tricky because we want to
	 * delete the target's downlink and the *following* key.  Easiest way
	 * is to copy the right sibling's downlink over the target downlink,
	 * and then delete the following item.
	 */
	page = BufferGetPage(pbuf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	if (parent_half_dead)
	{
		PageIndexTupleDelete(page, poffset);
		opaque->btpo_flags |= BTP_HALF_DEAD;
	}
	else
	{
		OffsetNumber nextoffset;

		itemid = PageGetItemId(page, poffset);
		btitem = (BTItem) PageGetItem(page, itemid);
		Assert(ItemPointerGetBlockNumber(&(btitem->bti_itup.t_tid)) == target);
		ItemPointerSet(&(btitem->bti_itup.t_tid), rightsib, P_HIKEY);

		nextoffset = OffsetNumberNext(poffset);
		/* This part is just for double-checking */
		itemid = PageGetItemId(page, nextoffset);
		btitem = (BTItem) PageGetItem(page, itemid);
		if (ItemPointerGetBlockNumber(&(btitem->bti_itup.t_tid)) != rightsib)
			elog(PANIC, "right sibling is not next child");

		PageIndexTupleDelete(page, nextoffset);
	}

	/*
	 * Update siblings' side-links.  Note the target page's side-links
	 * will continue to point to the siblings.
	 */
	if (BufferIsValid(lbuf))
	{
		page = BufferGetPage(lbuf);
		opaque = (BTPageOpaque) PageGetSpecialPointer(page);
		Assert(opaque->btpo_next == target);
		opaque->btpo_next = rightsib;
	}
	page = BufferGetPage(rbuf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	Assert(opaque->btpo_prev == target);
	opaque->btpo_prev = leftsib;
	rightsib_empty = (P_FIRSTDATAKEY(opaque) > PageGetMaxOffsetNumber(page));

	/*
	 * Mark the page itself deleted.  It can be recycled when all current
	 * transactions are gone; or immediately if we're doing VACUUM FULL.
	 */
	page = BufferGetPage(buf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	opaque->btpo_flags |= BTP_DELETED;
	opaque->btpo.xact =
		vacuum_full ? FrozenTransactionId : ReadNewTransactionId();

	/* And update the metapage, if needed */
	if (BufferIsValid(metabuf))
	{
		metad->btm_fastroot = rightsib;
		metad->btm_fastlevel = targetlevel;
	}

	/* XLOG stuff */
	if (!rel->rd_istemp)
	{
		xl_btree_delete_page xlrec;
		xl_btree_metadata xlmeta;
		uint8		xlinfo;
		XLogRecPtr	recptr;
		XLogRecData rdata[5];
		XLogRecData *nextrdata;

		xlrec.target.node = rel->rd_node;
		ItemPointerSet(&(xlrec.target.tid), parent, poffset);
		xlrec.deadblk = target;
		xlrec.leftblk = leftsib;
		xlrec.rightblk = rightsib;

		rdata[0].buffer = InvalidBuffer;
		rdata[0].data = (char *) &xlrec;
		rdata[0].len = SizeOfBtreeDeletePage;
		rdata[0].next = nextrdata = &(rdata[1]);

		if (BufferIsValid(metabuf))
		{
			xlmeta.root = metad->btm_root;
			xlmeta.level = metad->btm_level;
			xlmeta.fastroot = metad->btm_fastroot;
			xlmeta.fastlevel = metad->btm_fastlevel;

			nextrdata->buffer = InvalidBuffer;
			nextrdata->data = (char *) &xlmeta;
			nextrdata->len = sizeof(xl_btree_metadata);
			nextrdata->next = nextrdata + 1;
			nextrdata++;
			xlinfo = XLOG_BTREE_DELETE_PAGE_META;
		}
		else
			xlinfo = XLOG_BTREE_DELETE_PAGE;

		nextrdata->buffer = pbuf;
		nextrdata->data = NULL;
		nextrdata->len = 0;
		nextrdata->next = nextrdata + 1;
		nextrdata++;

		nextrdata->buffer = rbuf;
		nextrdata->data = NULL;
		nextrdata->len = 0;
		nextrdata->next = NULL;

		if (BufferIsValid(lbuf))
		{
			nextrdata->next = nextrdata + 1;
			nextrdata++;
			nextrdata->buffer = lbuf;
			nextrdata->data = NULL;
			nextrdata->len = 0;
			nextrdata->next = NULL;
		}

		recptr = XLogInsert(RM_BTREE_ID, xlinfo, rdata);

		if (BufferIsValid(metabuf))
		{
			PageSetLSN(metapg, recptr);
			PageSetSUI(metapg, ThisStartUpID);
		}
		page = BufferGetPage(pbuf);
		PageSetLSN(page, recptr);
		PageSetSUI(page, ThisStartUpID);
		page = BufferGetPage(rbuf);
		PageSetLSN(page, recptr);
		PageSetSUI(page, ThisStartUpID);
		page = BufferGetPage(buf);
		PageSetLSN(page, recptr);
		PageSetSUI(page, ThisStartUpID);
		if (BufferIsValid(lbuf))
		{
			page = BufferGetPage(lbuf);
			PageSetLSN(page, recptr);
			PageSetSUI(page, ThisStartUpID);
		}
	}

	END_CRIT_SECTION();

	/* Write and release buffers */
	if (BufferIsValid(metabuf))
		_bt_wrtbuf(rel, metabuf);
	_bt_wrtbuf(rel, pbuf);
	_bt_wrtbuf(rel, rbuf);
	_bt_wrtbuf(rel, buf);
	if (BufferIsValid(lbuf))
		_bt_wrtbuf(rel, lbuf);

	/*
	 * If parent became half dead, recurse to try to delete it. Otherwise,
	 * if right sibling is empty and is now the last child of the parent,
	 * recurse to try to delete it.  (These cases cannot apply at the same
	 * time, though the second case might itself recurse to the first.)
	 */
	if (parent_half_dead)
	{
		buf = _bt_getbuf(rel, parent, BT_READ);
		return _bt_pagedel(rel, buf, vacuum_full) + 1;
	}
	if (parent_one_child && rightsib_empty)
	{
		buf = _bt_getbuf(rel, rightsib, BT_READ);
		return _bt_pagedel(rel, buf, vacuum_full) + 1;
	}

	return 1;
}
