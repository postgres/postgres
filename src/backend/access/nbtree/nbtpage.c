/*-------------------------------------------------------------------------
 *
 * nbtpage.c
 *	  BTree-specific page management code for the Postgres btree access
 *	  method.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
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
#include "access/nbtxlog.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "storage/indexfsm.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "utils/snapmgr.h"

static BTMetaPageData *_bt_getmeta(Relation rel, Buffer metabuf);
static bool _bt_mark_page_halfdead(Relation rel, Buffer leafbuf,
								   BTStack stack);
static bool _bt_unlink_halfdead_page(Relation rel, Buffer leafbuf,
									 BlockNumber scanblkno,
									 bool *rightsib_empty,
									 TransactionId *oldestBtpoXact,
									 uint32 *ndeleted);
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
	metad->btm_oldest_btpo_xact = InvalidTransactionId;
	metad->btm_last_cleanup_num_heap_tuples = -1.0;

	metaopaque = (BTPageOpaque) PageGetSpecialPointer(page);
	metaopaque->btpo_flags = BTP_META;

	/*
	 * Set pd_lower just past the end of the metadata.  This is essential,
	 * because without doing so, metadata will be lost if xlog.c compresses
	 * the page.
	 */
	((PageHeader) page)->pd_lower =
		((char *) metad + sizeof(BTMetaPageData)) - (char *) page;
}

/*
 *	_bt_upgrademetapage() -- Upgrade a meta-page from an old format to version
 *		3, the last version that can be updated without broadly affecting
 *		on-disk compatibility.  (A REINDEX is required to upgrade to v4.)
 *
 *		This routine does purely in-memory image upgrade.  Caller is
 *		responsible for locking, WAL-logging etc.
 */
void
_bt_upgrademetapage(Page page)
{
	BTMetaPageData *metad;
	BTPageOpaque metaopaque PG_USED_FOR_ASSERTS_ONLY;

	metad = BTPageGetMeta(page);
	metaopaque = (BTPageOpaque) PageGetSpecialPointer(page);

	/* It must be really a meta page of upgradable version */
	Assert(metaopaque->btpo_flags & BTP_META);
	Assert(metad->btm_version < BTREE_NOVAC_VERSION);
	Assert(metad->btm_version >= BTREE_MIN_VERSION);

	/* Set version number and fill extra fields added into version 3 */
	metad->btm_version = BTREE_NOVAC_VERSION;
	metad->btm_oldest_btpo_xact = InvalidTransactionId;
	metad->btm_last_cleanup_num_heap_tuples = -1.0;

	/* Adjust pd_lower (see _bt_initmetapage() for details) */
	((PageHeader) page)->pd_lower =
		((char *) metad + sizeof(BTMetaPageData)) - (char *) page;
}

/*
 * Get metadata from share-locked buffer containing metapage, while performing
 * standard sanity checks.
 *
 * Callers that cache data returned here in local cache should note that an
 * on-the-fly upgrade using _bt_upgrademetapage() can change the version field
 * and BTREE_NOVAC_VERSION specific fields without invalidating local cache.
 */
static BTMetaPageData *
_bt_getmeta(Relation rel, Buffer metabuf)
{
	Page		metapg;
	BTPageOpaque metaopaque;
	BTMetaPageData *metad;

	metapg = BufferGetPage(metabuf);
	metaopaque = (BTPageOpaque) PageGetSpecialPointer(metapg);
	metad = BTPageGetMeta(metapg);

	/* sanity-check the metapage */
	if (!P_ISMETA(metaopaque) ||
		metad->btm_magic != BTREE_MAGIC)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" is not a btree",
						RelationGetRelationName(rel))));

	if (metad->btm_version < BTREE_MIN_VERSION ||
		metad->btm_version > BTREE_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("version mismatch in index \"%s\": file version %d, "
						"current version %d, minimal supported version %d",
						RelationGetRelationName(rel),
						metad->btm_version, BTREE_VERSION, BTREE_MIN_VERSION)));

	return metad;
}

/*
 *	_bt_update_meta_cleanup_info() -- Update cleanup-related information in
 *									  the metapage.
 *
 *		This routine checks if provided cleanup-related information is matching
 *		to those written in the metapage.  On mismatch, metapage is overwritten.
 */
