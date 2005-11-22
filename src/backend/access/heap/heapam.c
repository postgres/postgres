/*-------------------------------------------------------------------------
 *
 * heapam.c
 *	  heap access method code
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/heap/heapam.c,v 1.200.2.2 2005/11/22 18:23:03 momjian Exp $
 *
 *
 * INTERFACE ROUTINES
 *		relation_open	- open any relation by relation OID
 *		relation_openrv - open any relation specified by a RangeVar
 *		relation_close	- close any relation
 *		heap_open		- open a heap relation by relation OID
 *		heap_openrv		- open a heap relation specified by a RangeVar
 *		heap_close		- (now just a macro for relation_close)
 *		heap_beginscan	- begin relation scan
 *		heap_rescan		- restart a relation scan
 *		heap_endscan	- end relation scan
 *		heap_getnext	- retrieve next tuple in scan
 *		heap_fetch		- retrieve tuple with given tid
 *		heap_insert		- insert tuple into a relation
 *		heap_delete		- delete a tuple from a relation
 *		heap_update		- replace a tuple in a relation with another tuple
 *		heap_markpos	- mark scan position
 *		heap_restrpos	- restore position to marked location
 *
 * NOTES
 *	  This file contains the heap_ routines which implement
 *	  the POSTGRES heap access method used for all POSTGRES
 *	  relations.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/hio.h"
#include "access/multixact.h"
#include "access/tuptoaster.h"
#include "access/valid.h"
#include "access/xlogutils.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/procarray.h"
#include "utils/inval.h"
#include "utils/relcache.h"


static XLogRecPtr log_heap_update(Relation reln, Buffer oldbuf,
		   ItemPointerData from, Buffer newbuf, HeapTuple newtup, bool move);


/* ----------------------------------------------------------------
 *						 heap support routines
 * ----------------------------------------------------------------
 */

/* ----------------
 *		initscan - scan code common to heap_beginscan and heap_rescan
 * ----------------
 */
static void
initscan(HeapScanDesc scan, ScanKey key)
{
	/*
	 * Determine the number of blocks we have to scan.
	 *
	 * It is sufficient to do this once at scan start, since any tuples added
	 * while the scan is in progress will be invisible to my transaction
	 * anyway...
	 */
	scan->rs_nblocks = RelationGetNumberOfBlocks(scan->rs_rd);

	scan->rs_ctup.t_datamcxt = NULL;
	scan->rs_ctup.t_data = NULL;
	scan->rs_cbuf = InvalidBuffer;

	/* we don't have a marked position... */
	ItemPointerSetInvalid(&(scan->rs_mctid));

	/*
	 * copy the scan key, if appropriate
	 */
	if (key != NULL)
		memcpy(scan->rs_key, key, scan->rs_nkeys * sizeof(ScanKeyData));

	pgstat_count_heap_scan(&scan->rs_pgstat_info);
}

/* ----------------
 *		heapgettup - fetch next heap tuple
 *
 *		routine used by heap_getnext() which does most of the
 *		real work in scanning tuples.
 *
 *		The passed-in *buffer must be either InvalidBuffer or the pinned
 *		current page of the scan.  If we have to move to another page,
 *		we will unpin this buffer (if valid).  On return, *buffer is either
 *		InvalidBuffer or the ID of a pinned buffer.
 * ----------------
 */
static void
heapgettup(Relation relation,
		   int dir,
		   HeapTuple tuple,
		   Buffer *buffer,
		   Snapshot snapshot,
		   int nkeys,
		   ScanKey key,
		   BlockNumber pages)
{
	ItemId		lpp;
	Page		dp;
	BlockNumber page;
	int			lines;
	OffsetNumber lineoff;
	int			linesleft;
	ItemPointer tid;

	tid = (tuple->t_data == NULL) ? NULL : &(tuple->t_self);

	/*
	 * debugging stuff
	 *
	 * check validity of arguments, here and for other functions too Note: no
	 * locking manipulations needed--this is a local function
	 */
#ifdef	HEAPDEBUGALL
	if (ItemPointerIsValid(tid))
		elog(DEBUG2, "heapgettup(%s, tid=0x%x[%d,%d], dir=%d, ...)",
			 RelationGetRelationName(relation), tid, tid->ip_blkid,
			 tid->ip_posid, dir);
	else
		elog(DEBUG2, "heapgettup(%s, tid=0x%x, dir=%d, ...)",
			 RelationGetRelationName(relation), tid, dir);

	elog(DEBUG2, "heapgettup(..., b=0x%x, nkeys=%d, key=0x%x", buffer, nkeys, key);

	elog(DEBUG2, "heapgettup: relation(%c)=`%s', %p",
		 relation->rd_rel->relkind, RelationGetRelationName(relation),
		 snapshot);
#endif   /* HEAPDEBUGALL */

	if (!ItemPointerIsValid(tid))
	{
		Assert(!PointerIsValid(tid));
		tid = NULL;
	}

	tuple->t_tableOid = RelationGetRelid(relation);

	/*
	 * return null immediately if relation is empty
	 */
	if (pages == 0)
	{
		if (BufferIsValid(*buffer))
			ReleaseBuffer(*buffer);
		*buffer = InvalidBuffer;
		tuple->t_datamcxt = NULL;
		tuple->t_data = NULL;
		return;
	}

	/*
	 * calculate next starting lineoff, given scan direction
	 */
	if (dir == 0)
	{
		/*
		 * ``no movement'' scan direction: refetch same tuple
		 */
		if (tid == NULL)
		{
			if (BufferIsValid(*buffer))
				ReleaseBuffer(*buffer);
			*buffer = InvalidBuffer;
			tuple->t_datamcxt = NULL;
			tuple->t_data = NULL;
			return;
		}

		*buffer = ReleaseAndReadBuffer(*buffer,
									   relation,
									   ItemPointerGetBlockNumber(tid));

		LockBuffer(*buffer, BUFFER_LOCK_SHARE);

		dp = (Page) BufferGetPage(*buffer);
		lineoff = ItemPointerGetOffsetNumber(tid);
		lpp = PageGetItemId(dp, lineoff);

		tuple->t_datamcxt = NULL;
		tuple->t_data = (HeapTupleHeader) PageGetItem((Page) dp, lpp);
		tuple->t_len = ItemIdGetLength(lpp);
		LockBuffer(*buffer, BUFFER_LOCK_UNLOCK);

		return;
	}
	else if (dir < 0)
	{
		/*
		 * reverse scan direction
		 */
		if (tid == NULL)
		{
			page = pages - 1;	/* final page */
		}
		else
		{
			page = ItemPointerGetBlockNumber(tid);		/* current page */
		}

		Assert(page < pages);

		*buffer = ReleaseAndReadBuffer(*buffer,
									   relation,
									   page);

		LockBuffer(*buffer, BUFFER_LOCK_SHARE);

		dp = (Page) BufferGetPage(*buffer);
		lines = PageGetMaxOffsetNumber(dp);
		if (tid == NULL)
		{
			lineoff = lines;	/* final offnum */
		}
		else
		{
			lineoff =			/* previous offnum */
				OffsetNumberPrev(ItemPointerGetOffsetNumber(tid));
		}
		/* page and lineoff now reference the physically previous tid */
	}
	else
	{
		/*
		 * forward scan direction
		 */
		if (tid == NULL)
		{
			page = 0;			/* first page */
			lineoff = FirstOffsetNumber;		/* first offnum */
		}
		else
		{
			page = ItemPointerGetBlockNumber(tid);		/* current page */
			lineoff =			/* next offnum */
				OffsetNumberNext(ItemPointerGetOffsetNumber(tid));
		}

		Assert(page < pages);

		*buffer = ReleaseAndReadBuffer(*buffer,
									   relation,
									   page);

		LockBuffer(*buffer, BUFFER_LOCK_SHARE);

		dp = (Page) BufferGetPage(*buffer);
		lines = PageGetMaxOffsetNumber(dp);
		/* page and lineoff now reference the physically next tid */
	}

	/* 'dir' is now non-zero */

	/*
	 * calculate line pointer and number of remaining items to check on this
	 * page.
	 */
	lpp = PageGetItemId(dp, lineoff);
	if (dir < 0)
		linesleft = lineoff - 1;
	else
		linesleft = lines - lineoff;

	/*
	 * advance the scan until we find a qualifying tuple or run out of stuff
	 * to scan
	 */
	for (;;)
	{
		while (linesleft >= 0)
		{
			if (ItemIdIsUsed(lpp))
			{
				bool		valid;

				tuple->t_datamcxt = NULL;
				tuple->t_data = (HeapTupleHeader) PageGetItem((Page) dp, lpp);
				tuple->t_len = ItemIdGetLength(lpp);
				ItemPointerSet(&(tuple->t_self), page, lineoff);

				/*
				 * if current tuple qualifies, return it.
				 */
				HeapTupleSatisfies(tuple, relation, *buffer, (PageHeader) dp,
								   snapshot, nkeys, key, valid);
				if (valid)
				{
					LockBuffer(*buffer, BUFFER_LOCK_UNLOCK);
					return;
				}
			}

			/*
			 * otherwise move to the next item on the page
			 */
			--linesleft;
			if (dir < 0)
			{
				--lpp;			/* move back in this page's ItemId array */
				--lineoff;
			}
			else
			{
				++lpp;			/* move forward in this page's ItemId array */
				++lineoff;
			}
		}

		/*
		 * if we get here, it means we've exhausted the items on this page and
		 * it's time to move to the next.
		 */
		LockBuffer(*buffer, BUFFER_LOCK_UNLOCK);

		/*
		 * return NULL if we've exhausted all the pages
		 */
		if ((dir < 0) ? (page == 0) : (page + 1 >= pages))
		{
			if (BufferIsValid(*buffer))
				ReleaseBuffer(*buffer);
			*buffer = InvalidBuffer;
			tuple->t_datamcxt = NULL;
			tuple->t_data = NULL;
			return;
		}

		page = (dir < 0) ? (page - 1) : (page + 1);

		Assert(page < pages);

		*buffer = ReleaseAndReadBuffer(*buffer,
									   relation,
									   page);

		LockBuffer(*buffer, BUFFER_LOCK_SHARE);
		dp = (Page) BufferGetPage(*buffer);
		lines = PageGetMaxOffsetNumber((Page) dp);
		linesleft = lines - 1;
		if (dir < 0)
		{
			lineoff = lines;
			lpp = PageGetItemId(dp, lines);
		}
		else
		{
			lineoff = FirstOffsetNumber;
			lpp = PageGetItemId(dp, FirstOffsetNumber);
		}
	}
}


#if defined(DISABLE_COMPLEX_MACRO)
/*
 * This is formatted so oddly so that the correspondence to the macro
 * definition in access/heapam.h is maintained.
 */
Datum
fastgetattr(HeapTuple tup, int attnum, TupleDesc tupleDesc,
			bool *isnull)
{
	return (
			(attnum) > 0 ?
			(
			 ((isnull) ? (*(isnull) = false) : (dummyret) NULL),
			 HeapTupleNoNulls(tup) ?
			 (
			  (tupleDesc)->attrs[(attnum) - 1]->attcacheoff >= 0 ?
			  (
			   fetchatt((tupleDesc)->attrs[(attnum) - 1],
						(char *) (tup)->t_data + (tup)->t_data->t_hoff +
						(tupleDesc)->attrs[(attnum) - 1]->attcacheoff)
			   )
			  :
			  nocachegetattr((tup), (attnum), (tupleDesc), (isnull))
			  )
			 :
			 (
			  att_isnull((attnum) - 1, (tup)->t_data->t_bits) ?
			  (
			   ((isnull) ? (*(isnull) = true) : (dummyret) NULL),
			   (Datum) NULL
			   )
			  :
			  (
			   nocachegetattr((tup), (attnum), (tupleDesc), (isnull))
			   )
			  )
			 )
			:
			(
			 (Datum) NULL
			 )
		);
}
#endif   /* defined(DISABLE_COMPLEX_MACRO) */


/* ----------------------------------------------------------------
 *					 heap access method interface
 * ----------------------------------------------------------------
 */

/* ----------------
 *		relation_open - open any relation by relation OID
 *
 *		If lockmode is not "NoLock", the specified kind of lock is
 *		obtained on the relation.  (Generally, NoLock should only be
 *		used if the caller knows it has some appropriate lock on the
 *		relation already.)
 *
 *		An error is raised if the relation does not exist.
 *
 *		NB: a "relation" is anything with a pg_class entry.  The caller is
 *		expected to check whether the relkind is something it can handle.
 * ----------------
 */
Relation
relation_open(Oid relationId, LOCKMODE lockmode)
{
	Relation	r;

	Assert(lockmode >= NoLock && lockmode < MAX_LOCKMODES);

	/* The relcache does all the real work... */
	r = RelationIdGetRelation(relationId);

	if (!RelationIsValid(r))
		elog(ERROR, "could not open relation with OID %u", relationId);

	if (lockmode != NoLock)
		LockRelation(r, lockmode);

	return r;
}

/* ----------------
 *		conditional_relation_open - open with option not to wait
 *
 *		As above, but if nowait is true, then throw an error rather than
 *		waiting when the lock is not immediately obtainable.
 * ----------------
 */
