/*-------------------------------------------------------------------------
 *
 * nbtpage.c
 *	  BTree-specific page management code for the Postgres btree access
 *	  method.
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/nbtree/nbtpage.c,v 1.106 2008/01/01 19:45:46 momjian Exp $
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
#include "access/transam.h"
#include "miscadmin.h"
#include "storage/freespace.h"
#include "storage/lmgr.h"
#include "utils/inval.h"


/*
 *	_bt_initmetapage() -- Fill a page buffer with a correct metapage image
 */
void
_bt_initmetapage(Page page, BlockNumber rootbknum, uint32 level)
{
	BTMetaPageData *metad;
	BTPageOpaque metaopaque;

	_bt_pageinit(page, BLCKSZ);

	metad = BTPageGetMeta(page);
	metad->btm_magic = BTREE_MAGIC;
	metad->btm_version = BTREE_VERSION;
	metad->btm_root = rootbknum;
	metad->btm_level = level;
	metad->btm_fastroot = rootbknum;
	metad->btm_fastlevel = level;

	metaopaque = (BTPageOpaque) PageGetSpecialPointer(page);
	metaopaque->btpo_flags = BTP_META;

	/*
	 * Set pd_lower just past the end of the metadata.	This is not essential
	 * but it makes the page look compressible to xlog.c.
	 */
	((PageHeader) page)->pd_lower =
		((char *) metad + sizeof(BTMetaPageData)) - (char *) page;
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

	/*
	 * Try to use previously-cached metapage data to find the root.  This
	 * normally saves one buffer access per index search, which is a very
	 * helpful savings in bufmgr traffic and hence contention.
	 */
	if (rel->rd_amcache != NULL)
	{
		metad = (BTMetaPageData *) rel->rd_amcache;
		/* We shouldn't have cached it if any of these fail */
		Assert(metad->btm_magic == BTREE_MAGIC);
		Assert(metad->btm_version == BTREE_VERSION);
		Assert(metad->btm_root != P_NONE);

		rootblkno = metad->btm_fastroot;
		Assert(rootblkno != P_NONE);
		rootlevel = metad->btm_fastlevel;

		rootbuf = _bt_getbuf(rel, rootblkno, BT_READ);
		rootpage = BufferGetPage(rootbuf);
		rootopaque = (BTPageOpaque) PageGetSpecialPointer(rootpage);

		/*
		 * Since the cache might be stale, we check the page more carefully
		 * here than normal.  We *must* check that it's not deleted. If it's
		 * not alone on its level, then we reject too --- this may be overly
		 * paranoid but better safe than sorry.  Note we don't check P_ISROOT,
		 * because that's not set in a "fast root".
		 */
		if (!P_IGNORE(rootopaque) &&
			rootopaque->btpo.level == rootlevel &&
			P_LEFTMOST(rootopaque) &&
			P_RIGHTMOST(rootopaque))
		{
			/* OK, accept cached page as the root */
			return rootbuf;
		}
		_bt_relbuf(rel, rootbuf);
		/* Cache is stale, throw it away */
		if (rel->rd_amcache)
			pfree(rel->rd_amcache);
		rel->rd_amcache = NULL;
	}

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
		 * Race condition:	if someone else initialized the metadata between
		 * the time we released the read lock and acquired the write lock, we
		 * must avoid doing it again.
		 */
		if (metad->btm_root != P_NONE)
		{
			/*
			 * Metadata initialized by someone else.  In order to guarantee no
			 * deadlocks, we have to release the metadata page and start all
			 * over again.	(Is that really true? But it's hardly worth trying
			 * to optimize this case.)
			 */
			_bt_relbuf(rel, metabuf);
			return _bt_getroot(rel, access);
		}

		/*
		 * Get, initialize, write, and leave a lock of the appropriate type on
		 * the new root page.  Since this is the first page in the tree, it's
		 * a leaf as well as the root.
		 */
		rootbuf = _bt_getbuf(rel, P_NEW, BT_WRITE);
		rootblkno = BufferGetBlockNumber(rootbuf);
		rootpage = BufferGetPage(rootbuf);
		rootopaque = (BTPageOpaque) PageGetSpecialPointer(rootpage);
		rootopaque->btpo_prev = rootopaque->btpo_next = P_NONE;
		rootopaque->btpo_flags = (BTP_LEAF | BTP_ROOT);
		rootopaque->btpo.level = 0;
		rootopaque->btpo_cycleid = 0;

		/* NO ELOG(ERROR) till meta is updated */
		START_CRIT_SECTION();

		metad->btm_root = rootblkno;
		metad->btm_level = 0;
		metad->btm_fastroot = rootblkno;
		metad->btm_fastlevel = 0;

		MarkBufferDirty(rootbuf);
		MarkBufferDirty(metabuf);

		/* XLOG stuff */
		if (!rel->rd_istemp)
		{
			xl_btree_newroot xlrec;
			XLogRecPtr	recptr;
			XLogRecData rdata;

			xlrec.node = rel->rd_node;
			xlrec.rootblk = rootblkno;
			xlrec.level = 0;

			rdata.data = (char *) &xlrec;
			rdata.len = SizeOfBtreeNewroot;
			rdata.buffer = InvalidBuffer;
			rdata.next = NULL;

			recptr = XLogInsert(RM_BTREE_ID, XLOG_BTREE_NEWROOT, &rdata);

			PageSetLSN(rootpage, recptr);
			PageSetTLI(rootpage, ThisTimeLineID);
			PageSetLSN(metapg, recptr);
			PageSetTLI(metapg, ThisTimeLineID);
		}

		END_CRIT_SECTION();

		/*
		 * Send out relcache inval for metapage change (probably unnecessary
		 * here, but let's be safe).
		 */
		CacheInvalidateRelcache(rel);

		/*
		 * swap root write lock for read lock.	There is no danger of anyone
		 * else accessing the new root page while it's unlocked, since no one
		 * else knows where it is yet.
		 */
		LockBuffer(rootbuf, BUFFER_LOCK_UNLOCK);
		LockBuffer(rootbuf, BT_READ);

		/* okay, metadata is correct, release lock on it */
		_bt_relbuf(rel, metabuf);
	}
	else
	{
		rootblkno = metad->btm_fastroot;
		Assert(rootblkno != P_NONE);
		rootlevel = metad->btm_fastlevel;

		/*
		 * Cache the metapage data for next time
		 */
		rel->rd_amcache = MemoryContextAlloc(rel->rd_indexcxt,
											 sizeof(BTMetaPageData));
		memcpy(rel->rd_amcache, metad, sizeof(BTMetaPageData));

		/*
		 * We are done with the metapage; arrange to release it via first
		 * _bt_relandgetbuf call
		 */
		rootbuf = metabuf;

		for (;;)
		{
			rootbuf = _bt_relandgetbuf(rel, rootbuf, rootblkno, BT_READ);
			rootpage = BufferGetPage(rootbuf);
			rootopaque = (BTPageOpaque) PageGetSpecialPointer(rootpage);

			if (!P_IGNORE(rootopaque))
				break;

			/* it's dead, Jim.  step right one page */
			if (P_RIGHTMOST(rootopaque))
				elog(ERROR, "no live root page found in index \"%s\"",
					 RelationGetRelationName(rel));
			rootblkno = rootopaque->btpo_next;
		}

		/* Note: can't check btpo.level on deleted pages */
		if (rootopaque->btpo.level != rootlevel)
			elog(ERROR, "root page %u of index \"%s\" has level %u, expected %u",
				 rootblkno, RelationGetRelationName(rel),
				 rootopaque->btpo.level, rootlevel);
	}

	/*
	 * By here, we have a pin and read lock on the root page, and no lock set
	 * on the metadata page.  Return the root page's buffer.
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

	/*
	 * We don't try to use cached metapage data here, since (a) this path is
	 * not performance-critical, and (b) if we are here it suggests our cache
	 * is out-of-date anyway.  In light of point (b), it's probably safest to
	 * actively flush any cached metapage info.
	 */
	if (rel->rd_amcache)
		pfree(rel->rd_amcache);
	rel->rd_amcache = NULL;

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

	/*
	 * We are done with the metapage; arrange to release it via first
	 * _bt_relandgetbuf call
	 */
	rootbuf = metabuf;

	for (;;)
	{
		rootbuf = _bt_relandgetbuf(rel, rootbuf, rootblkno, BT_READ);
		rootpage = BufferGetPage(rootbuf);
		rootopaque = (BTPageOpaque) PageGetSpecialPointer(rootpage);

		if (!P_IGNORE(rootopaque))
			break;

		/* it's dead, Jim.  step right one page */
		if (P_RIGHTMOST(rootopaque))
			elog(ERROR, "no live root page found in index \"%s\"",
				 RelationGetRelationName(rel));
		rootblkno = rootopaque->btpo_next;
	}

	/* Note: can't check btpo.level on deleted pages */
	if (rootopaque->btpo.level != rootlevel)
		elog(ERROR, "root page %u of index \"%s\" has level %u, expected %u",
			 rootblkno, RelationGetRelationName(rel),
			 rootopaque->btpo.level, rootlevel);

	return rootbuf;
}

