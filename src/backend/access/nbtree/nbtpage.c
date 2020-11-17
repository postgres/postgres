/*-------------------------------------------------------------------------
 *
 * nbtpage.c
 *	  BTree-specific page management code for the Postgres btree access
 *	  method.
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
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
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "storage/indexfsm.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "utils/memdebug.h"
#include "utils/snapmgr.h"

static BTMetaPageData *_bt_getmeta(Relation rel, Buffer metabuf);
static void _bt_log_reuse_page(Relation rel, BlockNumber blkno,
							   TransactionId latestRemovedXid);
static TransactionId _bt_xid_horizon(Relation rel, Relation heapRel, Page page,
									 OffsetNumber *deletable, int ndeletable);
static bool _bt_mark_page_halfdead(Relation rel, Buffer leafbuf,
								   BTStack stack);
static bool _bt_unlink_halfdead_page(Relation rel, Buffer leafbuf,
									 BlockNumber scanblkno,
									 bool *rightsib_empty,
									 TransactionId *oldestBtpoXact,
									 uint32 *ndeleted);
static bool _bt_lock_subtree_parent(Relation rel, BlockNumber child,
									BTStack stack,
									Buffer *subtreeparent,
									OffsetNumber *poffset,
									BlockNumber *topparent,
									BlockNumber *topparentrightsib);

/*
 *	_bt_initmetapage() -- Fill a page buffer with a correct metapage image
 */
void
_bt_initmetapage(Page page, BlockNumber rootbknum, uint32 level,
				 bool allequalimage)
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
	metad->btm_allequalimage = allequalimage;

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
	/* Only a REINDEX can set this field */
	Assert(!metad->btm_allequalimage);
	metad->btm_allequalimage = false;

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
	_bt_unlockbuf(rel, metabuf);
	_bt_lockbuf(rel, metabuf, BT_WRITE);

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
		md.allequalimage = metad->btm_allequalimage;

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
 *		itself.  If no root page exists yet, we have to create one.
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
		Assert(!metad->btm_allequalimage ||
			   metad->btm_version > BTREE_NOVAC_VERSION);
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
		_bt_unlockbuf(rel, metabuf);
		_bt_lockbuf(rel, metabuf, BT_WRITE);

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
			md.allequalimage = metad->btm_allequalimage;

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
		_bt_unlockbuf(rel, rootbuf);
		_bt_lockbuf(rel, rootbuf, BT_READ);

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
	Assert(!metad->btm_allequalimage ||
		   metad->btm_version > BTREE_NOVAC_VERSION);
	Assert(metad->btm_fastroot != P_NONE);

	return metad->btm_fastlevel;
}

/*
 *	_bt_metaversion() -- Get version/status info from metapage.
 *
 *		Sets caller's *heapkeyspace and *allequalimage arguments using data
 *		from the B-Tree metapage (could be locally-cached version).  This
 *		information needs to be stashed in insertion scankey, so we provide a
 *		single function that fetches both at once.
 *
 *		This is used to determine the rules that must be used to descend a
 *		btree.  Version 4 indexes treat heap TID as a tiebreaker attribute.
 *		pg_upgrade'd version 3 indexes need extra steps to preserve reasonable
 *		performance when inserting a new BTScanInsert-wise duplicate tuple
 *		among many leaf pages already full of such duplicates.
 *
 *		Also sets allequalimage field, which indicates whether or not it is
 *		safe to apply deduplication.  We rely on the assumption that
 *		btm_allequalimage will be zero'ed on heapkeyspace indexes that were
 *		pg_upgrade'd from Postgres 12.
 */
void
_bt_metaversion(Relation rel, bool *heapkeyspace, bool *allequalimage)
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
			*heapkeyspace = metad->btm_version > BTREE_NOVAC_VERSION;
			*allequalimage = metad->btm_allequalimage;

			_bt_relbuf(rel, metabuf);
			return;
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
	Assert(!metad->btm_allequalimage ||
		   metad->btm_version > BTREE_NOVAC_VERSION);
	Assert(metad->btm_fastroot != P_NONE);

	*heapkeyspace = metad->btm_version > BTREE_NOVAC_VERSION;
	*allequalimage = metad->btm_allequalimage;
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
 *		The general rule in nbtree is that it's never okay to access a
 *		page without holding both a buffer pin and a buffer lock on
 *		the page's buffer.
 *
 *		When this routine returns, the appropriate lock is set on the
 *		requested buffer and its reference count has been incremented
 *		(ie, the buffer is "locked and pinned").  Also, we apply
 *		_bt_checkpage to sanity-check the page (except in P_NEW case),
 *		and perform Valgrind client requests that help Valgrind detect
 *		unsafe page accesses.
 *
 *		Note: raw LockBuffer() calls are disallowed in nbtree; all
 *		buffer lock requests need to go through wrapper functions such
 *		as _bt_lockbuf().
 */