Relation
conditional_relation_open(Oid relationId, LOCKMODE lockmode, bool nowait)
{
	Relation	r;

	Assert(lockmode >= NoLock && lockmode < MAX_LOCKMODES);

	/* The relcache does all the real work... */
	r = RelationIdGetRelation(relationId);

	if (!RelationIsValid(r))
		elog(ERROR, "could not open relation with OID %u", relationId);

	if (lockmode != NoLock)
	{
		if (nowait)
		{
			if (!ConditionalLockRelation(r, lockmode))
				ereport(ERROR,
						(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
						 errmsg("could not obtain lock on relation \"%s\"",
								RelationGetRelationName(r))));
		}
		else
			LockRelation(r, lockmode);
	}

	return r;
}

/* ----------------
 *		relation_openrv - open any relation specified by a RangeVar
 *
 *		As above, but the relation is specified by a RangeVar.
 * ----------------
 */
Relation
relation_openrv(const RangeVar *relation, LOCKMODE lockmode)
{
	Oid			relOid;

	/*
	 * Check for shared-cache-inval messages before trying to open the
	 * relation.  This is needed to cover the case where the name identifies a
	 * rel that has been dropped and recreated since the start of our
	 * transaction: if we don't flush the old syscache entry then we'll latch
	 * onto that entry and suffer an error when we do LockRelation. Note that
	 * relation_open does not need to do this, since a relation's OID never
	 * changes.
	 *
	 * We skip this if asked for NoLock, on the assumption that the caller has
	 * already ensured some appropriate lock is held.
	 */
	if (lockmode != NoLock)
		AcceptInvalidationMessages();

	/* Look up the appropriate relation using namespace search */
	relOid = RangeVarGetRelid(relation, false);

	/* Let relation_open do the rest */
	return relation_open(relOid, lockmode);
}

/* ----------------
 *		relation_close - close any relation
 *
 *		If lockmode is not "NoLock", we first release the specified lock.
 *
 *		Note that it is often sensible to hold a lock beyond relation_close;
 *		in that case, the lock is released automatically at xact end.
 * ----------------
 */
void
relation_close(Relation relation, LOCKMODE lockmode)
{
	Assert(lockmode >= NoLock && lockmode < MAX_LOCKMODES);

	if (lockmode != NoLock)
		UnlockRelation(relation, lockmode);

	/* The relcache does the real work... */
	RelationClose(relation);
}


/* ----------------
 *		heap_open - open a heap relation by relation OID
 *
 *		This is essentially relation_open plus check that the relation
 *		is not an index or special relation.  (The caller should also check
 *		that it's not a view before assuming it has storage.)
 * ----------------
 */
Relation
heap_open(Oid relationId, LOCKMODE lockmode)
{
	Relation	r;

	r = relation_open(relationId, lockmode);

	if (r->rd_rel->relkind == RELKIND_INDEX)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is an index",
						RelationGetRelationName(r))));
	else if (r->rd_rel->relkind == RELKIND_SPECIAL)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is a special relation",
						RelationGetRelationName(r))));
	else if (r->rd_rel->relkind == RELKIND_COMPOSITE_TYPE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is a composite type",
						RelationGetRelationName(r))));

	pgstat_initstats(&r->pgstat_info, r);

	return r;
}

/* ----------------
 *		heap_openrv - open a heap relation specified
 *		by a RangeVar node
 *
 *		As above, but relation is specified by a RangeVar.
 * ----------------
 */
Relation
heap_openrv(const RangeVar *relation, LOCKMODE lockmode)
{
	Relation	r;

	r = relation_openrv(relation, lockmode);

	if (r->rd_rel->relkind == RELKIND_INDEX)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is an index",
						RelationGetRelationName(r))));
	else if (r->rd_rel->relkind == RELKIND_SPECIAL)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is a special relation",
						RelationGetRelationName(r))));
	else if (r->rd_rel->relkind == RELKIND_COMPOSITE_TYPE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is a composite type",
						RelationGetRelationName(r))));

	pgstat_initstats(&r->pgstat_info, r);

	return r;
}


/* ----------------
 *		heap_beginscan	- begin relation scan
 * ----------------
 */
HeapScanDesc
heap_beginscan(Relation relation, Snapshot snapshot,
			   int nkeys, ScanKey key)
{
	HeapScanDesc scan;

	/*
	 * increment relation ref count while scanning relation
	 *
	 * This is just to make really sure the relcache entry won't go away while
	 * the scan has a pointer to it.  Caller should be holding the rel open
	 * anyway, so this is redundant in all normal scenarios...
	 */
	RelationIncrementReferenceCount(relation);

	/*
	 * allocate and initialize scan descriptor
	 */
	scan = (HeapScanDesc) palloc(sizeof(HeapScanDescData));

	scan->rs_rd = relation;
	scan->rs_snapshot = snapshot;
	scan->rs_nkeys = nkeys;

	/*
	 * we do this here instead of in initscan() because heap_rescan also calls
	 * initscan() and we don't want to allocate memory again
	 */
	if (nkeys > 0)
		scan->rs_key = (ScanKey) palloc(sizeof(ScanKeyData) * nkeys);
	else
		scan->rs_key = NULL;

	pgstat_initstats(&scan->rs_pgstat_info, relation);

	initscan(scan, key);

	return scan;
}

/* ----------------
 *		heap_rescan		- restart a relation scan
 * ----------------
 */
void
heap_rescan(HeapScanDesc scan,
			ScanKey key)
{
	/*
	 * unpin scan buffers
	 */
	if (BufferIsValid(scan->rs_cbuf))
		ReleaseBuffer(scan->rs_cbuf);

	/*
	 * reinitialize scan descriptor
	 */
	initscan(scan, key);
}

/* ----------------
 *		heap_endscan	- end relation scan
 *
 *		See how to integrate with index scans.
 *		Check handling if reldesc caching.
 * ----------------
 */
void
heap_endscan(HeapScanDesc scan)
{
	/* Note: no locking manipulations needed */

	/*
	 * unpin scan buffers
	 */
	if (BufferIsValid(scan->rs_cbuf))
		ReleaseBuffer(scan->rs_cbuf);

	/*
	 * decrement relation reference count and free scan descriptor storage
	 */
	RelationDecrementReferenceCount(scan->rs_rd);

	if (scan->rs_key)
		pfree(scan->rs_key);

	pfree(scan);
}

/* ----------------
 *		heap_getnext	- retrieve next tuple in scan
 *
 *		Fix to work with index relations.
 *		We don't return the buffer anymore, but you can get it from the
 *		returned HeapTuple.
 * ----------------
 */

#ifdef HEAPDEBUGALL
#define HEAPDEBUG_1 \
	elog(DEBUG2, "heap_getnext([%s,nkeys=%d],dir=%d) called", \
		 RelationGetRelationName(scan->rs_rd), scan->rs_nkeys, (int) direction)
#define HEAPDEBUG_2 \
	elog(DEBUG2, "heap_getnext returning EOS")
#define HEAPDEBUG_3 \
	elog(DEBUG2, "heap_getnext returning tuple")
#else
#define HEAPDEBUG_1
#define HEAPDEBUG_2
#define HEAPDEBUG_3
#endif   /* !defined(HEAPDEBUGALL) */


HeapTuple
heap_getnext(HeapScanDesc scan, ScanDirection direction)
{
	/* Note: no locking manipulations needed */

	HEAPDEBUG_1;				/* heap_getnext( info ) */

	/*
	 * Note: we depend here on the -1/0/1 encoding of ScanDirection.
	 */
	heapgettup(scan->rs_rd,
			   (int) direction,
			   &(scan->rs_ctup),
			   &(scan->rs_cbuf),
			   scan->rs_snapshot,
			   scan->rs_nkeys,
			   scan->rs_key,
			   scan->rs_nblocks);

	if (scan->rs_ctup.t_data == NULL && !BufferIsValid(scan->rs_cbuf))
	{
		HEAPDEBUG_2;			/* heap_getnext returning EOS */
		return NULL;
	}

	/*
	 * if we get here it means we have a new current scan tuple, so point to
	 * the proper return buffer and return the tuple.
	 */

	HEAPDEBUG_3;				/* heap_getnext returning tuple */

	if (scan->rs_ctup.t_data != NULL)
		pgstat_count_heap_getnext(&scan->rs_pgstat_info);

	return ((scan->rs_ctup.t_data == NULL) ? NULL : &(scan->rs_ctup));
}

/*
 *	heap_fetch		- retrieve tuple with given tid
 *
 * On entry, tuple->t_self is the TID to fetch.  We pin the buffer holding
 * the tuple, fill in the remaining fields of *tuple, and check the tuple
 * against the specified snapshot.
 *
 * If successful (tuple found and passes snapshot time qual), then *userbuf
 * is set to the buffer holding the tuple and TRUE is returned.  The caller
 * must unpin the buffer when done with the tuple.
 *
 * If the tuple is not found (ie, item number references a deleted slot),
 * then tuple->t_data is set to NULL and FALSE is returned.
 *
 * If the tuple is found but fails the time qual check, then FALSE is returned
 * but tuple->t_data is left pointing to the tuple.
 *
 * keep_buf determines what is done with the buffer in the FALSE-result cases.
 * When the caller specifies keep_buf = true, we retain the pin on the buffer
 * and return it in *userbuf (so the caller must eventually unpin it); when
 * keep_buf = false, the pin is released and *userbuf is set to InvalidBuffer.
 *
 * It is somewhat inconsistent that we ereport() on invalid block number but
 * return false on invalid item number.  There are a couple of reasons though.
 * One is that the caller can relatively easily check the block number for
 * validity, but cannot check the item number without reading the page
 * himself.  Another is that when we are following a t_ctid link, we can be
 * reasonably confident that the page number is valid (since VACUUM shouldn't
 * truncate off the destination page without having killed the referencing
 * tuple first), but the item number might well not be good.
 */
bool
heap_fetch(Relation relation,
		   Snapshot snapshot,
		   HeapTuple tuple,
		   Buffer *userbuf,
		   bool keep_buf,
		   PgStat_Info *pgstat_info)
{
	/* Assume *userbuf is undefined on entry */
	*userbuf = InvalidBuffer;
	return heap_release_fetch(relation, snapshot, tuple,
							  userbuf, keep_buf, pgstat_info);
}

/*
 *	heap_release_fetch		- retrieve tuple with given tid
 *
 * This has the same API as heap_fetch except that if *userbuf is not
 * InvalidBuffer on entry, that buffer will be released before reading
 * the new page.  This saves a separate ReleaseBuffer step and hence
 * one entry into the bufmgr when looping through multiple fetches.
 * Also, if *userbuf is the same buffer that holds the target tuple,
 * we avoid bufmgr manipulation altogether.
 */
bool
heap_release_fetch(Relation relation,
				   Snapshot snapshot,
				   HeapTuple tuple,
				   Buffer *userbuf,
				   bool keep_buf,
				   PgStat_Info *pgstat_info)
{
	ItemPointer tid = &(tuple->t_self);
	ItemId		lp;
	Buffer		buffer;
	PageHeader	dp;
	OffsetNumber offnum;
	bool		valid;

	/*
	 * get the buffer from the relation descriptor. Note that this does a
	 * buffer pin, and releases the old *userbuf if not InvalidBuffer.
	 */
	buffer = ReleaseAndReadBuffer(*userbuf, relation,
								  ItemPointerGetBlockNumber(tid));

	/*
	 * Need share lock on buffer to examine tuple commit status.
	 */
	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	dp = (PageHeader) BufferGetPage(buffer);

	/*
	 * We'd better check for out-of-range offnum in case of VACUUM since the
	 * TID was obtained.
	 */
	offnum = ItemPointerGetOffsetNumber(tid);
	if (offnum < FirstOffsetNumber || offnum > PageGetMaxOffsetNumber(dp))
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		if (keep_buf)
			*userbuf = buffer;
		else
		{
			ReleaseBuffer(buffer);
			*userbuf = InvalidBuffer;
		}
		tuple->t_datamcxt = NULL;
		tuple->t_data = NULL;
		return false;
	}

	/*
	 * get the item line pointer corresponding to the requested tid
	 */
	lp = PageGetItemId(dp, offnum);

	/*
	 * Must check for deleted tuple.
	 */
	if (!ItemIdIsUsed(lp))
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		if (keep_buf)
			*userbuf = buffer;
		else
		{
			ReleaseBuffer(buffer);
			*userbuf = InvalidBuffer;
		}
		tuple->t_datamcxt = NULL;
		tuple->t_data = NULL;
		return false;
	}

	/*
	 * fill in *tuple fields
	 */
	tuple->t_datamcxt = NULL;
	tuple->t_data = (HeapTupleHeader) PageGetItem((Page) dp, lp);
	tuple->t_len = ItemIdGetLength(lp);
	tuple->t_tableOid = RelationGetRelid(relation);

	/*
	 * check time qualification of tuple, then release lock
	 */
	HeapTupleSatisfies(tuple, relation, buffer, dp,
					   snapshot, 0, NULL, valid);

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

	if (valid)
	{
		/*
		 * All checks passed, so return the tuple as valid. Caller is now
		 * responsible for releasing the buffer.
		 */
		*userbuf = buffer;

		/* Count the successful fetch in *pgstat_info, if given. */
		if (pgstat_info != NULL)
			pgstat_count_heap_fetch(pgstat_info);

		return true;
	}

	/* Tuple failed time qual, but maybe caller wants to see it anyway. */
	if (keep_buf)
		*userbuf = buffer;
	else
	{
		ReleaseBuffer(buffer);
		*userbuf = InvalidBuffer;
	}

	return false;
}

