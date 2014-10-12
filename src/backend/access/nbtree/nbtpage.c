/*-------------------------------------------------------------------------
 *
 * nbtpage.c
 *	  BTree-specific page management code for the Postgres btree access
 *	  method.
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/nbtree/nbtpage.c
 *
 *	NOTES
 *	   Postgres btree pages look like ordinary relation pages.  The opaque
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
#include "storage/indexfsm.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "utils/snapmgr.h"

static bool _bt_mark_page_halfdead(Relation rel, Buffer buf, BTStack stack);
static bool _bt_unlink_halfdead_page(Relation rel, Buffer leafbuf,
						 bool *rightsib_empty);
static bool _bt_lock_branch_parent(Relation rel, BlockNumber child,
					   BTStack stack, Buffer *topparent, OffsetNumber *topoff,
					   BlockNumber *target, BlockNumber *rightsib);
static void _bt_log_reuse_page(Relation rel, BlockNumber blkno,
				   TransactionId latestRemovedXid);

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
	 * Set pd_lower just past the end of the metadata.  This is not essential
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
 *		and no root page exists, we just return InvalidBuffer.  For
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
			 * over again.  (Is that really true? But it's hardly worth trying
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
		if (RelationNeedsWAL(rel))
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
			PageSetLSN(metapg, recptr);
		}

		END_CRIT_SECTION();

		/*
		 * swap root write lock for read lock.  There is no danger of anyone
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
 * from whatever non-root page we were at.  If we ever do need to lock the
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
 *	_bt_getrootheight() -- Get the height of the btree search tree.
 *
 *		We return the level (counting from zero) of the current fast root.
 *		This represents the number of tree levels we'd have to descend through
 *		to start any btree index search.
 *
 *		This is used by the planner for cost-estimation purposes.  Since it's
 *		only an estimate, slightly-stale data is fine, hence we don't worry
 *		about updating previously cached data.
 */
int
_bt_getrootheight(Relation rel)
{
	BTMetaPageData *metad;

	/*
	 * We can get what we need from the cached metapage data.  If it's not
	 * cached yet, load it.  Sanity checks here must match _bt_getroot().
	 */
	if (rel->rd_amcache == NULL)
	{
		Buffer		metabuf;
		Page		metapg;
		BTPageOpaque metaopaque;

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

		/*
		 * If there's no root page yet, _bt_getroot() doesn't expect a cache
		 * to be made, so just stop here and report the index height is zero.
		 * (XXX perhaps _bt_getroot() should be changed to allow this case.)
		 */
		if (metad->btm_root == P_NONE)
		{
			_bt_relbuf(rel, metabuf);
			return 0;
		}

		/*
		 * Cache the metapage data for next time
		 */
		rel->rd_amcache = MemoryContextAlloc(rel->rd_indexcxt,
											 sizeof(BTMetaPageData));
		memcpy(rel->rd_amcache, metad, sizeof(BTMetaPageData));

		_bt_relbuf(rel, metabuf);
	}

	metad = (BTMetaPageData *) rel->rd_amcache;
	/* We shouldn't have cached it if any of these fail */
	Assert(metad->btm_magic == BTREE_MAGIC);
	Assert(metad->btm_version == BTREE_VERSION);
	Assert(metad->btm_fastroot != P_NONE);

	return metad->btm_fastlevel;
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
	 * page header or is all-zero.  We have to defend against the all-zero
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
	if (PageGetSpecialSize(page) != MAXALIGN(sizeof(BTPageOpaqueData)))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" contains corrupted page at block %u",
						RelationGetRelationName(rel),
						BufferGetBlockNumber(buf)),
				 errhint("Please REINDEX it.")));
}

/*
 * Log the reuse of a page from the FSM.
 */
static void
_bt_log_reuse_page(Relation rel, BlockNumber blkno, TransactionId latestRemovedXid)
{
	if (!RelationNeedsWAL(rel))
		return;

	/* No ereport(ERROR) until changes are logged */
	START_CRIT_SECTION();

	/*
	 * We don't do MarkBufferDirty here because we're about to initialise the
	 * page, and nobody else can see it yet.
	 */

	/* XLOG stuff */
	{
		XLogRecData rdata[1];
		xl_btree_reuse_page xlrec_reuse;

		xlrec_reuse.node = rel->rd_node;
		xlrec_reuse.block = blkno;
		xlrec_reuse.latestRemovedXid = latestRemovedXid;
		rdata[0].data = (char *) &xlrec_reuse;
		rdata[0].len = SizeOfBtreeReusePage;
		rdata[0].buffer = InvalidBuffer;
		rdata[0].next = NULL;

		XLogInsert(RM_BTREE_ID, XLOG_BTREE_REUSE_PAGE, rdata);

		/*
		 * We don't do PageSetLSN here because we're about to initialise the
		 * page, so no need.
		 */
	}

	END_CRIT_SECTION();
}

