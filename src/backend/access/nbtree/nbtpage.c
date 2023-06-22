/*-------------------------------------------------------------------------
 *
 * nbtpage.c
 *	  BTree-specific page management code for the Postgres btree access
 *	  method.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
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
#include "storage/procarray.h"
#include "utils/memdebug.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

static BTMetaPageData *_bt_getmeta(Relation rel, Buffer metabuf);
static void _bt_log_reuse_page(Relation rel, BlockNumber blkno,
							   FullTransactionId safexid);
static void _bt_delitems_delete(Relation rel, Buffer buf,
								TransactionId latestRemovedXid,
								OffsetNumber *deletable, int ndeletable,
								BTVacuumPosting *updatable, int nupdatable);
static char *_bt_delitems_update(BTVacuumPosting *updatable, int nupdatable,
								 OffsetNumber *updatedoffsets,
								 Size *updatedbuflen, bool needswal);
static bool _bt_mark_page_halfdead(Relation rel, Buffer leafbuf,
								   BTStack stack);
static bool _bt_unlink_halfdead_page(Relation rel, Buffer leafbuf,
									 BlockNumber scanblkno,
									 bool *rightsib_empty,
									 BTVacState *vstate);
static bool _bt_lock_subtree_parent(Relation rel, BlockNumber child,
									BTStack stack,
									Buffer *subtreeparent,
									OffsetNumber *poffset,
									BlockNumber *topparent,
									BlockNumber *topparentrightsib);
static void _bt_pendingfsm_add(BTVacState *vstate, BlockNumber target,
							   FullTransactionId safexid);

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
	metad->btm_last_cleanup_num_delpages = 0;
	metad->btm_last_cleanup_num_heap_tuples = -1.0;
	metad->btm_allequalimage = allequalimage;

	metaopaque = BTPageGetOpaque(page);
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
	metaopaque = BTPageGetOpaque(page);

	/* It must be really a meta page of upgradable version */
	Assert(metaopaque->btpo_flags & BTP_META);
	Assert(metad->btm_version < BTREE_NOVAC_VERSION);
	Assert(metad->btm_version >= BTREE_MIN_VERSION);

	/* Set version number and fill extra fields added into version 3 */
	metad->btm_version = BTREE_NOVAC_VERSION;
	metad->btm_last_cleanup_num_delpages = 0;
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
	metaopaque = BTPageGetOpaque(metapg);
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
 * _bt_vacuum_needs_cleanup() -- Checks if index needs cleanup
 *
 * Called by btvacuumcleanup when btbulkdelete was never called because no
 * index tuples needed to be deleted.
 */
bool
_bt_vacuum_needs_cleanup(Relation rel)
{
	Buffer		metabuf;
	Page		metapg;
	BTMetaPageData *metad;
	uint32		btm_version;
	BlockNumber prev_num_delpages;

	/*
	 * Copy details from metapage to local variables quickly.
	 *
	 * Note that we deliberately avoid using cached version of metapage here.
	 */
	metabuf = _bt_getbuf(rel, BTREE_METAPAGE, BT_READ);
	metapg = BufferGetPage(metabuf);
	metad = BTPageGetMeta(metapg);
	btm_version = metad->btm_version;

	if (btm_version < BTREE_NOVAC_VERSION)
	{
		/*
		 * Metapage needs to be dynamically upgraded to store fields that are
		 * only present when btm_version >= BTREE_NOVAC_VERSION
		 */
		_bt_relbuf(rel, metabuf);
		return true;
	}

	prev_num_delpages = metad->btm_last_cleanup_num_delpages;
	_bt_relbuf(rel, metabuf);

	/*
	 * Trigger cleanup in rare cases where prev_num_delpages exceeds 5% of the
	 * total size of the index.  We can reasonably expect (though are not
	 * guaranteed) to be able to recycle this many pages if we decide to do a
	 * btvacuumscan call during the ongoing btvacuumcleanup.  For further
	 * details see the nbtree/README section on placing deleted pages in the
	 * FSM.
	 */
	if (prev_num_delpages > 0 &&
		prev_num_delpages > RelationGetNumberOfBlocks(rel) / 20)
		return true;

	return false;
}

/*
 * _bt_set_cleanup_info() -- Update metapage for btvacuumcleanup.
 *
 * Called at the end of btvacuumcleanup, when num_delpages value has been
 * finalized.
 */