/*
 *	heap_get_latest_tid -  get the latest tid of a specified tuple
 *
 * Actually, this gets the latest version that is visible according to
 * the passed snapshot.  You can pass SnapshotDirty to get the very latest,
 * possibly uncommitted version.
 *
 * *tid is both an input and an output parameter: it is updated to
 * show the latest version of the row.	Note that it will not be changed
 * if no version of the row passes the snapshot test.
 */
void
heap_get_latest_tid(Relation relation,
					Snapshot snapshot,
					ItemPointer tid)
{
	BlockNumber blk;
	ItemPointerData ctid;
	TransactionId priorXmax;

	/* this is to avoid Assert failures on bad input */
	if (!ItemPointerIsValid(tid))
		return;

	/*
	 * Since this can be called with user-supplied TID, don't trust the input
	 * too much.  (RelationGetNumberOfBlocks is an expensive check, so we
	 * don't check t_ctid links again this way.  Note that it would not do to
	 * call it just once and save the result, either.)
	 */
	blk = ItemPointerGetBlockNumber(tid);
	if (blk >= RelationGetNumberOfBlocks(relation))
		elog(ERROR, "block number %u is out of range for relation \"%s\"",
			 blk, RelationGetRelationName(relation));

	/*
	 * Loop to chase down t_ctid links.  At top of loop, ctid is the tuple we
	 * need to examine, and *tid is the TID we will return if ctid turns out
	 * to be bogus.
	 *
	 * Note that we will loop until we reach the end of the t_ctid chain.
	 * Depending on the snapshot passed, there might be at most one visible
	 * version of the row, but we don't try to optimize for that.
	 */
	ctid = *tid;
	priorXmax = InvalidTransactionId;	/* cannot check first XMIN */
	for (;;)
	{
		Buffer		buffer;
		PageHeader	dp;
		OffsetNumber offnum;
		ItemId		lp;
		HeapTupleData tp;
		bool		valid;

		/*
		 * Read, pin, and lock the page.
		 */
		buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(&ctid));
		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		dp = (PageHeader) BufferGetPage(buffer);

		/*
		 * Check for bogus item number.  This is not treated as an error
		 * condition because it can happen while following a t_ctid link. We
		 * just assume that the prior tid is OK and return it unchanged.
		 */
		offnum = ItemPointerGetOffsetNumber(&ctid);
		if (offnum < FirstOffsetNumber || offnum > PageGetMaxOffsetNumber(dp))
		{
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			ReleaseBuffer(buffer);
			break;
		}
		lp = PageGetItemId(dp, offnum);
		if (!ItemIdIsUsed(lp))
		{
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			ReleaseBuffer(buffer);
			break;
		}

		/* OK to access the tuple */
		tp.t_self = ctid;
		tp.t_datamcxt = NULL;
		tp.t_data = (HeapTupleHeader) PageGetItem(dp, lp);
		tp.t_len = ItemIdGetLength(lp);

		/*
		 * After following a t_ctid link, we might arrive at an unrelated
		 * tuple.  Check for XMIN match.
		 */
		if (TransactionIdIsValid(priorXmax) &&
		  !TransactionIdEquals(priorXmax, HeapTupleHeaderGetXmin(tp.t_data)))
		{
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			ReleaseBuffer(buffer);
			break;
		}

		/*
		 * Check time qualification of tuple; if visible, set it as the new
		 * result candidate.
		 */
		HeapTupleSatisfies(&tp, relation, buffer, dp,
						   snapshot, 0, NULL, valid);
		if (valid)
			*tid = ctid;

		/*
		 * If there's a valid t_ctid link, follow it, else we're done.
		 */
		if ((tp.t_data->t_infomask & (HEAP_XMAX_INVALID | HEAP_IS_LOCKED)) ||
			ItemPointerEquals(&tp.t_self, &tp.t_data->t_ctid))
		{
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			ReleaseBuffer(buffer);
			break;
		}

		ctid = tp.t_data->t_ctid;
		priorXmax = HeapTupleHeaderGetXmax(tp.t_data);
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
	}							/* end of loop */
}

/*
 *	heap_insert		- insert tuple into a heap
 *
 * The new tuple is stamped with current transaction ID and the specified
 * command ID.
 *
 * If use_wal is false, the new tuple is not logged in WAL, even for a
 * non-temp relation.  Safe usage of this behavior requires that we arrange
 * that all new tuples go into new pages not containing any tuples from other
 * transactions, that the relation gets fsync'd before commit, and that the
 * transaction emits at least one WAL record to ensure RecordTransactionCommit
 * will decide to WAL-log the commit.
 *
 * use_fsm is passed directly to RelationGetBufferForTuple, which see for
 * more info.
 *
 * The return value is the OID assigned to the tuple (either here or by the
 * caller), or InvalidOid if no OID.  The header fields of *tup are updated
 * to match the stored tuple; in particular tup->t_self receives the actual
 * TID where the tuple was stored.	But note that any toasting of fields
 * within the tuple data is NOT reflected into *tup.
 */
Oid
heap_insert(Relation relation, HeapTuple tup, CommandId cid,
			bool use_wal, bool use_fsm)
{
	TransactionId xid = GetCurrentTransactionId();
	HeapTuple	heaptup;
	Buffer		buffer;

	if (relation->rd_rel->relhasoids)
	{
#ifdef NOT_USED
		/* this is redundant with an Assert in HeapTupleSetOid */
		Assert(tup->t_data->t_infomask & HEAP_HASOID);
#endif

		/*
		 * If the object id of this tuple has already been assigned, trust the
		 * caller.	There are a couple of ways this can happen.  At initial db
		 * creation, the backend program sets oids for tuples. When we define
		 * an index, we set the oid.  Finally, in the future, we may allow
		 * users to set their own object ids in order to support a persistent
		 * object store (objects need to contain pointers to one another).
		 */
		if (!OidIsValid(HeapTupleGetOid(tup)))
			HeapTupleSetOid(tup, GetNewOid(relation));
	}
	else
	{
		/* check there is not space for an OID */
		Assert(!(tup->t_data->t_infomask & HEAP_HASOID));
	}

	tup->t_data->t_infomask &= ~(HEAP_XACT_MASK);
	tup->t_data->t_infomask |= HEAP_XMAX_INVALID;
	HeapTupleHeaderSetXmin(tup->t_data, xid);
	HeapTupleHeaderSetCmin(tup->t_data, cid);
	HeapTupleHeaderSetXmax(tup->t_data, 0);		/* zero out Datum fields */
	HeapTupleHeaderSetCmax(tup->t_data, 0);		/* for cleanliness */
	tup->t_tableOid = RelationGetRelid(relation);

	/*
	 * If the new tuple is too big for storage or contains already toasted
	 * out-of-line attributes from some other relation, invoke the toaster.
	 *
	 * Note: below this point, heaptup is the data we actually intend to store
	 * into the relation; tup is the caller's original untoasted data.
	 */
	if (HeapTupleHasExternal(tup) ||
		(MAXALIGN(tup->t_len) > TOAST_TUPLE_THRESHOLD))
		heaptup = toast_insert_or_update(relation, tup, NULL);
	else
		heaptup = tup;

	/* Find buffer to insert this tuple into */
	buffer = RelationGetBufferForTuple(relation, heaptup->t_len,
									   InvalidBuffer, use_fsm);

	/* NO EREPORT(ERROR) from here till changes are logged */
	START_CRIT_SECTION();

	RelationPutHeapTuple(relation, buffer, heaptup);

	/* XLOG stuff */
	if (relation->rd_istemp)
	{
		/* No XLOG record, but still need to flag that XID exists on disk */
		MyXactMadeTempRelUpdate = true;
	}
	else if (use_wal)
	{
		xl_heap_insert xlrec;
		xl_heap_header xlhdr;
		XLogRecPtr	recptr;
		XLogRecData rdata[3];
		Page		page = BufferGetPage(buffer);
		uint8		info = XLOG_HEAP_INSERT;

		xlrec.target.node = relation->rd_node;
		xlrec.target.tid = heaptup->t_self;
		rdata[0].data = (char *) &xlrec;
		rdata[0].len = SizeOfHeapInsert;
		rdata[0].buffer = InvalidBuffer;
		rdata[0].next = &(rdata[1]);

		xlhdr.t_natts = heaptup->t_data->t_natts;
		xlhdr.t_infomask = heaptup->t_data->t_infomask;
		xlhdr.t_hoff = heaptup->t_data->t_hoff;

		/*
		 * note we mark rdata[1] as belonging to buffer; if XLogInsert decides
		 * to write the whole page to the xlog, we don't need to store
		 * xl_heap_header in the xlog.
		 */
		rdata[1].data = (char *) &xlhdr;
		rdata[1].len = SizeOfHeapHeader;
		rdata[1].buffer = buffer;
		rdata[1].buffer_std = true;
		rdata[1].next = &(rdata[2]);

		/* PG73FORMAT: write bitmap [+ padding] [+ oid] + data */
		rdata[2].data = (char *) heaptup->t_data + offsetof(HeapTupleHeaderData, t_bits);
		rdata[2].len = heaptup->t_len - offsetof(HeapTupleHeaderData, t_bits);
		rdata[2].buffer = buffer;
		rdata[2].buffer_std = true;
		rdata[2].next = NULL;

		/*
		 * If this is the single and first tuple on page, we can reinit the
		 * page instead of restoring the whole thing.  Set flag, and hide
		 * buffer references from XLogInsert.
		 */
		if (ItemPointerGetOffsetNumber(&(heaptup->t_self)) == FirstOffsetNumber &&
			PageGetMaxOffsetNumber(page) == FirstOffsetNumber)
		{
			info |= XLOG_HEAP_INIT_PAGE;
			rdata[1].buffer = rdata[2].buffer = InvalidBuffer;
		}

		recptr = XLogInsert(RM_HEAP_ID, info, rdata);

		PageSetLSN(page, recptr);
		PageSetTLI(page, ThisTimeLineID);
	}

	END_CRIT_SECTION();

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	WriteBuffer(buffer);

	/*
	 * If tuple is cachable, mark it for invalidation from the caches in case
	 * we abort.  Note it is OK to do this after WriteBuffer releases the
	 * buffer, because the heaptup data structure is all in local memory, not
	 * in the shared buffer.
	 */
	CacheInvalidateHeapTuple(relation, heaptup);

	pgstat_count_heap_insert(&relation->pgstat_info);

	/*
	 * If heaptup is a private copy, release it.  Don't forget to copy t_self
	 * back to the caller's image, too.
	 */
	if (heaptup != tup)
	{
		tup->t_self = heaptup->t_self;
		heap_freetuple(heaptup);
	}

	return HeapTupleGetOid(tup);
}

/*
 *	simple_heap_insert - insert a tuple
 *
 * Currently, this routine differs from heap_insert only in supplying
 * a default command ID.  But it should be used rather than using
 * heap_insert directly in most places where we are modifying system catalogs.
 */
Oid
simple_heap_insert(Relation relation, HeapTuple tup)
{
	return heap_insert(relation, tup, GetCurrentCommandId(), true, true);
}

/*
 *	heap_delete - delete a tuple
 *
 * NB: do not call this directly unless you are prepared to deal with
 * concurrent-update conditions.  Use simple_heap_delete instead.
 *
 *	relation - table to be modified (caller must hold suitable lock)
 *	tid - TID of tuple to be deleted
 *	ctid - output parameter, used only for failure case (see below)
 *	update_xmax - output parameter, used only for failure case (see below)
 *	cid - delete command ID (used for visibility test, and stored into
 *		cmax if successful)
 *	crosscheck - if not InvalidSnapshot, also check tuple against this
 *	wait - true if should wait for any conflicting update to commit/abort
 *
 * Normal, successful return value is HeapTupleMayBeUpdated, which
 * actually means we did delete it.  Failure return codes are
 * HeapTupleSelfUpdated, HeapTupleUpdated, or HeapTupleBeingUpdated
 * (the last only possible if wait == false).
 *
 * In the failure cases, the routine returns the tuple's t_ctid and t_xmax.
 * If t_ctid is the same as tid, the tuple was deleted; if different, the
 * tuple was updated, and t_ctid is the location of the replacement tuple.
 * (t_xmax is needed to verify that the replacement tuple matches.)
 */
HTSU_Result
heap_delete(Relation relation, ItemPointer tid,
			ItemPointer ctid, TransactionId *update_xmax,
			CommandId cid, Snapshot crosscheck, bool wait)
{
	HTSU_Result result;
	TransactionId xid = GetCurrentTransactionId();
	ItemId		lp;
	HeapTupleData tp;
	PageHeader	dp;
	Buffer		buffer;
	bool		have_tuple_lock = false;

	Assert(ItemPointerIsValid(tid));

	buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(tid));
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	dp = (PageHeader) BufferGetPage(buffer);
	lp = PageGetItemId(dp, ItemPointerGetOffsetNumber(tid));

	tp.t_datamcxt = NULL;
	tp.t_data = (HeapTupleHeader) PageGetItem(dp, lp);
	tp.t_len = ItemIdGetLength(lp);
	tp.t_self = *tid;