/*
 *	_bt_getbuf() -- Get a buffer by block number for read or write.
 *
 *		blkno == P_NEW means to get an unallocated index page.  The page
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
		 * the page is still free.  (For example, an already-free page could
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
			blkno = GetFreeIndexPage(rel);
			if (blkno == InvalidBlockNumber)
				break;
			buf = ReadBuffer(rel, blkno);
			if (ConditionalLockBuffer(buf))
			{
				page = BufferGetPage(buf);
				if (_bt_page_recyclable(page))
				{
					/*
					 * If we are generating WAL for Hot Standby then create a
					 * WAL record that will allow us to conflict with queries
					 * running on standby.
					 */
					if (XLogStandbyInfoActive())
					{
						BTPageOpaque opaque = (BTPageOpaque) PageGetSpecialPointer(page);

						_bt_log_reuse_page(rel, blkno, opaque->btpo.xact);
					}

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
		Assert(PageIsNew(page));
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
 * callers.
 *
 * The original motivation for using this was to avoid two entries to the
 * bufmgr when one would do.  However, now it's mainly just a notational
 * convenience.  The only case where it saves work over _bt_relbuf/_bt_getbuf
 * is when the target page is the same one already in the buffer.
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
		TransactionIdPrecedes(opaque->btpo.xact, RecentGlobalXmin))
		return true;
	return false;
}

/*
 * Delete item(s) from a btree page during VACUUM.
 *
 * This must only be used for deleting leaf items.  Deleting an item on a
 * non-leaf page has to be done as part of an atomic action that includes
 * deleting the page it points to.
 *
 * This routine assumes that the caller has pinned and locked the buffer.
 * Also, the given itemnos *must* appear in increasing order in the array.
 *
 * We record VACUUMs and b-tree deletes differently in WAL. InHotStandby
 * we need to be able to pin all of the blocks in the btree in physical
 * order when replaying the effects of a VACUUM, just as we do for the
 * original VACUUM itself. lastBlockVacuumed allows us to tell whether an
 * intermediate range of blocks has had no changes at all by VACUUM,
 * and so must be scanned anyway during replay. We always write a WAL record
 * for the last block in the index, whether or not it contained any items
 * to be removed. This allows us to scan right up to end of index to
 * ensure correct locking.
 */
