/*-------------------------------------------------------------------------
 *
 * xlogutils.c
 *
 * PostgreSQL transaction log manager utility routines
 *
 * This file contains support routines that are used by XLOG replay functions.
 * None of this code is used during normal system operation.
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/backend/access/transam/xlogutils.c,v 1.55 2008/06/12 09:12:30 heikki Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xlogutils.h"
#include "storage/bufmgr.h"
#include "storage/smgr.h"
#include "utils/hsearch.h"


/*
 * During XLOG replay, we may see XLOG records for incremental updates of
 * pages that no longer exist, because their relation was later dropped or
 * truncated.  (Note: this is only possible when full_page_writes = OFF,
 * since when it's ON, the first reference we see to a page should always
 * be a full-page rewrite not an incremental update.)  Rather than simply
 * ignoring such records, we make a note of the referenced page, and then
 * complain if we don't actually see a drop or truncate covering the page
 * later in replay.
 */
typedef struct xl_invalid_page_key
{
	RelFileNode node;			/* the relation */
	BlockNumber blkno;			/* the page */
} xl_invalid_page_key;

typedef struct xl_invalid_page
{
	xl_invalid_page_key key;	/* hash key ... must be first */
	bool		present;		/* page existed but contained zeroes */
} xl_invalid_page;

static HTAB *invalid_page_tab = NULL;