l1:
	result = HeapTupleSatisfiesUpdate(tp.t_data, cid, buffer);

	if (result == HeapTupleInvisible)
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
		elog(ERROR, "attempted to delete invisible tuple");
	}
	else if (result == HeapTupleBeingUpdated && wait)
	{
		TransactionId xwait;
		uint16		infomask;

		/* must copy state data before unlocking buffer */
		xwait = HeapTupleHeaderGetXmax(tp.t_data);
		infomask = tp.t_data->t_infomask;

		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

		/*
		 * Acquire tuple lock to establish our priority for the tuple (see
		 * heap_lock_tuple).  LockTuple will release us when we are
		 * next-in-line for the tuple.
		 *
		 * If we are forced to "start over" below, we keep the tuple lock;
		 * this arranges that we stay at the head of the line while rechecking
		 * tuple state.
		 */
		if (!have_tuple_lock)
		{
			LockTuple(relation, &(tp.t_self), ExclusiveLock);
			have_tuple_lock = true;
		}

		/*
		 * Sleep until concurrent transaction ends.  Note that we don't care
		 * if the locker has an exclusive or shared lock, because we need
		 * exclusive.
		 */

		if (infomask & HEAP_XMAX_IS_MULTI)
		{
			/* wait for multixact */
			MultiXactIdWait((MultiXactId) xwait);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

			/*
			 * If xwait had just locked the tuple then some other xact could
			 * update this tuple before we get to this point.  Check for xmax
			 * change, and start over if so.
			 */
			if (!(tp.t_data->t_infomask & HEAP_XMAX_IS_MULTI) ||
				!TransactionIdEquals(HeapTupleHeaderGetXmax(tp.t_data),
									 xwait))
				goto l1;

			/*
			 * You might think the multixact is necessarily done here, but not
			 * so: it could have surviving members, namely our own xact or
			 * other subxacts of this backend.	It is legal for us to delete
			 * the tuple in either case, however (the latter case is
			 * essentially a situation of upgrading our former shared lock to
			 * exclusive).	We don't bother changing the on-disk hint bits
			 * since we are about to overwrite the xmax altogether.
			 */
		}
		else
		{
			/* wait for regular transaction to end */
			XactLockTableWait(xwait);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

			/*
			 * xwait is done, but if xwait had just locked the tuple then some
			 * other xact could update this tuple before we get to this point.
			 * Check for xmax change, and start over if so.
			 */
			if ((tp.t_data->t_infomask & HEAP_XMAX_IS_MULTI) ||
				!TransactionIdEquals(HeapTupleHeaderGetXmax(tp.t_data),
									 xwait))
				goto l1;

			/* Otherwise we can mark it committed or aborted */
			if (!(tp.t_data->t_infomask & (HEAP_XMAX_COMMITTED |
										   HEAP_XMAX_INVALID)))
			{
				if (TransactionIdDidCommit(xwait))
					tp.t_data->t_infomask |= HEAP_XMAX_COMMITTED;
				else
					tp.t_data->t_infomask |= HEAP_XMAX_INVALID;
				SetBufferCommitInfoNeedsSave(buffer);
			}
		}

		/*
		 * We may overwrite if previous xmax aborted, or if it committed but
		 * only locked the tuple without updating it.
		 */
		if (tp.t_data->t_infomask & (HEAP_XMAX_INVALID |
									 HEAP_IS_LOCKED))
			result = HeapTupleMayBeUpdated;
		else
			result = HeapTupleUpdated;
	}

	if (crosscheck != InvalidSnapshot && result == HeapTupleMayBeUpdated)
	{
		/* Perform additional check for serializable RI updates */
		if (!HeapTupleSatisfiesSnapshot(tp.t_data, crosscheck, buffer))
			result = HeapTupleUpdated;
	}

	if (result != HeapTupleMayBeUpdated)
	{
		Assert(result == HeapTupleSelfUpdated ||
			   result == HeapTupleUpdated ||
			   result == HeapTupleBeingUpdated);
		Assert(!(tp.t_data->t_infomask & HEAP_XMAX_INVALID));
		*ctid = tp.t_data->t_ctid;
		*update_xmax = HeapTupleHeaderGetXmax(tp.t_data);
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
		if (have_tuple_lock)
			UnlockTuple(relation, &(tp.t_self), ExclusiveLock);
		return result;
	}

	START_CRIT_SECTION();

	/* store transaction information of xact deleting the tuple */
	tp.t_data->t_infomask &= ~(HEAP_XMAX_COMMITTED |
							   HEAP_XMAX_INVALID |
							   HEAP_XMAX_IS_MULTI |
							   HEAP_IS_LOCKED |
							   HEAP_MOVED);
	HeapTupleHeaderSetXmax(tp.t_data, xid);
	HeapTupleHeaderSetCmax(tp.t_data, cid);
	/* Make sure there is no forward chain link in t_ctid */
	tp.t_data->t_ctid = tp.t_self;

	/* XLOG stuff */
	if (!relation->rd_istemp)
	{
		xl_heap_delete xlrec;
		XLogRecPtr	recptr;
		XLogRecData rdata[2];

		xlrec.target.node = relation->rd_node;
		xlrec.target.tid = tp.t_self;
		rdata[0].data = (char *) &xlrec;
		rdata[0].len = SizeOfHeapDelete;
		rdata[0].buffer = InvalidBuffer;
		rdata[0].next = &(rdata[1]);

		rdata[1].data = NULL;
		rdata[1].len = 0;
		rdata[1].buffer = buffer;
		rdata[1].buffer_std = true;
		rdata[1].next = NULL;

		recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_DELETE, rdata);

		PageSetLSN(dp, recptr);
		PageSetTLI(dp, ThisTimeLineID);
	}
	else
	{
		/* No XLOG record, but still need to flag that XID exists on disk */
		MyXactMadeTempRelUpdate = true;
	}

	END_CRIT_SECTION();

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

	/*
	 * If the tuple has toasted out-of-line attributes, we need to delete
	 * those items too.  We have to do this before WriteBuffer because we need
	 * to look at the contents of the tuple, but it's OK to release the
	 * context lock on the buffer first.
	 */
	if (HeapTupleHasExternal(&tp))
		toast_delete(relation, &tp);

	/*
	 * Mark tuple for invalidation from system caches at next command
	 * boundary. We have to do this before WriteBuffer because we need to look
	 * at the contents of the tuple, so we need to hold our refcount on the
	 * buffer.
	 */
	CacheInvalidateHeapTuple(relation, &tp);

	WriteBuffer(buffer);

	/*
	 * Release the lmgr tuple lock, if we had it.
	 */
	if (have_tuple_lock)
		UnlockTuple(relation, &(tp.t_self), ExclusiveLock);

	pgstat_count_heap_delete(&relation->pgstat_info);

	return HeapTupleMayBeUpdated;
}

/*
 *	simple_heap_delete - delete a tuple
 *
 * This routine may be used to delete a tuple when concurrent updates of
 * the target tuple are not expected (for example, because we have a lock
 * on the relation associated with the tuple).	Any failure is reported
 * via ereport().
 */
void
simple_heap_delete(Relation relation, ItemPointer tid)
{
	HTSU_Result result;
	ItemPointerData update_ctid;
	TransactionId update_xmax;

	result = heap_delete(relation, tid,
						 &update_ctid, &update_xmax,
						 GetCurrentCommandId(), InvalidSnapshot,
						 true /* wait for commit */ );
	switch (result)
	{
		case HeapTupleSelfUpdated:
			/* Tuple was already updated in current command? */
			elog(ERROR, "tuple already updated by self");
			break;

		case HeapTupleMayBeUpdated:
			/* done successfully */
			break;

		case HeapTupleUpdated:
			elog(ERROR, "tuple concurrently updated");
			break;

		default:
			elog(ERROR, "unrecognized heap_delete status: %u", result);
			break;
	}
}

/*
 *	heap_update - replace a tuple
 *
 * NB: do not call this directly unless you are prepared to deal with
 * concurrent-update conditions.  Use simple_heap_update instead.
 *
 *	relation - table to be modified (caller must hold suitable lock)
 *	otid - TID of old tuple to be replaced
 *	newtup - newly constructed tuple data to store
 *	ctid - output parameter, used only for failure case (see below)
 *	update_xmax - output parameter, used only for failure case (see below)
 *	cid - update command ID (used for visibility test, and stored into
 *		cmax/cmin if successful)
 *	crosscheck - if not InvalidSnapshot, also check old tuple against this
 *	wait - true if should wait for any conflicting update to commit/abort
 *
 * Normal, successful return value is HeapTupleMayBeUpdated, which
 * actually means we *did* update it.  Failure return codes are
 * HeapTupleSelfUpdated, HeapTupleUpdated, or HeapTupleBeingUpdated
 * (the last only possible if wait == false).
 *
 * On success, the header fields of *newtup are updated to match the new
 * stored tuple; in particular, newtup->t_self is set to the TID where the
 * new tuple was inserted.	However, any TOAST changes in the new tuple's
 * data are not reflected into *newtup.
 *
 * In the failure cases, the routine returns the tuple's t_ctid and t_xmax.
 * If t_ctid is the same as otid, the tuple was deleted; if different, the
 * tuple was updated, and t_ctid is the location of the replacement tuple.
 * (t_xmax is needed to verify that the replacement tuple matches.)
 */
HTSU_Result
heap_update(Relation relation, ItemPointer otid, HeapTuple newtup,
			ItemPointer ctid, TransactionId *update_xmax,
			CommandId cid, Snapshot crosscheck, bool wait)
{
	HTSU_Result result;
	TransactionId xid = GetCurrentTransactionId();
	ItemId		lp;
	HeapTupleData oldtup;
	HeapTuple	heaptup;
	PageHeader	dp;
	Buffer		buffer,
				newbuf;
	bool		need_toast,
				already_marked;
	Size		newtupsize,
				pagefree;
	bool		have_tuple_lock = false;

	Assert(ItemPointerIsValid(otid));

	buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(otid));
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	dp = (PageHeader) BufferGetPage(buffer);
	lp = PageGetItemId(dp, ItemPointerGetOffsetNumber(otid));

	oldtup.t_datamcxt = NULL;
	oldtup.t_data = (HeapTupleHeader) PageGetItem(dp, lp);
	oldtup.t_len = ItemIdGetLength(lp);
	oldtup.t_self = *otid;

	/*
	 * Note: beyond this point, use oldtup not otid to refer to old tuple.
	 * otid may very well point at newtup->t_self, which we will overwrite
	 * with the new tuple's location, so there's great risk of confusion if we
	 * use otid anymore.
	 */