Buffer
_bt_getbuf(Relation rel, BlockNumber blkno, int access)
{
	Buffer		buf;

	if (blkno != P_NEW)
	{
		/* Read an existing block of the relation */
		buf = ReadBuffer(rel, blkno);
		_bt_lockbuf(rel, buf, access);
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
			if (_bt_conditionallockbuf(rel, buf))
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
		_bt_lockbuf(rel, buf, BT_WRITE);

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
		_bt_unlockbuf(rel, obuf);
	buf = ReleaseAndReadBuffer(obuf, rel, blkno);
	_bt_lockbuf(rel, buf, access);

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
	_bt_unlockbuf(rel, buf);
	ReleaseBuffer(buf);
}

/*
 *	_bt_lockbuf() -- lock a pinned buffer.
 *
 * Lock is acquired without acquiring another pin.  This is like a raw
 * LockBuffer() call, but performs extra steps needed by Valgrind.
 *
 * Note: Caller may need to call _bt_checkpage() with buf when pin on buf
 * wasn't originally acquired in _bt_getbuf() or _bt_relandgetbuf().
 */
void
_bt_lockbuf(Relation rel, Buffer buf, int access)
{
	/* LockBuffer() asserts that pin is held by this backend */
	LockBuffer(buf, access);

	/*
	 * It doesn't matter that _bt_unlockbuf() won't get called in the
	 * event of an nbtree error (e.g. a unique violation error).  That
	 * won't cause Valgrind false positives.
	 *
	 * The nbtree client requests are superimposed on top of the
	 * bufmgr.c buffer pin client requests.  In the event of an nbtree
	 * error the buffer will certainly get marked as defined when the
	 * backend once again acquires its first pin on the buffer. (Of
	 * course, if the backend never touches the buffer again then it
	 * doesn't matter that it remains non-accessible to Valgrind.)
	 *
	 * Note: When an IndexTuple C pointer gets computed using an
	 * ItemId read from a page while a lock was held, the C pointer
	 * becomes unsafe to dereference forever as soon as the lock is
	 * released.  Valgrind can only detect cases where the pointer
	 * gets dereferenced with no _current_ lock/pin held, though.
	 */
	if (!RelationUsesLocalBuffers(rel))
		VALGRIND_MAKE_MEM_DEFINED(BufferGetPage(buf), BLCKSZ);
}

/*
 *	_bt_unlockbuf() -- unlock a pinned buffer.
 */
void
_bt_unlockbuf(Relation rel, Buffer buf)
{
	/*
	 * Buffer is pinned and locked, which means that it is expected to be
	 * defined and addressable.  Check that proactively.
	 */
	VALGRIND_CHECK_MEM_IS_DEFINED(BufferGetPage(buf), BLCKSZ);

	/* LockBuffer() asserts that pin is held by this backend */
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);

	if (!RelationUsesLocalBuffers(rel))
		VALGRIND_MAKE_MEM_NOACCESS(BufferGetPage(buf), BLCKSZ);
}

/*
 *	_bt_conditionallockbuf() -- conditionally BT_WRITE lock pinned
 *	buffer.
 *
 * Note: Caller may need to call _bt_checkpage() with buf when pin on buf
 * wasn't originally acquired in _bt_getbuf() or _bt_relandgetbuf().
 */
bool
_bt_conditionallockbuf(Relation rel, Buffer buf)
{
	/* ConditionalLockBuffer() asserts that pin is held by this backend */
	if (!ConditionalLockBuffer(buf))
		return false;

	if (!RelationUsesLocalBuffers(rel))
		VALGRIND_MAKE_MEM_DEFINED(BufferGetPage(buf), BLCKSZ);

	return true;
}

/*
 *	_bt_upgradelockbufcleanup() -- upgrade lock to super-exclusive/cleanup
 *	lock.
 */
void
_bt_upgradelockbufcleanup(Relation rel, Buffer buf)
{
	/*
	 * Buffer is pinned and locked, which means that it is expected to be
	 * defined and addressable.  Check that proactively.
	 */
	VALGRIND_CHECK_MEM_IS_DEFINED(BufferGetPage(buf), BLCKSZ);

	/* LockBuffer() asserts that pin is held by this backend */
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	LockBufferForCleanup(buf);
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
		GlobalVisCheckRemovableXid(NULL, opaque->btpo.xact))
		return true;
	return false;
}

/*
 * Delete item(s) from a btree leaf page during VACUUM.
 *
 * This routine assumes that the caller has a super-exclusive write lock on
 * the buffer.  Also, the given deletable and updatable arrays *must* be
 * sorted in ascending order.
 *
 * Routine deals with deleting TIDs when some (but not all) of the heap TIDs
 * in an existing posting list item are to be removed by VACUUM.  This works
 * by updating/overwriting an existing item with caller's new version of the
 * item (a version that lacks the TIDs that are to be deleted).
 *
 * We record VACUUMs and b-tree deletes differently in WAL.  Deletes must
 * generate their own latestRemovedXid by accessing the heap directly, whereas
 * VACUUMs rely on the initial heap scan taking care of it indirectly.  Also,
 * only VACUUM can perform granular deletes of individual TIDs in posting list
 * tuples.
 */
