/*-------------------------------------------------------------------------
 *
 * xlogutils.c
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/backend/access/transam/xlogutils.c,v 1.29 2004/02/10 01:55:24 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup.h"
#include "access/xlogutils.h"
#include "catalog/pg_database.h"
#include "storage/bufpage.h"
#include "storage/smgr.h"
#include "utils/hsearch.h"
#include "utils/relcache.h"


/*
 * ---------------------------------------------------------------
 *
 * Index support functions
 *
 *----------------------------------------------------------------
 */

/*
 * Check if specified heap tuple was inserted by given
 * xaction/command and return
 *
 * - -1 if not
 * - 0	if there is no tuple at all
 * - 1	if yes
 */
int
XLogIsOwnerOfTuple(RelFileNode hnode, ItemPointer iptr,
				   TransactionId xid, CommandId cid)
{
	Relation	reln;
	Buffer		buffer;
	Page		page;
	ItemId		lp;
	HeapTupleHeader htup;

	reln = XLogOpenRelation(false, RM_HEAP_ID, hnode);
	if (!RelationIsValid(reln))
		return (0);

	buffer = ReadBuffer(reln, ItemPointerGetBlockNumber(iptr));
	if (!BufferIsValid(buffer))
		return (0);

	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	page = (Page) BufferGetPage(buffer);
	if (PageIsNew((PageHeader) page) ||
		ItemPointerGetOffsetNumber(iptr) > PageGetMaxOffsetNumber(page))
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
		return (0);
	}
	lp = PageGetItemId(page, ItemPointerGetOffsetNumber(iptr));
	if (!ItemIdIsUsed(lp) || ItemIdDeleted(lp))
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
		return (0);
	}

	htup = (HeapTupleHeader) PageGetItem(page, lp);

	Assert(PageGetSUI(page) == ThisStartUpID);
	if (!TransactionIdEquals(HeapTupleHeaderGetXmin(htup), xid) ||
		HeapTupleHeaderGetCmin(htup) != cid)
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
		return (-1);
	}

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(buffer);
	return (1);
}

/*
 * MUST BE CALLED ONLY ON RECOVERY.
 *
 * Check if exists valid (inserted by not aborted xaction) heap tuple
 * for given item pointer
 */
bool
XLogIsValidTuple(RelFileNode hnode, ItemPointer iptr)
{
	Relation	reln;
	Buffer		buffer;
	Page		page;
	ItemId		lp;
	HeapTupleHeader htup;

	reln = XLogOpenRelation(false, RM_HEAP_ID, hnode);
	if (!RelationIsValid(reln))
		return (false);

	buffer = ReadBuffer(reln, ItemPointerGetBlockNumber(iptr));
	if (!BufferIsValid(buffer))
		return (false);

	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	page = (Page) BufferGetPage(buffer);
	if (PageIsNew((PageHeader) page) ||
		ItemPointerGetOffsetNumber(iptr) > PageGetMaxOffsetNumber(page))
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
		return (false);
	}

	if (PageGetSUI(page) != ThisStartUpID)
	{
		Assert(PageGetSUI(page) < ThisStartUpID);
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
		return (true);
	}

	lp = PageGetItemId(page, ItemPointerGetOffsetNumber(iptr));
	if (!ItemIdIsUsed(lp) || ItemIdDeleted(lp))
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
		return (false);
	}

	htup = (HeapTupleHeader) PageGetItem(page, lp);

	/* MUST CHECK WASN'T TUPLE INSERTED IN PREV STARTUP */

	if (!(htup->t_infomask & HEAP_XMIN_COMMITTED))
	{
		if (htup->t_infomask & HEAP_XMIN_INVALID ||
			(htup->t_infomask & HEAP_MOVED_IN &&
			 TransactionIdDidAbort(HeapTupleHeaderGetXvac(htup))) ||
			TransactionIdDidAbort(HeapTupleHeaderGetXmin(htup)))
		{
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			ReleaseBuffer(buffer);
			return (false);
		}
	}

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(buffer);
	return (true);
}

/*
 * ---------------------------------------------------------------
 *
 * Storage related support functions
 *
 *----------------------------------------------------------------
 */

Buffer
XLogReadBuffer(bool extend, Relation reln, BlockNumber blkno)
{
	BlockNumber lastblock = RelationGetNumberOfBlocks(reln);
	Buffer		buffer;

	if (blkno >= lastblock)
	{
		buffer = InvalidBuffer;
		if (extend)				/* we do this in recovery only - no locks */
		{
			Assert(InRecovery);
			while (lastblock <= blkno)
			{
				if (buffer != InvalidBuffer)
					ReleaseBuffer(buffer);		/* must be WriteBuffer()? */
				buffer = ReadBuffer(reln, P_NEW);
				lastblock++;
			}
		}
		if (buffer != InvalidBuffer)
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		return (buffer);
	}

	buffer = ReadBuffer(reln, blkno);
	if (buffer != InvalidBuffer)
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	return (buffer);
}

/*
 * "Relation" cache
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

	if (rdesc->reldata.rd_smgr != NULL)
		smgrclose(rdesc->reldata.rd_smgr);

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
		return (&(_xlrelarr[_xlast]));
	}

	/* reuse */
	res = _xlrelarr[0].moreRecently;

	_xl_remove_hash_entry(res);

	_xlast--;
	return (res);
}


void
XLogInitRelationCache(void)
{
	_xl_init_rel_cache();
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

Relation
XLogOpenRelation(bool redo, RmgrId rmid, RelFileNode rnode)
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
		 * though, since we are presumably running by ourselves and can't
		 * have any lock conflicts ...
		 */
		res->reldata.rd_lockInfo.lockRelId.dbId = rnode.tblNode;
		res->reldata.rd_lockInfo.lockRelId.relId = rnode.relNode;

		hentry = (XLogRelCacheEntry *)
			hash_search(_xlrelcache, (void *) &rnode, HASH_ENTER, &found);

		if (hentry == NULL)
			elog(PANIC, "XLogOpenRelation: out of memory for cache");

		if (found)
			elog(PANIC, "XLogOpenRelation: file found on insert into cache");

		hentry->rdesc = res;

		res->reldata.rd_targblock = InvalidBlockNumber;
		res->reldata.rd_smgr = smgropen(res->reldata.rd_node);
		/*
		 * Create the target file if it doesn't already exist.  This lets
		 * us cope if the replay sequence contains writes to a relation
		 * that is later deleted.  (The original coding of this routine
		 * would instead return NULL, causing the writes to be suppressed.
		 * But that seems like it risks losing valuable data if the filesystem
		 * loses an inode during a crash.  Better to write the data until we
		 * are actually told to delete the file.)
		 */
		smgrcreate(res->reldata.rd_smgr, res->reldata.rd_istemp, true);
	}

	res->moreRecently = &(_xlrelarr[0]);
	res->lessRecently = _xlrelarr[0].lessRecently;
	_xlrelarr[0].lessRecently = res;
	res->lessRecently->moreRecently = res;

	return (&(res->reldata));
}