/*
 *	_bt_checkpage() -- Verify that a freshly-read page looks sane.
 */
void
_bt_checkpage(Relation rel, Buffer buf)
{
	Page		page = BufferGetPage(buf);

	/*
	 * ReadBuffer verifies that every newly-read page passes
	 * PageHeaderIsValid, which means it either contains a reasonably sane
	 * page header or is all-zero.	We have to defend against the all-zero
	 * case, however.
	 */
	if (PageIsNew(page))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
			 errmsg("index \"%s\" contains unexpected zero page at block %u",
					RelationGetRelationName(rel),
					BufferGetBlockNumber(buf)),
				 errhint("Please REINDEX it.")));

	/*
	 * Additionally check that the special area looks sane.
	 */
	if (((PageHeader) (page))->pd_special !=
		(BLCKSZ - MAXALIGN(sizeof(BTPageOpaqueData))))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" contains corrupted page at block %u",
						RelationGetRelationName(rel),
						BufferGetBlockNumber(buf)),
				 errhint("Please REINDEX it.")));
}

/*
 *	_bt_getbuf() -- Get a buffer by block number for read or write.
 *
 *		blkno == P_NEW means to get an unallocated index page.	The page
 *		will be initialized before returning it.
 *
 *		When this routine returns, the appropriate lock is set on the
 *		requested buffer and its reference count has been incremented
 *		(ie, the buffer is "locked and pinned").  Also, we apply
 *		_bt_checkpage to sanity-check the page (except in P_NEW case).
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
		_bt_checkpage(rel, buf);
	}
	else
	{
		bool		needLock;
		Page		page;

		Assert(access == BT_WRITE);

		/*
		 * First see if the FSM knows of any free pages.
		 *
		 * We can't trust the FSM's report unreservedly; we have to check that
		 * the page is still free.	(For example, an already-free page could
		 * have been re-used between the time the last VACUUM scanned it and
		 * the time the VACUUM made its FSM updates.)
		 *
		 * In fact, it's worse than that: we can't even assume that it's safe
		 * to take a lock on the reported page.  If somebody else has a lock
		 * on it, or even worse our own caller does, we could deadlock.  (The
		 * own-caller scenario is actually not improbable. Consider an index
		 * on a serial or timestamp column.  Nearly all splits will be at the
		 * rightmost page, so it's entirely likely that _bt_split will call us
		 * while holding a lock on the page most recently acquired from FSM. A
		 * VACUUM running concurrently with the previous split could well have
		 * placed that page back in FSM.)
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
		 * We have to use a lock to ensure no one else is extending the rel at
		 * the same time, else we will both try to initialize the same new
		 * page.  We can skip locking for new or temp relations, however,
		 * since no one else could be accessing them.
		 */
		needLock = !RELATION_IS_LOCAL(rel);

		if (needLock)
			LockRelationForExtension(rel, ExclusiveLock);

		buf = ReadBuffer(rel, P_NEW);

		/* Acquire buffer lock on new page */
		LockBuffer(buf, BT_WRITE);

		/*
		 * Release the file-extension lock; it's now OK for someone else to
		 * extend the relation some more.  Note that we cannot release this
		 * lock before we have buffer lock on the new page, or we risk a race
		 * condition against btvacuumscan --- see comments therein.
		 */
		if (needLock)
			UnlockRelationForExtension(rel, ExclusiveLock);

		/* Initialize the new page before returning it */
		page = BufferGetPage(buf);
		Assert(PageIsNew((PageHeader) page));
		_bt_pageinit(page, BufferGetPageSize(buf));
	}

	/* ref count and lock type are correct */
	return buf;
}

