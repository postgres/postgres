/*-------------------------------------------------------------------------
 *
 * nbtpage.c
 *	  BTree-specific page management code for the Postgres btree access
 *	  method.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/nbtree/nbtpage.c,v 1.49 2001/01/29 07:28:17 vadim Exp $
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

#include <time.h>

#include "access/nbtree.h"
#include "miscadmin.h"
#include "storage/lmgr.h"

extern bool FixBTree;	/* comments in nbtree.c */
extern Buffer _bt_fixroot(Relation rel, Buffer oldrootbuf, bool release);

/*
 *	We use high-concurrency locking on btrees.	There are two cases in
 *	which we don't do locking.  One is when we're building the btree.
 *	Since the creating transaction has not committed, no one can see
 *	the index, and there's no reason to share locks.  The second case
 *	is when we're just starting up the database system.  We use some
 *	special-purpose initialization code in the relation cache manager
 *	(see utils/cache/relcache.c) to allow us to do indexed scans on
 *	the system catalogs before we'd normally be able to.  This happens
 *	before the lock table is fully initialized, so we can't use it.
 *	Strictly speaking, this violates 2pl, but we don't do 2pl on the
 *	system catalogs anyway, so I declare this to be okay.
 */

#define USELOCKING		(!BuildingBtree && !IsInitProcessingMode())

/*
 *	_bt_metapinit() -- Initialize the metadata page of a btree.
 */