void
_bt_delitems_vacuum(Relation rel, Buffer buf,
					OffsetNumber *deletable, int ndeletable,
					BTVacuumPosting *updatable, int nupdatable)
{
	Page		page = BufferGetPage(buf);
	BTPageOpaque opaque;
	Size		itemsz;
	char	   *updatedbuf = NULL;
	Size		updatedbuflen = 0;
	OffsetNumber updatedoffsets[MaxIndexTuplesPerPage];

	/* Shouldn't be called unless there's something to do */
	Assert(ndeletable > 0 || nupdatable > 0);

	for (int i = 0; i < nupdatable; i++)
	{
		/* Replace work area IndexTuple with updated version */
		_bt_update_posting(updatable[i]);

		/* Maintain array of updatable page offsets for WAL record */
		updatedoffsets[i] = updatable[i]->updatedoffset;
	}

	/* XLOG stuff -- allocate and fill buffer before critical section */
	if (nupdatable > 0 && RelationNeedsWAL(rel))
	{
		Size		offset = 0;

		for (int i = 0; i < nupdatable; i++)
		{
			BTVacuumPosting vacposting = updatable[i];

			itemsz = SizeOfBtreeUpdate +
				vacposting->ndeletedtids * sizeof(uint16);
			updatedbuflen += itemsz;
		}

		updatedbuf = palloc(updatedbuflen);
		for (int i = 0; i < nupdatable; i++)
		{
			BTVacuumPosting vacposting = updatable[i];
			xl_btree_update update;

			update.ndeletedtids = vacposting->ndeletedtids;
			memcpy(updatedbuf + offset, &update.ndeletedtids,
				   SizeOfBtreeUpdate);
			offset += SizeOfBtreeUpdate;

			itemsz = update.ndeletedtids * sizeof(uint16);
			memcpy(updatedbuf + offset, vacposting->deletetids, itemsz);
			offset += itemsz;
		}
	}

	/* No ereport(ERROR) until changes are logged */
	START_CRIT_SECTION();

	/*
	 * Handle posting tuple updates.
	 *
	 * Deliberately do this before handling simple deletes.  If we did it the
	 * other way around (i.e. WAL record order -- simple deletes before
	 * updates) then we'd have to make compensating changes to the 'updatable'
	 * array of offset numbers.
	 *
	 * PageIndexTupleOverwrite() won't unset each item's LP_DEAD bit when it
	 * happens to already be set.  It's important that we not interfere with
	 * _bt_delitems_delete().
	 */
	for (int i = 0; i < nupdatable; i++)
	{
		OffsetNumber updatedoffset = updatedoffsets[i];
		IndexTuple	itup;

		itup = updatable[i]->itup;
		itemsz = MAXALIGN(IndexTupleSize(itup));
		if (!PageIndexTupleOverwrite(page, updatedoffset, (Item) itup,
									 itemsz))
			elog(PANIC, "failed to update partially dead item in block %u of index \"%s\"",
				 BufferGetBlockNumber(buf), RelationGetRelationName(rel));
	}

	/* Now handle simple deletes of entire tuples */
	if (ndeletable > 0)
		PageIndexMultiDelete(page, deletable, ndeletable);

	/*
	 * We can clear the vacuum cycle ID since this page has certainly been
	 * processed by the current vacuum scan.
	 */
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	opaque->btpo_cycleid = 0;

	/*
	 * Clear the BTP_HAS_GARBAGE page flag.
	 *
	 * This flag indicates the presence of LP_DEAD items on the page (though
	 * not reliably).  Note that we only trust it with pg_upgrade'd
	 * !heapkeyspace indexes.  That's why clearing it here won't usually
	 * interfere with _bt_delitems_delete().
	 */
	opaque->btpo_flags &= ~BTP_HAS_GARBAGE;

	MarkBufferDirty(buf);

	/* XLOG stuff */
	if (RelationNeedsWAL(rel))
	{
		XLogRecPtr	recptr;
		xl_btree_vacuum xlrec_vacuum;

		xlrec_vacuum.ndeleted = ndeletable;
		xlrec_vacuum.nupdated = nupdatable;

		XLogBeginInsert();
		XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
		XLogRegisterData((char *) &xlrec_vacuum, SizeOfBtreeVacuum);

		if (ndeletable > 0)
			XLogRegisterBufData(0, (char *) deletable,
								ndeletable * sizeof(OffsetNumber));

		if (nupdatable > 0)
		{
			XLogRegisterBufData(0, (char *) updatedoffsets,
								nupdatable * sizeof(OffsetNumber));
			XLogRegisterBufData(0, updatedbuf, updatedbuflen);
		}

		recptr = XLogInsert(RM_BTREE_ID, XLOG_BTREE_VACUUM);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	/* can't leak memory here */
	if (updatedbuf != NULL)
		pfree(updatedbuf);
	/* free tuples generated by calling _bt_update_posting() */
	for (int i = 0; i < nupdatable; i++)
		pfree(updatable[i]->itup);
}

/*
 * Delete item(s) from a btree leaf page during single-page cleanup.
 *
 * This routine assumes that the caller has pinned and write locked the
 * buffer.  Also, the given deletable array *must* be sorted in ascending
 * order.
 *
 * This is nearly the same as _bt_delitems_vacuum as far as what it does to
 * the page, but it needs to generate its own latestRemovedXid by accessing
 * the heap.  This is used by the REDO routine to generate recovery conflicts.
 * Also, it doesn't handle posting list tuples unless the entire tuple can be
 * deleted as a whole (since there is only one LP_DEAD bit per line pointer).
 */
void
_bt_delitems_delete(Relation rel, Buffer buf,
					OffsetNumber *deletable, int ndeletable,
					Relation heapRel)
{
	Page		page = BufferGetPage(buf);
	BTPageOpaque opaque;
	TransactionId latestRemovedXid = InvalidTransactionId;

	/* Shouldn't be called unless there's something to do */
	Assert(ndeletable > 0);

	if (XLogStandbyInfoActive() && RelationNeedsWAL(rel))
		latestRemovedXid =
			_bt_xid_horizon(rel, heapRel, page, deletable, ndeletable);

	/* No ereport(ERROR) until changes are logged */
	START_CRIT_SECTION();

	/* Fix the page */
	PageIndexMultiDelete(page, deletable, ndeletable);

	/*
	 * Unlike _bt_delitems_vacuum, we *must not* clear the vacuum cycle ID,
	 * because this is not called by VACUUM
	 */
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);

	/*
	 * Clear the BTP_HAS_GARBAGE page flag.
	 *
	 * This flag indicates the presence of LP_DEAD items on the page (though
	 * not reliably).  Note that we only trust it with pg_upgrade'd
	 * !heapkeyspace indexes.
	 */
	opaque->btpo_flags &= ~BTP_HAS_GARBAGE;

	MarkBufferDirty(buf);

	/* XLOG stuff */
	if (RelationNeedsWAL(rel))
	{
		XLogRecPtr	recptr;
		xl_btree_delete xlrec_delete;

		xlrec_delete.latestRemovedXid = latestRemovedXid;
		xlrec_delete.ndeleted = ndeletable;

		XLogBeginInsert();
		XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
		XLogRegisterData((char *) &xlrec_delete, SizeOfBtreeDelete);

		/*
		 * The deletable array is not in the buffer, but pretend that it is.
		 * When XLogInsert stores the whole buffer, the array need not be
		 * stored too.
		 */
		XLogRegisterBufData(0, (char *) deletable,
							ndeletable * sizeof(OffsetNumber));

		recptr = XLogInsert(RM_BTREE_ID, XLOG_BTREE_DELETE);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();
}

/*
 * Get the latestRemovedXid from the table entries pointed to by the non-pivot
 * tuples being deleted.
 *
 * This is a specialized version of index_compute_xid_horizon_for_tuples().
 * It's needed because btree tuples don't always store table TID using the
 * standard index tuple header field.
 */
static TransactionId
_bt_xid_horizon(Relation rel, Relation heapRel, Page page,
				OffsetNumber *deletable, int ndeletable)
{
	TransactionId latestRemovedXid = InvalidTransactionId;
	int			spacenhtids;
	int			nhtids;
	ItemPointer htids;

	/* Array will grow iff there are posting list tuples to consider */
	spacenhtids = ndeletable;
	nhtids = 0;
	htids = (ItemPointer) palloc(sizeof(ItemPointerData) * spacenhtids);
	for (int i = 0; i < ndeletable; i++)
	{
		ItemId		itemid;
		IndexTuple	itup;

		itemid = PageGetItemId(page, deletable[i]);
		itup = (IndexTuple) PageGetItem(page, itemid);

		Assert(ItemIdIsDead(itemid));
		Assert(!BTreeTupleIsPivot(itup));

		if (!BTreeTupleIsPosting(itup))
		{
			if (nhtids + 1 > spacenhtids)
			{
				spacenhtids *= 2;
				htids = (ItemPointer)
					repalloc(htids, sizeof(ItemPointerData) * spacenhtids);
			}

			Assert(ItemPointerIsValid(&itup->t_tid));
			ItemPointerCopy(&itup->t_tid, &htids[nhtids]);
			nhtids++;
		}
		else
		{
			int			nposting = BTreeTupleGetNPosting(itup);

			if (nhtids + nposting > spacenhtids)
			{
				spacenhtids = Max(spacenhtids * 2, nhtids + nposting);
				htids = (ItemPointer)
					repalloc(htids, sizeof(ItemPointerData) * spacenhtids);
			}

			for (int j = 0; j < nposting; j++)
			{
				ItemPointer htid = BTreeTupleGetPostingN(itup, j);

				Assert(ItemPointerIsValid(htid));
				ItemPointerCopy(htid, &htids[nhtids]);
				nhtids++;
			}
		}
	}

	Assert(nhtids >= ndeletable);

	latestRemovedXid =
		table_compute_xid_horizon_for_tuples(heapRel, htids, nhtids);

	pfree(htids);

	return latestRemovedXid;
}

/*
 * Check that leftsib page (the btpo_prev of target page) is not marked with
 * INCOMPLETE_SPLIT flag.  Used during page deletion.
 *
 * Returning true indicates that page flag is set in leftsib (which is
 * definitely still the left sibling of target).  When that happens, the
 * target doesn't have a downlink in parent, and the page deletion algorithm
 * isn't prepared to handle that.  Deletion of the target page (or the whole
 * subtree that contains the target page) cannot take place.
 *
 * Caller should not have a lock on the target page itself, since pages on the
 * same level must always be locked left to right to avoid deadlocks.
 */
static bool
_bt_leftsib_splitflag(Relation rel, BlockNumber leftsib, BlockNumber target)
{
	Buffer		buf;
	Page		page;
	BTPageOpaque opaque;
	bool		result;

	/* Easy case: No left sibling */
	if (leftsib == P_NONE)
		return false;

	buf = _bt_getbuf(rel, leftsib, BT_READ);
	page = BufferGetPage(buf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);

	/*
	 * If the left sibling was concurrently split, so that its next-pointer
	 * doesn't point to the current page anymore, the split that created
	 * target must be completed.  Caller can reasonably expect that there will
	 * be a downlink to the target page that it can relocate using its stack.
	 * (We don't allow splitting an incompletely split page again until the
	 * previous split has been completed.)
	 */
	result = (opaque->btpo_next == target && P_INCOMPLETE_SPLIT(opaque));
	_bt_relbuf(rel, buf);

	return result;
}

/*
 * Check that leafrightsib page (the btpo_next of target leaf page) is not
 * marked with ISHALFDEAD flag.  Used during page deletion.
 *
 * Returning true indicates that page flag is set in leafrightsib, so page
 * deletion cannot go ahead.  Our caller is not prepared to deal with the case
 * where the parent page does not have a pivot tuples whose downlink points to
 * leafrightsib (due to an earlier interrupted VACUUM operation).  It doesn't
 * seem worth going to the trouble of teaching our caller to deal with it.
 * The situation will be resolved after VACUUM finishes the deletion of the
 * half-dead page (when a future VACUUM operation reaches the target page
 * again).
 *
 * _bt_leftsib_splitflag() is called for both leaf pages and internal pages.
 * _bt_rightsib_halfdeadflag() is only called for leaf pages, though.  This is
 * okay because of the restriction on deleting pages that are the rightmost
 * page of their parent (i.e. that such deletions can only take place when the
 * entire subtree must be deleted).  The leaf level check made here will apply
 * to a right "cousin" leaf page rather than a simple right sibling leaf page
 * in cases where caller actually goes on to attempt deleting pages that are
 * above the leaf page.  The right cousin leaf page is representative of the
 * left edge of the subtree to the right of the to-be-deleted subtree as a
 * whole, which is exactly the condition that our caller cares about.
 * (Besides, internal pages are never marked half-dead, so it isn't even
 * possible to _directly_ assess if an internal page is part of some other
 * to-be-deleted subtree.)
 */
static bool
_bt_rightsib_halfdeadflag(Relation rel, BlockNumber leafrightsib)
{
	Buffer		buf;
	Page		page;
	BTPageOpaque opaque;
	bool		result;

	Assert(leafrightsib != P_NONE);

	buf = _bt_getbuf(rel, leafrightsib, BT_READ);
	page = BufferGetPage(buf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);

	Assert(P_ISLEAF(opaque) && !P_ISDELETED(opaque));
	result = P_ISHALFDEAD(opaque);
	_bt_relbuf(rel, buf);

	return result;
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
	 * left sibling does not have its INCOMPLETE_SPLIT flag set, including
	 * when the current target page is to the right of caller's initial page
	 * (the scanblkno page).
	 */
	BTStack		stack = NULL;

	for (;;)
	{
		page = BufferGetPage(leafbuf);
		opaque = (BTPageOpaque) PageGetSpecialPointer(page);

		/*
		 * Internal pages are never deleted directly, only as part of deleting
		 * the whole subtree all the way down to leaf level.
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
		 * the left sibling doesn't have its INCOMPLETE_SPLIT flag set using
		 * _bt_leftsib_splitflag().  On the first iteration, we temporarily
		 * release the lock on scanblkno/leafbuf, check the left sibling, and
		 * construct a search stack to scanblkno.  On subsequent iterations,
		 * we know we stepped right from a page that passed these tests, so
		 * it's OK.
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
		 * page, if we are going to delete a taller subtree), and mark the
		 * leafbuf page half-dead
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
				BlockNumber leftsib,
							leafblkno;
				Buffer		sleafbuf;

				itemid = PageGetItemId(page, P_HIKEY);
				targetkey = CopyIndexTuple((IndexTuple) PageGetItem(page, itemid));

				leftsib = opaque->btpo_prev;
				leafblkno = BufferGetBlockNumber(leafbuf);

				/*
				 * To avoid deadlocks, we'd better drop the leaf page lock
				 * before going further.
				 */
				_bt_unlockbuf(rel, leafbuf);

				/*
				 * Check that the left sibling of leafbuf (if any) is not
				 * marked with INCOMPLETE_SPLIT flag before proceeding
				 */
				Assert(leafblkno == scanblkno);
				if (_bt_leftsib_splitflag(rel, leftsib, leafblkno))
				{
					ReleaseBuffer(leafbuf);
					Assert(ndeleted == 0);
					return ndeleted;
				}

				/* we need an insertion scan key for the search, so build one */
				itup_key = _bt_mkscankey(rel, targetkey);
				/* find the leftmost leaf page with matching pivot/high key */
				itup_key->pivotsearch = true;
				stack = _bt_search(rel, itup_key, &sleafbuf, BT_READ, NULL);
				/* won't need a second lock or pin on leafbuf */
				_bt_relbuf(rel, sleafbuf);

				/*
				 * Re-lock the leaf page, and start over to use our stack
				 * within _bt_mark_page_halfdead.  We must do it that way
				 * because it's possible that leafbuf can no longer be
				 * deleted.  We need to recheck.
				 *
				 * Note: We can't simply hold on to the sleafbuf lock instead,
				 * because it's barely possible that sleafbuf is not the same
				 * page as leafbuf.  This happens when leafbuf split after our
				 * original lock was dropped, but before _bt_search finished
				 * its descent.  We rely on the assumption that we'll find
				 * leafbuf isn't safe to delete anymore in this scenario.
				 * (Page deletion can cope with the stack being to the left of
				 * leafbuf, but not to the right of leafbuf.)
				 */
				_bt_lockbuf(rel, leafbuf, BT_WRITE);
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
		 * _bt_unlink_halfdead_page unlinks the topmost page from the subtree,
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
		 *
		 * Note: This relies on the assumption that _bt_getstackbuf() will be
		 * able to reuse our original descent stack with a different child
		 * block (provided that the child block is to the right of the
		 * original leaf page reached by _bt_search()). It will even update
		 * the descent stack each time we loop around, avoiding repeated work.
		 */
		if (!rightsib_empty)
			break;

		leafbuf = _bt_getbuf(rel, rightsib, BT_WRITE);
	}

	return ndeleted;
}

/*
 * First stage of page deletion.
 *
 * Establish the height of the to-be-deleted subtree with leafbuf at its
 * lowest level, remove the downlink to the subtree, and mark leafbuf
 * half-dead.  The final to-be-deleted subtree is usually just leafbuf itself,
 * but may include additional internal pages (at most one per level of the
 * tree below the root).
 *
 * Returns 'false' if leafbuf is unsafe to delete, usually because leafbuf is
 * the rightmost child of its parent (and parent has more than one downlink).
 * Returns 'true' when the first stage of page deletion completed
 * successfully.
 */
static bool
_bt_mark_page_halfdead(Relation rel, Buffer leafbuf, BTStack stack)
{
	BlockNumber leafblkno;
	BlockNumber leafrightsib;
	BlockNumber topparent;
	BlockNumber topparentrightsib;
	ItemId		itemid;
	Page		page;
	BTPageOpaque opaque;
	Buffer		subtreeparent;
	OffsetNumber poffset;
	OffsetNumber nextoffset;
	IndexTuple	itup;
	IndexTupleData trunctuple;

	page = BufferGetPage(leafbuf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);

	Assert(!P_RIGHTMOST(opaque) && !P_ISROOT(opaque) &&
		   P_ISLEAF(opaque) && !P_IGNORE(opaque) &&
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
	 * delete the downlink.  It would fail the "right sibling of target page
	 * is also the next child in parent page" cross-check below.
	 */
	if (_bt_rightsib_halfdeadflag(rel, leafrightsib))
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
	 * and lock the parent of the root of the to-be-deleted subtree (the
	 * "subtree parent").  _bt_lock_subtree_parent() locks the subtree parent
	 * for us.  We remove the downlink to the "top parent" page (subtree root
	 * page) from the subtree parent page below.
	 *
	 * Initialize topparent to be leafbuf page now.  The final to-be-deleted
	 * subtree is often a degenerate one page subtree consisting only of the
	 * leafbuf page.  When that happens, the leafbuf page is the final subtree
	 * root page/top parent page.
	 */
	topparent = leafblkno;
	topparentrightsib = leafrightsib;
	if (!_bt_lock_subtree_parent(rel, leafblkno, stack,
								 &subtreeparent, &poffset,
								 &topparent, &topparentrightsib))
		return false;

	/*
	 * Check that the parent-page index items we're about to delete/overwrite
	 * in subtree parent page contain what we expect.  This can fail if the
	 * index has become corrupt for some reason.  We want to throw any error
	 * before entering the critical section --- otherwise it'd be a PANIC.
	 */
	page = BufferGetPage(subtreeparent);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);

#ifdef USE_ASSERT_CHECKING

	/*
	 * This is just an assertion because _bt_lock_subtree_parent should have
	 * guaranteed tuple has the expected contents
	 */
	itemid = PageGetItemId(page, poffset);
	itup = (IndexTuple) PageGetItem(page, itemid);
	Assert(BTreeTupleGetDownLink(itup) == topparent);
#endif

	nextoffset = OffsetNumberNext(poffset);
	itemid = PageGetItemId(page, nextoffset);
	itup = (IndexTuple) PageGetItem(page, itemid);
	if (BTreeTupleGetDownLink(itup) != topparentrightsib)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg_internal("right sibling %u of block %u is not next child %u of block %u in index \"%s\"",
								 topparentrightsib, topparent,
								 BTreeTupleGetDownLink(itup),
								 BufferGetBlockNumber(subtreeparent),
								 RelationGetRelationName(rel))));

	/*
	 * Any insert which would have gone on the leaf block will now go to its
	 * right sibling.  In other words, the key space moves right.
	 */
	PredicateLockPageCombine(rel, leafblkno, leafrightsib);

	/* No ereport(ERROR) until changes are logged */
	START_CRIT_SECTION();

	/*
	 * Update parent of subtree.  We want to delete the downlink to the top
	 * parent page/root of the subtree, and the *following* key.  Easiest way
	 * is to copy the right sibling's downlink over the downlink that points
	 * to top parent page, and then delete the right sibling's original pivot
	 * tuple.
	 *
	 * Lanin and Shasha make the key space move left when deleting a page,
	 * whereas the key space moves right here.  That's why we cannot simply
	 * delete the pivot tuple with the downlink to the top parent page.  See
	 * nbtree/README.
	 */
	page = BufferGetPage(subtreeparent);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);

	itemid = PageGetItemId(page, poffset);
	itup = (IndexTuple) PageGetItem(page, itemid);
	BTreeTupleSetDownLink(itup, topparentrightsib);

	nextoffset = OffsetNumberNext(poffset);
	PageIndexTupleDelete(page, nextoffset);

	/*
	 * Mark the leaf page as half-dead, and stamp it with a link to the top
	 * parent page.  When the leaf page is also the top parent page, the link
	 * is set to InvalidBlockNumber.
	 */
	page = BufferGetPage(leafbuf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	opaque->btpo_flags |= BTP_HALF_DEAD;

	Assert(PageGetMaxOffsetNumber(page) == P_HIKEY);
	MemSet(&trunctuple, 0, sizeof(IndexTupleData));
	trunctuple.t_info = sizeof(IndexTupleData);
	if (topparent != leafblkno)
		BTreeTupleSetTopParent(&trunctuple, topparent);
	else
		BTreeTupleSetTopParent(&trunctuple, InvalidBlockNumber);

	if (!PageIndexTupleOverwrite(page, P_HIKEY, (Item) &trunctuple,
								 IndexTupleSize(&trunctuple)))
		elog(ERROR, "could not overwrite high key in half-dead page");

	/* Must mark buffers dirty before XLogInsert */
	MarkBufferDirty(subtreeparent);
	MarkBufferDirty(leafbuf);

	/* XLOG stuff */
	if (RelationNeedsWAL(rel))
	{
		xl_btree_mark_page_halfdead xlrec;
		XLogRecPtr	recptr;

		xlrec.poffset = poffset;
		xlrec.leafblk = leafblkno;
		if (topparent != leafblkno)
			xlrec.topparent = topparent;
		else
			xlrec.topparent = InvalidBlockNumber;

		XLogBeginInsert();
		XLogRegisterBuffer(0, leafbuf, REGBUF_WILL_INIT);
		XLogRegisterBuffer(1, subtreeparent, REGBUF_STANDARD);

		page = BufferGetPage(leafbuf);
		opaque = (BTPageOpaque) PageGetSpecialPointer(page);
		xlrec.leftblk = opaque->btpo_prev;
		xlrec.rightblk = opaque->btpo_next;

		XLogRegisterData((char *) &xlrec, SizeOfBtreeMarkPageHalfDead);

		recptr = XLogInsert(RM_BTREE_ID, XLOG_BTREE_MARK_PAGE_HALFDEAD);

		page = BufferGetPage(subtreeparent);
		PageSetLSN(page, recptr);
		page = BufferGetPage(leafbuf);
		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	_bt_relbuf(rel, subtreeparent);
	return true;
}

/*
 * Second stage of page deletion.
 *
 * Unlinks a single page (in the subtree undergoing deletion) from its
 * siblings.  Also marks the page deleted.
 *
 * To get rid of the whole subtree, including the leaf page itself, call here
 * until the leaf page is deleted.  The original "top parent" established in
 * the first stage of deletion is deleted in the first call here, while the
 * leaf page is deleted in the last call here.  Note that the leaf page itself
 * is often the initial top parent page.
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
	PageHeader	header;
	BTPageOpaque opaque;
	bool		rightsib_is_rightmost;
	int			targetlevel;
	IndexTuple	leafhikey;
	BlockNumber nextchild;

	page = BufferGetPage(leafbuf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);

	Assert(P_ISLEAF(opaque) && !P_ISDELETED(opaque) && P_ISHALFDEAD(opaque));

	/*
	 * Remember some information about the leaf page.
	 */
	itemid = PageGetItemId(page, P_HIKEY);
	leafhikey = (IndexTuple) PageGetItem(page, itemid);
	target = BTreeTupleGetTopParent(leafhikey);
	leafleftsib = opaque->btpo_prev;
	leafrightsib = opaque->btpo_next;

	_bt_unlockbuf(rel, leafbuf);

	/*
	 * Check here, as calling loops will have locks held, preventing
	 * interrupts from being processed.
	 */
	CHECK_FOR_INTERRUPTS();

	/* Unlink the current top parent of the subtree */
	if (!BlockNumberIsValid(target))
	{
		/* Target is leaf page (or leaf page is top parent, if you prefer) */
		target = leafblkno;

		buf = leafbuf;
		leftsib = leafleftsib;
		targetlevel = 0;
	}
	else
	{
		/* Target is the internal page taken from leaf's top parent link */
		Assert(target != leafblkno);

		/* Fetch the block number of the target's left sibling */
		buf = _bt_getbuf(rel, target, BT_READ);
		page = BufferGetPage(buf);
		opaque = (BTPageOpaque) PageGetSpecialPointer(page);
		leftsib = opaque->btpo_prev;
		targetlevel = opaque->btpo.level;
		Assert(targetlevel > 0);

		/*
		 * To avoid deadlocks, we'd better drop the target page lock before
		 * going further.
		 */
		_bt_unlockbuf(rel, buf);
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
		_bt_lockbuf(rel, leafbuf, BT_WRITE);
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

			/*
			 * It'd be good to check for interrupts here, but it's not easy to
			 * do so because a lock is always held. This block isn't
			 * frequently reached, so hopefully the consequences of not
			 * checking interrupts aren't too bad.
			 */

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
	_bt_lockbuf(rel, buf, BT_WRITE);
	page = BufferGetPage(buf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);

	/*
	 * Check page is still empty etc, else abandon deletion.  This is just for
	 * paranoia's sake; a half-dead page cannot resurrect because there can be
	 * only one vacuum process running at a time.
	 */
	if (P_RIGHTMOST(opaque) || P_ISROOT(opaque) || P_ISDELETED(opaque))
		elog(ERROR, "half-dead page changed status unexpectedly in block %u of index \"%s\"",
			 target, RelationGetRelationName(rel));

	if (opaque->btpo_prev != leftsib)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg_internal("left link changed unexpectedly in block %u of index \"%s\"",
								 target, RelationGetRelationName(rel))));

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

		/* Remember the next non-leaf child down in the subtree */
		itemid = PageGetItemId(page, P_FIRSTDATAKEY(opaque));
		nextchild = BTreeTupleGetDownLink((IndexTuple) PageGetItem(page, itemid));
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
	if (opaque->btpo_prev != target)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg_internal("right sibling's left-link doesn't match: "
								 "block %u links to %u instead of expected %u in index \"%s\"",
								 rightsib, opaque->btpo_prev, target,
								 RelationGetRelationName(rel))));
	rightsib_is_rightmost = P_RIGHTMOST(opaque);
	*rightsib_empty = (P_FIRSTDATAKEY(opaque) > PageGetMaxOffsetNumber(page));

	/*
	 * If we are deleting the next-to-last page on the target's level, then
	 * the rightsib is a candidate to become the new fast root. (In theory, it
	 * might be possible to push the fast root even further down, but the odds
	 * of doing so are slim, and the locking considerations daunting.)
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
	 * subtree.
	 *
	 * Note: We rely on the fact that a buffer pin on the leaf page has been
	 * held since leafhikey was initialized.  This is safe, though only
	 * because the page was already half-dead at that point.  The leaf page
	 * cannot have been modified by any other backend during the period when
	 * no lock was held.
	 */
	if (target != leafblkno)
		BTreeTupleSetTopParent(leafhikey, nextchild);

	/*
	 * Mark the page itself deleted.  It can be recycled when all current
	 * transactions are gone.  Storing GetTopTransactionId() would work, but
	 * we're in VACUUM and would not otherwise have an XID.  Having already
	 * updated links to the target, ReadNewTransactionId() suffices as an
	 * upper bound.  Any scan having retained a now-stale link is advertising
	 * in its PGPROC an xmin less than or equal to the value we read here.  It
	 * will continue to do so, holding back the xmin horizon, for the duration
	 * of that scan.
	 */
	page = BufferGetPage(buf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	Assert(P_ISHALFDEAD(opaque) || !P_ISLEAF(opaque));
	opaque->btpo_flags &= ~BTP_HALF_DEAD;
	opaque->btpo_flags |= BTP_DELETED;
	opaque->btpo.xact = ReadNewTransactionId();

	/*
	 * Remove the remaining tuples on the page.  This keeps things simple for
	 * WAL consistency checking.
	 */
	header = (PageHeader) page;
	header->pd_lower = SizeOfPageHeaderData;
	header->pd_upper = header->pd_special;

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
			xlmeta.allequalimage = metad->btm_allequalimage;

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

	/* If the target is not leafbuf, we're done with it now -- release it */
	if (target != leafblkno)
		_bt_relbuf(rel, buf);

	return true;
}

