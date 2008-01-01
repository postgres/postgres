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
 * $PostgreSQL: pgsql/src/backend/access/transam/xlogutils.c,v 1.51 2008/01/01 19:45:48 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xlogutils.h"
#include "storage/bufpage.h"
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
XLogReadBuffer(Relation reln, BlockNumber blkno, bool init)
{
	BlockNumber lastblock = RelationGetNumberOfBlocks(reln);
	Buffer		buffer;

	Assert(blkno != P_NEW);

	if (blkno < lastblock)
	{
		/* page exists in file */
		if (init)
			buffer = ReadOrZeroBuffer(reln, blkno);
		else
			buffer = ReadBuffer(reln, blkno);
	}
	else
	{
		/* hm, page doesn't exist in file */
		if (!init)
		{
			log_invalid_page(reln->rd_node, blkno, false);
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
			buffer = ReadBuffer(reln, P_NEW);
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
			log_invalid_page(reln->rd_node, blkno, true);
			return InvalidBuffer;
		}
	}

	return buffer;
}


/*
 * Lightweight "Relation" cache --- this substitutes for the normal relcache
 * during XLOG replay.
 */

typedef struct XLogRelDesc
{
	RelationData reldata;
	struct XLogRelDesc *lessRecently;
	struct XLogRelDesc *moreRecently;
} XLogRelDesc;

typedef struct XLogRelCacheEntry
{
	RelFileNode rnode;
	XLogRelDesc *rdesc;
} XLogRelCacheEntry;

static HTAB *_xlrelcache;
static XLogRelDesc *_xlrelarr = NULL;
static Form_pg_class _xlpgcarr = NULL;
static int	_xlast = 0;
static int	_xlcnt = 0;

#define _XLOG_RELCACHESIZE	512

static void
_xl_init_rel_cache(void)
{
	HASHCTL		ctl;

	_xlcnt = _XLOG_RELCACHESIZE;
	_xlast = 0;
	_xlrelarr = (XLogRelDesc *) malloc(sizeof(XLogRelDesc) * _xlcnt);
	memset(_xlrelarr, 0, sizeof(XLogRelDesc) * _xlcnt);
	_xlpgcarr = (Form_pg_class) malloc(sizeof(FormData_pg_class) * _xlcnt);
	memset(_xlpgcarr, 0, sizeof(FormData_pg_class) * _xlcnt);

	_xlrelarr[0].moreRecently = &(_xlrelarr[0]);
	_xlrelarr[0].lessRecently = &(_xlrelarr[0]);

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(RelFileNode);
	ctl.entrysize = sizeof(XLogRelCacheEntry);
	ctl.hash = tag_hash;

	_xlrelcache = hash_create("XLOG relcache", _XLOG_RELCACHESIZE,
							  &ctl, HASH_ELEM | HASH_FUNCTION);
}

static void
_xl_remove_hash_entry(XLogRelDesc *rdesc)
{
	Form_pg_class tpgc = rdesc->reldata.rd_rel;
	XLogRelCacheEntry *hentry;

	rdesc->lessRecently->moreRecently = rdesc->moreRecently;
	rdesc->moreRecently->lessRecently = rdesc->lessRecently;

	hentry = (XLogRelCacheEntry *) hash_search(_xlrelcache,
					  (void *) &(rdesc->reldata.rd_node), HASH_REMOVE, NULL);
	if (hentry == NULL)
		elog(PANIC, "_xl_remove_hash_entry: file was not found in cache");

	RelationCloseSmgr(&(rdesc->reldata));

	memset(rdesc, 0, sizeof(XLogRelDesc));
	memset(tpgc, 0, sizeof(FormData_pg_class));
	rdesc->reldata.rd_rel = tpgc;
}

static XLogRelDesc *
_xl_new_reldesc(void)
{
	XLogRelDesc *res;

	_xlast++;
	if (_xlast < _xlcnt)
	{
		_xlrelarr[_xlast].reldata.rd_rel = &(_xlpgcarr[_xlast]);
		return &(_xlrelarr[_xlast]);
	}

	/* reuse */
	res = _xlrelarr[0].moreRecently;

	_xl_remove_hash_entry(res);

	_xlast--;
	return res;
}


void
XLogInitRelationCache(void)
{
	_xl_init_rel_cache();
	invalid_page_tab = NULL;
}