l2:
	result = HeapTupleSatisfiesUpdate(oldtup.t_data, cid, buffer);

	if (result == HeapTupleInvisible)
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
		elog(ERROR, "attempted to update invisible tuple");
	}
	else if (result == HeapTupleBeingUpdated && wait)
	{
		TransactionId xwait;
		uint16		infomask;

		/* must copy state data before unlocking buffer */
		xwait = HeapTupleHeaderGetXmax(oldtup.t_data);
		infomask = oldtup.t_data->t_infomask;

		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

		/*
		 * Acquire tuple lock to establish our priority for the tuple (see
		 * heap_lock_tuple).  LockTuple will release us when we are
		 * next-in-line for the tuple.
		 *
		 * If we are forced to "start over" below, we keep the tuple lock;
		 * this arranges that we stay at the head of the line while rechecking
		 * tuple state.
		 */
		if (!have_tuple_lock)
		{
			LockTuple(relation, &(oldtup.t_self), ExclusiveLock);
			have_tuple_lock = true;
		}

		/*
		 * Sleep until concurrent transaction ends.  Note that we don't care
		 * if the locker has an exclusive or shared lock, because we need
		 * exclusive.
		 */

		if (infomask & HEAP_XMAX_IS_MULTI)
		{
			/* wait for multixact */
			MultiXactIdWait((MultiXactId) xwait);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

			/*
			 * If xwait had just locked the tuple then some other xact could
			 * update this tuple before we get to this point.  Check for xmax
			 * change, and start over if so.
			 */
			if (!(oldtup.t_data->t_infomask & HEAP_XMAX_IS_MULTI) ||
				!TransactionIdEquals(HeapTupleHeaderGetXmax(oldtup.t_data),
									 xwait))
				goto l2;

			/*
			 * You might think the multixact is necessarily done here, but not
			 * so: it could have surviving members, namely our own xact or
			 * other subxacts of this backend.	It is legal for us to update
			 * the tuple in either case, however (the latter case is
			 * essentially a situation of upgrading our former shared lock to
			 * exclusive).	We don't bother changing the on-disk hint bits
			 * since we are about to overwrite the xmax altogether.
			 */
		}
		else
		{
			/* wait for regular transaction to end */
			XactLockTableWait(xwait);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

			/*
			 * xwait is done, but if xwait had just locked the tuple then some
			 * other xact could update this tuple before we get to this point.
			 * Check for xmax change, and start over if so.
			 */
			if ((oldtup.t_data->t_infomask & HEAP_XMAX_IS_MULTI) ||
				!TransactionIdEquals(HeapTupleHeaderGetXmax(oldtup.t_data),
									 xwait))
				goto l2;

			/* Otherwise we can mark it committed or aborted */
			if (!(oldtup.t_data->t_infomask & (HEAP_XMAX_COMMITTED |
											   HEAP_XMAX_INVALID)))
			{
				if (TransactionIdDidCommit(xwait))
					oldtup.t_data->t_infomask |= HEAP_XMAX_COMMITTED;
				else
					oldtup.t_data->t_infomask |= HEAP_XMAX_INVALID;
				SetBufferCommitInfoNeedsSave(buffer);
			}
		}

		/*
		 * We may overwrite if previous xmax aborted, or if it committed but
		 * only locked the tuple without updating it.
		 */
		if (oldtup.t_data->t_infomask & (HEAP_XMAX_INVALID |
										 HEAP_IS_LOCKED))
			result = HeapTupleMayBeUpdated;
		else
			result = HeapTupleUpdated;
	}

	if (crosscheck != InvalidSnapshot && result == HeapTupleMayBeUpdated)
	{
		/* Perform additional check for serializable RI updates */
		if (!HeapTupleSatisfiesSnapshot(oldtup.t_data, crosscheck, buffer))
			result = HeapTupleUpdated;
	}

	if (result != HeapTupleMayBeUpdated)
	{
		Assert(result == HeapTupleSelfUpdated ||
			   result == HeapTupleUpdated ||
			   result == HeapTupleBeingUpdated);
		Assert(!(oldtup.t_data->t_infomask & HEAP_XMAX_INVALID));
		*ctid = oldtup.t_data->t_ctid;
		*update_xmax = HeapTupleHeaderGetXmax(oldtup.t_data);
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
		if (have_tuple_lock)
			UnlockTuple(relation, &(oldtup.t_self), ExclusiveLock);
		return result;
	}

	/* Fill in OID and transaction status data for newtup */
	if (relation->rd_rel->relhasoids)
	{
#ifdef NOT_USED
		/* this is redundant with an Assert in HeapTupleSetOid */
		Assert(newtup->t_data->t_infomask & HEAP_HASOID);
#endif
		HeapTupleSetOid(newtup, HeapTupleGetOid(&oldtup));
	}
	else
	{
		/* check there is not space for an OID */
		Assert(!(newtup->t_data->t_infomask & HEAP_HASOID));
	}

	newtup->t_data->t_infomask &= ~(HEAP_XACT_MASK);
	newtup->t_data->t_infomask |= (HEAP_XMAX_INVALID | HEAP_UPDATED);
	HeapTupleHeaderSetXmin(newtup->t_data, xid);
	HeapTupleHeaderSetCmin(newtup->t_data, cid);
	HeapTupleHeaderSetXmax(newtup->t_data, 0);	/* zero out Datum fields */
	HeapTupleHeaderSetCmax(newtup->t_data, 0);	/* for cleanliness */

	/*
	 * If the toaster needs to be activated, OR if the new tuple will not fit
	 * on the same page as the old, then we need to release the context lock
	 * (but not the pin!) on the old tuple's buffer while we are off doing
	 * TOAST and/or table-file-extension work.	We must mark the old tuple to
	 * show that it's already being updated, else other processes may try to
	 * update it themselves.
	 *
	 * We need to invoke the toaster if there are already any out-of-line
	 * toasted values present, or if the new tuple is over-threshold.
	 */
	newtupsize = MAXALIGN(newtup->t_len);

	need_toast = (HeapTupleHasExternal(&oldtup) ||
				  HeapTupleHasExternal(newtup) ||
				  newtupsize > TOAST_TUPLE_THRESHOLD);

	pagefree = PageGetFreeSpace((Page) dp);

	if (need_toast || newtupsize > pagefree)
	{
		oldtup.t_data->t_infomask &= ~(HEAP_XMAX_COMMITTED |
									   HEAP_XMAX_INVALID |
									   HEAP_XMAX_IS_MULTI |
									   HEAP_IS_LOCKED |
									   HEAP_MOVED);
		HeapTupleHeaderSetXmax(oldtup.t_data, xid);
		HeapTupleHeaderSetCmax(oldtup.t_data, cid);
		/* temporarily make it look not-updated */
		oldtup.t_data->t_ctid = oldtup.t_self;
		already_marked = true;
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

		/*
		 * Let the toaster do its thing, if needed.
		 *
		 * Note: below this point, heaptup is the data we actually intend to
		 * store into the relation; newtup is the caller's original untoasted
		 * data.
		 */
		if (need_toast)
		{
			heaptup = toast_insert_or_update(relation, newtup, &oldtup);
			newtupsize = MAXALIGN(heaptup->t_len);
		}
		else
			heaptup = newtup;

		/*
		 * Now, do we need a new page for the tuple, or not?  This is a bit
		 * tricky since someone else could have added tuples to the page while
		 * we weren't looking.  We have to recheck the available space after
		 * reacquiring the buffer lock.  But don't bother to do that if the
		 * former amount of free space is still not enough; it's unlikely
		 * there's more free now than before.
		 *
		 * What's more, if we need to get a new page, we will need to acquire
		 * buffer locks on both old and new pages.	To avoid deadlock against
		 * some other backend trying to get the same two locks in the other
		 * order, we must be consistent about the order we get the locks in.
		 * We use the rule "lock the lower-numbered page of the relation
		 * first".  To implement this, we must do RelationGetBufferForTuple
		 * while not holding the lock on the old page, and we must rely on it
		 * to get the locks on both pages in the correct order.
		 */
		if (newtupsize > pagefree)
		{
			/* Assume there's no chance to put heaptup on same page. */
			newbuf = RelationGetBufferForTuple(relation, heaptup->t_len,
											   buffer, true);
		}
		else
		{
			/* Re-acquire the lock on the old tuple's page. */
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
			/* Re-check using the up-to-date free space */
			pagefree = PageGetFreeSpace((Page) dp);
			if (newtupsize > pagefree)
			{
				/*
				 * Rats, it doesn't fit anymore.  We must now unlock and
				 * relock to avoid deadlock.  Fortunately, this path should
				 * seldom be taken.
				 */
				LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
				newbuf = RelationGetBufferForTuple(relation, heaptup->t_len,
												   buffer, true);
			}
			else
			{
				/* OK, it fits here, so we're done. */
				newbuf = buffer;
			}
		}
	}
	else
	{
		/* No TOAST work needed, and it'll fit on same page */
		already_marked = false;
		newbuf = buffer;
		heaptup = newtup;
	}

	/*
	 * At this point newbuf and buffer are both pinned and locked, and newbuf
	 * has enough space for the new tuple.	If they are the same buffer, only
	 * one pin is held.
	 */

	/* NO EREPORT(ERROR) from here till changes are logged */
	START_CRIT_SECTION();

	RelationPutHeapTuple(relation, newbuf, heaptup);	/* insert new tuple */

	if (!already_marked)
	{
		oldtup.t_data->t_infomask &= ~(HEAP_XMAX_COMMITTED |
									   HEAP_XMAX_INVALID |
									   HEAP_XMAX_IS_MULTI |
									   HEAP_IS_LOCKED |
									   HEAP_MOVED);
		HeapTupleHeaderSetXmax(oldtup.t_data, xid);
		HeapTupleHeaderSetCmax(oldtup.t_data, cid);
	}

	/* record address of new tuple in t_ctid of old one */
	oldtup.t_data->t_ctid = heaptup->t_self;

	/* XLOG stuff */
	if (!relation->rd_istemp)
	{
		XLogRecPtr	recptr = log_heap_update(relation, buffer, oldtup.t_self,
											 newbuf, heaptup, false);

		if (newbuf != buffer)
		{
			PageSetLSN(BufferGetPage(newbuf), recptr);
			PageSetTLI(BufferGetPage(newbuf), ThisTimeLineID);
		}
		PageSetLSN(BufferGetPage(buffer), recptr);
		PageSetTLI(BufferGetPage(buffer), ThisTimeLineID);
	}
	else
	{
		/* No XLOG record, but still need to flag that XID exists on disk */
		MyXactMadeTempRelUpdate = true;
	}

	END_CRIT_SECTION();

	if (newbuf != buffer)
		LockBuffer(newbuf, BUFFER_LOCK_UNLOCK);
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

	/*
	 * Mark old tuple for invalidation from system caches at next command
	 * boundary. We have to do this before WriteBuffer because we need to look
	 * at the contents of the tuple, so we need to hold our refcount.
	 */
	CacheInvalidateHeapTuple(relation, &oldtup);

	if (newbuf != buffer)
		WriteBuffer(newbuf);
	WriteBuffer(buffer);

	/*
	 * If new tuple is cachable, mark it for invalidation from the caches in
	 * case we abort.  Note it is OK to do this after WriteBuffer releases the
	 * buffer, because the heaptup data structure is all in local memory, not
	 * in the shared buffer.
	 */
	CacheInvalidateHeapTuple(relation, heaptup);

	/*
	 * Release the lmgr tuple lock, if we had it.
	 */
	if (have_tuple_lock)
		UnlockTuple(relation, &(oldtup.t_self), ExclusiveLock);

	pgstat_count_heap_update(&relation->pgstat_info);

	/*
	 * If heaptup is a private copy, release it.  Don't forget to copy t_self
	 * back to the caller's image, too.
	 */
	if (heaptup != newtup)
	{
		newtup->t_self = heaptup->t_self;
		heap_freetuple(heaptup);
	}

	return HeapTupleMayBeUpdated;
}

/*
 *	simple_heap_update - replace a tuple
 *
 * This routine may be used to update a tuple when concurrent updates of
 * the target tuple are not expected (for example, because we have a lock
 * on the relation associated with the tuple).	Any failure is reported
 * via ereport().
 */
void
simple_heap_update(Relation relation, ItemPointer otid, HeapTuple tup)
{
	HTSU_Result result;
	ItemPointerData update_ctid;
	TransactionId update_xmax;

	result = heap_update(relation, otid, tup,
						 &update_ctid, &update_xmax,
						 GetCurrentCommandId(), InvalidSnapshot,
						 true /* wait for commit */ );
	switch (result)
	{
		case HeapTupleSelfUpdated:
			/* Tuple was already updated in current command? */
			elog(ERROR, "tuple already updated by self");
			break;

		case HeapTupleMayBeUpdated:
			/* done successfully */
			break;

		case HeapTupleUpdated:
			elog(ERROR, "tuple concurrently updated");
			break;

		default:
			elog(ERROR, "unrecognized heap_update status: %u", result);
			break;
	}
}

/*
 *	heap_lock_tuple - lock a tuple in shared or exclusive mode
 *
 * Note that this acquires a buffer pin, which the caller must release.
 *
 * Input parameters:
 *	relation: relation containing tuple (caller must hold suitable lock)
 *	tuple->t_self: TID of tuple to lock (rest of struct need not be valid)
 *	cid: current command ID (used for visibility test, and stored into
 *		tuple's cmax if lock is successful)
 *	mode: indicates if shared or exclusive tuple lock is desired
 *	nowait: if true, ereport rather than blocking if lock not available
 *
 * Output parameters:
 *	*tuple: all fields filled in
 *	*buffer: set to buffer holding tuple (pinned but not locked at exit)
 *	*ctid: set to tuple's t_ctid, but only in failure cases
 *	*update_xmax: set to tuple's xmax, but only in failure cases
 *
 * Function result may be:
 *	HeapTupleMayBeUpdated: lock was successfully acquired
 *	HeapTupleSelfUpdated: lock failed because tuple updated by self
 *	HeapTupleUpdated: lock failed because tuple updated by other xact
 *
 * In the failure cases, the routine returns the tuple's t_ctid and t_xmax.
 * If t_ctid is the same as t_self, the tuple was deleted; if different, the
 * tuple was updated, and t_ctid is the location of the replacement tuple.
 * (t_xmax is needed to verify that the replacement tuple matches.)
 *
 *
 * NOTES: because the shared-memory lock table is of finite size, but users
 * could reasonably want to lock large numbers of tuples, we do not rely on
 * the standard lock manager to store tuple-level locks over the long term.
 * Instead, a tuple is marked as locked by setting the current transaction's
 * XID as its XMAX, and setting additional infomask bits to distinguish this
 * usage from the more normal case of having deleted the tuple.  When
 * multiple transactions concurrently share-lock a tuple, the first locker's
 * XID is replaced in XMAX with a MultiTransactionId representing the set of
 * XIDs currently holding share-locks.
 *
 * When it is necessary to wait for a tuple-level lock to be released, the
 * basic delay is provided by XactLockTableWait or MultiXactIdWait on the
 * contents of the tuple's XMAX.  However, that mechanism will release all
 * waiters concurrently, so there would be a race condition as to which
 * waiter gets the tuple, potentially leading to indefinite starvation of
 * some waiters.  The possibility of share-locking makes the problem much
 * worse --- a steady stream of share-lockers can easily block an exclusive
 * locker forever.	To provide more reliable semantics about who gets a
 * tuple-level lock first, we use the standard lock manager.  The protocol
 * for waiting for a tuple-level lock is really
 *		LockTuple()
 *		XactLockTableWait()
 *		mark tuple as locked by me
 *		UnlockTuple()
 * When there are multiple waiters, arbitration of who is to get the lock next
 * is provided by LockTuple().	However, at most one tuple-level lock will
 * be held or awaited per backend at any time, so we don't risk overflow
 * of the lock table.  Note that incoming share-lockers are required to
 * do LockTuple as well, if there is any conflict, to ensure that they don't
 * starve out waiting exclusive-lockers.  However, if there is not any active
 * conflict for a tuple, we don't incur any extra overhead.
 */