void
_bt_update_meta_cleanup_info(Relation rel, TransactionId oldestBtpoXact,
							 float8 numHeapTuples)
{
	Buffer		metabuf;
	Page		metapg;
	BTMetaPageData *metad;
	bool		needsRewrite = false;
	XLogRecPtr	recptr;

	/* read the metapage and check if it needs rewrite */
	metabuf = _bt_getbuf(rel, BTREE_METAPAGE, BT_READ);
	metapg = BufferGetPage(metabuf);
	metad = BTPageGetMeta(metapg);

	/* outdated version of metapage always needs rewrite */
	if (metad->btm_version < BTREE_NOVAC_VERSION)
		needsRewrite = true;
	else if (metad->btm_oldest_btpo_xact != oldestBtpoXact ||
			 metad->btm_last_cleanup_num_heap_tuples != numHeapTuples)
		needsRewrite = true;

	if (!needsRewrite)
	{
		_bt_relbuf(rel, metabuf);
		return;
	}

	/* trade in our read lock for a write lock */
	LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
	LockBuffer(metabuf, BT_WRITE);

	START_CRIT_SECTION();

	/* upgrade meta-page if needed */
	if (metad->btm_version < BTREE_NOVAC_VERSION)
		_bt_upgrademetapage(metapg);

	/* update cleanup-related information */
	metad->btm_oldest_btpo_xact = oldestBtpoXact;
	metad->btm_last_cleanup_num_heap_tuples = numHeapTuples;
	MarkBufferDirty(metabuf);

	/* write wal record if needed */
	if (RelationNeedsWAL(rel))
	{
		xl_btree_metadata md;

		XLogBeginInsert();
		XLogRegisterBuffer(0, metabuf, REGBUF_WILL_INIT | REGBUF_STANDARD);

		Assert(metad->btm_version >= BTREE_NOVAC_VERSION);
		md.version = metad->btm_version;
		md.root = metad->btm_root;
		md.level = metad->btm_level;
		md.fastroot = metad->btm_fastroot;
		md.fastlevel = metad->btm_fastlevel;
		md.oldest_btpo_xact = oldestBtpoXact;
		md.last_cleanup_num_heap_tuples = numHeapTuples;

		XLogRegisterBufData(0, (char *) &md, sizeof(xl_btree_metadata));

		recptr = XLogInsert(RM_BTREE_ID, XLOG_BTREE_META_CLEANUP);

		PageSetLSN(metapg, recptr);
	}

	END_CRIT_SECTION();
	_bt_relbuf(rel, metabuf);
}

/*
 *	_bt_getroot() -- Get the root page of the btree.
 *
 *		Since the root page can move around the btree file, we have to read
 *		its location from the metadata page, and then read the root page
 *		itself.  If no root page exists yet, we have to create one.  The
 *		standard class of race conditions exists here; I think I covered
 *		them all in the intricate dance of lock requests below.
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
		Assert(metad->btm_version >= BTREE_MIN_VERSION);
		Assert(metad->btm_version <= BTREE_VERSION);
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
	metad = _bt_getmeta(rel, metabuf);

	/* if no root page initialized yet, do it */
	if (metad->btm_root == P_NONE)
	{
		Page		metapg;

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
		/* Get raw page pointer for metapage */
		metapg = BufferGetPage(metabuf);

		/* NO ELOG(ERROR) till meta is updated */
		START_CRIT_SECTION();

		/* upgrade metapage if needed */
		if (metad->btm_version < BTREE_NOVAC_VERSION)
			_bt_upgrademetapage(metapg);

		metad->btm_root = rootblkno;
		metad->btm_level = 0;
		metad->btm_fastroot = rootblkno;
		metad->btm_fastlevel = 0;
		metad->btm_oldest_btpo_xact = InvalidTransactionId;
		metad->btm_last_cleanup_num_heap_tuples = -1.0;

		MarkBufferDirty(rootbuf);
		MarkBufferDirty(metabuf);

		/* XLOG stuff */
		if (RelationNeedsWAL(rel))
		{
			xl_btree_newroot xlrec;
			XLogRecPtr	recptr;
			xl_btree_metadata md;

			XLogBeginInsert();
			XLogRegisterBuffer(0, rootbuf, REGBUF_WILL_INIT);
			XLogRegisterBuffer(2, metabuf, REGBUF_WILL_INIT | REGBUF_STANDARD);

			Assert(metad->btm_version >= BTREE_NOVAC_VERSION);
			md.version = metad->btm_version;
			md.root = rootblkno;
			md.level = 0;
			md.fastroot = rootblkno;
			md.fastlevel = 0;
			md.oldest_btpo_xact = InvalidTransactionId;
			md.last_cleanup_num_heap_tuples = -1.0;

			XLogRegisterBufData(2, (char *) &md, sizeof(xl_btree_metadata));

			xlrec.rootblk = rootblkno;
			xlrec.level = 0;

			XLogRegisterData((char *) &xlrec, SizeOfBtreeNewroot);

			recptr = XLogInsert(RM_BTREE_ID, XLOG_BTREE_NEWROOT);

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

		/* okay, metadata is correct, release lock on it without caching */
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

	if (!P_ISMETA(metaopaque) ||
		metad->btm_magic != BTREE_MAGIC)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" is not a btree",
						RelationGetRelationName(rel))));

	if (metad->btm_version < BTREE_MIN_VERSION ||
		metad->btm_version > BTREE_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("version mismatch in index \"%s\": file version %d, "
						"current version %d, minimal supported version %d",
						RelationGetRelationName(rel),
						metad->btm_version, BTREE_VERSION, BTREE_MIN_VERSION)));

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

	if (rel->rd_amcache == NULL)
	{
		Buffer		metabuf;

		metabuf = _bt_getbuf(rel, BTREE_METAPAGE, BT_READ);
		metad = _bt_getmeta(rel, metabuf);

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

	/* Get cached page */
	metad = (BTMetaPageData *) rel->rd_amcache;
	/* We shouldn't have cached it if any of these fail */
	Assert(metad->btm_magic == BTREE_MAGIC);
	Assert(metad->btm_version >= BTREE_MIN_VERSION);
	Assert(metad->btm_version <= BTREE_VERSION);
	Assert(metad->btm_fastroot != P_NONE);

	return metad->btm_fastlevel;
}