void
_bt_metapinit(Relation rel)
{
	Buffer		buf;
	Page		pg;
	int			nblocks;
	BTMetaPageData metad;
	BTPageOpaque op;

	/* can't be sharing this with anyone, now... */
	if (USELOCKING)
		LockRelation(rel, AccessExclusiveLock);

	if ((nblocks = RelationGetNumberOfBlocks(rel)) != 0)
	{
		elog(ERROR, "Cannot initialize non-empty btree %s",
			 RelationGetRelationName(rel));
	}

	buf = ReadBuffer(rel, P_NEW);
	pg = BufferGetPage(buf);
	_bt_pageinit(pg, BufferGetPageSize(buf));

	metad.btm_magic = BTREE_MAGIC;
	metad.btm_version = BTREE_VERSION;
	metad.btm_root = P_NONE;
	metad.btm_level = 0;
	memcpy((char *) BTPageGetMeta(pg), (char *) &metad, sizeof(metad));

	op = (BTPageOpaque) PageGetSpecialPointer(pg);
	op->btpo_flags = BTP_META;

	WriteBuffer(buf);

	/* all done */
	if (USELOCKING)
		UnlockRelation(rel, AccessExclusiveLock);
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
	BTMetaPageData *metad;

	metabuf = _bt_getbuf(rel, BTREE_METAPAGE, BT_READ);
	metapg = BufferGetPage(metabuf);
	metaopaque = (BTPageOpaque) PageGetSpecialPointer(metapg);
	metad = BTPageGetMeta(metapg);

	if (!(metaopaque->btpo_flags & BTP_META) ||
		metad->btm_magic != BTREE_MAGIC)
		elog(ERROR, "Index %s is not a btree",
			 RelationGetRelationName(rel));

	if (metad->btm_version != BTREE_VERSION)
		elog(ERROR, "Version mismatch on %s: version %d file, version %d code",
			 RelationGetRelationName(rel),
			 metad->btm_version, BTREE_VERSION);

	/* if no root page initialized yet, do it */
	if (metad->btm_root == P_NONE)
	{
		/* If access = BT_READ, caller doesn't want us to create root yet */
		if (access == BT_READ)
		{
			_bt_relbuf(rel, metabuf, BT_READ);
			return InvalidBuffer;
		}

		/* trade in our read lock for a write lock */
		LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
		LockBuffer(metabuf, BT_WRITE);

		/*
		 * Race condition:	if someone else initialized the metadata
		 * between the time we released the read lock and acquired the
		 * write lock, above, we must avoid doing it again.
		 */
		if (metad->btm_root == P_NONE)
		{

			/*
			 * Get, initialize, write, and leave a lock of the appropriate
			 * type on the new root page.  Since this is the first page in
			 * the tree, it's a leaf as well as the root.
			 */
			rootbuf = _bt_getbuf(rel, P_NEW, BT_WRITE);
			rootblkno = BufferGetBlockNumber(rootbuf);
			rootpage = BufferGetPage(rootbuf);

			/* NO ELOG(ERROR) till meta is updated */
			START_CRIT_SECTION();

			metad->btm_root = rootblkno;
			metad->btm_level = 1;

			_bt_pageinit(rootpage, BufferGetPageSize(rootbuf));
			rootopaque = (BTPageOpaque) PageGetSpecialPointer(rootpage);
			rootopaque->btpo_flags |= (BTP_LEAF | BTP_ROOT);

			/* XLOG stuff */
			{
				xl_btree_newroot	xlrec;
				XLogRecPtr			recptr;
				XLogRecData			rdata;

				xlrec.node = rel->rd_node;
				xlrec.level = 1;
				BlockIdSet(&(xlrec.rootblk), rootblkno);
				rdata.buffer = InvalidBuffer;
				rdata.data = (char*)&xlrec;
				rdata.len = SizeOfBtreeNewroot;
				rdata.next = NULL;

				recptr = XLogInsert(RM_BTREE_ID,
							XLOG_BTREE_NEWROOT|XLOG_BTREE_LEAF, &rdata);

				PageSetLSN(rootpage, recptr);
				PageSetSUI(rootpage, ThisStartUpID);
				PageSetLSN(metapg, recptr);
				PageSetSUI(metapg, ThisStartUpID);
			}

			END_CRIT_SECTION();

			_bt_wrtnorelbuf(rel, rootbuf);

			/* swap write lock for read lock */
			LockBuffer(rootbuf, BUFFER_LOCK_UNLOCK);
			LockBuffer(rootbuf, BT_READ);

			/* okay, metadata is correct, write and release it */
			_bt_wrtbuf(rel, metabuf);
		}
		else
		{
			/*
			 * Metadata initialized by someone else.  In order to
			 * guarantee no deadlocks, we have to release the metadata
			 * page and start all over again.
			 */
			_bt_relbuf(rel, metabuf, BT_WRITE);
			return _bt_getroot(rel, access);
		}
	}
	else
	{
		rootblkno = metad->btm_root;
		_bt_relbuf(rel, metabuf, BT_READ);		/* done with the meta page */

		rootbuf = _bt_getbuf(rel, rootblkno, BT_READ);
	}

	/*
	 * Race condition:	If the root page split between the time we looked
	 * at the metadata page and got the root buffer, then we got the wrong
	 * buffer.  Release it and try again.
	 */
	rootpage = BufferGetPage(rootbuf);
	rootopaque = (BTPageOpaque) PageGetSpecialPointer(rootpage);

	if (! P_ISROOT(rootopaque))
	{
		/*
		 * It happened, but if root page splitter failed to create
		 * new root page then we'll go in loop trying to call
		 * _bt_getroot again and again.
		 */
		if (FixBTree)
		{
			Buffer	newrootbuf;

check_parent:;
			if (rootopaque->btpo_parent == BTREE_METAPAGE)	/* unupdated! */
			{
				LockBuffer(rootbuf, BUFFER_LOCK_UNLOCK);
				LockBuffer(rootbuf, BT_WRITE);

				/* handle concurrent fix of root page */
				if (rootopaque->btpo_parent == BTREE_METAPAGE)	/* unupdated! */
				{
					elog(NOTICE, "bt_getroot: fixing root page");
					newrootbuf = _bt_fixroot(rel, rootbuf, true);
					LockBuffer(newrootbuf, BUFFER_LOCK_UNLOCK);
					LockBuffer(newrootbuf, BT_READ);
					rootbuf = newrootbuf;
					rootpage = BufferGetPage(rootbuf);
					rootopaque = (BTPageOpaque) PageGetSpecialPointer(rootpage);
					/* New root might be splitted while changing lock */
					if (P_ISROOT(rootopaque))
						return(rootbuf);
					/* rootbuf is read locked */
					goto check_parent;
				}
				else	/* someone else already fixed root */
				{
					LockBuffer(rootbuf, BUFFER_LOCK_UNLOCK);
					LockBuffer(rootbuf, BT_READ);
				}
			}
			/*
			 * Ok, here we have old root page with btpo_parent pointing
			 * to upper level - check parent page because of there is
			 * good chance that parent is root page.
			 */
			newrootbuf = _bt_getbuf(rel, rootopaque->btpo_parent, BT_READ);
			_bt_relbuf(rel, rootbuf, BT_READ);
			rootbuf = newrootbuf;
			rootpage = BufferGetPage(rootbuf);
			rootopaque = (BTPageOpaque) PageGetSpecialPointer(rootpage);
			if (P_ISROOT(rootopaque))
				return(rootbuf);
			/* no luck -:( */
		}

		/* try again */
		_bt_relbuf(rel, rootbuf, BT_READ);
		return _bt_getroot(rel, access);
	}

	/*
	 * By here, we have a correct lock on the root block, its reference
	 * count is correct, and we have no lock set on the metadata page.
	 * Return the root block.
	 */
	return rootbuf;
}