/*
 * Establish how tall the to-be-deleted subtree will be during the first stage
 * of page deletion.
 *
 * Caller's child argument is the block number of the page caller wants to
 * delete (this is leafbuf's block number, except when we're called
 * recursively).  stack is a search stack leading to it.  Note that we will
 * update the stack entry(s) to reflect current downlink positions --- this is
 * similar to the corresponding point in page split handling.
 *
 * If "first stage" caller cannot go ahead with deleting _any_ pages, returns
 * false.  Returns true on success, in which case caller can use certain
 * details established here to perform the first stage of deletion.  This
 * function is the last point at which page deletion may be deemed unsafe
 * (barring index corruption, or unexpected concurrent page deletions).
 *
 * We write lock the parent of the root of the to-be-deleted subtree for
 * caller on success (i.e. we leave our lock on the *subtreeparent buffer for
 * caller).  Caller will have to remove a downlink from *subtreeparent.  We
 * also set a *subtreeparent offset number in *poffset, to indicate the
 * location of the pivot tuple that contains the relevant downlink.
 *
 * The root of the to-be-deleted subtree is called the "top parent".  Note
 * that the leafbuf page is often the final "top parent" page (you can think
 * of the leafbuf page as a degenerate single page subtree when that happens).
 * Caller should initialize *topparent to the target leafbuf page block number
 * (while *topparentrightsib should be set to leafbuf's right sibling block
 * number).  We will update *topparent (and *topparentrightsib) for caller
 * here, though only when it turns out that caller will delete at least one
 * internal page (i.e. only when caller needs to store a valid link to the top
 * parent block in the leafbuf page using BTreeTupleSetTopParent()).
 */