HTSU_Result
heap_lock_tuple(Relation relation, HeapTuple tuple, Buffer *buffer,
				ItemPointer ctid, TransactionId *update_xmax,
				CommandId cid, LockTupleMode mode, bool nowait)
{
	HTSU_Result result;
	ItemPointer tid = &(tuple->t_self);
	ItemId		lp;
	PageHeader	dp;
	TransactionId xid;
	uint16		new_infomask;
	LOCKMODE	tuple_lock_type;
	bool		have_tuple_lock = false;

	tuple_lock_type = (mode == LockTupleShared) ? ShareLock : ExclusiveLock;

	*buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(tid));
	LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);

	dp = (PageHeader) BufferGetPage(*buffer);
	lp = PageGetItemId(dp, ItemPointerGetOffsetNumber(tid));
	Assert(ItemIdIsUsed(lp));

	tuple->t_datamcxt = NULL;
	tuple->t_data = (HeapTupleHeader) PageGetItem((Page) dp, lp);
	tuple->t_len = ItemIdGetLength(lp);
	tuple->t_tableOid = RelationGetRelid(relation);

l3:
	result = HeapTupleSatisfiesUpdate(tuple->t_data, cid, *buffer);

	if (result == HeapTupleInvisible)
	{
		LockBuffer(*buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(*buffer);
		elog(ERROR, "attempted to lock invisible tuple");
	}
	else if (result == HeapTupleBeingUpdated)
	{
		TransactionId xwait;
		uint16		infomask;

		/* must copy state data before unlocking buffer */
		xwait = HeapTupleHeaderGetXmax(tuple->t_data);
		infomask = tuple->t_data->t_infomask;

		LockBuffer(*buffer, BUFFER_LOCK_UNLOCK);

		/*
		 * Acquire tuple lock to establish our priority for the tuple.
		 * LockTuple will release us when we are next-in-line for the tuple.
		 * We must do this even if we are share-locking.
		 *
		 * If we are forced to "start over" below, we keep the tuple lock;
		 * this arranges that we stay at the head of the line while rechecking
		 * tuple state.
		 */
		if (!have_tuple_lock)
		{
			if (nowait)
			{
				if (!ConditionalLockTuple(relation, tid, tuple_lock_type))
					ereport(ERROR,
							(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
					errmsg("could not obtain lock on row in relation \"%s\"",
						   RelationGetRelationName(relation))));
			}
			else
				LockTuple(relation, tid, tuple_lock_type);
			have_tuple_lock = true;
		}

		if (mode == LockTupleShared && (infomask & HEAP_XMAX_SHARED_LOCK))
		{
			/*
			 * Acquiring sharelock when there's at least one sharelocker
			 * already.  We need not wait for him/them to complete.
			 */
			LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);

			/*
			 * Make sure it's still a shared lock, else start over.  (It's OK
			 * if the ownership of the shared lock has changed, though.)
			 */
			if (!(tuple->t_data->t_infomask & HEAP_XMAX_SHARED_LOCK))
				goto l3;
		}
		else if (infomask & HEAP_XMAX_IS_MULTI)
		{
			/* wait for multixact to end */
			if (nowait)
			{
				if (!ConditionalMultiXactIdWait((MultiXactId) xwait))
					ereport(ERROR,
							(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
					errmsg("could not obtain lock on row in relation \"%s\"",
						   RelationGetRelationName(relation))));
			}
			else
				MultiXactIdWait((MultiXactId) xwait);

			LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);

			/*
			 * If xwait had just locked the tuple then some other xact could
			 * update this tuple before we get to this point. Check for xmax
			 * change, and start over if so.
			 */
			if (!(tuple->t_data->t_infomask & HEAP_XMAX_IS_MULTI) ||
				!TransactionIdEquals(HeapTupleHeaderGetXmax(tuple->t_data),
									 xwait))
				goto l3;

			/*
			 * You might think the multixact is necessarily done here, but not
			 * so: it could have surviving members, namely our own xact or
			 * other subxacts of this backend.	It is legal for us to lock the
			 * tuple in either case, however.  We don't bother changing the
			 * on-disk hint bits since we are about to overwrite the xmax
			 * altogether.
			 */
		}
		else
		{
			/* wait for regular transaction to end */
			if (nowait)
			{
				if (!ConditionalXactLockTableWait(xwait))
					ereport(ERROR,
							(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
					errmsg("could not obtain lock on row in relation \"%s\"",
						   RelationGetRelationName(relation))));
			}
			else
				XactLockTableWait(xwait);

			LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);

			/*
			 * xwait is done, but if xwait had just locked the tuple then some
			 * other xact could update this tuple before we get to this point.
			 * Check for xmax change, and start over if so.
			 */
			if ((tuple->t_data->t_infomask & HEAP_XMAX_IS_MULTI) ||
				!TransactionIdEquals(HeapTupleHeaderGetXmax(tuple->t_data),
									 xwait))
				goto l3;

			/* Otherwise we can mark it committed or aborted */
			if (!(tuple->t_data->t_infomask & (HEAP_XMAX_COMMITTED |
											   HEAP_XMAX_INVALID)))
			{
				if (TransactionIdDidCommit(xwait))
					tuple->t_data->t_infomask |= HEAP_XMAX_COMMITTED;
				else
					tuple->t_data->t_infomask |= HEAP_XMAX_INVALID;
				SetBufferCommitInfoNeedsSave(*buffer);
			}
		}

		/*
		 * We may lock if previous xmax aborted, or if it committed but only
		 * locked the tuple without updating it.  The case where we didn't
		 * wait because we are joining an existing shared lock is correctly
		 * handled, too.
		 */
		if (tuple->t_data->t_infomask & (HEAP_XMAX_INVALID |
										 HEAP_IS_LOCKED))
			result = HeapTupleMayBeUpdated;
		else
			result = HeapTupleUpdated;
	}

	if (result != HeapTupleMayBeUpdated)
	{
		Assert(result == HeapTupleSelfUpdated || result == HeapTupleUpdated);
		Assert(!(tuple->t_data->t_infomask & HEAP_XMAX_INVALID));
		*ctid = tuple->t_data->t_ctid;
		*update_xmax = HeapTupleHeaderGetXmax(tuple->t_data);
		LockBuffer(*buffer, BUFFER_LOCK_UNLOCK);
		if (have_tuple_lock)
			UnlockTuple(relation, tid, tuple_lock_type);
		return result;
	}

	/*
	 * Compute the new xmax and infomask to store into the tuple.  Note we do
	 * not modify the tuple just yet, because that would leave it in the wrong
	 * state if multixact.c elogs.
	 */
	xid = GetCurrentTransactionId();

	new_infomask = tuple->t_data->t_infomask;

	new_infomask &= ~(HEAP_XMAX_COMMITTED |
					  HEAP_XMAX_INVALID |
					  HEAP_XMAX_IS_MULTI |
					  HEAP_IS_LOCKED |
					  HEAP_MOVED);

	if (mode == LockTupleShared)
	{
		TransactionId xmax = HeapTupleHeaderGetXmax(tuple->t_data);
		uint16		old_infomask = tuple->t_data->t_infomask;

		/*
		 * If this is the first acquisition of a shared lock in the current
		 * transaction, set my per-backend OldestMemberMXactId setting. We can
		 * be certain that the transaction will never become a member of any
		 * older MultiXactIds than that.  (We have to do this even if we end
		 * up just using our own TransactionId below, since some other backend
		 * could incorporate our XID into a MultiXact immediately afterwards.)
		 */
		MultiXactIdSetOldestMember();

		new_infomask |= HEAP_XMAX_SHARED_LOCK;

		/*
		 * Check to see if we need a MultiXactId because there are multiple
		 * lockers.
		 *
		 * HeapTupleSatisfiesUpdate will have set the HEAP_XMAX_INVALID bit if
		 * the xmax was a MultiXactId but it was not running anymore. There is
		 * a race condition, which is that the MultiXactId may have finished
		 * since then, but that uncommon case is handled within
		 * MultiXactIdExpand.
		 *
		 * There is a similar race condition possible when the old xmax was a
		 * regular TransactionId.  We test TransactionIdIsInProgress again
		 * just to narrow the window, but it's still possible to end up
		 * creating an unnecessary MultiXactId.  Fortunately this is harmless.
		 */
		if (!(old_infomask & (HEAP_XMAX_INVALID | HEAP_XMAX_COMMITTED)))
		{
			if (old_infomask & HEAP_XMAX_IS_MULTI)
			{
				/*
				 * If the XMAX is already a MultiXactId, then we need to
				 * expand it to include our own TransactionId.
				 */
				xid = MultiXactIdExpand((MultiXactId) xmax, xid);
				new_infomask |= HEAP_XMAX_IS_MULTI;
			}
			else if (TransactionIdIsInProgress(xmax))
			{
				if (TransactionIdEquals(xmax, xid))
				{
					/*
					 * If the old locker is ourselves, we'll just mark the
					 * tuple again with our own TransactionId.	However we
					 * have to consider the possibility that we had exclusive
					 * rather than shared lock before --- if so, be careful to
					 * preserve the exclusivity of the lock.
					 */
					if (!(old_infomask & HEAP_XMAX_SHARED_LOCK))
					{
						new_infomask &= ~HEAP_XMAX_SHARED_LOCK;
						new_infomask |= HEAP_XMAX_EXCL_LOCK;
						mode = LockTupleExclusive;
					}
				}
				else
				{
					/*
					 * If the Xmax is a valid TransactionId, then we need to
					 * create a new MultiXactId that includes both the old
					 * locker and our own TransactionId.
					 */
					xid = MultiXactIdCreate(xmax, xid);
					new_infomask |= HEAP_XMAX_IS_MULTI;
				}
			}
			else
			{
				/*
				 * Can get here iff HeapTupleSatisfiesUpdate saw the old xmax
				 * as running, but it finished before
				 * TransactionIdIsInProgress() got to run.	Treat it like
				 * there's no locker in the tuple.
				 */
			}
		}
		else
		{
			/*
			 * There was no previous locker, so just insert our own
			 * TransactionId.
			 */
		}
	}
	else
	{
		/* We want an exclusive lock on the tuple */
		new_infomask |= HEAP_XMAX_EXCL_LOCK;
	}

	START_CRIT_SECTION();

	/*
	 * Store transaction information of xact locking the tuple.
	 *
	 * Note: our CID is meaningless if storing a MultiXactId, but no harm in
	 * storing it anyway.
	 */
	tuple->t_data->t_infomask = new_infomask;
	HeapTupleHeaderSetXmax(tuple->t_data, xid);
	HeapTupleHeaderSetCmax(tuple->t_data, cid);
	/* Make sure there is no forward chain link in t_ctid */
	tuple->t_data->t_ctid = *tid;

	/*
	 * XLOG stuff.	You might think that we don't need an XLOG record because
	 * there is no state change worth restoring after a crash.	You would be
	 * wrong however: we have just written either a TransactionId or a
	 * MultiXactId that may never have been seen on disk before, and we need
	 * to make sure that there are XLOG entries covering those ID numbers.
	 * Else the same IDs might be re-used after a crash, which would be
	 * disastrous if this page made it to disk before the crash.  Essentially
	 * we have to enforce the WAL log-before-data rule even in this case.
	 * (Also, in a PITR log-shipping or 2PC environment, we have to have XLOG
	 * entries for everything anyway.)
	 */
	if (!relation->rd_istemp)
	{
		xl_heap_lock xlrec;
		XLogRecPtr	recptr;
		XLogRecData rdata[2];

		xlrec.target.node = relation->rd_node;
		xlrec.target.tid = tuple->t_self;
		xlrec.locking_xid = xid;
		xlrec.xid_is_mxact = ((new_infomask & HEAP_XMAX_IS_MULTI) != 0);
		xlrec.shared_lock = (mode == LockTupleShared);
		rdata[0].data = (char *) &xlrec;
		rdata[0].len = SizeOfHeapLock;
		rdata[0].buffer = InvalidBuffer;
		rdata[0].next = &(rdata[1]);

		rdata[1].data = NULL;
		rdata[1].len = 0;
		rdata[1].buffer = *buffer;
		rdata[1].buffer_std = true;
		rdata[1].next = NULL;

		recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_LOCK, rdata);

		PageSetLSN(dp, recptr);
		PageSetTLI(dp, ThisTimeLineID);
	}
	else
	{
		/* No XLOG record, but still need to flag that XID exists on disk */
		MyXactMadeTempRelUpdate = true;
	}

	END_CRIT_SECTION();

	LockBuffer(*buffer, BUFFER_LOCK_UNLOCK);

	WriteNoReleaseBuffer(*buffer);

	/*
	 * Now that we have successfully marked the tuple as locked, we can
	 * release the lmgr tuple lock, if we had it.
	 */
	if (have_tuple_lock)
		UnlockTuple(relation, tid, tuple_lock_type);

	return HeapTupleMayBeUpdated;
}