void
_bt_delitems_vacuum(Relation rel, Buffer buf,
					OffsetNumber *itemnos, int nitems,
					BlockNumber lastBlockVacuumed)
{
	Page		page = BufferGetPage(buf);
	BTPageOpaque opaque;

	/* No ereport(ERROR) until changes are logged */
	START_CRIT_SECTION();

	/* Fix the page */
	if (nitems > 0)
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
	if (RelationNeedsWAL(rel))
	{
		XLogRecPtr	recptr;
		XLogRecData rdata[2];
		xl_btree_vacuum xlrec_vacuum;

		xlrec_vacuum.node = rel->rd_node;
		xlrec_vacuum.block = BufferGetBlockNumber(buf);

		xlrec_vacuum.lastBlockVacuumed = lastBlockVacuumed;
		rdata[0].data = (char *) &xlrec_vacuum;
		rdata[0].len = SizeOfBtreeVacuum;
		rdata[0].buffer = InvalidBuffer;
		rdata[0].next = &(rdata[1]);

		/*
		 * The target-offsets array is not in the buffer, but pretend that it
		 * is.  When XLogInsert stores the whole buffer, the offsets array
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

		recptr = XLogInsert(RM_BTREE_ID, XLOG_BTREE_VACUUM, rdata);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();
}

/*
 * Delete item(s) from a btree page during single-page cleanup.
 *
 * As above, must only be used on leaf pages.
 *
 * This routine assumes that the caller has pinned and locked the buffer.
 * Also, the given itemnos *must* appear in increasing order in the array.
 *
 * This is nearly the same as _bt_delitems_vacuum as far as what it does to
 * the page, but the WAL logging considerations are quite different.  See
 * comments for _bt_delitems_vacuum.
 */
void
_bt_delitems_delete(Relation rel, Buffer buf,
					OffsetNumber *itemnos, int nitems,
					Relation heapRel)
{
	Page		page = BufferGetPage(buf);
	BTPageOpaque opaque;

	/* Shouldn't be called unless there's something to do */
	Assert(nitems > 0);

	/* No ereport(ERROR) until changes are logged */
	START_CRIT_SECTION();

	/* Fix the page */
	PageIndexMultiDelete(page, itemnos, nitems);

	/*
	 * Unlike _bt_delitems_vacuum, we *must not* clear the vacuum cycle ID,
	 * because this is not called by VACUUM.
	 */

	/*
	 * Mark the page as not containing any LP_DEAD items.  This is not
	 * certainly true (there might be some that have recently been marked, but
	 * weren't included in our target-item list), but it will almost always be
	 * true and it doesn't seem worth an additional page scan to check it.
	 * Remember that BTP_HAS_GARBAGE is only a hint anyway.
	 */
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	opaque->btpo_flags &= ~BTP_HAS_GARBAGE;

	MarkBufferDirty(buf);

	/* XLOG stuff */
	if (RelationNeedsWAL(rel))
	{
		XLogRecPtr	recptr;
		XLogRecData rdata[3];
		xl_btree_delete xlrec_delete;

		xlrec_delete.node = rel->rd_node;
		xlrec_delete.hnode = heapRel->rd_node;
		xlrec_delete.block = BufferGetBlockNumber(buf);
		xlrec_delete.nitems = nitems;

		rdata[0].data = (char *) &xlrec_delete;
		rdata[0].len = SizeOfBtreeDelete;
		rdata[0].buffer = InvalidBuffer;
		rdata[0].next = &(rdata[1]);

		/*
		 * We need the target-offsets array whether or not we store the whole
		 * buffer, to allow us to find the latestRemovedXid on a standby
		 * server.
		 */
		rdata[1].data = (char *) itemnos;
		rdata[1].len = nitems * sizeof(OffsetNumber);
		rdata[1].buffer = InvalidBuffer;
		rdata[1].next = &(rdata[2]);

		rdata[2].data = NULL;
		rdata[2].len = 0;
		rdata[2].buffer = buf;
		rdata[2].buffer_std = true;
		rdata[2].next = NULL;

		recptr = XLogInsert(RM_BTREE_ID, XLOG_BTREE_DELETE, rdata);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();
}

/*
 * Returns true, if the given block has the half-dead flag set.
 */
static bool
_bt_is_page_halfdead(Relation rel, BlockNumber blk)
{
	Buffer		buf;
	Page		page;
	BTPageOpaque opaque;
	bool		result;

	buf = _bt_getbuf(rel, blk, BT_READ);
	page = BufferGetPage(buf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);

	result = P_ISHALFDEAD(opaque);
	_bt_relbuf(rel, buf);

	return result;
}

/*
 * Subroutine to find the parent of the branch we're deleting.  This climbs
 * up the tree until it finds a page with more than one child, i.e. a page
 * that will not be totally emptied by the deletion.  The chain of pages below
 * it, with one downlink each, will form the branch that we need to delete.
 *
 * If we cannot remove the downlink from the parent, because it's the
 * rightmost entry, returns false.  On success, *topparent and *topoff are set
 * to the buffer holding the parent, and the offset of the downlink in it.
 * *topparent is write-locked, the caller is responsible for releasing it when
 * done.  *target is set to the topmost page in the branch to-be-deleted, i.e.
 * the page whose downlink *topparent / *topoff point to, and *rightsib to its
 * right sibling.
 *
 * "child" is the leaf page we wish to delete, and "stack" is a search stack
 * leading to it (approximately).  Note that we will update the stack
 * entry(s) to reflect current downlink positions --- this is harmless and
 * indeed saves later search effort in _bt_pagedel.  The caller should
 * initialize *target and *rightsib to the leaf page and its right sibling.
 *
 * Note: it's OK to release page locks on any internal pages between the leaf
 * and *topparent, because a safe deletion can't become unsafe due to
 * concurrent activity.  An internal page can only acquire an entry if the
 * child is split, but that cannot happen as long as we hold a lock on the
 * leaf.
 */
static bool
_bt_lock_branch_parent(Relation rel, BlockNumber child, BTStack stack,
					   Buffer *topparent, OffsetNumber *topoff,
					   BlockNumber *target, BlockNumber *rightsib)
{
	BlockNumber parent;
	OffsetNumber poffset,
				maxoff;
	Buffer		pbuf;
	Page		page;
	BTPageOpaque opaque;
	BlockNumber leftsib;

	/*
	 * Locate the downlink of "child" in the parent (updating the stack entry
	 * if needed)
	 */
	ItemPointerSet(&(stack->bts_btentry.t_tid), child, P_HIKEY);
	pbuf = _bt_getstackbuf(rel, stack, BT_WRITE);
	if (pbuf == InvalidBuffer)
		elog(ERROR, "failed to re-find parent key in index \"%s\" for deletion target page %u",
			 RelationGetRelationName(rel), child);
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
			if (P_RIGHTMOST(opaque) || P_ISROOT(opaque) ||
				P_INCOMPLETE_SPLIT(opaque))
			{
				_bt_relbuf(rel, pbuf);
				return false;
			}

			*target = parent;
			*rightsib = opaque->btpo_next;
			leftsib = opaque->btpo_prev;

			_bt_relbuf(rel, pbuf);

			/*
			 * Like in _bt_pagedel, check that the left sibling is not marked
			 * with INCOMPLETE_SPLIT flag.  That would mean that there is no
			 * downlink to the page to be deleted, and the page deletion
			 * algorithm isn't prepared to handle that.
			 */
			if (leftsib != P_NONE)
			{
				Buffer		lbuf;
				Page		lpage;
				BTPageOpaque lopaque;

				lbuf = _bt_getbuf(rel, leftsib, BT_READ);
				lpage = BufferGetPage(lbuf);
				lopaque = (BTPageOpaque) PageGetSpecialPointer(lpage);

				/*
				 * If the left sibling was concurrently split, so that its
				 * next-pointer doesn't point to the current page anymore, the
				 * split that created the current page must be completed. (We
				 * don't allow splitting an incompletely split page again
				 * until the previous split has been completed)
				 */
				if (lopaque->btpo_next == parent &&
					P_INCOMPLETE_SPLIT(lopaque))
				{
					_bt_relbuf(rel, lbuf);
					return false;
				}
				_bt_relbuf(rel, lbuf);
			}

			/*
			 * Perform the same check on this internal level that
			 * _bt_mark_page_halfdead performed on the leaf level.
			 */
			if (_bt_is_page_halfdead(rel, *rightsib))
			{
				elog(DEBUG1, "could not delete page %u because its right sibling %u is half-dead",
					 parent, *rightsib);
				return false;
			}

			return _bt_lock_branch_parent(rel, parent, stack->bts_parent,
										topparent, topoff, target, rightsib);
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
		*topparent = pbuf;
		*topoff = poffset;
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
 * Returns the number of pages successfully deleted (zero if page cannot
 * be deleted now; could be more than one if parent or sibling pages were
 * deleted too).
 *
 * NOTE: this leaks memory.  Rather than trying to clean up everything
 * carefully, it's better to run it in a temp context that can be reset
 * frequently.
 */
int
_bt_pagedel(Relation rel, Buffer buf)
{
	int			ndeleted = 0;
	BlockNumber rightsib;
	bool		rightsib_empty;
	Page		page;
	BTPageOpaque opaque;

	/*
	 * "stack" is a search stack leading (approximately) to the target page.
	 * It is initially NULL, but when iterating, we keep it to avoid
	 * duplicated search effort.
	 *
	 * Also, when "stack" is not NULL, we have already checked that the
	 * current page is not the right half of an incomplete split, i.e. the
	 * left sibling does not have its INCOMPLETE_SPLIT flag set.
	 */
	BTStack		stack = NULL;

	for (;;)
	{
		page = BufferGetPage(buf);
		opaque = (BTPageOpaque) PageGetSpecialPointer(page);

		/*
		 * Internal pages are never deleted directly, only as part of deleting
		 * the whole branch all the way down to leaf level.
		 */
		if (!P_ISLEAF(opaque))
		{
			/*
			 * Pre-9.4 page deletion only marked internal pages as half-dead,
			 * but now we only use that flag on leaf pages. The old algorithm
			 * was never supposed to leave half-dead pages in the tree, it was
			 * just a transient state, but it was nevertheless possible in
			 * error scenarios. We don't know how to deal with them here. They
			 * are harmless as far as searches are considered, but inserts
			 * into the deleted keyspace could add out-of-order downlinks in
			 * the upper levels. Log a notice, hopefully the admin will notice
			 * and reindex.
			 */
			if (P_ISHALFDEAD(opaque))
				ereport(LOG,
						(errcode(ERRCODE_INDEX_CORRUPTED),
					errmsg("index \"%s\" contains a half-dead internal page",
						   RelationGetRelationName(rel)),
						 errhint("This can be caused by an interrupted VACUUM in version 9.3 or older, before upgrade. Please REINDEX it.")));
			_bt_relbuf(rel, buf);
			return ndeleted;
		}

		/*
		 * We can never delete rightmost pages nor root pages.  While at it,
		 * check that page is not already deleted and is empty.
		 *
		 * To keep the algorithm simple, we also never delete an incompletely
		 * split page (they should be rare enough that this doesn't make any
		 * meaningful difference to disk usage):
		 *
		 * The INCOMPLETE_SPLIT flag on the page tells us if the page is the
		 * left half of an incomplete split, but ensuring that it's not the
		 * right half is more complicated.  For that, we have to check that
		 * the left sibling doesn't have its INCOMPLETE_SPLIT flag set.  On
		 * the first iteration, we temporarily release the lock on the current
		 * page, and check the left sibling and also construct a search stack
		 * to.  On subsequent iterations, we know we stepped right from a page
		 * that passed these tests, so it's OK.
		 */
		if (P_RIGHTMOST(opaque) || P_ISROOT(opaque) || P_ISDELETED(opaque) ||
			P_FIRSTDATAKEY(opaque) <= PageGetMaxOffsetNumber(page) ||
			P_INCOMPLETE_SPLIT(opaque))
		{
			/* Should never fail to delete a half-dead page */
			Assert(!P_ISHALFDEAD(opaque));

			_bt_relbuf(rel, buf);
			return ndeleted;
		}

		/*
		 * First, remove downlink pointing to the page (or a parent of the
		 * page, if we are going to delete a taller branch), and mark the page
		 * as half-dead.
		 */
		if (!P_ISHALFDEAD(opaque))
		{
			/*
			 * We need an approximate pointer to the page's parent page.  We
			 * use the standard search mechanism to search for the page's high
			 * key; this will give us a link to either the current parent or
			 * someplace to its left (if there are multiple equal high keys).
			 *
			 * Also check if this is the right-half of an incomplete split
			 * (see comment above).
			 */
			if (!stack)
			{
				ScanKey		itup_scankey;
				ItemId		itemid;
				IndexTuple	targetkey;
				Buffer		lbuf;
				BlockNumber leftsib;

				itemid = PageGetItemId(page, P_HIKEY);
				targetkey = CopyIndexTuple((IndexTuple) PageGetItem(page, itemid));

				leftsib = opaque->btpo_prev;

				/*
				 * To avoid deadlocks, we'd better drop the leaf page lock
				 * before going further.
				 */
				LockBuffer(buf, BUFFER_LOCK_UNLOCK);

				/*
				 * Fetch the left sibling, to check that it's not marked with
				 * INCOMPLETE_SPLIT flag.  That would mean that the page
				 * to-be-deleted doesn't have a downlink, and the page
				 * deletion algorithm isn't prepared to handle that.
				 */
				if (!P_LEFTMOST(opaque))
				{
					BTPageOpaque lopaque;
					Page		lpage;

					lbuf = _bt_getbuf(rel, leftsib, BT_READ);
					lpage = BufferGetPage(lbuf);
					lopaque = (BTPageOpaque) PageGetSpecialPointer(lpage);
					/*
					 * If the left sibling is split again by another backend,
					 * after we released the lock, we know that the first
					 * split must have finished, because we don't allow an
					 * incompletely-split page to be split again.  So we don't
					 * need to walk right here.
					 */
					if (lopaque->btpo_next == BufferGetBlockNumber(buf) &&
						P_INCOMPLETE_SPLIT(lopaque))
					{
						ReleaseBuffer(buf);
						_bt_relbuf(rel, lbuf);
						return ndeleted;
					}
					_bt_relbuf(rel, lbuf);
				}

				/* we need an insertion scan key for the search, so build one */
				itup_scankey = _bt_mkscankey(rel, targetkey);
				/* find the leftmost leaf page containing this key */
				stack = _bt_search(rel, rel->rd_rel->relnatts, itup_scankey,
								   false, &lbuf, BT_READ);
				/* don't need a pin on the page */
				_bt_relbuf(rel, lbuf);

				/*
				 * Re-lock the leaf page, and start over, to re-check that the
				 * page can still be deleted.
				 */
				LockBuffer(buf, BT_WRITE);
				continue;
			}

			if (!_bt_mark_page_halfdead(rel, buf, stack))
			{
				_bt_relbuf(rel, buf);
				return ndeleted;
			}
		}

		/*
		 * Then unlink it from its siblings.  Each call to
		 * _bt_unlink_halfdead_page unlinks the topmost page from the branch,
		 * making it shallower.  Iterate until the leaf page is gone.
		 */
		rightsib_empty = false;
		while (P_ISHALFDEAD(opaque))
		{
			if (!_bt_unlink_halfdead_page(rel, buf, &rightsib_empty))
			{
				_bt_relbuf(rel, buf);
				return ndeleted;
			}
			ndeleted++;
		}

		rightsib = opaque->btpo_next;

		_bt_relbuf(rel, buf);

		/*
		 * The page has now been deleted. If its right sibling is completely
		 * empty, it's possible that the reason we haven't deleted it earlier
		 * is that it was the rightmost child of the parent. Now that we
		 * removed the downlink for this page, the right sibling might now be
		 * the only child of the parent, and could be removed. It would be
		 * picked up by the next vacuum anyway, but might as well try to
		 * remove it now, so loop back to process the right sibling.
		 */
		if (!rightsib_empty)
			break;

		buf = _bt_getbuf(rel, rightsib, BT_WRITE);
	}

	return ndeleted;
}

/*
 * First stage of page deletion.  Remove the downlink to the top of the
 * branch being deleted, and mark the leaf page as half-dead.
 */
static bool
_bt_mark_page_halfdead(Relation rel, Buffer leafbuf, BTStack stack)
{
	BlockNumber leafblkno;
	BlockNumber leafrightsib;
	BlockNumber target;
	BlockNumber rightsib;
	ItemId		itemid;
	Page		page;
	BTPageOpaque opaque;
	Buffer		topparent;
	OffsetNumber topoff;
	OffsetNumber nextoffset;
	IndexTuple	itup;
	IndexTupleData trunctuple;

	page = BufferGetPage(leafbuf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);

	Assert(!P_RIGHTMOST(opaque) && !P_ISROOT(opaque) && !P_ISDELETED(opaque) &&
		   !P_ISHALFDEAD(opaque) && P_ISLEAF(opaque) &&
		   P_FIRSTDATAKEY(opaque) > PageGetMaxOffsetNumber(page));

	/*
	 * Save info about the leaf page.
	 */
	leafblkno = BufferGetBlockNumber(leafbuf);
	leafrightsib = opaque->btpo_next;

	/*
	 * Before attempting to lock the parent page, check that the right
	 * sibling is not in half-dead state.  A half-dead right sibling would
	 * have no downlink in the parent, which would be highly confusing later
	 * when we delete the downlink that follows the current page's downlink.
	 * (I believe the deletion would work correctly, but it would fail the
	 * cross-check we make that the following downlink points to the right
	 * sibling of the delete page.)
	 */
	if (_bt_is_page_halfdead(rel, leafrightsib))
	{
		elog(DEBUG1, "could not delete page %u because its right sibling %u is half-dead",
			 leafblkno, leafrightsib);
		return false;
	}

	/*
	 * We cannot delete a page that is the rightmost child of its immediate
	 * parent, unless it is the only child --- in which case the parent has to
	 * be deleted too, and the same condition applies recursively to it. We
	 * have to check this condition all the way up before trying to delete,
	 * and lock the final parent of the to-be-deleted branch.
	 */
	rightsib = leafrightsib;
	target = leafblkno;
	if (!_bt_lock_branch_parent(rel, leafblkno, stack,
								&topparent, &topoff, &target, &rightsib))
		return false;

	/*
	 * Check that the parent-page index items we're about to delete/overwrite
	 * contain what we expect.  This can fail if the index has become corrupt
	 * for some reason.  We want to throw any error before entering the
	 * critical section --- otherwise it'd be a PANIC.
	 *
	 * The test on the target item is just an Assert because
	 * _bt_lock_branch_parent should have guaranteed it has the expected
	 * contents.  The test on the next-child downlink is known to sometimes
	 * fail in the field, though.
	 */
	page = BufferGetPage(topparent);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);

#ifdef USE_ASSERT_CHECKING
	itemid = PageGetItemId(page, topoff);
	itup = (IndexTuple) PageGetItem(page, itemid);
	Assert(ItemPointerGetBlockNumber(&(itup->t_tid)) == target);
#endif

	nextoffset = OffsetNumberNext(topoff);
	itemid = PageGetItemId(page, nextoffset);
	itup = (IndexTuple) PageGetItem(page, itemid);
	if (ItemPointerGetBlockNumber(&(itup->t_tid)) != rightsib)
		elog(ERROR, "right sibling %u of block %u is not next child %u of block %u in index \"%s\"",
			 rightsib, target, ItemPointerGetBlockNumber(&(itup->t_tid)),
			 BufferGetBlockNumber(topparent), RelationGetRelationName(rel));

	/*
	 * Any insert which would have gone on the leaf block will now go to its
	 * right sibling.
	 */
	PredicateLockPageCombine(rel, leafblkno, leafrightsib);

	/* No ereport(ERROR) until changes are logged */
	START_CRIT_SECTION();

	/*
	 * Update parent.  The normal case is a tad tricky because we want to
	 * delete the target's downlink and the *following* key.  Easiest way is
	 * to copy the right sibling's downlink over the target downlink, and then
	 * delete the following item.
	 */
	page = BufferGetPage(topparent);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);

	itemid = PageGetItemId(page, topoff);
	itup = (IndexTuple) PageGetItem(page, itemid);
	ItemPointerSet(&(itup->t_tid), rightsib, P_HIKEY);

	nextoffset = OffsetNumberNext(topoff);
	PageIndexTupleDelete(page, nextoffset);

	/*
	 * Mark the leaf page as half-dead, and stamp it with a pointer to the
	 * highest internal page in the branch we're deleting.  We use the tid of
	 * the high key to store it.
	 */
	page = BufferGetPage(leafbuf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	opaque->btpo_flags |= BTP_HALF_DEAD;

	PageIndexTupleDelete(page, P_HIKEY);
	Assert(PageGetMaxOffsetNumber(page) == 0);
	MemSet(&trunctuple, 0, sizeof(IndexTupleData));
	trunctuple.t_info = sizeof(IndexTupleData);
	if (target != leafblkno)
		ItemPointerSet(&trunctuple.t_tid, target, P_HIKEY);
	else
		ItemPointerSetInvalid(&trunctuple.t_tid);
	if (PageAddItem(page, (Item) &trunctuple, sizeof(IndexTupleData), P_HIKEY,
					false, false) == InvalidOffsetNumber)
		elog(ERROR, "could not add dummy high key to half-dead page");

	/* Must mark buffers dirty before XLogInsert */
	MarkBufferDirty(topparent);
	MarkBufferDirty(leafbuf);

	/* XLOG stuff */
	if (RelationNeedsWAL(rel))
	{
		xl_btree_mark_page_halfdead xlrec;
		XLogRecPtr	recptr;
		XLogRecData rdata[2];

		xlrec.target.node = rel->rd_node;
		ItemPointerSet(&(xlrec.target.tid), BufferGetBlockNumber(topparent), topoff);
		xlrec.leafblk = leafblkno;
		if (target != leafblkno)
			xlrec.topparent = target;
		else
			xlrec.topparent = InvalidBlockNumber;

		page = BufferGetPage(leafbuf);
		opaque = (BTPageOpaque) PageGetSpecialPointer(page);
		xlrec.leftblk = opaque->btpo_prev;
		xlrec.rightblk = opaque->btpo_next;

		rdata[0].data = (char *) &xlrec;
		rdata[0].len = SizeOfBtreeMarkPageHalfDead;
		rdata[0].buffer = InvalidBuffer;
		rdata[0].next = &(rdata[1]);

		rdata[1].data = NULL;
		rdata[1].len = 0;
		rdata[1].buffer = topparent;
		rdata[1].buffer_std = true;
		rdata[1].next = NULL;

		recptr = XLogInsert(RM_BTREE_ID, XLOG_BTREE_MARK_PAGE_HALFDEAD, rdata);

		page = BufferGetPage(topparent);
		PageSetLSN(page, recptr);
		page = BufferGetPage(leafbuf);
		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	_bt_relbuf(rel, topparent);
	return true;
}

/*
 * Unlink a page in a branch of half-dead pages from its siblings.
 *
 * If the leaf page still has a downlink pointing to it, unlinks the highest
 * parent in the to-be-deleted branch instead of the leaf page.  To get rid
 * of the whole branch, including the leaf page itself, iterate until the
 * leaf page is deleted.
 *
 * Returns 'false' if the page could not be unlinked (shouldn't happen).
 * If the (new) right sibling of the page is empty, *rightsib_empty is set
 * to true.
 */
static bool
_bt_unlink_halfdead_page(Relation rel, Buffer leafbuf, bool *rightsib_empty)
{
	BlockNumber leafblkno = BufferGetBlockNumber(leafbuf);
	BlockNumber leafleftsib;
	BlockNumber leafrightsib;
	BlockNumber target;
	BlockNumber leftsib;
	BlockNumber rightsib;
	Buffer		lbuf = InvalidBuffer;
	Buffer		buf;
	Buffer		rbuf;
	Buffer		metabuf = InvalidBuffer;
	Page		metapg = NULL;
	BTMetaPageData *metad = NULL;
	ItemId		itemid;
	Page		page;
	BTPageOpaque opaque;
	bool		rightsib_is_rightmost;
	int			targetlevel;
	ItemPointer leafhikey;
	BlockNumber nextchild;
	BlockNumber topblkno;

	page = BufferGetPage(leafbuf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);

	Assert(P_ISLEAF(opaque) && P_ISHALFDEAD(opaque));

	/*
	 * Remember some information about the leaf page.
	 */
	itemid = PageGetItemId(page, P_HIKEY);
	leafhikey = &((IndexTuple) PageGetItem(page, itemid))->t_tid;
	leafleftsib = opaque->btpo_prev;
	leafrightsib = opaque->btpo_next;

	LockBuffer(leafbuf, BUFFER_LOCK_UNLOCK);

	/*
	 * If the leaf page still has a parent pointing to it (or a chain of
	 * parents), we don't unlink the leaf page yet, but the topmost remaining
	 * parent in the branch.
	 */
	if (ItemPointerIsValid(leafhikey))
	{
		topblkno = ItemPointerGetBlockNumber(leafhikey);
		target = topblkno;

		/* fetch the block number of the topmost parent's left sibling */
		buf = _bt_getbuf(rel, topblkno, BT_READ);
		page = BufferGetPage(buf);
		opaque = (BTPageOpaque) PageGetSpecialPointer(page);
		leftsib = opaque->btpo_prev;
		targetlevel = opaque->btpo.level;

		/*
		 * To avoid deadlocks, we'd better drop the target page lock before
		 * going further.
		 */
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	}
	else
	{
		topblkno = InvalidBlockNumber;
		target = leafblkno;

		buf = leafbuf;
		leftsib = opaque->btpo_prev;
		targetlevel = 0;
	}

	/*
	 * We have to lock the pages we need to modify in the standard order:
	 * moving right, then up.  Else we will deadlock against other writers.
	 *
	 * So, first lock the leaf page, if it's not the target.  Then find and
	 * write-lock the current left sibling of the target page.  The sibling
	 * that was current a moment ago could have split, so we may have to move
	 * right.  This search could fail if either the sibling or the target page
	 * was deleted by someone else meanwhile; if so, give up.  (Right now,
	 * that should never happen, since page deletion is only done in VACUUM
	 * and there shouldn't be multiple VACUUMs concurrently on the same
	 * table.)
	 */
	if (target != leafblkno)
		LockBuffer(leafbuf, BT_WRITE);
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
				return false;
			}
			lbuf = _bt_getbuf(rel, leftsib, BT_WRITE);
			page = BufferGetPage(lbuf);
			opaque = (BTPageOpaque) PageGetSpecialPointer(page);
		}
	}
	else
		lbuf = InvalidBuffer;

	/*
	 * Next write-lock the target page itself.  It should be okay to take just
	 * a write lock not a superexclusive lock, since no scans would stop on an
	 * empty page.
	 */
	LockBuffer(buf, BT_WRITE);
	page = BufferGetPage(buf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);

	/*
	 * Check page is still empty etc, else abandon deletion.  This is just for
	 * paranoia's sake; a half-dead page cannot resurrect because there can be
	 * only one vacuum process running at a time.
	 */
	if (P_RIGHTMOST(opaque) || P_ISROOT(opaque) || P_ISDELETED(opaque))
	{
		elog(ERROR, "half-dead page changed status unexpectedly in block %u of index \"%s\"",
			 target, RelationGetRelationName(rel));
	}
	if (opaque->btpo_prev != leftsib)
		elog(ERROR, "left link changed unexpectedly in block %u of index \"%s\"",
			 target, RelationGetRelationName(rel));

	if (target == leafblkno)
	{
		if (P_FIRSTDATAKEY(opaque) <= PageGetMaxOffsetNumber(page) ||
			!P_ISLEAF(opaque) || !P_ISHALFDEAD(opaque))
			elog(ERROR, "half-dead page changed status unexpectedly in block %u of index \"%s\"",
				 target, RelationGetRelationName(rel));
		nextchild = InvalidBlockNumber;
	}
	else
	{
		if (P_FIRSTDATAKEY(opaque) != PageGetMaxOffsetNumber(page) ||
			P_ISLEAF(opaque))
			elog(ERROR, "half-dead page changed status unexpectedly in block %u of index \"%s\"",
				 target, RelationGetRelationName(rel));

		/* remember the next child down in the branch. */
		itemid = PageGetItemId(page, P_FIRSTDATAKEY(opaque));
		nextchild = ItemPointerGetBlockNumber(&((IndexTuple) PageGetItem(page, itemid))->t_tid);
	}

	/*
	 * And next write-lock the (current) right sibling.
	 */
	rightsib = opaque->btpo_next;
	rbuf = _bt_getbuf(rel, rightsib, BT_WRITE);
	page = BufferGetPage(rbuf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	if (opaque->btpo_prev != target)
		elog(ERROR, "right sibling's left-link doesn't match: "
			 "block %u links to %u instead of expected %u in index \"%s\"",
			 rightsib, opaque->btpo_prev, target,
			 RelationGetRelationName(rel));
	rightsib_is_rightmost = P_RIGHTMOST(opaque);
	*rightsib_empty = (P_FIRSTDATAKEY(opaque) > PageGetMaxOffsetNumber(page));

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
	if (leftsib == P_NONE && rightsib_is_rightmost)
	{
		page = BufferGetPage(rbuf);
		opaque = (BTPageOpaque) PageGetSpecialPointer(page);
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
	 * Update siblings' side-links.  Note the target page's side-links will
	 * continue to point to the siblings.  Asserts here are just rechecking
	 * things we already verified above.
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

	/*
	 * If we deleted a parent of the targeted leaf page, instead of the leaf
	 * itself, update the leaf to point to the next remaining child in the
	 * branch.
	 */
	if (target != leafblkno)
	{
		if (nextchild == leafblkno)
			ItemPointerSetInvalid(leafhikey);
		else
			ItemPointerSet(leafhikey, nextchild, P_HIKEY);
	}

	/*
	 * Mark the page itself deleted.  It can be recycled when all current
	 * transactions are gone.  Storing GetTopTransactionId() would work, but
	 * we're in VACUUM and would not otherwise have an XID.  Having already
	 * updated links to the target, ReadNewTransactionId() suffices as an
	 * upper bound.  Any scan having retained a now-stale link is advertising
	 * in its PGXACT an xmin less than or equal to the value we read here.  It
	 * will continue to do so, holding back RecentGlobalXmin, for the duration
	 * of that scan.
	 */
	page = BufferGetPage(buf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	opaque->btpo_flags &= ~BTP_HALF_DEAD;
	opaque->btpo_flags |= BTP_DELETED;
	opaque->btpo.xact = ReadNewTransactionId();

	/* And update the metapage, if needed */
	if (BufferIsValid(metabuf))
	{
		metad->btm_fastroot = rightsib;
		metad->btm_fastlevel = targetlevel;
		MarkBufferDirty(metabuf);
	}

	/* Must mark buffers dirty before XLogInsert */
	MarkBufferDirty(rbuf);
	MarkBufferDirty(buf);
	if (BufferIsValid(lbuf))
		MarkBufferDirty(lbuf);
	if (target != leafblkno)
		MarkBufferDirty(leafbuf);

	/* XLOG stuff */
	if (RelationNeedsWAL(rel))
	{
		xl_btree_unlink_page xlrec;
		xl_btree_metadata xlmeta;
		uint8		xlinfo;
		XLogRecPtr	recptr;
		XLogRecData rdata[4];
		XLogRecData *nextrdata;

		xlrec.node = rel->rd_node;

		/* information on the unlinked block */
		xlrec.deadblk = target;
		xlrec.leftsib = leftsib;
		xlrec.rightsib = rightsib;
		xlrec.btpo_xact = opaque->btpo.xact;

		/* information needed to recreate the leaf block (if not the target) */
		xlrec.leafblk = leafblkno;
		xlrec.leafleftsib = leafleftsib;
		xlrec.leafrightsib = leafrightsib;
		xlrec.topparent = nextchild;

		rdata[0].data = (char *) &xlrec;
		rdata[0].len = SizeOfBtreeUnlinkPage;
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
			xlinfo = XLOG_BTREE_UNLINK_PAGE_META;
		}
		else
			xlinfo = XLOG_BTREE_UNLINK_PAGE;

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
		}
		page = BufferGetPage(rbuf);
		PageSetLSN(page, recptr);
		page = BufferGetPage(buf);
		PageSetLSN(page, recptr);
		if (BufferIsValid(lbuf))
		{
			page = BufferGetPage(lbuf);
			PageSetLSN(page, recptr);
		}
		if (target != leafblkno)
		{
			page = BufferGetPage(leafbuf);
			PageSetLSN(page, recptr);
		}
	}

	END_CRIT_SECTION();

	/* release metapage */
	if (BufferIsValid(metabuf))
		_bt_relbuf(rel, metabuf);

	/* release siblings */
	if (BufferIsValid(lbuf))
		_bt_relbuf(rel, lbuf);
	_bt_relbuf(rel, rbuf);

	/*
	 * Release the target, if it was not the leaf block.  The leaf is always
	 * kept locked.
	 */
	if (target != leafblkno)
		_bt_relbuf(rel, buf);

	return true;
}