static bool
_bt_lock_subtree_parent(Relation rel, BlockNumber child, BTStack stack,
						Buffer *subtreeparent, OffsetNumber *poffset,
						BlockNumber *topparent, BlockNumber *topparentrightsib)
{
	BlockNumber parent,
				leftsibparent;
	OffsetNumber parentoffset,
				maxoff;
	Buffer		pbuf;
	Page		page;
	BTPageOpaque opaque;

	/*
	 * Locate the pivot tuple whose downlink points to "child".  Write lock
	 * the parent page itself.
	 */
	pbuf = _bt_getstackbuf(rel, stack, child);
	if (pbuf == InvalidBuffer)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg_internal("failed to re-find parent key in index \"%s\" for deletion target page %u",
								 RelationGetRelationName(rel), child)));
	parent = stack->bts_blkno;
	parentoffset = stack->bts_offset;

	page = BufferGetPage(pbuf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	maxoff = PageGetMaxOffsetNumber(page);
	leftsibparent = opaque->btpo_prev;

	/*
	 * _bt_getstackbuf() completes page splits on returned parent buffer when
	 * required.
	 *
	 * In general it's a bad idea for VACUUM to use up more disk space, which
	 * is why page deletion does not finish incomplete page splits most of the
	 * time.  We allow this limited exception because the risk is much lower,
	 * and the potential downside of not proceeding is much higher:  A single
	 * internal page with the INCOMPLETE_SPLIT flag set might otherwise
	 * prevent us from deleting hundreds of empty leaf pages from one level
	 * down.
	 */
	Assert(!P_INCOMPLETE_SPLIT(opaque));

	if (parentoffset < maxoff)
	{
		/*
		 * Child is not the rightmost child in parent, so it's safe to delete
		 * the subtree whose root/topparent is child page
		 */
		*subtreeparent = pbuf;
		*poffset = parentoffset;
		return true;
	}

	/*
	 * Child is the rightmost child of parent.
	 *
	 * Since it's the rightmost child of parent, deleting the child (or
	 * deleting the subtree whose root/topparent is the child page) is only
	 * safe when it's also possible to delete the parent.
	 */
	Assert(parentoffset == maxoff);
	if (parentoffset != P_FIRSTDATAKEY(opaque) || P_RIGHTMOST(opaque))
	{
		/*
		 * Child isn't parent's only child, or parent is rightmost on its
		 * entire level.  Definitely cannot delete any pages.
		 */
		_bt_relbuf(rel, pbuf);
		return false;
	}

	/*
	 * Now make sure that the parent deletion is itself safe by examining the
	 * child's grandparent page.  Recurse, passing the parent page as the
	 * child page (child's grandparent is the parent on the next level up). If
	 * parent deletion is unsafe, then child deletion must also be unsafe (in
	 * which case caller cannot delete any pages at all).
	 */
	*topparent = parent;
	*topparentrightsib = opaque->btpo_next;

	/*
	 * Release lock on parent before recursing.
	 *
	 * It's OK to release page locks on parent before recursive call locks
	 * grandparent.  An internal page can only acquire an entry if the child
	 * is split, but that cannot happen as long as we still hold a lock on the
	 * leafbuf page.
	 */
	_bt_relbuf(rel, pbuf);

	/*
	 * Before recursing, check that the left sibling of parent (if any) is not
	 * marked with INCOMPLETE_SPLIT flag first (must do so after we drop the
	 * parent lock).
	 *
	 * Note: We deliberately avoid completing incomplete splits here.
	 */
	if (_bt_leftsib_splitflag(rel, leftsibparent, parent))
		return false;

	/* Recurse to examine child page's grandparent page */
	return _bt_lock_subtree_parent(rel, parent, stack->bts_parent,
								   subtreeparent, poffset,
								   topparent, topparentrightsib);
}