/*
 *	_bt_getbuf() -- Get a buffer by block number for read or write.
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
		Page		page;

		/*
		 * Extend the relation by one page.
		 *
		 * Extend bufmgr code is unclean and so we have to use extra locking
		 * here.
		 */
		LockPage(rel, 0, ExclusiveLock);
		buf = ReadBuffer(rel, blkno);
		LockBuffer(buf, access);
		UnlockPage(rel, 0, ExclusiveLock);

		/* Initialize the new page before returning it */
		page = BufferGetPage(buf);
		_bt_pageinit(page, BufferGetPageSize(buf));
	}

	/* ref count and lock type are correct */
	return buf;
}

/*
 *	_bt_relbuf() -- release a locked buffer.
 *
 * Lock and pin (refcount) are both dropped.
 */
void
_bt_relbuf(Relation rel, Buffer buf, int access)
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
 * dirty here, the real I/O happens later.  Since we can't persuade the
 * Unix kernel to schedule disk writes in a particular order, there's not
 * much point in worrying about this.  The most we can say is that all the
 * writes will occur before commit.
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
 */
void
_bt_pageinit(Page page, Size size)
{

	/*
	 * Cargo_cult programming -- don't really need this to be zero, but
	 * creating new pages is an infrequent occurrence and it makes me feel
	 * good when I know they're empty.
	 */

	MemSet(page, 0, size);

	PageInit(page, size, sizeof(BTPageOpaqueData));
	((BTPageOpaque) PageGetSpecialPointer(page))->btpo_parent =
		InvalidBlockNumber;
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
 */
void
_bt_metaproot(Relation rel, BlockNumber rootbknum, int level)
{
	Buffer		metabuf;
	Page		metap;
	BTPageOpaque metaopaque;
	BTMetaPageData *metad;

	metabuf = _bt_getbuf(rel, BTREE_METAPAGE, BT_WRITE);
	metap = BufferGetPage(metabuf);
	metaopaque = (BTPageOpaque) PageGetSpecialPointer(metap);
	Assert(metaopaque->btpo_flags & BTP_META);
	metad = BTPageGetMeta(metap);
	metad->btm_root = rootbknum;
	if (level == 0)				/* called from _do_insert */
		metad->btm_level += 1;
	else
		metad->btm_level = level;		/* called from btsort */
	_bt_wrtbuf(rel, metabuf);
}

/*
 * Delete an item from a btree.  It had better be a leaf item...
 */
void
_bt_pagedel(Relation rel, ItemPointer tid)
{
	Buffer		buf;
	Page		page;
	BlockNumber blkno;
	OffsetNumber offno;

	blkno = ItemPointerGetBlockNumber(tid);
	offno = ItemPointerGetOffsetNumber(tid);

	buf = _bt_getbuf(rel, blkno, BT_WRITE);
	page = BufferGetPage(buf);

	START_CRIT_SECTION();
	PageIndexTupleDelete(page, offno);
	/* XLOG stuff */
	{
		xl_btree_delete	xlrec;
		XLogRecPtr		recptr;
		XLogRecData		rdata[2];

		xlrec.target.node = rel->rd_node;
		xlrec.target.tid = *tid;
		rdata[0].buffer = InvalidBuffer;
		rdata[0].data = (char*)&xlrec;
		rdata[0].len = SizeOfBtreeDelete;
		rdata[0].next = &(rdata[1]);

		rdata[1].buffer = buf;
		rdata[1].data = NULL;
		rdata[1].len = 0;
		rdata[1].next = NULL;

		recptr = XLogInsert(RM_BTREE_ID, XLOG_BTREE_DELETE, rdata);

		PageSetLSN(page, recptr);
		PageSetSUI(page, ThisStartUpID);
	}
	END_CRIT_SECTION();

	/* write the buffer and release the lock */
	_bt_wrtbuf(rel, buf);
}