/* ----------------
 *		heap_markpos	- mark scan position
 * ----------------
 */
void
heap_markpos(HeapScanDesc scan)
{
	/* Note: no locking manipulations needed */

	if (scan->rs_ctup.t_data != NULL)
		scan->rs_mctid = scan->rs_ctup.t_self;
	else
		ItemPointerSetInvalid(&scan->rs_mctid);
}

/* ----------------
 *		heap_restrpos	- restore position to marked location
 * ----------------
 */
void
heap_restrpos(HeapScanDesc scan)
{
	/* XXX no amrestrpos checking that ammarkpos called */

	/* Note: no locking manipulations needed */

	/*
	 * unpin scan buffers
	 */
	if (BufferIsValid(scan->rs_cbuf))
		ReleaseBuffer(scan->rs_cbuf);
	scan->rs_cbuf = InvalidBuffer;

	if (!ItemPointerIsValid(&scan->rs_mctid))
	{
		scan->rs_ctup.t_datamcxt = NULL;
		scan->rs_ctup.t_data = NULL;
	}
	else
	{
		scan->rs_ctup.t_self = scan->rs_mctid;
		scan->rs_ctup.t_datamcxt = NULL;
		scan->rs_ctup.t_data = (HeapTupleHeader) 0x1;	/* for heapgettup */
		heapgettup(scan->rs_rd,
				   0,
				   &(scan->rs_ctup),
				   &(scan->rs_cbuf),
				   scan->rs_snapshot,
				   0,
				   NULL,
				   scan->rs_nblocks);
	}
}

XLogRecPtr
log_heap_clean(Relation reln, Buffer buffer, OffsetNumber *unused, int uncnt)
{
	xl_heap_clean xlrec;
	XLogRecPtr	recptr;
	XLogRecData rdata[2];

	/* Caller should not call me on a temp relation */
	Assert(!reln->rd_istemp);

	xlrec.node = reln->rd_node;
	xlrec.block = BufferGetBlockNumber(buffer);

	rdata[0].data = (char *) &xlrec;
	rdata[0].len = SizeOfHeapClean;
	rdata[0].buffer = InvalidBuffer;
	rdata[0].next = &(rdata[1]);

	/*
	 * The unused-offsets array is not actually in the buffer, but pretend
	 * that it is.	When XLogInsert stores the whole buffer, the offsets array
	 * need not be stored too.
	 */
	if (uncnt > 0)
	{
		rdata[1].data = (char *) unused;
		rdata[1].len = uncnt * sizeof(OffsetNumber);
	}
	else
	{
		rdata[1].data = NULL;
		rdata[1].len = 0;
	}
	rdata[1].buffer = buffer;
	rdata[1].buffer_std = true;
	rdata[1].next = NULL;

	recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_CLEAN, rdata);

	return (recptr);
}

static XLogRecPtr
log_heap_update(Relation reln, Buffer oldbuf, ItemPointerData from,
				Buffer newbuf, HeapTuple newtup, bool move)
{
	/*
	 * Note: xlhdr is declared to have adequate size and correct alignment for
	 * an xl_heap_header.  However the two tids, if present at all, will be
	 * packed in with no wasted space after the xl_heap_header; they aren't
	 * necessarily aligned as implied by this struct declaration.
	 */
	struct
	{
		xl_heap_header hdr;
		TransactionId tid1;
		TransactionId tid2;
	}			xlhdr;
	int			hsize = SizeOfHeapHeader;
	xl_heap_update xlrec;
	XLogRecPtr	recptr;
	XLogRecData rdata[4];
	Page		page = BufferGetPage(newbuf);
	uint8		info = (move) ? XLOG_HEAP_MOVE : XLOG_HEAP_UPDATE;

	/* Caller should not call me on a temp relation */
	Assert(!reln->rd_istemp);

	xlrec.target.node = reln->rd_node;
	xlrec.target.tid = from;
	xlrec.newtid = newtup->t_self;
	rdata[0].data = (char *) &xlrec;
	rdata[0].len = SizeOfHeapUpdate;
	rdata[0].buffer = InvalidBuffer;
	rdata[0].next = &(rdata[1]);

	rdata[1].data = NULL;
	rdata[1].len = 0;
	rdata[1].buffer = oldbuf;
	rdata[1].buffer_std = true;
	rdata[1].next = &(rdata[2]);

	xlhdr.hdr.t_natts = newtup->t_data->t_natts;
	xlhdr.hdr.t_infomask = newtup->t_data->t_infomask;
	xlhdr.hdr.t_hoff = newtup->t_data->t_hoff;
	if (move)					/* remember xmax & xmin */
	{
		TransactionId xid[2];	/* xmax, xmin */

		if (newtup->t_data->t_infomask & (HEAP_XMAX_INVALID | HEAP_IS_LOCKED))
			xid[0] = InvalidTransactionId;
		else
			xid[0] = HeapTupleHeaderGetXmax(newtup->t_data);
		xid[1] = HeapTupleHeaderGetXmin(newtup->t_data);
		memcpy((char *) &xlhdr + hsize,
			   (char *) xid,
			   2 * sizeof(TransactionId));
		hsize += 2 * sizeof(TransactionId);
	}

	/*
	 * As with insert records, we need not store the rdata[2] segment if we
	 * decide to store the whole buffer instead.
	 */
	rdata[2].data = (char *) &xlhdr;
	rdata[2].len = hsize;
	rdata[2].buffer = newbuf;
	rdata[2].buffer_std = true;
	rdata[2].next = &(rdata[3]);

	/* PG73FORMAT: write bitmap [+ padding] [+ oid] + data */
	rdata[3].data = (char *) newtup->t_data + offsetof(HeapTupleHeaderData, t_bits);
	rdata[3].len = newtup->t_len - offsetof(HeapTupleHeaderData, t_bits);
	rdata[3].buffer = newbuf;
	rdata[3].buffer_std = true;
	rdata[3].next = NULL;

	/* If new tuple is the single and first tuple on page... */
	if (ItemPointerGetOffsetNumber(&(newtup->t_self)) == FirstOffsetNumber &&
		PageGetMaxOffsetNumber(page) == FirstOffsetNumber)
	{
		info |= XLOG_HEAP_INIT_PAGE;
		rdata[2].buffer = rdata[3].buffer = InvalidBuffer;
	}

	recptr = XLogInsert(RM_HEAP_ID, info, rdata);

	return (recptr);
}

XLogRecPtr
log_heap_move(Relation reln, Buffer oldbuf, ItemPointerData from,
			  Buffer newbuf, HeapTuple newtup)
{
	return (log_heap_update(reln, oldbuf, from, newbuf, newtup, true));
}

static void
heap_xlog_clean(XLogRecPtr lsn, XLogRecord *record)
{
	xl_heap_clean *xlrec = (xl_heap_clean *) XLogRecGetData(record);
	Relation	reln;
	Buffer		buffer;
	Page		page;

	if (record->xl_info & XLR_BKP_BLOCK_1)
		return;

	reln = XLogOpenRelation(xlrec->node);
	if (!RelationIsValid(reln))
		return;

	buffer = XLogReadBuffer(false, reln, xlrec->block);
	if (!BufferIsValid(buffer))
		elog(PANIC, "heap_clean_redo: no block");

	page = (Page) BufferGetPage(buffer);
	if (PageIsNew((PageHeader) page))
		elog(PANIC, "heap_clean_redo: uninitialized page");

	if (XLByteLE(lsn, PageGetLSN(page)))
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
		return;
	}

	if (record->xl_len > SizeOfHeapClean)
	{
		OffsetNumber *unused;
		OffsetNumber *unend;
		ItemId		lp;

		unused = (OffsetNumber *) ((char *) xlrec + SizeOfHeapClean);
		unend = (OffsetNumber *) ((char *) xlrec + record->xl_len);

		while (unused < unend)
		{
			lp = PageGetItemId(page, *unused + 1);
			lp->lp_flags &= ~LP_USED;
			unused++;
		}
	}

	PageRepairFragmentation(page, NULL);

	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	WriteBuffer(buffer);
}

static void
heap_xlog_newpage(XLogRecPtr lsn, XLogRecord *record)
{
	xl_heap_newpage *xlrec = (xl_heap_newpage *) XLogRecGetData(record);
	Relation	reln;
	Buffer		buffer;
	Page		page;

	/*
	 * Note: the NEWPAGE log record is used for both heaps and indexes, so do
	 * not do anything that assumes we are touching a heap.
	 */

	if (record->xl_info & XLR_BKP_BLOCK_1)
		return;

	reln = XLogOpenRelation(xlrec->node);
	if (!RelationIsValid(reln))
		return;
	buffer = XLogReadBuffer(true, reln, xlrec->blkno);
	if (!BufferIsValid(buffer))
		elog(PANIC, "heap_newpage_redo: no block");
	page = (Page) BufferGetPage(buffer);

	Assert(record->xl_len == SizeOfHeapNewpage + BLCKSZ);
	memcpy(page, (char *) xlrec + SizeOfHeapNewpage, BLCKSZ);

	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	WriteBuffer(buffer);
}

static void
heap_xlog_delete(XLogRecPtr lsn, XLogRecord *record)
{
	xl_heap_delete *xlrec = (xl_heap_delete *) XLogRecGetData(record);
	Relation	reln;
	Buffer		buffer;
	Page		page;
	OffsetNumber offnum;
	ItemId		lp = NULL;
	HeapTupleHeader htup;

	if (record->xl_info & XLR_BKP_BLOCK_1)
		return;

	reln = XLogOpenRelation(xlrec->target.node);

	if (!RelationIsValid(reln))
		return;

	buffer = XLogReadBuffer(false, reln,
							ItemPointerGetBlockNumber(&(xlrec->target.tid)));
	if (!BufferIsValid(buffer))
		elog(PANIC, "heap_delete_redo: no block");

	page = (Page) BufferGetPage(buffer);
	if (PageIsNew((PageHeader) page))
		elog(PANIC, "heap_delete_redo: uninitialized page");

	if (XLByteLE(lsn, PageGetLSN(page)))		/* changes are applied */
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
		return;
	}

	offnum = ItemPointerGetOffsetNumber(&(xlrec->target.tid));
	if (PageGetMaxOffsetNumber(page) >= offnum)
		lp = PageGetItemId(page, offnum);

	if (PageGetMaxOffsetNumber(page) < offnum || !ItemIdIsUsed(lp))
		elog(PANIC, "heap_delete_redo: invalid lp");

	htup = (HeapTupleHeader) PageGetItem(page, lp);

	htup->t_infomask &= ~(HEAP_XMAX_COMMITTED |
						  HEAP_XMAX_INVALID |
						  HEAP_XMAX_IS_MULTI |
						  HEAP_IS_LOCKED |
						  HEAP_MOVED);
	HeapTupleHeaderSetXmax(htup, record->xl_xid);
	HeapTupleHeaderSetCmax(htup, FirstCommandId);
	/* Make sure there is no forward chain link in t_ctid */
	htup->t_ctid = xlrec->target.tid;
	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	WriteBuffer(buffer);
}

static void
heap_xlog_insert(XLogRecPtr lsn, XLogRecord *record)
{
	xl_heap_insert *xlrec = (xl_heap_insert *) XLogRecGetData(record);
	Relation	reln;
	Buffer		buffer;
	Page		page;
	OffsetNumber offnum;
	struct
	{
		HeapTupleHeaderData hdr;
		char		data[MaxTupleSize];
	}			tbuf;
	HeapTupleHeader htup;
	xl_heap_header xlhdr;
	uint32		newlen;

	if (record->xl_info & XLR_BKP_BLOCK_1)
		return;

	reln = XLogOpenRelation(xlrec->target.node);

	if (!RelationIsValid(reln))
		return;

	buffer = XLogReadBuffer(true, reln,
							ItemPointerGetBlockNumber(&(xlrec->target.tid)));
	if (!BufferIsValid(buffer))
		return;

	page = (Page) BufferGetPage(buffer);
	if (PageIsNew((PageHeader) page) &&
		!(record->xl_info & XLOG_HEAP_INIT_PAGE))
		elog(PANIC, "heap_insert_redo: uninitialized page");

	if (record->xl_info & XLOG_HEAP_INIT_PAGE)
		PageInit(page, BufferGetPageSize(buffer), 0);

	if (XLByteLE(lsn, PageGetLSN(page)))		/* changes are applied */
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
		return;
	}

	offnum = ItemPointerGetOffsetNumber(&(xlrec->target.tid));
	if (PageGetMaxOffsetNumber(page) + 1 < offnum)
		elog(PANIC, "heap_insert_redo: invalid max offset number");

	newlen = record->xl_len - SizeOfHeapInsert - SizeOfHeapHeader;
	Assert(newlen <= MaxTupleSize);
	memcpy((char *) &xlhdr,
		   (char *) xlrec + SizeOfHeapInsert,
		   SizeOfHeapHeader);
	htup = &tbuf.hdr;
	MemSet((char *) htup, 0, sizeof(HeapTupleHeaderData));
	/* PG73FORMAT: get bitmap [+ padding] [+ oid] + data */
	memcpy((char *) htup + offsetof(HeapTupleHeaderData, t_bits),
		   (char *) xlrec + SizeOfHeapInsert + SizeOfHeapHeader,
		   newlen);
	newlen += offsetof(HeapTupleHeaderData, t_bits);
	htup->t_natts = xlhdr.t_natts;
	htup->t_infomask = xlhdr.t_infomask;
	htup->t_hoff = xlhdr.t_hoff;
	HeapTupleHeaderSetXmin(htup, record->xl_xid);
	HeapTupleHeaderSetCmin(htup, FirstCommandId);
	htup->t_ctid = xlrec->target.tid;

	offnum = PageAddItem(page, (Item) htup, newlen, offnum,
						 LP_USED | OverwritePageMode);
	if (offnum == InvalidOffsetNumber)
		elog(PANIC, "heap_insert_redo: failed to add tuple");
	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	WriteBuffer(buffer);
}