/* Log a reference to an invalid page */
static void
log_invalid_page(RelFileNode node, BlockNumber blkno, bool present)
{
	xl_invalid_page_key key;
	xl_invalid_page *hentry;
	bool		found;

	/*
	 * Log references to invalid pages at DEBUG1 level.  This allows some
	 * tracing of the cause (note the elog context mechanism will tell us
	 * something about the XLOG record that generated the reference).
	 */
	if (present)
		elog(DEBUG1, "page %u of relation %u/%u/%u is uninitialized",
			 blkno, node.spcNode, node.dbNode, node.relNode);
	else
		elog(DEBUG1, "page %u of relation %u/%u/%u does not exist",
			 blkno, node.spcNode, node.dbNode, node.relNode);

	if (invalid_page_tab == NULL)
	{
		/* create hash table when first needed */
		HASHCTL		ctl;

		memset(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(xl_invalid_page_key);
		ctl.entrysize = sizeof(xl_invalid_page);
		ctl.hash = tag_hash;

		invalid_page_tab = hash_create("XLOG invalid-page table",
									   100,
									   &ctl,
									   HASH_ELEM | HASH_FUNCTION);
	}

	/* we currently assume xl_invalid_page_key contains no padding */
	key.node = node;
	key.blkno = blkno;
	hentry = (xl_invalid_page *)
		hash_search(invalid_page_tab, (void *) &key, HASH_ENTER, &found);

	if (!found)
	{
		/* hash_search already filled in the key */
		hentry->present = present;
	}
	else
	{
		/* repeat reference ... leave "present" as it was */
	}
}

/* Forget any invalid pages >= minblkno, because they've been dropped */
static void
forget_invalid_pages(RelFileNode node, BlockNumber minblkno)
{
	HASH_SEQ_STATUS status;
	xl_invalid_page *hentry;

	if (invalid_page_tab == NULL)
		return;					/* nothing to do */

	hash_seq_init(&status, invalid_page_tab);

	while ((hentry = (xl_invalid_page *) hash_seq_search(&status)) != NULL)
	{
		if (RelFileNodeEquals(hentry->key.node, node) &&
			hentry->key.blkno >= minblkno)
		{
			elog(DEBUG2, "page %u of relation %u/%u/%u has been dropped",
				 hentry->key.blkno, hentry->key.node.spcNode,
				 hentry->key.node.dbNode, hentry->key.node.relNode);

			if (hash_search(invalid_page_tab,
							(void *) &hentry->key,
							HASH_REMOVE, NULL) == NULL)
				elog(ERROR, "hash table corrupted");
		}
	}
}

/* Forget any invalid pages in a whole database */
static void
forget_invalid_pages_db(Oid dbid)
{
	HASH_SEQ_STATUS status;
	xl_invalid_page *hentry;

	if (invalid_page_tab == NULL)
		return;					/* nothing to do */

	hash_seq_init(&status, invalid_page_tab);

	while ((hentry = (xl_invalid_page *) hash_seq_search(&status)) != NULL)
	{
		if (hentry->key.node.dbNode == dbid)
		{
			elog(DEBUG2, "page %u of relation %u/%u/%u has been dropped",
				 hentry->key.blkno, hentry->key.node.spcNode,
				 hentry->key.node.dbNode, hentry->key.node.relNode);

			if (hash_search(invalid_page_tab,
							(void *) &hentry->key,
							HASH_REMOVE, NULL) == NULL)
				elog(ERROR, "hash table corrupted");
		}
	}
}

/* Complain about any remaining invalid-page entries */
void
XLogCheckInvalidPages(void)
{
	HASH_SEQ_STATUS status;
	xl_invalid_page *hentry;
	bool		foundone = false;

	if (invalid_page_tab == NULL)
		return;					/* nothing to do */

	hash_seq_init(&status, invalid_page_tab);

	/*
	 * Our strategy is to emit WARNING messages for all remaining entries and
	 * only PANIC after we've dumped all the available info.
	 */
	while ((hentry = (xl_invalid_page *) hash_seq_search(&status)) != NULL)
	{
		if (hentry->present)
			elog(WARNING, "page %u of relation %u/%u/%u was uninitialized",
				 hentry->key.blkno, hentry->key.node.spcNode,
				 hentry->key.node.dbNode, hentry->key.node.relNode);
		else
			elog(WARNING, "page %u of relation %u/%u/%u did not exist",
				 hentry->key.blkno, hentry->key.node.spcNode,
				 hentry->key.node.dbNode, hentry->key.node.relNode);
		foundone = true;
	}

	if (foundone)
		elog(PANIC, "WAL contains references to invalid pages");

	hash_destroy(invalid_page_tab);
	invalid_page_tab = NULL;
}


/*
 * XLogReadBuffer
 *		Read a page during XLOG replay
 *
 * This is functionally comparable to ReadBuffer followed by
 * LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE): you get back a pinned
 * and locked buffer.  (Getting the lock is not really necessary, since we
 * expect that this is only used during single-process XLOG replay, but
 * some subroutines such as MarkBufferDirty will complain if we don't.)
 *
 * If "init" is true then the caller intends to rewrite the page fully
 * using the info in the XLOG record.  In this case we will extend the
 * relation if needed to make the page exist, and we will not complain about
 * the page being "new" (all zeroes); in fact, we usually will supply a
 * zeroed buffer without reading the page at all, so as to avoid unnecessary
 * failure if the page is present on disk but has corrupt headers.
 *
 * If "init" is false then the caller needs the page to be valid already.
 * If the page doesn't exist or contains zeroes, we return InvalidBuffer.
 * In this case the caller should silently skip the update on this page.
 * (In this situation, we expect that the page was later dropped or truncated.
 * If we don't see evidence of that later in the WAL sequence, we'll complain
 * at the end of WAL replay.)
 */
Buffer
XLogReadBuffer(RelFileNode rnode, BlockNumber blkno, bool init)
{
	BlockNumber lastblock;
	Buffer		buffer;
	SMgrRelation smgr;

	Assert(blkno != P_NEW);

	/* Open the relation at smgr level */
	smgr = smgropen(rnode);

	/*
	 * Create the target file if it doesn't already exist.  This lets us cope
	 * if the replay sequence contains writes to a relation that is later
	 * deleted.  (The original coding of this routine would instead suppress
	 * the writes, but that seems like it risks losing valuable data if the
	 * filesystem loses an inode during a crash.  Better to write the data
	 * until we are actually told to delete the file.)
	 */
	smgrcreate(smgr, false, true);

	lastblock = smgrnblocks(smgr);

	if (blkno < lastblock)
	{
		/* page exists in file */
		buffer = ReadBufferWithoutRelcache(rnode, false, blkno, init);
	}
	else
	{
		/* hm, page doesn't exist in file */
		if (!init)
		{
			log_invalid_page(rnode, blkno, false);
			return InvalidBuffer;
		}
		/* OK to extend the file */
		/* we do this in recovery only - no rel-extension lock needed */
		Assert(InRecovery);
		buffer = InvalidBuffer;
		while (blkno >= lastblock)
		{
			if (buffer != InvalidBuffer)
				ReleaseBuffer(buffer);
			buffer = ReadBufferWithoutRelcache(rnode, false, P_NEW, false);
			lastblock++;
		}
		Assert(BufferGetBlockNumber(buffer) == blkno);
	}

	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	if (!init)
	{
		/* check that page has been initialized */
		Page		page = (Page) BufferGetPage(buffer);

		if (PageIsNew((PageHeader) page))
		{
			UnlockReleaseBuffer(buffer);
			log_invalid_page(rnode, blkno, true);
			return InvalidBuffer;
		}
	}

	return buffer;
}


/*
 * Struct actually returned by XLogFakeRelcacheEntry, though the declared
 * return type is Relation.
 */
typedef struct
{
	RelationData		reldata;	/* Note: this must be first */
	FormData_pg_class	pgc;
} FakeRelCacheEntryData;

typedef FakeRelCacheEntryData *FakeRelCacheEntry;

/*
 * Create a fake relation cache entry for a physical relation
 *
 * It's often convenient to use the same functions in XLOG replay as in the
 * main codepath, but those functions typically work with a relcache entry. 
 * We don't have a working relation cache during XLOG replay, but this 
 * function can be used to create a fake relcache entry instead. Only the 
 * fields related to physical storage, like rd_rel, are initialized, so the 
 * fake entry is only usable in low-level operations like ReadBuffer().
 *
 * Caller must free the returned entry with FreeFakeRelcacheEntry().
 */
Relation
CreateFakeRelcacheEntry(RelFileNode rnode)
{
	FakeRelCacheEntry fakeentry;
	Relation rel;

	/* Allocate the Relation struct and all related space in one block. */
	fakeentry = palloc0(sizeof(FakeRelCacheEntryData));
	rel = (Relation) fakeentry;

	rel->rd_rel = &fakeentry->pgc;
	rel->rd_node = rnode;

	/* We don't know the name of the relation; use relfilenode instead */
	sprintf(RelationGetRelationName(rel), "%u", rnode.relNode);

	/*
	 * We set up the lockRelId in case anything tries to lock the dummy
	 * relation.  Note that this is fairly bogus since relNode may be
	 * different from the relation's OID.  It shouldn't really matter
	 * though, since we are presumably running by ourselves and can't have
	 * any lock conflicts ...
	 */
	rel->rd_lockInfo.lockRelId.dbId = rnode.dbNode;
	rel->rd_lockInfo.lockRelId.relId = rnode.relNode;

	rel->rd_targblock = InvalidBlockNumber;
	rel->rd_smgr = NULL;

	return rel;
}

/*
 * Free a fake relation cache entry.
 */
void
FreeFakeRelcacheEntry(Relation fakerel)
{
	pfree(fakerel);
}

/*
 * Drop a relation during XLOG replay
 *
 * This is called when the relation is about to be deleted; we need to remove
 * any open "invalid-page" records for the relation.
 */
void
XLogDropRelation(RelFileNode rnode)
{
	/* Tell smgr to forget about this relation as well */
	smgrclosenode(rnode);

	forget_invalid_pages(rnode, 0);
}

/*
 * Drop a whole database during XLOG replay
 *
 * As above, but for DROP DATABASE instead of dropping a single rel
 */
void
XLogDropDatabase(Oid dbid)
{
	/*
	 * This is unnecessarily heavy-handed, as it will close SMgrRelation
	 * objects for other databases as well. DROP DATABASE occurs seldom
	 * enough that it's not worth introducing a variant of smgrclose for
	 * just this purpose. XXX: Or should we rather leave the smgr entries
	 * dangling?
	 */
	smgrcloseall();

	forget_invalid_pages_db(dbid);
}

/*
 * Truncate a relation during XLOG replay
 *
 * We need to clean up any open "invalid-page" records for the dropped pages.
 */
void
XLogTruncateRelation(RelFileNode rnode, BlockNumber nblocks)
{
	forget_invalid_pages(rnode, nblocks);
}