/*
 *	_bt_relandgetbuf() -- release a locked buffer and get another one.
 *
 * This is equivalent to _bt_relbuf followed by _bt_getbuf, with the
 * exception that blkno may not be P_NEW.  Also, if obuf is InvalidBuffer
 * then it reduces to just _bt_getbuf; allowing this case simplifies some
 * callers. The motivation for using this is to avoid two entries to the
 * bufmgr when one will do.
 */
Buffer
_bt_relandgetbuf(Relation rel, Buffer obuf, BlockNumber blkno, int access)
{
	Buffer		buf;

	Assert(blkno != P_NEW);
	if (BufferIsValid(obuf))
		LockBuffer(obuf, BUFFER_LOCK_UNLOCK);
	buf = ReleaseAndReadBuffer(obuf, rel, blkno);
	LockBuffer(buf, access);
	_bt_checkpage(rel, buf);
	return buf;
}

/*
 *	_bt_relbuf() -- release a locked buffer.
 *
 * Lock and pin (refcount) are both dropped.
 */
void
_bt_relbuf(Relation rel, Buffer buf)
{
	UnlockReleaseBuffer(buf);
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
 * This exists to make sure _bt_getbuf and btvacuumscan have the same
 * policy about whether a page is safe to re-use.
 */
bool
_bt_page_recyclable(Page page)
{
	BTPageOpaque opaque;

	/*
	 * It's possible to find an all-zeroes page in an index --- for example, a
	 * backend might successfully extend the relation one page and then crash
	 * before it is able to make a WAL entry for adding the page. If we find a
	 * zeroed page then reclaim it.
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
 * Delete item(s) from a btree page.
 *
 * This must only be used for deleting leaf items.	Deleting an item on a
 * non-leaf page has to be done as part of an atomic action that includes
 * deleting the page it points to.
 *
 * This routine assumes that the caller has pinned and locked the buffer.
 * Also, the given itemnos *must* appear in increasing order in the array.
 */
void
_bt_delitems(Relation rel, Buffer buf,
			 OffsetNumber *itemnos, int nitems)
{
	Page		page = BufferGetPage(buf);
	BTPageOpaque opaque;

	/* No ereport(ERROR) until changes are logged */
	START_CRIT_SECTION();

	/* Fix the page */
	PageIndexMultiDelete(page, itemnos, nitems);

	/*
	 * We can clear the vacuum cycle ID since this page has certainly been
	 * processed by the current vacuum scan.
	 */
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	opaque->btpo_cycleid = 0;

	/*
	 * Mark the page as not containing any LP_DEAD items.  This is not
	 * certainly true (there might be some that have recently been marked, but
	 * weren't included in our target-item list), but it will almost always be
	 * true and it doesn't seem worth an additional page scan to check it.
	 * Remember that BTP_HAS_GARBAGE is only a hint anyway.
	 */
	opaque->btpo_flags &= ~BTP_HAS_GARBAGE;

	MarkBufferDirty(buf);

	/* XLOG stuff */
	if (!rel->rd_istemp)
	{
		xl_btree_delete xlrec;
		XLogRecPtr	recptr;
		XLogRecData rdata[2];

		xlrec.node = rel->rd_node;
		xlrec.block = BufferGetBlockNumber(buf);

		rdata[0].data = (char *) &xlrec;
		rdata[0].len = SizeOfBtreeDelete;
		rdata[0].buffer = InvalidBuffer;
		rdata[0].next = &(rdata[1]);

		/*
		 * The target-offsets array is not in the buffer, but pretend that it
		 * is.	When XLogInsert stores the whole buffer, the offsets array
		 * need not be stored too.
		 */
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
		rdata[1].buffer = buf;
		rdata[1].buffer_std = true;
		rdata[1].next = NULL;

		recptr = XLogInsert(RM_BTREE_ID, XLOG_BTREE_DELETE, rdata);

		PageSetLSN(page, recptr);
		PageSetTLI(page, ThisTimeLineID);
	}

	END_CRIT_SECTION();
}

/*
 * Subroutine to pre-check whether a page deletion is safe, that is, its
 * parent page would be left in a valid or deletable state.
 *
 * "target" is the page we wish to delete, and "stack" is a search stack
 * leading to it (approximately).  Note that we will update the stack
 * entry(s) to reflect current downlink positions --- this is harmless and
 * indeed saves later search effort in _bt_pagedel.
 *
 * Note: it's OK to release page locks after checking, because a safe
 * deletion can't become unsafe due to concurrent activity.  A non-rightmost
 * page cannot become rightmost unless there's a concurrent page deletion,
 * but only VACUUM does page deletion and we only allow one VACUUM on an index
 * at a time.  An only child could acquire a sibling (of the same parent) only
 * by being split ... but that would make it a non-rightmost child so the
 * deletion is still safe.
 */
static bool
_bt_parent_deletion_safe(Relation rel, BlockNumber target, BTStack stack)
{
	BlockNumber parent;
	OffsetNumber poffset,
				maxoff;
	Buffer		pbuf;
	Page		page;
	BTPageOpaque opaque;

	/*
	 * In recovery mode, assume the deletion being replayed is valid.  We
	 * can't always check it because we won't have a full search stack, and we
	 * should complain if there's a problem, anyway.
	 */
	if (InRecovery)
		return true;

	/* Locate the parent's downlink (updating the stack entry if needed) */
	ItemPointerSet(&(stack->bts_btentry.t_tid), target, P_HIKEY);
	pbuf = _bt_getstackbuf(rel, stack, BT_READ);
	if (pbuf == InvalidBuffer)
		elog(ERROR, "failed to re-find parent key in index \"%s\" for deletion target page %u",
			 RelationGetRelationName(rel), target);
	parent = stack->bts_blkno;
	poffset = stack->bts_offset;

	page = BufferGetPage(pbuf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	maxoff = PageGetMaxOffsetNumber(page);

	/*
	 * If the target is the rightmost child of its parent, then we can't
	 * delete, unless it's also the only child.
	 */
	if (poffset >= maxoff)
	{
		/* It's rightmost child... */
		if (poffset == P_FIRSTDATAKEY(opaque))
		{
			/*
			 * It's only child, so safe if parent would itself be removable.
			 * We have to check the parent itself, and then recurse to test
			 * the conditions at the parent's parent.
			 */
			if (P_RIGHTMOST(opaque) || P_ISROOT(opaque))
			{
				_bt_relbuf(rel, pbuf);
				return false;
			}

			_bt_relbuf(rel, pbuf);
			return _bt_parent_deletion_safe(rel, parent, stack->bts_parent);
		}
		else
		{
			/* Unsafe to delete */
			_bt_relbuf(rel, pbuf);
			return false;
		}
	}
	else
	{
		/* Not rightmost child, so safe to delete */
		_bt_relbuf(rel, pbuf);
		return true;
	}
}

/*
 * _bt_pagedel() -- Delete a page from the b-tree, if legal to do so.
 *
 * This action unlinks the page from the b-tree structure, removing all
 * pointers leading to it --- but not touching its own left and right links.
 * The page cannot be physically reclaimed right away, since other processes
 * may currently be trying to follow links leading to the page; they have to
 * be allowed to use its right-link to recover.  See nbtree/README.
 *
 * On entry, the target buffer must be pinned and locked (either read or write
 * lock is OK).  This lock and pin will be dropped before exiting.
 *
 * The "stack" argument can be a search stack leading (approximately) to the
 * target page, or NULL --- outside callers typically pass NULL since they
 * have not done such a search, but internal recursion cases pass the stack
 * to avoid duplicated search effort.
 *
 * Returns the number of pages successfully deleted (zero if page cannot
 * be deleted now; could be more than one if parent pages were deleted too).
 *
 * NOTE: this leaks memory.  Rather than trying to clean up everything
 * carefully, it's better to run it in a temp context that can be reset
 * frequently.
 */
int
_bt_pagedel(Relation rel, Buffer buf, BTStack stack, bool vacuum_full)
{
	int			result;
	BlockNumber target,
				leftsib,
				rightsib,
				parent;
	OffsetNumber poffset,
				maxoff;
	uint32		targetlevel,
				ilevel;
	ItemId		itemid;
	IndexTuple	targetkey,
				itup;
	ScanKey		itup_scankey;
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
	 * We can never delete rightmost pages nor root pages.	While at it, check
	 * that page is not already deleted and is empty.
	 */
	page = BufferGetPage(buf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	if (P_RIGHTMOST(opaque) || P_ISROOT(opaque) || P_ISDELETED(opaque) ||
		P_FIRSTDATAKEY(opaque) <= PageGetMaxOffsetNumber(page))
	{
		/* Should never fail to delete a half-dead page */
		Assert(!P_ISHALFDEAD(opaque));

		_bt_relbuf(rel, buf);
		return 0;
	}

	/*
	 * Save info about page, including a copy of its high key (it must have
	 * one, being non-rightmost).
	 */
	target = BufferGetBlockNumber(buf);
	targetlevel = opaque->btpo.level;
	leftsib = opaque->btpo_prev;
	itemid = PageGetItemId(page, P_HIKEY);
	targetkey = CopyIndexTuple((IndexTuple) PageGetItem(page, itemid));

	/*
	 * To avoid deadlocks, we'd better drop the target page lock before going
	 * further.
	 */
	_bt_relbuf(rel, buf);

	/*
	 * We need an approximate pointer to the page's parent page.  We use the
	 * standard search mechanism to search for the page's high key; this will
	 * give us a link to either the current parent or someplace to its left
	 * (if there are multiple equal high keys).  In recursion cases, the
	 * caller already generated a search stack and we can just re-use that
	 * work.
	 */
	if (stack == NULL)
	{
		if (!InRecovery)
		{
			/* we need an insertion scan key to do our search, so build one */
			itup_scankey = _bt_mkscankey(rel, targetkey);
			/* find the leftmost leaf page containing this key */
			stack = _bt_search(rel, rel->rd_rel->relnatts, itup_scankey, false,
							   &lbuf, BT_READ);
			/* don't need a pin on that either */
			_bt_relbuf(rel, lbuf);

			/*
			 * If we are trying to delete an interior page, _bt_search did
			 * more than we needed.  Locate the stack item pointing to our
			 * parent level.
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
		}
		else
		{
			/*
			 * During WAL recovery, we can't use _bt_search (for one reason,
			 * it might invoke user-defined comparison functions that expect
			 * facilities not available in recovery mode).	Instead, just set
			 * up a dummy stack pointing to the left end of the parent tree
			 * level, from which _bt_getstackbuf will walk right to the parent
			 * page.  Painful, but we don't care too much about performance in
			 * this scenario.
			 */
			pbuf = _bt_get_endpoint(rel, targetlevel + 1, false);
			stack = (BTStack) palloc(sizeof(BTStackData));
			stack->bts_blkno = BufferGetBlockNumber(pbuf);
			stack->bts_offset = InvalidOffsetNumber;
			/* bts_btentry will be initialized below */
			stack->bts_parent = NULL;
			_bt_relbuf(rel, pbuf);
		}
	}

	/*
	 * We cannot delete a page that is the rightmost child of its immediate
	 * parent, unless it is the only child --- in which case the parent has to
	 * be deleted too, and the same condition applies recursively to it. We
	 * have to check this condition all the way up before trying to delete. We
	 * don't need to re-test when deleting a non-leaf page, though.
	 */
	if (targetlevel == 0 &&
		!_bt_parent_deletion_safe(rel, target, stack))
		return 0;

	/*
	 * We have to lock the pages we need to modify in the standard order:
	 * moving right, then up.  Else we will deadlock against other writers.
	 *
	 * So, we need to find and write-lock the current left sibling of the
	 * target page.  The sibling that was current a moment ago could have
	 * split, so we may have to move right.  This search could fail if either
	 * the sibling or the target page was deleted by someone else meanwhile;
	 * if so, give up.	(Right now, that should never happen, since page
	 * deletion is only done in VACUUM and there shouldn't be multiple VACUUMs
	 * concurrently on the same table.)
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
				elog(LOG, "no left sibling (concurrent deletion?) in \"%s\"",
					 RelationGetRelationName(rel));
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
	 * Next write-lock the target page itself.	It should be okay to take just
	 * a write lock not a superexclusive lock, since no scans would stop on an
	 * empty page.
	 */
	buf = _bt_getbuf(rel, target, BT_WRITE);
	page = BufferGetPage(buf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);

	/*
	 * Check page is still empty etc, else abandon deletion.  The empty check
	 * is necessary since someone else might have inserted into it while we
	 * didn't have it locked; the others are just for paranoia's sake.
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
		elog(ERROR, "left link changed unexpectedly in block %u of index \"%s\"",
			 target, RelationGetRelationName(rel));

	/*
	 * And next write-lock the (current) right sibling.
	 */
	rightsib = opaque->btpo_next;
	rbuf = _bt_getbuf(rel, rightsib, BT_WRITE);

	/*
	 * Next find and write-lock the current parent of the target page. This is
	 * essentially the same as the corresponding step of splitting.
	 */
	ItemPointerSet(&(stack->bts_btentry.t_tid), target, P_HIKEY);
	pbuf = _bt_getstackbuf(rel, stack, BT_WRITE);
	if (pbuf == InvalidBuffer)
		elog(ERROR, "failed to re-find parent key in index \"%s\" for deletion target page %u",
			 RelationGetRelationName(rel), target);
	parent = stack->bts_blkno;
	poffset = stack->bts_offset;

	/*
	 * If the target is the rightmost child of its parent, then we can't
	 * delete, unless it's also the only child --- in which case the parent
	 * changes to half-dead status.  The "can't delete" case should have been
	 * detected by _bt_parent_deletion_safe, so complain if we see it now.
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
			elog(ERROR, "failed to delete rightmost child %u of block %u in index \"%s\"",
				 target, parent, RelationGetRelationName(rel));
	}
	else
	{
		/* Will there be exactly one child left in this parent? */
		if (OffsetNumberNext(P_FIRSTDATAKEY(opaque)) == maxoff)
			parent_one_child = true;
	}

	/*
	 * If we are deleting the next-to-last page on the target's level, then
	 * the rightsib is a candidate to become the new fast root. (In theory, it
	 * might be possible to push the fast root even further down, but the odds
	 * of doing so are slim, and the locking considerations daunting.)
	 *
	 * We don't support handling this in the case where the parent is becoming
	 * half-dead, even though it theoretically could occur.
	 *
	 * We can safely acquire a lock on the metapage here --- see comments for
	 * _bt_newroot().
	 */
	if (leftsib == P_NONE && !parent_half_dead)
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
			 * The expected case here is btm_fastlevel == targetlevel+1; if
			 * the fastlevel is <= targetlevel, something is wrong, and we
			 * choose to overwrite it to fix it.
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
	 * delete the target's downlink and the *following* key.  Easiest way is
	 * to copy the right sibling's downlink over the target downlink, and then
	 * delete the following item.
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
		itup = (IndexTuple) PageGetItem(page, itemid);
		Assert(ItemPointerGetBlockNumber(&(itup->t_tid)) == target);
		ItemPointerSet(&(itup->t_tid), rightsib, P_HIKEY);

		nextoffset = OffsetNumberNext(poffset);
		/* This part is just for double-checking */
		itemid = PageGetItemId(page, nextoffset);
		itup = (IndexTuple) PageGetItem(page, itemid);
		if (ItemPointerGetBlockNumber(&(itup->t_tid)) != rightsib)
			elog(PANIC, "right sibling %u of block %u is not next child of %u in index \"%s\"",
				 rightsib, target, BufferGetBlockNumber(pbuf),
				 RelationGetRelationName(rel));
		PageIndexTupleDelete(page, nextoffset);
	}

	/*
	 * Update siblings' side-links.  Note the target page's side-links will
	 * continue to point to the siblings.
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
	opaque->btpo_flags &= ~BTP_HALF_DEAD;
	opaque->btpo_flags |= BTP_DELETED;
	opaque->btpo.xact =
		vacuum_full ? FrozenTransactionId : ReadNewTransactionId();

	/* And update the metapage, if needed */
	if (BufferIsValid(metabuf))
	{
		metad->btm_fastroot = rightsib;
		metad->btm_fastlevel = targetlevel;
		MarkBufferDirty(metabuf);
	}

	/* Must mark buffers dirty before XLogInsert */
	MarkBufferDirty(pbuf);
	MarkBufferDirty(rbuf);
	MarkBufferDirty(buf);
	if (BufferIsValid(lbuf))
		MarkBufferDirty(lbuf);

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

		rdata[0].data = (char *) &xlrec;
		rdata[0].len = SizeOfBtreeDeletePage;
		rdata[0].buffer = InvalidBuffer;
		rdata[0].next = nextrdata = &(rdata[1]);

		if (BufferIsValid(metabuf))
		{
			xlmeta.root = metad->btm_root;
			xlmeta.level = metad->btm_level;
			xlmeta.fastroot = metad->btm_fastroot;
			xlmeta.fastlevel = metad->btm_fastlevel;

			nextrdata->data = (char *) &xlmeta;
			nextrdata->len = sizeof(xl_btree_metadata);
			nextrdata->buffer = InvalidBuffer;
			nextrdata->next = nextrdata + 1;
			nextrdata++;
			xlinfo = XLOG_BTREE_DELETE_PAGE_META;
		}
		else if (parent_half_dead)
			xlinfo = XLOG_BTREE_DELETE_PAGE_HALF;
		else
			xlinfo = XLOG_BTREE_DELETE_PAGE;

		nextrdata->data = NULL;
		nextrdata->len = 0;
		nextrdata->next = nextrdata + 1;
		nextrdata->buffer = pbuf;
		nextrdata->buffer_std = true;
		nextrdata++;

		nextrdata->data = NULL;
		nextrdata->len = 0;
		nextrdata->buffer = rbuf;
		nextrdata->buffer_std = true;
		nextrdata->next = NULL;

		if (BufferIsValid(lbuf))
		{
			nextrdata->next = nextrdata + 1;
			nextrdata++;
			nextrdata->data = NULL;
			nextrdata->len = 0;
			nextrdata->buffer = lbuf;
			nextrdata->buffer_std = true;
			nextrdata->next = NULL;
		}

		recptr = XLogInsert(RM_BTREE_ID, xlinfo, rdata);

		if (BufferIsValid(metabuf))
		{
			PageSetLSN(metapg, recptr);
			PageSetTLI(metapg, ThisTimeLineID);
		}
		page = BufferGetPage(pbuf);
		PageSetLSN(page, recptr);
		PageSetTLI(page, ThisTimeLineID);
		page = BufferGetPage(rbuf);
		PageSetLSN(page, recptr);
		PageSetTLI(page, ThisTimeLineID);
		page = BufferGetPage(buf);
		PageSetLSN(page, recptr);
		PageSetTLI(page, ThisTimeLineID);
		if (BufferIsValid(lbuf))
		{
			page = BufferGetPage(lbuf);
			PageSetLSN(page, recptr);
			PageSetTLI(page, ThisTimeLineID);
		}
	}

	END_CRIT_SECTION();

	/* release metapage; send out relcache inval if metapage changed */
	if (BufferIsValid(metabuf))
	{
		CacheInvalidateRelcache(rel);
		_bt_relbuf(rel, metabuf);
	}
	/* can always release leftsib immediately */
	if (BufferIsValid(lbuf))
		_bt_relbuf(rel, lbuf);

	/*
	 * If parent became half dead, recurse to delete it. Otherwise, if right
	 * sibling is empty and is now the last child of the parent, recurse to
	 * try to delete it.  (These cases cannot apply at the same time, though
	 * the second case might itself recurse to the first.)
	 *
	 * When recursing to parent, we hold the lock on the target page until
	 * done.  This delays any insertions into the keyspace that was just
	 * effectively reassigned to the parent's right sibling.  If we allowed
	 * that, and there were enough such insertions before we finish deleting
	 * the parent, page splits within that keyspace could lead to inserting
	 * out-of-order keys into the grandparent level.  It is thought that that
	 * wouldn't have any serious consequences, but it still seems like a
	 * pretty bad idea.
	 */
	if (parent_half_dead)
	{
		/* recursive call will release pbuf */
		_bt_relbuf(rel, rbuf);
		result = _bt_pagedel(rel, pbuf, stack->bts_parent, vacuum_full) + 1;
		_bt_relbuf(rel, buf);
	}
	else if (parent_one_child && rightsib_empty)
	{
		_bt_relbuf(rel, pbuf);
		_bt_relbuf(rel, buf);
		/* recursive call will release rbuf */
		result = _bt_pagedel(rel, rbuf, stack, vacuum_full) + 1;
	}
	else
	{
		_bt_relbuf(rel, pbuf);
		_bt_relbuf(rel, buf);
		_bt_relbuf(rel, rbuf);
		result = 1;
	}

	return result;
}