/*
 *	_bt_heapkeyspace() -- is heap TID being treated as a key?
 *
 *		This is used to determine the rules that must be used to descend a
 *		btree.  Version 4 indexes treat heap TID as a tiebreaker attribute.
 *		pg_upgrade'd version 3 indexes need extra steps to preserve reasonable
 *		performance when inserting a new BTScanInsert-wise duplicate tuple
 *		among many leaf pages already full of such duplicates.
 */
bool
_bt_heapkeyspace(Relation rel)
{
	BTMetaPageData *metad;

	if (rel->rd_amcache == NULL)
	{
		Buffer		metabuf;

		metabuf = _bt_getbuf(rel, BTREE_METAPAGE, BT_READ);
		metad = _bt_getmeta(rel, metabuf);

		/*
		 * If there's no root page yet, _bt_getroot() doesn't expect a cache
		 * to be made, so just stop here.  (XXX perhaps _bt_getroot() should
		 * be changed to allow this case.)
		 */
		if (metad->btm_root == P_NONE)
		{
			uint32		btm_version = metad->btm_version;

			_bt_relbuf(rel, metabuf);
			return btm_version > BTREE_NOVAC_VERSION;
		}

		/*
		 * Cache the metapage data for next time
		 *
		 * An on-the-fly version upgrade performed by _bt_upgrademetapage()
		 * can change the nbtree version for an index without invalidating any
		 * local cache.  This is okay because it can only happen when moving
		 * from version 2 to version 3, both of which are !heapkeyspace
		 * versions.
		 */
		rel->rd_amcache = MemoryContextAlloc(rel->rd_indexcxt,
											 sizeof(BTMetaPageData));
		memcpy(rel->rd_amcache, metad, sizeof(BTMetaPageData));
		_bt_relbuf(rel, metabuf);
	}

	/* Get cached page */
	metad = (BTMetaPageData *) rel->rd_amcache;
	/* We shouldn't have cached it if any of these fail */
	Assert(metad->btm_magic == BTREE_MAGIC);
	Assert(metad->btm_version >= BTREE_MIN_VERSION);
	Assert(metad->btm_version <= BTREE_VERSION);
	Assert(metad->btm_fastroot != P_NONE);

	return metad->btm_version > BTREE_NOVAC_VERSION;
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
	xl_btree_reuse_page xlrec_reuse;

	/*
	 * Note that we don't register the buffer with the record, because this
	 * operation doesn't modify the page. This record only exists to provide a
	 * conflict point for Hot Standby.
	 */

	/* XLOG stuff */
	xlrec_reuse.node = rel->rd_node;
	xlrec_reuse.block = blkno;
	xlrec_reuse.latestRemovedXid = latestRemovedXid;

	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec_reuse, SizeOfBtreeReusePage);

	XLogInsert(RM_BTREE_ID, XLOG_BTREE_REUSE_PAGE);
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
					 * running on standby, in case they have snapshots older
					 * than btpo.xact.  This can only apply if the page does
					 * have a valid btpo.xact value, ie not if it's new.  (We
					 * must check that because an all-zero page has no special
					 * space.)
					 */
					if (XLogStandbyInfoActive() && RelationNeedsWAL(rel) &&
						!PageIsNew(page))
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
 * policy about whether a page is safe to re-use.  But note that _bt_getbuf
 * knows enough to distinguish the PageIsNew condition from the other one.
 * At some point it might be appropriate to redesign this to have a three-way
 * result value.
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
		xl_btree_vacuum xlrec_vacuum;

		xlrec_vacuum.lastBlockVacuumed = lastBlockVacuumed;

		XLogBeginInsert();
		XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
		XLogRegisterData((char *) &xlrec_vacuum, SizeOfBtreeVacuum);

		/*
		 * The target-offsets array is not in the buffer, but pretend that it
		 * is.  When XLogInsert stores the whole buffer, the offsets array
		 * need not be stored too.
		 */
		if (nitems > 0)
			XLogRegisterBufData(0, (char *) itemnos, nitems * sizeof(OffsetNumber));

		recptr = XLogInsert(RM_BTREE_ID, XLOG_BTREE_VACUUM);

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
	TransactionId latestRemovedXid = InvalidTransactionId;

	/* Shouldn't be called unless there's something to do */
	Assert(nitems > 0);

	if (XLogStandbyInfoActive() && RelationNeedsWAL(rel))
		latestRemovedXid =
			index_compute_xid_horizon_for_tuples(rel, heapRel, buf,
												 itemnos, nitems);

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
		xl_btree_delete xlrec_delete;

		xlrec_delete.latestRemovedXid = latestRemovedXid;
		xlrec_delete.nitems = nitems;

		XLogBeginInsert();
		XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
		XLogRegisterData((char *) &xlrec_delete, SizeOfBtreeDelete);

		/*
		 * We need the target-offsets array whether or not we store the whole
		 * buffer, to allow us to find the latestRemovedXid on a standby
		 * server.
		 */
		XLogRegisterData((char *) itemnos, nitems * sizeof(OffsetNumber));

		recptr = XLogInsert(RM_BTREE_ID, XLOG_BTREE_DELETE);

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
 * leading to it (it actually leads to the leftmost leaf page with a high key
 * matching that of the page to be deleted in !heapkeyspace indexes).  Note
 * that we will update the stack entry(s) to reflect current downlink
 * positions --- this is essentially the same as the corresponding step of
 * splitting, and is not expected to affect caller.  The caller should
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
	 * Locate the downlink of "child" in the parent, updating the stack entry
	 * if needed.  This is how !heapkeyspace indexes deal with having
	 * non-unique high keys in leaf level pages.  Even heapkeyspace indexes
	 * can have a stale stack due to insertions into the parent.
	 */
	stack->bts_btentry = child;
	pbuf = _bt_getstackbuf(rel, stack);
	if (pbuf == InvalidBuffer)
	{
		/*
		 * Failed to "re-find" a pivot tuple whose downlink matched our child
		 * block number on the parent level -- the index must be corrupt.
		 * Don't even try to delete the leafbuf subtree.  Just report the
		 * issue and press on with vacuuming the index.
		 *
		 * Note: _bt_getstackbuf() recovers from concurrent page splits that
		 * take place on the parent level.  Its approach is a near-exhaustive
		 * linear search.  This also gives it a surprisingly good chance of
		 * recovering in the event of a buggy or inconsistent opclass.  But we
		 * don't rely on that here.
		 */
		ereport(LOG,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg_internal("failed to re-find parent key in index \"%s\" for deletion target page %u",
								 RelationGetRelationName(rel), child)));
		return false;
	}
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
 * _bt_pagedel() -- Delete a leaf page from the b-tree, if legal to do so.
 *
 * This action unlinks the leaf page from the b-tree structure, removing all
 * pointers leading to it --- but not touching its own left and right links.
 * The page cannot be physically reclaimed right away, since other processes
 * may currently be trying to follow links leading to the page; they have to
 * be allowed to use its right-link to recover.  See nbtree/README.
 *
 * On entry, the target buffer must be pinned and locked (either read or write
 * lock is OK).  The page must be an empty leaf page, which may be half-dead
 * already (a half-dead page should only be passed to us when an earlier
 * VACUUM operation was interrupted, though).  Note in particular that caller
 * should never pass a buffer containing an existing deleted page here.  The
 * lock and pin on caller's buffer will be dropped before we return.
 *
 * Returns the number of pages successfully deleted (zero if page cannot
 * be deleted now; could be more than one if parent or right sibling pages
 * were deleted too).  Note that this does not include pages that we delete
 * that the btvacuumscan scan has yet to reach; they'll get counted later
 * instead.
 *
 * Maintains *oldestBtpoXact for any pages that get deleted.  Caller is
 * responsible for maintaining *oldestBtpoXact in the case of pages that were
 * deleted by a previous VACUUM.
 *
 * NOTE: this leaks memory.  Rather than trying to clean up everything
 * carefully, it's better to run it in a temp context that can be reset
 * frequently.
 */