/*
 * Handles UPDATE & MOVE
 */
static void
heap_xlog_update(XLogRecPtr lsn, XLogRecord *record, bool move)
{
	xl_heap_update *xlrec = (xl_heap_update *) XLogRecGetData(record);
	Relation	reln = XLogOpenRelation(xlrec->target.node);
	Buffer		buffer;
	bool		samepage =
	(ItemPointerGetBlockNumber(&(xlrec->newtid)) ==
	 ItemPointerGetBlockNumber(&(xlrec->target.tid)));
	Page		page;
	OffsetNumber offnum;
	ItemId		lp = NULL;
	HeapTupleHeader htup;
	struct
	{
		HeapTupleHeaderData hdr;
		char		data[MaxTupleSize];
	}			tbuf;
	xl_heap_header xlhdr;
	int			hsize;
	uint32		newlen;

	if (!RelationIsValid(reln))
		return;

	if (record->xl_info & XLR_BKP_BLOCK_1)
		goto newt;

	/* Deal with old tuple version */

	buffer = XLogReadBuffer(false, reln,
							ItemPointerGetBlockNumber(&(xlrec->target.tid)));
	if (!BufferIsValid(buffer))
		elog(PANIC, "heap_update_redo: no block");

	page = (Page) BufferGetPage(buffer);
	if (PageIsNew((PageHeader) page))
		elog(PANIC, "heap_update_redo: uninitialized old page");

	if (XLByteLE(lsn, PageGetLSN(page)))		/* changes are applied */
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
		if (samepage)
			return;
		goto newt;
	}

	offnum = ItemPointerGetOffsetNumber(&(xlrec->target.tid));
	if (PageGetMaxOffsetNumber(page) >= offnum)
		lp = PageGetItemId(page, offnum);

	if (PageGetMaxOffsetNumber(page) < offnum || !ItemIdIsUsed(lp))
		elog(PANIC, "heap_update_redo: invalid lp");

	htup = (HeapTupleHeader) PageGetItem(page, lp);

	if (move)
	{
		htup->t_infomask &= ~(HEAP_XMIN_COMMITTED |
							  HEAP_XMIN_INVALID |
							  HEAP_MOVED_IN);
		htup->t_infomask |= HEAP_MOVED_OFF;
		HeapTupleHeaderSetXvac(htup, record->xl_xid);
		/* Make sure there is no forward chain link in t_ctid */
		htup->t_ctid = xlrec->target.tid;
	}
	else
	{
		htup->t_infomask &= ~(HEAP_XMAX_COMMITTED |
							  HEAP_XMAX_INVALID |
							  HEAP_XMAX_IS_MULTI |
							  HEAP_IS_LOCKED |
							  HEAP_MOVED);
		HeapTupleHeaderSetXmax(htup, record->xl_xid);
		HeapTupleHeaderSetCmax(htup, FirstCommandId);
		/* Set forward chain link in t_ctid */
		htup->t_ctid = xlrec->newtid;
	}
	if (samepage)
		goto newsame;
	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	WriteBuffer(buffer);

	/* Deal with new tuple */

newt:;

	if ((record->xl_info & XLR_BKP_BLOCK_2) ||
		((record->xl_info & XLR_BKP_BLOCK_1) && samepage))
		return;

	buffer = XLogReadBuffer(true, reln,
							ItemPointerGetBlockNumber(&(xlrec->newtid)));
	if (!BufferIsValid(buffer))
		return;

	page = (Page) BufferGetPage(buffer);

newsame:;
	if (PageIsNew((PageHeader) page) &&
		!(record->xl_info & XLOG_HEAP_INIT_PAGE))
		elog(PANIC, "heap_update_redo: uninitialized page");

	if (record->xl_info & XLOG_HEAP_INIT_PAGE)
		PageInit(page, BufferGetPageSize(buffer), 0);

	if (XLByteLE(lsn, PageGetLSN(page)))		/* changes are applied */
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
		return;
	}

	offnum = ItemPointerGetOffsetNumber(&(xlrec->newtid));
	if (PageGetMaxOffsetNumber(page) + 1 < offnum)
		elog(PANIC, "heap_update_redo: invalid max offset number");

	hsize = SizeOfHeapUpdate + SizeOfHeapHeader;
	if (move)
		hsize += (2 * sizeof(TransactionId));

	newlen = record->xl_len - hsize;
	Assert(newlen <= MaxTupleSize);
	memcpy((char *) &xlhdr,
		   (char *) xlrec + SizeOfHeapUpdate,
		   SizeOfHeapHeader);
	htup = &tbuf.hdr;
	MemSet((char *) htup, 0, sizeof(HeapTupleHeaderData));
	/* PG73FORMAT: get bitmap [+ padding] [+ oid] + data */
	memcpy((char *) htup + offsetof(HeapTupleHeaderData, t_bits),
		   (char *) xlrec + hsize,
		   newlen);
	newlen += offsetof(HeapTupleHeaderData, t_bits);
	htup->t_natts = xlhdr.t_natts;
	htup->t_infomask = xlhdr.t_infomask;
	htup->t_hoff = xlhdr.t_hoff;

	if (move)
	{
		TransactionId xid[2];	/* xmax, xmin */

		memcpy((char *) xid,
			   (char *) xlrec + SizeOfHeapUpdate + SizeOfHeapHeader,
			   2 * sizeof(TransactionId));
		HeapTupleHeaderSetXmin(htup, xid[1]);
		HeapTupleHeaderSetXmax(htup, xid[0]);
		HeapTupleHeaderSetXvac(htup, record->xl_xid);
	}
	else
	{
		HeapTupleHeaderSetXmin(htup, record->xl_xid);
		HeapTupleHeaderSetCmin(htup, FirstCommandId);
	}
	/* Make sure there is no forward chain link in t_ctid */
	htup->t_ctid = xlrec->newtid;

	offnum = PageAddItem(page, (Item) htup, newlen, offnum,
						 LP_USED | OverwritePageMode);
	if (offnum == InvalidOffsetNumber)
		elog(PANIC, "heap_update_redo: failed to add tuple");
	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	WriteBuffer(buffer);
}

static void
heap_xlog_lock(XLogRecPtr lsn, XLogRecord *record)
{
	xl_heap_lock *xlrec = (xl_heap_lock *) XLogRecGetData(record);
	Relation	reln;
	Buffer		buffer;
	Page		page;
	OffsetNumber offnum;
	ItemId		lp = NULL;
	HeapTupleHeader htup;

	if (record->xl_info & XLR_BKP_BLOCK_1)
		return;

	reln = XLogOpenRelation(xlrec->target.node);

	if (!RelationIsValid(reln))
		return;

	buffer = XLogReadBuffer(false, reln,
							ItemPointerGetBlockNumber(&(xlrec->target.tid)));
	if (!BufferIsValid(buffer))
		elog(PANIC, "heap_lock_redo: no block");

	page = (Page) BufferGetPage(buffer);
	if (PageIsNew((PageHeader) page))
		elog(PANIC, "heap_lock_redo: uninitialized page");

	if (XLByteLE(lsn, PageGetLSN(page)))		/* changes are applied */
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
		return;
	}

	offnum = ItemPointerGetOffsetNumber(&(xlrec->target.tid));
	if (PageGetMaxOffsetNumber(page) >= offnum)
		lp = PageGetItemId(page, offnum);

	if (PageGetMaxOffsetNumber(page) < offnum || !ItemIdIsUsed(lp))
		elog(PANIC, "heap_lock_redo: invalid lp");

	htup = (HeapTupleHeader) PageGetItem(page, lp);

	htup->t_infomask &= ~(HEAP_XMAX_COMMITTED |
						  HEAP_XMAX_INVALID |
						  HEAP_XMAX_IS_MULTI |
						  HEAP_IS_LOCKED |
						  HEAP_MOVED);
	if (xlrec->xid_is_mxact)
		htup->t_infomask |= HEAP_XMAX_IS_MULTI;
	if (xlrec->shared_lock)
		htup->t_infomask |= HEAP_XMAX_SHARED_LOCK;
	else
		htup->t_infomask |= HEAP_XMAX_EXCL_LOCK;
	HeapTupleHeaderSetXmax(htup, xlrec->locking_xid);
	HeapTupleHeaderSetCmax(htup, FirstCommandId);
	/* Make sure there is no forward chain link in t_ctid */
	htup->t_ctid = xlrec->target.tid;
	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	WriteBuffer(buffer);
}

void
heap_redo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;

	info &= XLOG_HEAP_OPMASK;
	if (info == XLOG_HEAP_INSERT)
		heap_xlog_insert(lsn, record);
	else if (info == XLOG_HEAP_DELETE)
		heap_xlog_delete(lsn, record);
	else if (info == XLOG_HEAP_UPDATE)
		heap_xlog_update(lsn, record, false);
	else if (info == XLOG_HEAP_MOVE)
		heap_xlog_update(lsn, record, true);
	else if (info == XLOG_HEAP_CLEAN)
		heap_xlog_clean(lsn, record);
	else if (info == XLOG_HEAP_NEWPAGE)
		heap_xlog_newpage(lsn, record);
	else if (info == XLOG_HEAP_LOCK)
		heap_xlog_lock(lsn, record);
	else
		elog(PANIC, "heap_redo: unknown op code %u", info);
}

static void
out_target(char *buf, xl_heaptid *target)
{
	sprintf(buf + strlen(buf), "rel %u/%u/%u; tid %u/%u",
			target->node.spcNode, target->node.dbNode, target->node.relNode,
			ItemPointerGetBlockNumber(&(target->tid)),
			ItemPointerGetOffsetNumber(&(target->tid)));
}

void
heap_desc(char *buf, uint8 xl_info, char *rec)
{
	uint8		info = xl_info & ~XLR_INFO_MASK;

	info &= XLOG_HEAP_OPMASK;
	if (info == XLOG_HEAP_INSERT)
	{
		xl_heap_insert *xlrec = (xl_heap_insert *) rec;

		strcat(buf, "insert: ");
		out_target(buf, &(xlrec->target));
	}
	else if (info == XLOG_HEAP_DELETE)
	{
		xl_heap_delete *xlrec = (xl_heap_delete *) rec;

		strcat(buf, "delete: ");
		out_target(buf, &(xlrec->target));
	}
	else if (info == XLOG_HEAP_UPDATE || info == XLOG_HEAP_MOVE)
	{
		xl_heap_update *xlrec = (xl_heap_update *) rec;

		if (info == XLOG_HEAP_UPDATE)
			strcat(buf, "update: ");
		else
			strcat(buf, "move: ");
		out_target(buf, &(xlrec->target));
		sprintf(buf + strlen(buf), "; new %u/%u",
				ItemPointerGetBlockNumber(&(xlrec->newtid)),
				ItemPointerGetOffsetNumber(&(xlrec->newtid)));
	}
	else if (info == XLOG_HEAP_CLEAN)
	{
		xl_heap_clean *xlrec = (xl_heap_clean *) rec;

		sprintf(buf + strlen(buf), "clean: rel %u/%u/%u; blk %u",
				xlrec->node.spcNode, xlrec->node.dbNode,
				xlrec->node.relNode, xlrec->block);
	}
	else if (info == XLOG_HEAP_NEWPAGE)
	{
		xl_heap_newpage *xlrec = (xl_heap_newpage *) rec;

		sprintf(buf + strlen(buf), "newpage: rel %u/%u/%u; blk %u",
				xlrec->node.spcNode, xlrec->node.dbNode,
				xlrec->node.relNode, xlrec->blkno);
	}
	else if (info == XLOG_HEAP_LOCK)
	{
		xl_heap_lock *xlrec = (xl_heap_lock *) rec;

		if (xlrec->shared_lock)
			strcat(buf, "shared_lock: ");
		else
			strcat(buf, "exclusive_lock: ");
		if (xlrec->xid_is_mxact)
			strcat(buf, "mxid ");
		else
			strcat(buf, "xid ");
		sprintf(buf + strlen(buf), "%u ", xlrec->locking_xid);
		out_target(buf, &(xlrec->target));
	}
	else
		strcat(buf, "UNKNOWN");
}