void
XLogCloseRelationCache(void)
{
	HASH_SEQ_STATUS status;
	XLogRelCacheEntry *hentry;

	if (!_xlrelarr)
		return;

	hash_seq_init(&status, _xlrelcache);

	while ((hentry = (XLogRelCacheEntry *) hash_seq_search(&status)) != NULL)
		_xl_remove_hash_entry(hentry->rdesc);

	hash_destroy(_xlrelcache);

	free(_xlrelarr);
	free(_xlpgcarr);

	_xlrelarr = NULL;
}

/*
 * Open a relation during XLOG replay
 *
 * Note: this once had an API that allowed NULL return on failure, but it
 * no longer does; any failure results in elog().
 */
Relation
XLogOpenRelation(RelFileNode rnode)
{
	XLogRelDesc *res;
	XLogRelCacheEntry *hentry;
	bool		found;

	hentry = (XLogRelCacheEntry *)
		hash_search(_xlrelcache, (void *) &rnode, HASH_FIND, NULL);

	if (hentry)
	{
		res = hentry->rdesc;

		res->lessRecently->moreRecently = res->moreRecently;
		res->moreRecently->lessRecently = res->lessRecently;
	}
	else
	{
		res = _xl_new_reldesc();

		sprintf(RelationGetRelationName(&(res->reldata)), "%u", rnode.relNode);

		res->reldata.rd_node = rnode;

		/*
		 * We set up the lockRelId in case anything tries to lock the dummy
		 * relation.  Note that this is fairly bogus since relNode may be
		 * different from the relation's OID.  It shouldn't really matter
		 * though, since we are presumably running by ourselves and can't have
		 * any lock conflicts ...
		 */
		res->reldata.rd_lockInfo.lockRelId.dbId = rnode.dbNode;
		res->reldata.rd_lockInfo.lockRelId.relId = rnode.relNode;

		hentry = (XLogRelCacheEntry *)
			hash_search(_xlrelcache, (void *) &rnode, HASH_ENTER, &found);

		if (found)
			elog(PANIC, "xlog relation already present on insert into cache");

		hentry->rdesc = res;

		res->reldata.rd_targblock = InvalidBlockNumber;
		res->reldata.rd_smgr = NULL;
		RelationOpenSmgr(&(res->reldata));

		/*
		 * Create the target file if it doesn't already exist.  This lets us
		 * cope if the replay sequence contains writes to a relation that is
		 * later deleted.  (The original coding of this routine would instead
		 * return NULL, causing the writes to be suppressed. But that seems
		 * like it risks losing valuable data if the filesystem loses an inode
		 * during a crash.	Better to write the data until we are actually
		 * told to delete the file.)
		 */
		smgrcreate(res->reldata.rd_smgr, res->reldata.rd_istemp, true);
	}

	res->moreRecently = &(_xlrelarr[0]);
	res->lessRecently = _xlrelarr[0].lessRecently;
	_xlrelarr[0].lessRecently = res;
	res->lessRecently->moreRecently = res;

	return &(res->reldata);
}

/*
 * Drop a relation during XLOG replay
 *
 * This is called when the relation is about to be deleted; we need to ensure
 * that there is no dangling smgr reference in the xlog relation cache.
 *
 * Currently, we don't bother to physically remove the relation from the
 * cache, we just let it age out normally.
 *
 * This also takes care of removing any open "invalid-page" records for
 * the relation.
 */
void
XLogDropRelation(RelFileNode rnode)
{
	XLogRelCacheEntry *hentry;

	hentry = (XLogRelCacheEntry *)
		hash_search(_xlrelcache, (void *) &rnode, HASH_FIND, NULL);

	if (hentry)
	{
		XLogRelDesc *rdesc = hentry->rdesc;

		RelationCloseSmgr(&(rdesc->reldata));
	}

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
	HASH_SEQ_STATUS status;
	XLogRelCacheEntry *hentry;

	hash_seq_init(&status, _xlrelcache);

	while ((hentry = (XLogRelCacheEntry *) hash_seq_search(&status)) != NULL)
	{
		XLogRelDesc *rdesc = hentry->rdesc;

		if (hentry->rnode.dbNode == dbid)
			RelationCloseSmgr(&(rdesc->reldata));
	}

	forget_invalid_pages_db(dbid);
}

/*
 * Truncate a relation during XLOG replay
 *
 * We don't need to do anything to the fake relcache, but we do need to
 * clean up any open "invalid-page" records for the dropped pages.
 */
void
XLogTruncateRelation(RelFileNode rnode, BlockNumber nblocks)
{
	forget_invalid_pages(rnode, nblocks);
}