uint32
_bt_pagedel(Relation rel, Buffer leafbuf, TransactionId *oldestBtpoXact)
{
	uint32		ndeleted = 0;
	BlockNumber rightsib;
	bool		rightsib_empty;
	Page		page;
	BTPageOpaque opaque;

	/*
	 * Save original leafbuf block number from caller.  Only deleted blocks
	 * that are <= scanblkno get counted in ndeleted return value.
	 */
	BlockNumber scanblkno = BufferGetBlockNumber(leafbuf);

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
		page = BufferGetPage(leafbuf);
		opaque = (BTPageOpaque) PageGetSpecialPointer(page);

		/*
		 * Internal pages are never deleted directly, only as part of deleting
		 * the whole branch all the way down to leaf level.
		 *
		 * Also check for deleted pages here.  Caller never passes us a fully
		 * deleted page.  Only VACUUM can delete pages, so there can't have
		 * been a concurrent deletion.  Assume that we reached any deleted
		 * page encountered here by following a sibling link, and that the
		 * index is corrupt.
		 */
		Assert(!P_ISDELETED(opaque));
		if (!P_ISLEAF(opaque) || P_ISDELETED(opaque))
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

			if (P_ISDELETED(opaque))
				ereport(LOG,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg_internal("found deleted block %u while following right link from block %u in index \"%s\"",
										 BufferGetBlockNumber(leafbuf),
										 scanblkno,
										 RelationGetRelationName(rel))));

			_bt_relbuf(rel, leafbuf);
			return ndeleted;
		}

		/*
		 * We can never delete rightmost pages nor root pages.  While at it,
		 * check that page is empty, since it's possible that the leafbuf page
		 * was empty a moment ago, but has since had some inserts.
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
		if (P_RIGHTMOST(opaque) || P_ISROOT(opaque) ||
			P_FIRSTDATAKEY(opaque) <= PageGetMaxOffsetNumber(page) ||
			P_INCOMPLETE_SPLIT(opaque))
		{
			/* Should never fail to delete a half-dead page */
			Assert(!P_ISHALFDEAD(opaque));

			_bt_relbuf(rel, leafbuf);
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
			 * use a variant of the standard search mechanism to search for
			 * the page's high key; this will give us a link to either the
			 * current parent or someplace to its left (if there are multiple
			 * equal high keys, which is possible with !heapkeyspace indexes).
			 *
			 * Also check if this is the right-half of an incomplete split
			 * (see comment above).
			 */
			if (!stack)
			{
				BTScanInsert itup_key;
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
				LockBuffer(leafbuf, BUFFER_LOCK_UNLOCK);

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
					if (lopaque->btpo_next == BufferGetBlockNumber(leafbuf) &&
						P_INCOMPLETE_SPLIT(lopaque))
					{
						ReleaseBuffer(leafbuf);
						_bt_relbuf(rel, lbuf);
						return ndeleted;
					}
					_bt_relbuf(rel, lbuf);
				}

				/* we need an insertion scan key for the search, so build one */
				itup_key = _bt_mkscankey(rel, targetkey);
				/* find the leftmost leaf page with matching pivot/high key */
				itup_key->pivotsearch = true;
				stack = _bt_search(rel, itup_key, &lbuf, BT_READ, NULL);
				/* don't need a lock or second pin on the page */
				_bt_relbuf(rel, lbuf);

				/*
				 * Re-lock the leaf page, and start over to use our stack
				 * within _bt_mark_page_halfdead.  We must do it that way
				 * because it's possible that leafbuf can no longer be
				 * deleted.  We need to recheck.
				 */
				LockBuffer(leafbuf, BT_WRITE);
				continue;
			}

			/*
			 * See if it's safe to delete the leaf page, and determine how
			 * many parent/internal pages above the leaf level will be
			 * deleted.  If it's safe then _bt_mark_page_halfdead will also
			 * perform the first phase of deletion, which includes marking the
			 * leafbuf page half-dead.
			 */
			Assert(P_ISLEAF(opaque) && !P_IGNORE(opaque));
			if (!_bt_mark_page_halfdead(rel, leafbuf, stack))
			{
				_bt_relbuf(rel, leafbuf);
				return ndeleted;
			}
		}

		/*
		 * Then unlink it from its siblings.  Each call to
		 * _bt_unlink_halfdead_page unlinks the topmost page from the branch,
		 * making it shallower.  Iterate until the leafbuf page is deleted.
		 *
		 * _bt_unlink_halfdead_page should never fail, since we established
		 * that deletion is generally safe in _bt_mark_page_halfdead.
		 */
		rightsib_empty = false;
		Assert(P_ISLEAF(opaque) && P_ISHALFDEAD(opaque));
		while (P_ISHALFDEAD(opaque))
		{
			/* Check for interrupts in _bt_unlink_halfdead_page */
			if (!_bt_unlink_halfdead_page(rel, leafbuf, scanblkno,
										  &rightsib_empty, oldestBtpoXact,
										  &ndeleted))
			{
				/* _bt_unlink_halfdead_page failed, released buffer */
				return ndeleted;
			}
		}

		Assert(P_ISLEAF(opaque) && P_ISDELETED(opaque));
		Assert(TransactionIdFollowsOrEquals(opaque->btpo.xact,
											*oldestBtpoXact));

		rightsib = opaque->btpo_next;

		_bt_relbuf(rel, leafbuf);

		/*
		 * Check here, as calling loops will have locks held, preventing
		 * interrupts from being processed.
		 */
		CHECK_FOR_INTERRUPTS();

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

		leafbuf = _bt_getbuf(rel, rightsib, BT_WRITE);
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
	 * Before attempting to lock the parent page, check that the right sibling
	 * is not in half-dead state.  A half-dead right sibling would have no
	 * downlink in the parent, which would be highly confusing later when we
	 * delete the downlink that follows the current page's downlink. (I
	 * believe the deletion would work correctly, but it would fail the
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
	 * and lock the final parent of the to-be-deleted subtree.
	 *
	 * However, we won't need to repeat the above _bt_is_page_halfdead() check
	 * for parent/ancestor pages because of the rightmost restriction. The
	 * leaf check will apply to a right "cousin" leaf page rather than a
	 * simple right sibling leaf page in cases where we actually go on to
	 * perform internal page deletion. The right cousin leaf page is
	 * representative of the left edge of the subtree to the right of the
	 * to-be-deleted subtree as a whole.  (Besides, internal pages are never
	 * marked half-dead, so it isn't even possible to directly assess if an
	 * internal page is part of some other to-be-deleted subtree.)
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
	Assert(BTreeInnerTupleGetDownLink(itup) == target);