void
_bt_set_cleanup_info(Relation rel, BlockNumber num_delpages)
{
	Buffer		metabuf;
	Page		metapg;
	BTMetaPageData *metad;

	/*
	 * On-disk compatibility note: The btm_last_cleanup_num_delpages metapage
	 * field started out as a TransactionId field called btm_oldest_btpo_xact.
	 * Both "versions" are just uint32 fields.  It was convenient to repurpose
	 * the field when we began to use 64-bit XIDs in deleted pages.
	 *
	 * It's possible that a pg_upgrade'd database will contain an XID value in
	 * what is now recognized as the metapage's btm_last_cleanup_num_delpages
	 * field.  _bt_vacuum_needs_cleanup() may even believe that this value
	 * indicates that there are lots of pages that it needs to recycle, when
	 * in reality there are only one or two.  The worst that can happen is
	 * that there will be a call to btvacuumscan a little earlier, which will
	 * set btm_last_cleanup_num_delpages to a sane value when we're called.
	 *
	 * Note also that the metapage's btm_last_cleanup_num_heap_tuples field is
	 * no longer used as of PostgreSQL 14.  We set it to -1.0 on rewrite, just
	 * to be consistent.
	 */
	metabuf = _bt_getbuf(rel, BTREE_METAPAGE, BT_READ);
	metapg = BufferGetPage(metabuf);
	metad = BTPageGetMeta(metapg);

	/* Don't miss chance to upgrade index/metapage when BTREE_MIN_VERSION */
	if (metad->btm_version >= BTREE_NOVAC_VERSION &&
		metad->btm_last_cleanup_num_delpages == num_delpages)
	{
		/* Usually means index continues to have num_delpages of 0 */
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
	metad->btm_last_cleanup_num_delpages = num_delpages;
	metad->btm_last_cleanup_num_heap_tuples = -1.0;
	MarkBufferDirty(metabuf);

	/* write wal record if needed */
	if (RelationNeedsWAL(rel))
	{
		xl_btree_metadata md;
		XLogRecPtr	recptr;

		XLogBeginInsert();
		XLogRegisterBuffer(0, metabuf, REGBUF_WILL_INIT | REGBUF_STANDARD);

		Assert(metad->btm_version >= BTREE_NOVAC_VERSION);
		md.version = metad->btm_version;
		md.root = metad->btm_root;
		md.level = metad->btm_level;
		md.fastroot = metad->btm_fastroot;
		md.fastlevel = metad->btm_fastlevel;
		md.last_cleanup_num_delpages = num_delpages;
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
		rootopaque = BTPageGetOpaque(rootpage);

		/*
		 * Since the cache might be stale, we check the page more carefully
		 * here than normal.  We *must* check that it's not deleted. If it's
		 * not alone on its level, then we reject too --- this may be overly
		 * paranoid but better safe than sorry.  Note we don't check P_ISROOT,
		 * because that's not set in a "fast root".
		 */
		if (!P_IGNORE(rootopaque) &&
			rootopaque->btpo_level == rootlevel &&
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
		rootopaque = BTPageGetOpaque(rootpage);
		rootopaque->btpo_prev = rootopaque->btpo_next = P_NONE;
		rootopaque->btpo_flags = (BTP_LEAF | BTP_ROOT);
		rootopaque->btpo_level = 0;
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
		metad->btm_last_cleanup_num_delpages = 0;
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
			md.last_cleanup_num_delpages = 0;
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
			rootopaque = BTPageGetOpaque(rootpage);

			if (!P_IGNORE(rootopaque))
				break;

			/* it's dead, Jim.  step right one page */
			if (P_RIGHTMOST(rootopaque))
				elog(ERROR, "no live root page found in index \"%s\"",
					 RelationGetRelationName(rel));
			rootblkno = rootopaque->btpo_next;
		}

		if (rootopaque->btpo_level != rootlevel)
			elog(ERROR, "root page %u of index \"%s\" has level %u, expected %u",
				 rootblkno, RelationGetRelationName(rel),
				 rootopaque->btpo_level, rootlevel);
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
	metaopaque = BTPageGetOpaque(metapg);
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
		rootopaque = BTPageGetOpaque(rootpage);

		if (!P_IGNORE(rootopaque))
			break;

		/* it's dead, Jim.  step right one page */
		if (P_RIGHTMOST(rootopaque))
			elog(ERROR, "no live root page found in index \"%s\"",
				 RelationGetRelationName(rel));
		rootblkno = rootopaque->btpo_next;
	}

	if (rootopaque->btpo_level != rootlevel)
		elog(ERROR, "root page %u of index \"%s\" has level %u, expected %u",
			 rootblkno, RelationGetRelationName(rel),
			 rootopaque->btpo_level, rootlevel);

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
_bt_log_reuse_page(Relation rel, BlockNumber blkno, FullTransactionId safexid)
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
	xlrec_reuse.latestRemovedFullXid = safexid;

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

				/*
				 * It's possible to find an all-zeroes page in an index.  For
				 * example, a backend might successfully extend the relation
				 * one page and then crash before it is able to make a WAL
				 * entry for adding the page.  If we find a zeroed page then
				 * reclaim it immediately.
				 */
				if (PageIsNew(page))
				{
					/* Okay to use page.  Initialize and return it. */
					_bt_pageinit(page, BufferGetPageSize(buf));
					return buf;
				}

				if (BTPageIsRecyclable(page))
				{
					/*
					 * If we are generating WAL for Hot Standby then create a
					 * WAL record that will allow us to conflict with queries
					 * running on standby, in case they have snapshots older
					 * than safexid value
					 */
					if (XLogStandbyInfoActive() && RelationNeedsWAL(rel))
						_bt_log_reuse_page(rel, blkno,
										   BTPageGetDeleteXid(page));

					/* Okay to use page.  Re-initialize and return it. */
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
	 * It doesn't matter that _bt_unlockbuf() won't get called in the event of
	 * an nbtree error (e.g. a unique violation error).  That won't cause
	 * Valgrind false positives.
	 *
	 * The nbtree client requests are superimposed on top of the bufmgr.c
	 * buffer pin client requests.  In the event of an nbtree error the buffer
	 * will certainly get marked as defined when the backend once again
	 * acquires its first pin on the buffer. (Of course, if the backend never
	 * touches the buffer again then it doesn't matter that it remains
	 * non-accessible to Valgrind.)
	 *
	 * Note: When an IndexTuple C pointer gets computed using an ItemId read
	 * from a page while a lock was held, the C pointer becomes unsafe to
	 * dereference forever as soon as the lock is released.  Valgrind can only
	 * detect cases where the pointer gets dereferenced with no _current_
	 * lock/pin held, though.
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
 *	_bt_upgradelockbufcleanup() -- upgrade lock to a full cleanup lock.
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
 * Delete item(s) from a btree leaf page during VACUUM.
 *
 * This routine assumes that the caller already has a full cleanup lock on
 * the buffer.  Also, the given deletable and updatable arrays *must* be
 * sorted in ascending order.
 *
 * Routine deals with deleting TIDs when some (but not all) of the heap TIDs
 * in an existing posting list item are to be removed.  This works by
 * updating/overwriting an existing item with caller's new version of the item
 * (a version that lacks the TIDs that are to be deleted).
 *
 * We record VACUUMs and b-tree deletes differently in WAL.  Deletes must
 * generate their own latestRemovedXid by accessing the table directly,
 * whereas VACUUMs rely on the initial VACUUM table scan performing
 * WAL-logging that takes care of the issue for the table's indexes
 * indirectly.  Also, we remove the VACUUM cycle ID from pages, which b-tree
 * deletes don't do.
 */
void
_bt_delitems_vacuum(Relation rel, Buffer buf,
					OffsetNumber *deletable, int ndeletable,
					BTVacuumPosting *updatable, int nupdatable)
{
	Page		page = BufferGetPage(buf);
	BTPageOpaque opaque;
	bool		needswal = RelationNeedsWAL(rel);
	char	   *updatedbuf = NULL;
	Size		updatedbuflen = 0;
	OffsetNumber updatedoffsets[MaxIndexTuplesPerPage];

	/* Shouldn't be called unless there's something to do */
	Assert(ndeletable > 0 || nupdatable > 0);

	/* Generate new version of posting lists without deleted TIDs */
	if (nupdatable > 0)
		updatedbuf = _bt_delitems_update(updatable, nupdatable,
										 updatedoffsets, &updatedbuflen,
										 needswal);

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
	 * any future simple index tuple deletion operations.
	 */
	for (int i = 0; i < nupdatable; i++)
	{
		OffsetNumber updatedoffset = updatedoffsets[i];
		IndexTuple	itup;
		Size		itemsz;

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
	opaque = BTPageGetOpaque(page);
	opaque->btpo_cycleid = 0;

	/*
	 * Clear the BTP_HAS_GARBAGE page flag.
	 *
	 * This flag indicates the presence of LP_DEAD items on the page (though
	 * not reliably).  Note that we only rely on it with pg_upgrade'd
	 * !heapkeyspace indexes.  That's why clearing it here won't usually
	 * interfere with simple index tuple deletion.
	 */
	opaque->btpo_flags &= ~BTP_HAS_GARBAGE;

	MarkBufferDirty(buf);

	/* XLOG stuff */
	if (needswal)
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
	/* free tuples allocated within _bt_delitems_update() */
	for (int i = 0; i < nupdatable; i++)
		pfree(updatable[i]->itup);
}

/*
 * Delete item(s) from a btree leaf page during single-page cleanup.
 *
 * This routine assumes that the caller has pinned and write locked the
 * buffer.  Also, the given deletable and updatable arrays *must* be sorted in
 * ascending order.
 *
 * Routine deals with deleting TIDs when some (but not all) of the heap TIDs
 * in an existing posting list item are to be removed.  This works by
 * updating/overwriting an existing item with caller's new version of the item
 * (a version that lacks the TIDs that are to be deleted).
 *
 * This is nearly the same as _bt_delitems_vacuum as far as what it does to
 * the page, but it needs its own latestRemovedXid from caller (caller gets
 * this from tableam).  This is used by the REDO routine to generate recovery
 * conflicts.  The other difference is that only _bt_delitems_vacuum will
 * clear page's VACUUM cycle ID.
 */
static void
_bt_delitems_delete(Relation rel, Buffer buf, TransactionId latestRemovedXid,
					OffsetNumber *deletable, int ndeletable,
					BTVacuumPosting *updatable, int nupdatable)
{
	Page		page = BufferGetPage(buf);
	BTPageOpaque opaque;
	bool		needswal = RelationNeedsWAL(rel);
	char	   *updatedbuf = NULL;
	Size		updatedbuflen = 0;
	OffsetNumber updatedoffsets[MaxIndexTuplesPerPage];

	/* Shouldn't be called unless there's something to do */
	Assert(ndeletable > 0 || nupdatable > 0);

	/* Generate new versions of posting lists without deleted TIDs */
	if (nupdatable > 0)
		updatedbuf = _bt_delitems_update(updatable, nupdatable,
										 updatedoffsets, &updatedbuflen,
										 needswal);

	/* No ereport(ERROR) until changes are logged */
	START_CRIT_SECTION();

	/* Handle updates and deletes just like _bt_delitems_vacuum */
	for (int i = 0; i < nupdatable; i++)
	{
		OffsetNumber updatedoffset = updatedoffsets[i];
		IndexTuple	itup;
		Size		itemsz;

		itup = updatable[i]->itup;
		itemsz = MAXALIGN(IndexTupleSize(itup));
		if (!PageIndexTupleOverwrite(page, updatedoffset, (Item) itup,
									 itemsz))
			elog(PANIC, "failed to update partially dead item in block %u of index \"%s\"",
				 BufferGetBlockNumber(buf), RelationGetRelationName(rel));
	}

	if (ndeletable > 0)
		PageIndexMultiDelete(page, deletable, ndeletable);

	/*
	 * Unlike _bt_delitems_vacuum, we *must not* clear the vacuum cycle ID at
	 * this point.  The VACUUM command alone controls vacuum cycle IDs.
	 */
	opaque = BTPageGetOpaque(page);

	/*
	 * Clear the BTP_HAS_GARBAGE page flag.
	 *
	 * This flag indicates the presence of LP_DEAD items on the page (though
	 * not reliably).  Note that we only rely on it with pg_upgrade'd
	 * !heapkeyspace indexes.
	 */
	opaque->btpo_flags &= ~BTP_HAS_GARBAGE;

	MarkBufferDirty(buf);

	/* XLOG stuff */
	if (needswal)
	{
		XLogRecPtr	recptr;
		xl_btree_delete xlrec_delete;

		xlrec_delete.latestRemovedXid = latestRemovedXid;
		xlrec_delete.ndeleted = ndeletable;
		xlrec_delete.nupdated = nupdatable;

		XLogBeginInsert();
		XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
		XLogRegisterData((char *) &xlrec_delete, SizeOfBtreeDelete);

		if (ndeletable > 0)
			XLogRegisterBufData(0, (char *) deletable,
								ndeletable * sizeof(OffsetNumber));

		if (nupdatable > 0)
		{
			XLogRegisterBufData(0, (char *) updatedoffsets,
								nupdatable * sizeof(OffsetNumber));
			XLogRegisterBufData(0, updatedbuf, updatedbuflen);
		}

		recptr = XLogInsert(RM_BTREE_ID, XLOG_BTREE_DELETE);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	/* can't leak memory here */
	if (updatedbuf != NULL)
		pfree(updatedbuf);
	/* free tuples allocated within _bt_delitems_update() */
	for (int i = 0; i < nupdatable; i++)
		pfree(updatable[i]->itup);
}

/*
 * Set up state needed to delete TIDs from posting list tuples via "updating"
 * the tuple.  Performs steps common to both _bt_delitems_vacuum and
 * _bt_delitems_delete.  These steps must take place before each function's
 * critical section begins.
 *
 * updatable and nupdatable are inputs, though note that we will use
 * _bt_update_posting() to replace the original itup with a pointer to a final
 * version in palloc()'d memory.  Caller should free the tuples when its done.
 *
 * The first nupdatable entries from updatedoffsets are set to the page offset
 * number for posting list tuples that caller updates.  This is mostly useful
 * because caller may need to WAL-log the page offsets (though we always do
 * this for caller out of convenience).
 *
 * Returns buffer consisting of an array of xl_btree_update structs that
 * describe the steps we perform here for caller (though only when needswal is
 * true).  Also sets *updatedbuflen to the final size of the buffer.  This
 * buffer is used by caller when WAL logging is required.
 */
static char *
_bt_delitems_update(BTVacuumPosting *updatable, int nupdatable,
					OffsetNumber *updatedoffsets, Size *updatedbuflen,
					bool needswal)
{
	char	   *updatedbuf = NULL;
	Size		buflen = 0;

	/* Shouldn't be called unless there's something to do */
	Assert(nupdatable > 0);

	for (int i = 0; i < nupdatable; i++)
	{
		BTVacuumPosting vacposting = updatable[i];
		Size		itemsz;

		/* Replace work area IndexTuple with updated version */
		_bt_update_posting(vacposting);

		/* Keep track of size of xl_btree_update for updatedbuf in passing */
		itemsz = SizeOfBtreeUpdate + vacposting->ndeletedtids * sizeof(uint16);
		buflen += itemsz;

		/* Build updatedoffsets buffer in passing */
		updatedoffsets[i] = vacposting->updatedoffset;
	}

	/* XLOG stuff */
	if (needswal)
	{
		Size		offset = 0;

		/* Allocate, set final size for caller */
		updatedbuf = palloc(buflen);
		*updatedbuflen = buflen;
		for (int i = 0; i < nupdatable; i++)
		{
			BTVacuumPosting vacposting = updatable[i];
			Size		itemsz;
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

	return updatedbuf;
}

/*
 * Comparator used by _bt_delitems_delete_check() to restore deltids array
 * back to its original leaf-page-wise sort order
 */
static int
_bt_delitems_cmp(const void *a, const void *b)
{
	TM_IndexDelete *indexdelete1 = (TM_IndexDelete *) a;
	TM_IndexDelete *indexdelete2 = (TM_IndexDelete *) b;

	if (indexdelete1->id > indexdelete2->id)
		return 1;
	if (indexdelete1->id < indexdelete2->id)
		return -1;

	Assert(false);

	return 0;
}

/*
 * Try to delete item(s) from a btree leaf page during single-page cleanup.
 *
 * nbtree interface to table_index_delete_tuples().  Deletes a subset of index
 * tuples from caller's deltids array: those whose TIDs are found safe to
 * delete by the tableam (or already marked LP_DEAD in index, and so already
 * known to be deletable by our simple index deletion caller).  We physically
 * delete index tuples from buf leaf page last of all (for index tuples where
 * that is known to be safe following our table_index_delete_tuples() call).
 *
 * Simple index deletion caller only includes TIDs from index tuples marked
 * LP_DEAD, as well as extra TIDs it found on the same leaf page that can be
 * included without increasing the total number of distinct table blocks for
 * the deletion operation as a whole.  This approach often allows us to delete
 * some extra index tuples that were practically free for tableam to check in
 * passing (when they actually turn out to be safe to delete).  It probably
 * only makes sense for the tableam to go ahead with these extra checks when
 * it is block-oriented (otherwise the checks probably won't be practically
 * free, which we rely on).  The tableam interface requires the tableam side
 * to handle the problem, though, so this is okay (we as an index AM are free
 * to make the simplifying assumption that all tableams must be block-based).
 *
 * Bottom-up index deletion caller provides all the TIDs from the leaf page,
 * without expecting that tableam will check most of them.  The tableam has
 * considerable discretion around which entries/blocks it checks.  Our role in
 * costing the bottom-up deletion operation is strictly advisory.
 *
 * Note: Caller must have added deltids entries (i.e. entries that go in
 * delstate's main array) in leaf-page-wise order: page offset number order,
 * TID order among entries taken from the same posting list tuple (tiebreak on
 * TID).  This order is convenient to work with here.
 *
 * Note: We also rely on the id field of each deltids element "capturing" this
 * original leaf-page-wise order.  That is, we expect to be able to get back
 * to the original leaf-page-wise order just by sorting deltids on the id
 * field (tableam will sort deltids for its own reasons, so we'll need to put
 * it back in leaf-page-wise order afterwards).
 */
void
_bt_delitems_delete_check(Relation rel, Buffer buf, Relation heapRel,
						  TM_IndexDeleteOp *delstate)
{
	Page		page = BufferGetPage(buf);
	TransactionId latestRemovedXid;
	OffsetNumber postingidxoffnum = InvalidOffsetNumber;
	int			ndeletable = 0,
				nupdatable = 0;
	OffsetNumber deletable[MaxIndexTuplesPerPage];
	BTVacuumPosting updatable[MaxIndexTuplesPerPage];

	/* Use tableam interface to determine which tuples to delete first */
	latestRemovedXid = table_index_delete_tuples(heapRel, delstate);

	/* Should not WAL-log latestRemovedXid unless it's required */
	if (!XLogStandbyInfoActive() || !RelationNeedsWAL(rel))
		latestRemovedXid = InvalidTransactionId;

	/*
	 * Construct a leaf-page-wise description of what _bt_delitems_delete()
	 * needs to do to physically delete index tuples from the page.
	 *
	 * Must sort deltids array to restore leaf-page-wise order (original order
	 * before call to tableam).  This is the order that the loop expects.
	 *
	 * Note that deltids array might be a lot smaller now.  It might even have
	 * no entries at all (with bottom-up deletion caller), in which case there
	 * is nothing left to do.
	 */
	qsort(delstate->deltids, delstate->ndeltids, sizeof(TM_IndexDelete),
		  _bt_delitems_cmp);
	if (delstate->ndeltids == 0)
	{
		Assert(delstate->bottomup);
		return;
	}

	/* We definitely have to delete at least one index tuple (or one TID) */
	for (int i = 0; i < delstate->ndeltids; i++)
	{
		TM_IndexStatus *dstatus = delstate->status + delstate->deltids[i].id;
		OffsetNumber idxoffnum = dstatus->idxoffnum;
		ItemId		itemid = PageGetItemId(page, idxoffnum);
		IndexTuple	itup = (IndexTuple) PageGetItem(page, itemid);
		int			nestedi,
					nitem;
		BTVacuumPosting vacposting;

		Assert(OffsetNumberIsValid(idxoffnum));

		if (idxoffnum == postingidxoffnum)
		{
			/*
			 * This deltid entry is a TID from a posting list tuple that has
			 * already been completely processed
			 */
			Assert(BTreeTupleIsPosting(itup));
			Assert(ItemPointerCompare(BTreeTupleGetHeapTID(itup),
									  &delstate->deltids[i].tid) < 0);
			Assert(ItemPointerCompare(BTreeTupleGetMaxHeapTID(itup),
									  &delstate->deltids[i].tid) >= 0);
			continue;
		}

		if (!BTreeTupleIsPosting(itup))
		{
			/* Plain non-pivot tuple */
			Assert(ItemPointerEquals(&itup->t_tid, &delstate->deltids[i].tid));
			if (dstatus->knowndeletable)
				deletable[ndeletable++] = idxoffnum;
			continue;
		}

		/*
		 * itup is a posting list tuple whose lowest deltids entry (which may
		 * or may not be for the first TID from itup) is considered here now.
		 * We should process all of the deltids entries for the posting list
		 * together now, though (not just the lowest).  Remember to skip over
		 * later itup-related entries during later iterations of outermost
		 * loop.
		 */
		postingidxoffnum = idxoffnum;	/* Remember work in outermost loop */
		nestedi = i;			/* Initialize for first itup deltids entry */
		vacposting = NULL;		/* Describes final action for itup */
		nitem = BTreeTupleGetNPosting(itup);
		for (int p = 0; p < nitem; p++)
		{
			ItemPointer ptid = BTreeTupleGetPostingN(itup, p);
			int			ptidcmp = -1;

			/*
			 * This nested loop reuses work across ptid TIDs taken from itup.
			 * We take advantage of the fact that both itup's TIDs and deltids
			 * entries (within a single itup/posting list grouping) must both
			 * be in ascending TID order.
			 */
			for (; nestedi < delstate->ndeltids; nestedi++)
			{
				TM_IndexDelete *tcdeltid = &delstate->deltids[nestedi];
				TM_IndexStatus *tdstatus = (delstate->status + tcdeltid->id);

				/* Stop once we get past all itup related deltids entries */
				Assert(tdstatus->idxoffnum >= idxoffnum);
				if (tdstatus->idxoffnum != idxoffnum)
					break;

				/* Skip past non-deletable itup related entries up front */
				if (!tdstatus->knowndeletable)
					continue;

				/* Entry is first partial ptid match (or an exact match)? */
				ptidcmp = ItemPointerCompare(&tcdeltid->tid, ptid);
				if (ptidcmp >= 0)
				{
					/* Greater than or equal (partial or exact) match... */
					break;
				}
			}

			/* ...exact ptid match to a deletable deltids entry? */
			if (ptidcmp != 0)
				continue;

			/* Exact match for deletable deltids entry -- ptid gets deleted */
			if (vacposting == NULL)
			{
				vacposting = palloc(offsetof(BTVacuumPostingData, deletetids) +
									nitem * sizeof(uint16));
				vacposting->itup = itup;
				vacposting->updatedoffset = idxoffnum;
				vacposting->ndeletedtids = 0;
			}
			vacposting->deletetids[vacposting->ndeletedtids++] = p;
		}

		/* Final decision on itup, a posting list tuple */

		if (vacposting == NULL)
		{
			/* No TIDs to delete from itup -- do nothing */
		}
		else if (vacposting->ndeletedtids == nitem)
		{
			/* Straight delete of itup (to delete all TIDs) */
			deletable[ndeletable++] = idxoffnum;
			/* Turns out we won't need granular information */
			pfree(vacposting);
		}
		else
		{
			/* Delete some (but not all) TIDs from itup */
			Assert(vacposting->ndeletedtids > 0 &&
				   vacposting->ndeletedtids < nitem);
			updatable[nupdatable++] = vacposting;
		}
	}

	/* Physically delete tuples (or TIDs) using deletable (or updatable) */
	_bt_delitems_delete(rel, buf, latestRemovedXid, deletable, ndeletable,
						updatable, nupdatable);

	/* be tidy */
	for (int i = 0; i < nupdatable; i++)
		pfree(updatable[i]);
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
	opaque = BTPageGetOpaque(page);

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
	opaque = BTPageGetOpaque(page);

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
 * Maintains bulk delete stats for caller, which are taken from vstate.  We
 * need to cooperate closely with caller here so that whole VACUUM operation
 * reliably avoids any double counting of subsidiary-to-leafbuf pages that we
 * delete in passing.  If such pages happen to be from a block number that is
 * ahead of the current scanblkno position, then caller is expected to count
 * them directly later on.  It's simpler for us to understand caller's
 * requirements than it would be for caller to understand when or how a
 * deleted page became deleted after the fact.
 *
 * NOTE: this leaks memory.  Rather than trying to clean up everything
 * carefully, it's better to run it in a temp context that can be reset
 * frequently.
 */
void
_bt_pagedel(Relation rel, Buffer leafbuf, BTVacState *vstate)
{
	BlockNumber rightsib;
	bool		rightsib_empty;
	Page		page;
	BTPageOpaque opaque;

	/*
	 * Save original leafbuf block number from caller.  Only deleted blocks
	 * that are <= scanblkno are added to bulk delete stat's pages_deleted
	 * count.
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
		opaque = BTPageGetOpaque(page);

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
			return;
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
			return;
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
					return;
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
				return;
			}
		}

		/*
		 * Then unlink it from its siblings.  Each call to
		 * _bt_unlink_halfdead_page unlinks the topmost page from the subtree,
		 * making it shallower.  Iterate until the leafbuf page is deleted.
		 */
		rightsib_empty = false;
		Assert(P_ISLEAF(opaque) && P_ISHALFDEAD(opaque));
		while (P_ISHALFDEAD(opaque))
		{
			/* Check for interrupts in _bt_unlink_halfdead_page */
			if (!_bt_unlink_halfdead_page(rel, leafbuf, scanblkno,
										  &rightsib_empty, vstate))
			{
				/*
				 * _bt_unlink_halfdead_page should never fail, since we
				 * established that deletion is generally safe in
				 * _bt_mark_page_halfdead -- index must be corrupt.
				 *
				 * Note that _bt_unlink_halfdead_page already released the
				 * lock and pin on leafbuf for us.
				 */
				Assert(false);
				return;
			}
		}

		Assert(P_ISLEAF(opaque) && P_ISDELETED(opaque));

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
	opaque = BTPageGetOpaque(page);

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

	page = BufferGetPage(subtreeparent);
	opaque = BTPageGetOpaque(page);

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

	/*
	 * Check that the parent-page index items we're about to delete/overwrite
	 * in subtree parent page contain what we expect.  This can fail if the
	 * index has become corrupt for some reason.  When that happens we back
	 * out of deletion of the leafbuf subtree.  (This is just like the case
	 * where _bt_lock_subtree_parent() cannot "re-find" leafbuf's downlink.)
	 */
	if (BTreeTupleGetDownLink(itup) != topparentrightsib)
	{
		ereport(LOG,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg_internal("right sibling %u of block %u is not next child %u of block %u in index \"%s\"",
								 topparentrightsib, topparent,
								 BTreeTupleGetDownLink(itup),
								 BufferGetBlockNumber(subtreeparent),
								 RelationGetRelationName(rel))));

		_bt_relbuf(rel, subtreeparent);
		Assert(false);
		return false;
	}

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
	opaque = BTPageGetOpaque(page);

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
	opaque = BTPageGetOpaque(page);
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
		opaque = BTPageGetOpaque(page);
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
 * Must hold pin and lock on leafbuf at entry (read or write doesn't matter).
 * On success exit, we'll be holding pin and write lock.  On failure exit,
 * we'll release both pin and lock before returning (we define it that way
 * to avoid having to reacquire a lock we already released).
 */
static bool
_bt_unlink_halfdead_page(Relation rel, Buffer leafbuf, BlockNumber scanblkno,
						 bool *rightsib_empty, BTVacState *vstate)
{
	BlockNumber leafblkno = BufferGetBlockNumber(leafbuf);
	IndexBulkDeleteResult *stats = vstate->stats;
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
	FullTransactionId safexid;
	bool		rightsib_is_rightmost;
	uint32		targetlevel;
	IndexTuple	leafhikey;
	BlockNumber leaftopparent;

	page = BufferGetPage(leafbuf);
	opaque = BTPageGetOpaque(page);

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
		opaque = BTPageGetOpaque(page);
		leftsib = opaque->btpo_prev;
		targetlevel = opaque->btpo_level;
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
	 * right.
	 */
	if (target != leafblkno)
		_bt_lockbuf(rel, leafbuf, BT_WRITE);
	if (leftsib != P_NONE)
	{
		lbuf = _bt_getbuf(rel, leftsib, BT_WRITE);
		page = BufferGetPage(lbuf);
		opaque = BTPageGetOpaque(page);
		while (P_ISDELETED(opaque) || opaque->btpo_next != target)
		{
			bool		leftsibvalid = true;

			/*
			 * Before we follow the link from the page that was the left
			 * sibling mere moments ago, validate its right link.  This
			 * reduces the opportunities for loop to fail to ever make any
			 * progress in the presence of index corruption.
			 *
			 * Note: we rely on the assumption that there can only be one
			 * vacuum process running at a time (against the same index).
			 */
			if (P_RIGHTMOST(opaque) || P_ISDELETED(opaque) ||
				leftsib == opaque->btpo_next)
				leftsibvalid = false;

			leftsib = opaque->btpo_next;
			_bt_relbuf(rel, lbuf);

			if (!leftsibvalid)
			{
				/*
				 * This is known to fail in the field; sibling link corruption
				 * is relatively common.  Press on with vacuuming rather than
				 * just throwing an ERROR.
				 */
				ereport(LOG,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg_internal("valid left sibling for deletion target could not be located: "
										 "left sibling %u of target %u with leafblkno %u and scanblkno %u on level %u of index \"%s\"",
										 leftsib, target, leafblkno, scanblkno,
										 targetlevel, RelationGetRelationName(rel))));

				/* Must release all pins and locks on failure exit */
				ReleaseBuffer(buf);
				if (target != leafblkno)
					_bt_relbuf(rel, leafbuf);

				return false;
			}

			CHECK_FOR_INTERRUPTS();

			/* step right one page */
			lbuf = _bt_getbuf(rel, leftsib, BT_WRITE);
			page = BufferGetPage(lbuf);
			opaque = BTPageGetOpaque(page);
		}
	}
	else
		lbuf = InvalidBuffer;

	/* Next write-lock the target page itself */
	_bt_lockbuf(rel, buf, BT_WRITE);
	page = BufferGetPage(buf);
	opaque = BTPageGetOpaque(page);

	/*
	 * Check page is still empty etc, else abandon deletion.  This is just for
	 * paranoia's sake; a half-dead page cannot resurrect because there can be
	 * only one vacuum process running at a time.
	 */
	if (P_RIGHTMOST(opaque) || P_ISROOT(opaque) || P_ISDELETED(opaque))
		elog(ERROR, "target page changed status unexpectedly in block %u of index \"%s\"",
			 target, RelationGetRelationName(rel));

	if (opaque->btpo_prev != leftsib)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg_internal("target page left link unexpectedly changed from %u to %u in block %u of index \"%s\"",
								 leftsib, opaque->btpo_prev, target,
								 RelationGetRelationName(rel))));

	if (target == leafblkno)
	{
		if (P_FIRSTDATAKEY(opaque) <= PageGetMaxOffsetNumber(page) ||
			!P_ISLEAF(opaque) || !P_ISHALFDEAD(opaque))
			elog(ERROR, "target leaf page changed status unexpectedly in block %u of index \"%s\"",
				 target, RelationGetRelationName(rel));

		/* Leaf page is also target page: don't set leaftopparent */
		leaftopparent = InvalidBlockNumber;
	}
	else
	{
		IndexTuple	finaldataitem;

		if (P_FIRSTDATAKEY(opaque) != PageGetMaxOffsetNumber(page) ||
			P_ISLEAF(opaque))
			elog(ERROR, "target internal page on level %u changed status unexpectedly in block %u of index \"%s\"",
				 targetlevel, target, RelationGetRelationName(rel));

		/* Target is internal: set leaftopparent for next call here...  */
		itemid = PageGetItemId(page, P_FIRSTDATAKEY(opaque));
		finaldataitem = (IndexTuple) PageGetItem(page, itemid);
		leaftopparent = BTreeTupleGetDownLink(finaldataitem);
		/* ...except when it would be a redundant pointer-to-self */
		if (leaftopparent == leafblkno)
			leaftopparent = InvalidBlockNumber;
	}

	/* No leaftopparent for level 0 (leaf page) or level 1 target */
	Assert(!BlockNumberIsValid(leaftopparent) || targetlevel > 1);

	/*
	 * And next write-lock the (current) right sibling.
	 */
	rightsib = opaque->btpo_next;
	rbuf = _bt_getbuf(rel, rightsib, BT_WRITE);
	page = BufferGetPage(rbuf);
	opaque = BTPageGetOpaque(page);

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
	 * We can safely acquire a lock on the metapage here --- see comments for
	 * _bt_newroot().
	 */
	if (leftsib == P_NONE && rightsib_is_rightmost)
	{
		page = BufferGetPage(rbuf);
		opaque = BTPageGetOpaque(page);
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
		opaque = BTPageGetOpaque(page);
		Assert(opaque->btpo_next == target);
		opaque->btpo_next = rightsib;
	}
	page = BufferGetPage(rbuf);
	opaque = BTPageGetOpaque(page);
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
		BTreeTupleSetTopParent(leafhikey, leaftopparent);

	/*
	 * Mark the page itself deleted.  It can be recycled when all current
	 * transactions are gone.  Storing GetTopTransactionId() would work, but
	 * we're in VACUUM and would not otherwise have an XID.  Having already
	 * updated links to the target, ReadNextFullTransactionId() suffices as an
	 * upper bound.  Any scan having retained a now-stale link is advertising
	 * in its PGPROC an xmin less than or equal to the value we read here.  It
	 * will continue to do so, holding back the xmin horizon, for the duration
	 * of that scan.
	 */
	page = BufferGetPage(buf);
	opaque = BTPageGetOpaque(page);
	Assert(P_ISHALFDEAD(opaque) || !P_ISLEAF(opaque));

	/*
	 * Store upper bound XID that's used to determine when deleted page is no
	 * longer needed as a tombstone
	 */
	safexid = ReadNextFullTransactionId();
	BTPageSetDeleted(page, safexid);
	opaque->btpo_cycleid = 0;

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

		/* information stored on the target/to-be-unlinked block */
		xlrec.leftsib = leftsib;
		xlrec.rightsib = rightsib;
		xlrec.level = targetlevel;
		xlrec.safexid = safexid;

		/* information needed to recreate the leaf block (if not the target) */
		xlrec.leafleftsib = leafleftsib;
		xlrec.leafrightsib = leafrightsib;
		xlrec.leaftopparent = leaftopparent;

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
			xlmeta.last_cleanup_num_delpages = metad->btm_last_cleanup_num_delpages;
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

	/* If the target is not leafbuf, we're done with it now -- release it */
	if (target != leafblkno)
		_bt_relbuf(rel, buf);

	/*
	 * Maintain pages_newly_deleted, which is simply the number of pages
	 * deleted by the ongoing VACUUM operation.
	 *
	 * Maintain pages_deleted in a way that takes into account how
	 * btvacuumpage() will count deleted pages that have yet to become
	 * scanblkno -- only count page when it's not going to get that treatment
	 * later on.
	 */
	stats->pages_newly_deleted++;
	if (target <= scanblkno)
		stats->pages_deleted++;

	/*
	 * Remember information about the target page (now a newly deleted page)
	 * in dedicated vstate space for later.  The page will be considered as a
	 * candidate to place in the FSM at the end of the current btvacuumscan()
	 * call.
	 */
	_bt_pendingfsm_add(vstate, target, safexid);

	/* Success - hold on to lock on leafbuf (might also have been target) */
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
		Assert(false);
		return false;
	}

	parent = stack->bts_blkno;
	parentoffset = stack->bts_offset;

	page = BufferGetPage(pbuf);
	opaque = BTPageGetOpaque(page);
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

/*
 * Initialize local memory state used by VACUUM for _bt_pendingfsm_finalize
 * optimization.
 *
 * Called at the start of a btvacuumscan().  Caller's cleanuponly argument
 * indicates if ongoing VACUUM has not (and will not) call btbulkdelete().
 *
 * We expect to allocate memory inside VACUUM's top-level memory context here.
 * The working buffer is subject to a limit based on work_mem.  Our strategy
 * when the array can no longer grow within the bounds of that limit is to
 * stop saving additional newly deleted pages, while proceeding as usual with
 * the pages that we can fit.
 */
void
_bt_pendingfsm_init(Relation rel, BTVacState *vstate, bool cleanuponly)
{
	int64		maxbufsize;

	/*
	 * Don't bother with optimization in cleanup-only case -- we don't expect
	 * any newly deleted pages.  Besides, cleanup-only calls to btvacuumscan()
	 * can only take place because this optimization didn't work out during
	 * the last VACUUM.
	 */
	if (cleanuponly)
		return;

	/*
	 * Cap maximum size of array so that we always respect work_mem.  Avoid
	 * int overflow here.
	 */
	vstate->bufsize = 256;
	maxbufsize = (work_mem * 1024L) / sizeof(BTPendingFSM);
	maxbufsize = Min(maxbufsize, INT_MAX);
	maxbufsize = Min(maxbufsize, MaxAllocSize / sizeof(BTPendingFSM));
	/* Stay sane with small work_mem */
	maxbufsize = Max(maxbufsize, vstate->bufsize);
	vstate->maxbufsize = maxbufsize;

	/* Allocate buffer, indicate that there are currently 0 pending pages */
	vstate->pendingpages = palloc(sizeof(BTPendingFSM) * vstate->bufsize);
	vstate->npendingpages = 0;
}

/*
 * Place any newly deleted pages (i.e. pages that _bt_pagedel() deleted during
 * the ongoing VACUUM operation) into the free space map -- though only when
 * it is actually safe to do so by now.
 *
 * Called at the end of a btvacuumscan(), just before free space map vacuuming
 * takes place.
 *
 * Frees memory allocated by _bt_pendingfsm_init(), if any.
 */
void
_bt_pendingfsm_finalize(Relation rel, BTVacState *vstate)
{
	IndexBulkDeleteResult *stats = vstate->stats;

	Assert(stats->pages_newly_deleted >= vstate->npendingpages);

	if (vstate->npendingpages == 0)
	{
		/* Just free memory when nothing to do */
		if (vstate->pendingpages)
			pfree(vstate->pendingpages);

		return;
	}

#ifdef DEBUG_BTREE_PENDING_FSM

	/*
	 * Debugging aid: Sleep for 5 seconds to greatly increase the chances of
	 * placing pending pages in the FSM.  Note that the optimization will
	 * never be effective without some other backend concurrently consuming an
	 * XID.
	 */
	pg_usleep(5000000L);
#endif

	/*
	 * Recompute VACUUM XID boundaries.
	 *
	 * We don't actually care about the oldest non-removable XID.  Computing
	 * the oldest such XID has a useful side-effect that we rely on: it
	 * forcibly updates the XID horizon state for this backend.  This step is
	 * essential; GlobalVisCheckRemovableFullXid() will not reliably recognize
	 * that it is now safe to recycle newly deleted pages without this step.
	 */
	GetOldestNonRemovableTransactionId(NULL);

	for (int i = 0; i < vstate->npendingpages; i++)
	{
		BlockNumber target = vstate->pendingpages[i].target;
		FullTransactionId safexid = vstate->pendingpages[i].safexid;

		/*
		 * Do the equivalent of checking BTPageIsRecyclable(), but without
		 * accessing the page again a second time.
		 *
		 * Give up on finding the first non-recyclable page -- all later pages
		 * must be non-recyclable too, since _bt_pendingfsm_add() adds pages
		 * to the array in safexid order.
		 */
		if (!GlobalVisCheckRemovableFullXid(NULL, safexid))
			break;

		RecordFreeIndexPage(rel, target);
		stats->pages_free++;
	}

	pfree(vstate->pendingpages);
}

/*
 * Maintain array of pages that were deleted during current btvacuumscan()
 * call, for use in _bt_pendingfsm_finalize()
 */
static void
_bt_pendingfsm_add(BTVacState *vstate,
				   BlockNumber target,
				   FullTransactionId safexid)
{
	Assert(vstate->npendingpages <= vstate->bufsize);
	Assert(vstate->bufsize <= vstate->maxbufsize);

#ifdef USE_ASSERT_CHECKING

	/*
	 * Verify an assumption made by _bt_pendingfsm_finalize(): pages from the
	 * array will always be in safexid order (since that is the order that we
	 * save them in here)
	 */
	if (vstate->npendingpages > 0)
	{
		FullTransactionId lastsafexid =
		vstate->pendingpages[vstate->npendingpages - 1].safexid;

		Assert(FullTransactionIdFollowsOrEquals(safexid, lastsafexid));
	}
#endif

	/*
	 * If temp buffer reaches maxbufsize/work_mem capacity then we discard
	 * information about this page.
	 *
	 * Note that this also covers the case where we opted to not use the
	 * optimization in _bt_pendingfsm_init().
	 */
	if (vstate->npendingpages == vstate->maxbufsize)
		return;

	/* Consider enlarging buffer */
	if (vstate->npendingpages == vstate->bufsize)
	{
		int			newbufsize = vstate->bufsize * 2;

		/* Respect work_mem */
		if (newbufsize > vstate->maxbufsize)
			newbufsize = vstate->maxbufsize;

		vstate->bufsize = newbufsize;
		vstate->pendingpages =
			repalloc(vstate->pendingpages,
					 sizeof(BTPendingFSM) * vstate->bufsize);
	}

	/* Save metadata for newly deleted page */
	vstate->pendingpages[vstate->npendingpages].target = target;
	vstate->pendingpages[vstate->npendingpages].safexid = safexid;
	vstate->npendingpages++;
}