#endif

	nextoffset = OffsetNumberNext(topoff);
	itemid = PageGetItemId(page, nextoffset);
	itup = (IndexTuple) PageGetItem(page, itemid);
	if (BTreeInnerTupleGetDownLink(itup) != rightsib)
	{
		ereport(LOG,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg_internal("right sibling %u of block %u is not next child %u of block %u in index \"%s\"",
								 rightsib, target,
								 BTreeInnerTupleGetDownLink(itup),
								 BufferGetBlockNumber(topparent),
								 RelationGetRelationName(rel))));

		_bt_relbuf(rel, topparent);

		return false;
	}

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
	BTreeInnerTupleSetDownLink(itup, rightsib);

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
		BTreeTupleSetTopParent(&trunctuple, target);
	else
		BTreeTupleSetTopParent(&trunctuple, InvalidBlockNumber);

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

		xlrec.poffset = topoff;
		xlrec.leafblk = leafblkno;
		if (target != leafblkno)
			xlrec.topparent = target;
		else
			xlrec.topparent = InvalidBlockNumber;

		XLogBeginInsert();
		XLogRegisterBuffer(0, leafbuf, REGBUF_WILL_INIT);
		XLogRegisterBuffer(1, topparent, REGBUF_STANDARD);

		page = BufferGetPage(leafbuf);
		opaque = (BTPageOpaque) PageGetSpecialPointer(page);
		xlrec.leftblk = opaque->btpo_prev;
		xlrec.rightblk = opaque->btpo_next;

		XLogRegisterData((char *) &xlrec, SizeOfBtreeMarkPageHalfDead);

		recptr = XLogInsert(RM_BTREE_ID, XLOG_BTREE_MARK_PAGE_HALFDEAD);

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
 * Returns 'false' if the page could not be unlinked (shouldn't happen).  If
 * the right sibling of the current target page is empty, *rightsib_empty is
 * set to true, allowing caller to delete the target's right sibling page in
 * passing.  Note that *rightsib_empty is only actually used by caller when
 * target page is leafbuf, following last call here for leafbuf/the subtree
 * containing leafbuf.  (We always set *rightsib_empty for caller, just to be
 * consistent.)
 *
 * We maintain *oldestBtpoXact for pages that are deleted by the current
 * VACUUM operation here.  This must be handled here because we conservatively
 * assume that there needs to be a new call to ReadNewTransactionId() each
 * time a page gets deleted.  See comments about the underlying assumption
 * below.
 *
 * Must hold pin and lock on leafbuf at entry (read or write doesn't matter).
 * On success exit, we'll be holding pin and write lock.  On failure exit,
 * we'll release both pin and lock before returning (we define it that way
 * to avoid having to reacquire a lock we already released).
 */
static bool
_bt_unlink_halfdead_page(Relation rel, Buffer leafbuf, BlockNumber scanblkno,
						 bool *rightsib_empty, TransactionId *oldestBtpoXact,
						 uint32 *ndeleted)
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
	IndexTuple	leafhikey;
	BlockNumber nextchild;

	page = BufferGetPage(leafbuf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);

	Assert(P_ISLEAF(opaque) && P_ISHALFDEAD(opaque));

	/*
	 * Remember some information about the leaf page.
	 */
	itemid = PageGetItemId(page, P_HIKEY);
	leafhikey = (IndexTuple) PageGetItem(page, itemid);
	leafleftsib = opaque->btpo_prev;
	leafrightsib = opaque->btpo_next;

	LockBuffer(leafbuf, BUFFER_LOCK_UNLOCK);

	/*
	 * Check here, as calling loops will have locks held, preventing
	 * interrupts from being processed.
	 */
	CHECK_FOR_INTERRUPTS();

	/*
	 * If the leaf page still has a parent pointing to it (or a chain of
	 * parents), we don't unlink the leaf page yet, but the topmost remaining
	 * parent in the branch.  Set 'target' and 'buf' to reference the page
	 * actually being unlinked.
	 */
	target = BTreeTupleGetTopParent(leafhikey);

	if (target != InvalidBlockNumber)
	{
		Assert(target != leafblkno);

		/* fetch the block number of the topmost parent's left sibling */
		buf = _bt_getbuf(rel, target, BT_READ);
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
		target = leafblkno;

		buf = leafbuf;
		leftsib = leafleftsib;
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
				elog(LOG, "no left sibling (concurrent deletion?) of block %u in \"%s\"",
					 target,
					 RelationGetRelationName(rel));
				if (target != leafblkno)
				{
					/* we have only a pin on target, but pin+lock on leafbuf */
					ReleaseBuffer(buf);
					_bt_relbuf(rel, leafbuf);
				}
				else
				{
					/* we have only a pin on leafbuf */
					ReleaseBuffer(leafbuf);
				}
				return false;
			}

			CHECK_FOR_INTERRUPTS();

			lbuf = _bt_getbuf(rel, leftsib, BT_WRITE);
			page = BufferGetPage(lbuf);
			opaque = (BTPageOpaque) PageGetSpecialPointer(page);
		}
	}
	else
		lbuf = InvalidBuffer;

	/*
	 * Next write-lock the target page itself.  It's okay to take a write lock
	 * rather than a superexclusive lock, since no scan will stop on an empty
	 * page.
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

		/* remember the next non-leaf child down in the branch. */
		itemid = PageGetItemId(page, P_FIRSTDATAKEY(opaque));
		nextchild = BTreeInnerTupleGetDownLink((IndexTuple) PageGetItem(page, itemid));
		if (nextchild == leafblkno)
			nextchild = InvalidBlockNumber;
	}

	/*
	 * And next write-lock the (current) right sibling.
	 */
	rightsib = opaque->btpo_next;
	rbuf = _bt_getbuf(rel, rightsib, BT_WRITE);
	page = BufferGetPage(rbuf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);

	/*
	 * Validate target's right sibling page.  Its left link must point back to
	 * the target page.
	 */
	if (opaque->btpo_prev != target)
	{
		/*
		 * This is known to fail in the field; sibling link corruption is
		 * relatively common.  Press on with vacuuming rather than just
		 * throwing an ERROR (same approach used for left-sibling's-right-link
		 * validation check a moment ago).
		 */
		ereport(LOG,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg_internal("right sibling's left-link doesn't match: "
								 "right sibling %u of target %u with leafblkno %u "
								 "and scanblkno %u spuriously links to non-target %u "
								 "on level %u of index \"%s\"",
								 rightsib, target, leafblkno,
								 scanblkno, opaque->btpo_prev,
								 targetlevel, RelationGetRelationName(rel))));

		/* Must release all pins and locks on failure exit */
		if (BufferIsValid(lbuf))
			_bt_relbuf(rel, lbuf);
		_bt_relbuf(rel, rbuf);
		_bt_relbuf(rel, buf);
		if (target != leafblkno)
			_bt_relbuf(rel, leafbuf);

		return false;
	}

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
		BTreeTupleSetTopParent(leafhikey, nextchild);

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
	Assert(P_ISHALFDEAD(opaque) || !P_ISLEAF(opaque));
	opaque->btpo_flags &= ~BTP_HALF_DEAD;
	opaque->btpo_flags |= BTP_DELETED;
	opaque->btpo.xact = ReadNewTransactionId();

	/* And update the metapage, if needed */
	if (BufferIsValid(metabuf))
	{
		/* upgrade metapage if needed */
		if (metad->btm_version < BTREE_NOVAC_VERSION)
			_bt_upgrademetapage(metapg);
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

		XLogBeginInsert();

		XLogRegisterBuffer(0, buf, REGBUF_WILL_INIT);
		if (BufferIsValid(lbuf))
			XLogRegisterBuffer(1, lbuf, REGBUF_STANDARD);
		XLogRegisterBuffer(2, rbuf, REGBUF_STANDARD);
		if (target != leafblkno)
			XLogRegisterBuffer(3, leafbuf, REGBUF_WILL_INIT);

		/* information on the unlinked block */
		xlrec.leftsib = leftsib;
		xlrec.rightsib = rightsib;
		xlrec.btpo_xact = opaque->btpo.xact;

		/* information needed to recreate the leaf block (if not the target) */
		xlrec.leafleftsib = leafleftsib;
		xlrec.leafrightsib = leafrightsib;
		xlrec.topparent = nextchild;

		XLogRegisterData((char *) &xlrec, SizeOfBtreeUnlinkPage);

		if (BufferIsValid(metabuf))
		{
			XLogRegisterBuffer(4, metabuf, REGBUF_WILL_INIT | REGBUF_STANDARD);

			Assert(metad->btm_version >= BTREE_NOVAC_VERSION);
			xlmeta.version = metad->btm_version;
			xlmeta.root = metad->btm_root;
			xlmeta.level = metad->btm_level;
			xlmeta.fastroot = metad->btm_fastroot;
			xlmeta.fastlevel = metad->btm_fastlevel;
			xlmeta.oldest_btpo_xact = metad->btm_oldest_btpo_xact;
			xlmeta.last_cleanup_num_heap_tuples = metad->btm_last_cleanup_num_heap_tuples;

			XLogRegisterBufData(4, (char *) &xlmeta, sizeof(xl_btree_metadata));
			xlinfo = XLOG_BTREE_UNLINK_PAGE_META;
		}
		else
			xlinfo = XLOG_BTREE_UNLINK_PAGE;

		recptr = XLogInsert(RM_BTREE_ID, xlinfo);

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

	if (!TransactionIdIsValid(*oldestBtpoXact) ||
		TransactionIdPrecedes(opaque->btpo.xact, *oldestBtpoXact))
		*oldestBtpoXact = opaque->btpo.xact;

	/*
	 * If btvacuumscan won't revisit this page in a future btvacuumpage call
	 * and count it as deleted then, we count it as deleted by current
	 * btvacuumpage call
	 */
	if (target <= scanblkno)
		(*ndeleted)++;

	/*
	 * Release the target, if it was not the leaf block.  The leaf is always
	 * kept locked.
	 */
	if (target != leafblkno)
		_bt_relbuf(rel, buf);

	return true;
}
