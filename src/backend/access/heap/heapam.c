/*-------------------------------------------------------------------------
 *
 * heapam.c
 *	  heap access method code
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/heap/heapam.c,v 1.157.2.2 2005/08/25 22:07:11 tgl Exp $
 *
 *
 * INTERFACE ROUTINES
 *		relation_open	- open any relation by relation OID
 *		relation_openrv - open any relation specified by a RangeVar
 *		relation_openr	- open a system relation by name
 *		relation_close	- close any relation
 *		heap_open		- open a heap relation by relation OID
 *		heap_openrv		- open a heap relation specified by a RangeVar
 *		heap_openr		- open a system heap relation by name
 *		heap_close		- (now just a macro for relation_close)
 *		heap_beginscan	- begin relation scan
 *		heap_rescan		- restart a relation scan
 *		heap_endscan	- end relation scan
 *		heap_getnext	- retrieve next tuple in scan
 *		heap_fetch		- retrieve tuple with tid
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
#include "access/tuptoaster.h"
#include "access/valid.h"
#include "access/xlogutils.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "miscadmin.h"
#include "utils/inval.h"
#include "utils/relcache.h"
#include "pgstat.h"


/* comments are in heap_update */
static xl_heaptid _locked_tuple_;
static void _heap_unlock_tuple(void *data);
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
	 * Make sure we have up-to-date idea of number of blocks in relation.
	 * It is sufficient to do this once at scan start, since any tuples
	 * added while the scan is in progress will be invisible to my
	 * transaction anyway...
	 */
	scan->rs_rd->rd_nblocks = RelationGetNumberOfBlocks(scan->rs_rd);

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
		   ScanKey key)
{
	ItemId		lpp;
	Page		dp;
	BlockNumber page;
	BlockNumber pages;
	int			lines;
	OffsetNumber lineoff;
	int			linesleft;
	ItemPointer tid;

	tid = (tuple->t_data == NULL) ? (ItemPointer) NULL : &(tuple->t_self);

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
#endif   /* !defined(HEAPLOGALL) */

	if (!ItemPointerIsValid(tid))
	{
		Assert(!PointerIsValid(tid));
		tid = NULL;
	}

	tuple->t_tableOid = relation->rd_id;

	/*
	 * return null immediately if relation is empty
	 */
	if ((pages = relation->rd_nblocks) == 0)
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
		if (!BufferIsValid(*buffer))
			elog(ERROR, "ReadBuffer failed");

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
		if (!BufferIsValid(*buffer))
			elog(ERROR, "ReadBuffer failed");

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
		if (!BufferIsValid(*buffer))
			elog(ERROR, "ReadBuffer failed");

		LockBuffer(*buffer, BUFFER_LOCK_SHARE);

		dp = (Page) BufferGetPage(*buffer);
		lines = PageGetMaxOffsetNumber(dp);
		/* page and lineoff now reference the physically next tid */
	}

	/* 'dir' is now non-zero */

	/*
	 * calculate line pointer and number of remaining items to check on
	 * this page.
	 */
	lpp = PageGetItemId(dp, lineoff);
	if (dir < 0)
		linesleft = lineoff - 1;
	else
		linesleft = lines - lineoff;

	/*
	 * advance the scan until we find a qualifying tuple or run out of
	 * stuff to scan
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
				++lpp;			/* move forward in this page's ItemId
								 * array */
				++lineoff;
			}
		}

		/*
		 * if we get here, it means we've exhausted the items on this page
		 * and it's time to move to the next.
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
		if (!BufferIsValid(*buffer))
			elog(ERROR, "ReadBuffer failed");

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
	 * In bootstrap mode, don't do any namespace processing.
	 */
	if (IsBootstrapProcessingMode())
	{
		Assert(relation->schemaname == NULL);
		return relation_openr(relation->relname, lockmode);
	}

	/*
	 * Check for shared-cache-inval messages before trying to open the
	 * relation.  This is needed to cover the case where the name
	 * identifies a rel that has been dropped and recreated since the
	 * start of our transaction: if we don't flush the old syscache entry
	 * then we'll latch onto that entry and suffer an error when we do
	 * LockRelation. Note that relation_open does not need to do this,
	 * since a relation's OID never changes.
	 *
	 * We skip this if asked for NoLock, on the assumption that the caller
	 * has already ensured some appropriate lock is held.
	 */
	if (lockmode != NoLock)
		AcceptInvalidationMessages();

	/* Look up the appropriate relation using namespace search */
	relOid = RangeVarGetRelid(relation, false);

	/* Let relation_open do the rest */
	return relation_open(relOid, lockmode);
}

/* ----------------
 *		relation_openr - open a system relation specified by name.
 *
 *		As above, but the relation is specified by an unqualified name;
 *		it is assumed to live in the system catalog namespace.
 * ----------------
 */
Relation
relation_openr(const char *sysRelationName, LOCKMODE lockmode)
{
	Relation	r;

	Assert(lockmode >= NoLock && lockmode < MAX_LOCKMODES);

	/*
	 * We assume we should not need to worry about the rel's OID changing,
	 * hence no need for AcceptInvalidationMessages here.
	 */

	/* The relcache does all the real work... */
	r = RelationSysNameGetRelation(sysRelationName);

	if (!RelationIsValid(r))
		elog(ERROR, "could not open relation \"%s\"", sysRelationName);

	if (lockmode != NoLock)
		LockRelation(r, lockmode);

	return r;
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
 *		heap_openr - open a system heap relation specified by name.
 *
 *		As above, but the relation is specified by an unqualified name;
 *		it is assumed to live in the system catalog namespace.
 * ----------------
 */
Relation
heap_openr(const char *sysRelationName, LOCKMODE lockmode)
{
	Relation	r;

	r = relation_openr(sysRelationName, lockmode);

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
	 * This is just to make really sure the relcache entry won't go away
	 * while the scan has a pointer to it.	Caller should be holding the
	 * rel open anyway, so this is redundant in all normal scenarios...
	 */
	RelationIncrementReferenceCount(relation);

	/* XXX someday assert SelfTimeQual if relkind == RELKIND_UNCATALOGED */
	if (relation->rd_rel->relkind == RELKIND_UNCATALOGED)
		snapshot = SnapshotSelf;

	/*
	 * allocate and initialize scan descriptor
	 */
	scan = (HeapScanDesc) palloc(sizeof(HeapScanDescData));

	scan->rs_rd = relation;
	scan->rs_snapshot = snapshot;
	scan->rs_nkeys = nkeys;

	/*
	 * we do this here instead of in initscan() because heap_rescan also
	 * calls initscan() and we don't want to allocate memory again
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

	pgstat_reset_heap_scan(&scan->rs_pgstat_info);
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
			   scan->rs_key);

	if (scan->rs_ctup.t_data == NULL && !BufferIsValid(scan->rs_cbuf))
	{
		HEAPDEBUG_2;			/* heap_getnext returning EOS */
		return NULL;
	}

	pgstat_count_heap_scan(&scan->rs_pgstat_info);

	/*
	 * if we get here it means we have a new current scan tuple, so point
	 * to the proper return buffer and return the tuple.
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
 * If the tuple is not found, then tuple->t_data is set to NULL, *userbuf
 * is set to InvalidBuffer, and FALSE is returned.
 *
 * If the tuple is found but fails the time qual check, then FALSE will be
 * returned. When the caller specifies keep_buf = true, we retain the pin
 * on the buffer and return it in *userbuf (so the caller can still access
 * the tuple); when keep_buf = false, the pin is released and *userbuf is set
 * to InvalidBuffer.
 *
 * It is somewhat inconsistent that we ereport() on invalid block number but
 * return false on invalid item number.  This is historical.  The only
 * justification I can see is that the caller can relatively easily check the
 * block number for validity, but cannot check the item number without reading
 * the page himself.
 */
bool
heap_fetch(Relation relation,
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
	 * buffer pin.
	 */
	buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(tid));

	if (!BufferIsValid(buffer))
		elog(ERROR, "ReadBuffer(\"%s\", %lu) failed",
			 RelationGetRelationName(relation),
			 (unsigned long) ItemPointerGetBlockNumber(tid));

	/*
	 * Need share lock on buffer to examine tuple commit status.
	 */
	LockBuffer(buffer, BUFFER_LOCK_SHARE);

	/*
	 * get the item line pointer corresponding to the requested tid
	 */
	dp = (PageHeader) BufferGetPage(buffer);
	offnum = ItemPointerGetOffsetNumber(tid);
	lp = PageGetItemId(dp, offnum);

	/*
	 * must check for deleted tuple (see for example analyze.c, which is
	 * careful to pass an offnum in range, but doesn't know if the offnum
	 * actually corresponds to an undeleted tuple).
	 */
	if (!ItemIdIsUsed(lp))
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
		*userbuf = InvalidBuffer;
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
	tuple->t_tableOid = relation->rd_id;

	/*
	 * check time qualification of tuple, then release lock
	 */
	HeapTupleSatisfies(tuple, relation, buffer, dp,
					   snapshot, 0, (ScanKey) NULL, valid);

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

	if (valid)
	{
		/*
		 * All checks passed, so return the tuple as valid. Caller is now
		 * responsible for releasing the buffer.
		 */
		*userbuf = buffer;

		/*
		 * Count the successful fetch in *pgstat_info if given, otherwise
		 * in the relation's default statistics area.
		 */
		if (pgstat_info != NULL)
			pgstat_count_heap_fetch(pgstat_info);
		else
			pgstat_count_heap_fetch(&relation->pgstat_info);

		return true;
	}

	/* Tuple failed time qual, but maybe caller wants to see it anyway. */
	if (keep_buf)
	{
		*userbuf = buffer;

		return false;
	}

	/* Okay to release pin on buffer. */
	ReleaseBuffer(buffer);

	*userbuf = InvalidBuffer;

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
 * show the latest version of the row.  Note that it will not be changed
 * if no version of the row passes the snapshot test.
 */
void
heap_get_latest_tid(Relation relation,
					Snapshot snapshot,
					ItemPointer tid)
{
	BlockNumber	blk;
	ItemPointerData ctid;
	TransactionId priorXmax;

	/* this is to avoid Assert failures on bad input */
	if (!ItemPointerIsValid(tid))
		return;

	/*
	 * Since this can be called with user-supplied TID, don't trust the
	 * input too much.  (RelationGetNumberOfBlocks is an expensive check,
	 * so we don't check t_ctid links again this way.  Note that it would
	 * not do to call it just once and save the result, either.)
	 */
	blk = ItemPointerGetBlockNumber(tid);
	if (blk >= RelationGetNumberOfBlocks(relation))
		elog(ERROR, "block number %u is out of range for relation \"%s\"",
			 blk, RelationGetRelationName(relation));

	/*
	 * Loop to chase down t_ctid links.  At top of loop, ctid is the
	 * tuple we need to examine, and *tid is the TID we will return if
	 * ctid turns out to be bogus.
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

		if (!BufferIsValid(buffer))
			elog(ERROR, "ReadBuffer(\"%s\", %lu) failed",
				 RelationGetRelationName(relation),
				 (unsigned long) ItemPointerGetBlockNumber(&ctid));

		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		dp = (PageHeader) BufferGetPage(buffer);

		/*
		 * Check for bogus item number.  This is not treated as an error
		 * condition because it can happen while following a t_ctid link.
		 * We just assume that the prior tid is OK and return it unchanged.
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
		if ((tp.t_data->t_infomask & (HEAP_XMAX_INVALID |
									  HEAP_MARKED_FOR_UPDATE)) ||
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
	}				/* end of loop */
}

/*
 *	heap_insert		- insert tuple into a heap
 *
 * The new tuple is stamped with current transaction ID and the specified
 * command ID.
 */
Oid
heap_insert(Relation relation, HeapTuple tup, CommandId cid)
{
	Buffer		buffer;

	if (relation->rd_rel->relhasoids)
	{
#ifdef NOT_USED
		/* this is redundant with an Assert in HeapTupleSetOid */
		Assert(tup->t_data->t_infomask & HEAP_HASOID);
#endif

		/*
		 * If the object id of this tuple has already been assigned, trust
		 * the caller.	There are a couple of ways this can happen.  At
		 * initial db creation, the backend program sets oids for tuples.
		 * When we define an index, we set the oid.  Finally, in the
		 * future, we may allow users to set their own object ids in order
		 * to support a persistent object store (objects need to contain
		 * pointers to one another).
		 */
		if (!OidIsValid(HeapTupleGetOid(tup)))
			HeapTupleSetOid(tup, newoid());
		else
			CheckMaxObjectId(HeapTupleGetOid(tup));
	}
	else
	{
		/* check there is not space for an OID */
		Assert(!(tup->t_data->t_infomask & HEAP_HASOID));
	}

	tup->t_data->t_infomask &= ~(HEAP_XACT_MASK);
	tup->t_data->t_infomask |= HEAP_XMAX_INVALID;
	HeapTupleHeaderSetXmin(tup->t_data, GetCurrentTransactionId());
	HeapTupleHeaderSetCmin(tup->t_data, cid);
	tup->t_tableOid = relation->rd_id;

#ifdef TUPLE_TOASTER_ACTIVE

	/*
	 * If the new tuple is too big for storage or contains already toasted
	 * attributes from some other relation, invoke the toaster.
	 */
	if (HeapTupleHasExtended(tup) ||
		(MAXALIGN(tup->t_len) > TOAST_TUPLE_THRESHOLD))
		heap_tuple_toast_attrs(relation, tup, NULL);
#endif

	/* Find buffer to insert this tuple into */
	buffer = RelationGetBufferForTuple(relation, tup->t_len, InvalidBuffer);

	/* NO EREPORT(ERROR) from here till changes are logged */
	START_CRIT_SECTION();

	RelationPutHeapTuple(relation, buffer, tup);

	pgstat_count_heap_insert(&relation->pgstat_info);

	/* XLOG stuff */
	if (!relation->rd_istemp)
	{
		xl_heap_insert xlrec;
		xl_heap_header xlhdr;
		XLogRecPtr	recptr;
		XLogRecData rdata[3];
		Page		page = BufferGetPage(buffer);
		uint8		info = XLOG_HEAP_INSERT;

		xlrec.target.node = relation->rd_node;
		xlrec.target.tid = tup->t_self;
		rdata[0].buffer = InvalidBuffer;
		rdata[0].data = (char *) &xlrec;
		rdata[0].len = SizeOfHeapInsert;
		rdata[0].next = &(rdata[1]);

		xlhdr.t_natts = tup->t_data->t_natts;
		xlhdr.t_infomask = tup->t_data->t_infomask;
		xlhdr.t_hoff = tup->t_data->t_hoff;

		/*
		 * note we mark rdata[1] as belonging to buffer; if XLogInsert
		 * decides to write the whole page to the xlog, we don't need to
		 * store xl_heap_header in the xlog.
		 */
		rdata[1].buffer = buffer;
		rdata[1].data = (char *) &xlhdr;
		rdata[1].len = SizeOfHeapHeader;
		rdata[1].next = &(rdata[2]);

		rdata[2].buffer = buffer;
		/* PG73FORMAT: write bitmap [+ padding] [+ oid] + data */
		rdata[2].data = (char *) tup->t_data + offsetof(HeapTupleHeaderData, t_bits);
		rdata[2].len = tup->t_len - offsetof(HeapTupleHeaderData, t_bits);
		rdata[2].next = NULL;

		/*
		 * If this is the single and first tuple on page, we can reinit
		 * the page instead of restoring the whole thing.  Set flag, and
		 * hide buffer references from XLogInsert.
		 */
		if (ItemPointerGetOffsetNumber(&(tup->t_self)) == FirstOffsetNumber &&
			PageGetMaxOffsetNumber(page) == FirstOffsetNumber)
		{
			info |= XLOG_HEAP_INIT_PAGE;
			rdata[1].buffer = rdata[2].buffer = InvalidBuffer;
		}

		recptr = XLogInsert(RM_HEAP_ID, info, rdata);

		PageSetLSN(page, recptr);
		PageSetSUI(page, ThisStartUpID);
	}
	else
	{
		/* No XLOG record, but still need to flag that XID exists on disk */
		MyXactMadeTempRelUpdate = true;
	}

	END_CRIT_SECTION();

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	WriteBuffer(buffer);

	/*
	 * If tuple is cachable, mark it for invalidation from the caches in
	 * case we abort.  Note it is OK to do this after WriteBuffer releases
	 * the buffer, because the "tup" data structure is all in local
	 * memory, not in the shared buffer.
	 */
	CacheInvalidateHeapTuple(relation, tup);

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
	return heap_insert(relation, tup, GetCurrentCommandId());
}

/*
 *	heap_delete		- delete a tuple
 *
 * NB: do not call this directly unless you are prepared to deal with
 * concurrent-update conditions.  Use simple_heap_delete instead.
 *
 *	relation - table to be modified
 *	tid - TID of tuple to be deleted
 *	ctid - output parameter, used only for failure case (see below)
 *	update_xmax - output parameter, used only for failure case (see below)
 *	cid - delete command ID to use in verifying tuple visibility
 *	crosscheck - if not SnapshotAny, also check tuple against this
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
int
heap_delete(Relation relation, ItemPointer tid,
			ItemPointer ctid, TransactionId *update_xmax,
			CommandId cid, Snapshot crosscheck, bool wait)
{
	ItemId		lp;
	HeapTupleData tp;
	PageHeader	dp;
	Buffer		buffer;
	int			result;
	uint16		sv_infomask;

	Assert(ItemPointerIsValid(tid));

	buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(tid));

	if (!BufferIsValid(buffer))
		elog(ERROR, "ReadBuffer failed");

	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	dp = (PageHeader) BufferGetPage(buffer);
	lp = PageGetItemId(dp, ItemPointerGetOffsetNumber(tid));

	tp.t_datamcxt = NULL;
	tp.t_data = (HeapTupleHeader) PageGetItem(dp, lp);
	tp.t_len = ItemIdGetLength(lp);
	tp.t_self = *tid;

l1:
	sv_infomask = tp.t_data->t_infomask;
	result = HeapTupleSatisfiesUpdate(tp.t_data, cid);
	if (sv_infomask != tp.t_data->t_infomask)
		SetBufferCommitInfoNeedsSave(buffer);

	if (result == HeapTupleInvisible)
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
		elog(ERROR, "attempted to delete invisible tuple");
	}
	else if (result == HeapTupleBeingUpdated && wait)
	{
		TransactionId xwait = HeapTupleHeaderGetXmax(tp.t_data);

		/* sleep until concurrent transaction ends */
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		XactLockTableWait(xwait);

		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		if (!TransactionIdDidCommit(xwait))
			goto l1;

		/*
		 * xwait is committed but if xwait had just marked the tuple for
		 * update then some other xaction could update this tuple before
		 * we got to this point.
		 */
		if (!TransactionIdEquals(HeapTupleHeaderGetXmax(tp.t_data), xwait))
			goto l1;
		if (!(tp.t_data->t_infomask & HEAP_XMAX_COMMITTED))
		{
			tp.t_data->t_infomask |= HEAP_XMAX_COMMITTED;
			SetBufferCommitInfoNeedsSave(buffer);
		}
		/* if tuple was marked for update but not updated... */
		if (tp.t_data->t_infomask & HEAP_MARKED_FOR_UPDATE)
			result = HeapTupleMayBeUpdated;
		else
			result = HeapTupleUpdated;
	}

	if (crosscheck != SnapshotAny && result == HeapTupleMayBeUpdated)
	{
		/* Perform additional check for serializable RI updates */
		sv_infomask = tp.t_data->t_infomask;
		if (!HeapTupleSatisfiesSnapshot(tp.t_data, crosscheck))
			result = HeapTupleUpdated;
		if (sv_infomask != tp.t_data->t_infomask)
			SetBufferCommitInfoNeedsSave(buffer);
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
		return result;
	}

	START_CRIT_SECTION();

	/* store transaction information of xact deleting the tuple */
	tp.t_data->t_infomask &= ~(HEAP_XMAX_COMMITTED |
							   HEAP_XMAX_INVALID |
							   HEAP_MARKED_FOR_UPDATE |
							   HEAP_MOVED);
	HeapTupleHeaderSetXmax(tp.t_data, GetCurrentTransactionId());
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
		rdata[0].buffer = InvalidBuffer;
		rdata[0].data = (char *) &xlrec;
		rdata[0].len = SizeOfHeapDelete;
		rdata[0].next = &(rdata[1]);

		rdata[1].buffer = buffer;
		rdata[1].data = NULL;
		rdata[1].len = 0;
		rdata[1].next = NULL;

		recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_DELETE, rdata);

		PageSetLSN(dp, recptr);
		PageSetSUI(dp, ThisStartUpID);
	}
	else
	{
		/* No XLOG record, but still need to flag that XID exists on disk */
		MyXactMadeTempRelUpdate = true;
	}

	END_CRIT_SECTION();

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

#ifdef TUPLE_TOASTER_ACTIVE

	/*
	 * If the relation has toastable attributes, we need to delete no
	 * longer needed items there too.  We have to do this before
	 * WriteBuffer because we need to look at the contents of the tuple,
	 * but it's OK to release the context lock on the buffer first.
	 */
	if (HeapTupleHasExtended(&tp))
		heap_tuple_toast_attrs(relation, NULL, &(tp));
#endif

	pgstat_count_heap_delete(&relation->pgstat_info);

	/*
	 * Mark tuple for invalidation from system caches at next command
	 * boundary. We have to do this before WriteBuffer because we need to
	 * look at the contents of the tuple, so we need to hold our refcount
	 * on the buffer.
	 */
	CacheInvalidateHeapTuple(relation, &tp);

	WriteBuffer(buffer);

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
	int			result;
	ItemPointerData update_ctid;
	TransactionId update_xmax;

	result = heap_delete(relation, tid,
						 &update_ctid, &update_xmax,
						 GetCurrentCommandId(), SnapshotAny,
						 true /* wait for commit */);
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
 *	relation - table to be modified
 *	otid - TID of old tuple to be replaced
 *	newtup - newly constructed tuple data to store
 *	ctid - output parameter, used only for failure case (see below)
 *	update_xmax - output parameter, used only for failure case (see below)
 *	cid - update command ID to use in verifying old tuple visibility
 *	crosscheck - if not SnapshotAny, also check old tuple against this
 *	wait - true if should wait for any conflicting update to commit/abort
 *
 * Normal, successful return value is HeapTupleMayBeUpdated, which
 * actually means we *did* update it.  Failure return codes are
 * HeapTupleSelfUpdated, HeapTupleUpdated, or HeapTupleBeingUpdated
 * (the last only possible if wait == false).
 *
 * On success, newtup->t_self is set to the TID where the new tuple
 * was inserted.
 *
 * In the failure cases, the routine returns the tuple's t_ctid and t_xmax.
 * If t_ctid is the same as otid, the tuple was deleted; if different, the
 * tuple was updated, and t_ctid is the location of the replacement tuple.
 * (t_xmax is needed to verify that the replacement tuple matches.)
 */
int
heap_update(Relation relation, ItemPointer otid, HeapTuple newtup,
			ItemPointer ctid, TransactionId *update_xmax,
			CommandId cid, Snapshot crosscheck, bool wait)
{
	ItemId		lp;
	HeapTupleData oldtup;
	PageHeader	dp;
	Buffer		buffer,
				newbuf;
	bool		need_toast,
				already_marked;
	Size		newtupsize,
				pagefree;
	int			result;
	uint16		sv_infomask;

	Assert(ItemPointerIsValid(otid));

	buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(otid));
	if (!BufferIsValid(buffer))
		elog(ERROR, "ReadBuffer failed");
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
	 * with the new tuple's location, so there's great risk of confusion
	 * if we use otid anymore.
	 */

l2:
	sv_infomask = oldtup.t_data->t_infomask;
	result = HeapTupleSatisfiesUpdate(oldtup.t_data, cid);
	if (sv_infomask != oldtup.t_data->t_infomask)
		SetBufferCommitInfoNeedsSave(buffer);

	if (result == HeapTupleInvisible)
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
		elog(ERROR, "attempted to update invisible tuple");
	}
	else if (result == HeapTupleBeingUpdated && wait)
	{
		TransactionId xwait = HeapTupleHeaderGetXmax(oldtup.t_data);

		/* sleep untill concurrent transaction ends */
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		XactLockTableWait(xwait);

		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		if (!TransactionIdDidCommit(xwait))
			goto l2;

		/*
		 * xwait is committed but if xwait had just marked the tuple for
		 * update then some other xaction could update this tuple before
		 * we got to this point.
		 */
		if (!TransactionIdEquals(HeapTupleHeaderGetXmax(oldtup.t_data), xwait))
			goto l2;
		if (!(oldtup.t_data->t_infomask & HEAP_XMAX_COMMITTED))
		{
			oldtup.t_data->t_infomask |= HEAP_XMAX_COMMITTED;
			SetBufferCommitInfoNeedsSave(buffer);
		}
		/* if tuple was marked for update but not updated... */
		if (oldtup.t_data->t_infomask & HEAP_MARKED_FOR_UPDATE)
			result = HeapTupleMayBeUpdated;
		else
			result = HeapTupleUpdated;
	}

	if (crosscheck != SnapshotAny && result == HeapTupleMayBeUpdated)
	{
		/* Perform additional check for serializable RI updates */
		sv_infomask = oldtup.t_data->t_infomask;
		if (!HeapTupleSatisfiesSnapshot(oldtup.t_data, crosscheck))
			result = HeapTupleUpdated;
		if (sv_infomask != oldtup.t_data->t_infomask)
			SetBufferCommitInfoNeedsSave(buffer);
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
	HeapTupleHeaderSetXmin(newtup->t_data, GetCurrentTransactionId());
	HeapTupleHeaderSetCmin(newtup->t_data, cid);

	/*
	 * If the toaster needs to be activated, OR if the new tuple will not
	 * fit on the same page as the old, then we need to release the
	 * context lock (but not the pin!) on the old tuple's buffer while we
	 * are off doing TOAST and/or table-file-extension work.  We must mark
	 * the old tuple to show that it's already being updated, else other
	 * processes may try to update it themselves. To avoid second XLOG log
	 * record, we use xact mgr hook to unlock old tuple without reading
	 * log if xact will abort before update is logged. In the event of
	 * crash prio logging, TQUAL routines will see HEAP_XMAX_UNLOGGED
	 * flag...
	 *
	 * NOTE: this trick is useless currently but saved for future when we'll
	 * implement UNDO and will re-use transaction IDs after postmaster
	 * startup.
	 *
	 * We need to invoke the toaster if there are already any toasted values
	 * present, or if the new tuple is over-threshold.
	 */
	need_toast = (HeapTupleHasExtended(&oldtup) ||
				  HeapTupleHasExtended(newtup) ||
				  (MAXALIGN(newtup->t_len) > TOAST_TUPLE_THRESHOLD));

	newtupsize = MAXALIGN(newtup->t_len);
	pagefree = PageGetFreeSpace((Page) dp);

	if (need_toast || newtupsize > pagefree)
	{
		_locked_tuple_.node = relation->rd_node;
		_locked_tuple_.tid = oldtup.t_self;
		XactPushRollback(_heap_unlock_tuple, (void *) &_locked_tuple_);

		oldtup.t_data->t_infomask &= ~(HEAP_XMAX_COMMITTED |
									   HEAP_XMAX_INVALID |
									   HEAP_MARKED_FOR_UPDATE |
									   HEAP_MOVED);
		oldtup.t_data->t_infomask |= HEAP_XMAX_UNLOGGED;
		HeapTupleHeaderSetXmax(oldtup.t_data, GetCurrentTransactionId());
		HeapTupleHeaderSetCmax(oldtup.t_data, cid);
		already_marked = true;
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

		/* Let the toaster do its thing */
		if (need_toast)
		{
			heap_tuple_toast_attrs(relation, newtup, &oldtup);
			newtupsize = MAXALIGN(newtup->t_len);
		}

		/*
		 * Now, do we need a new page for the tuple, or not?  This is a
		 * bit tricky since someone else could have added tuples to the
		 * page while we weren't looking.  We have to recheck the
		 * available space after reacquiring the buffer lock.  But don't
		 * bother to do that if the former amount of free space is still
		 * not enough; it's unlikely there's more free now than before.
		 *
		 * What's more, if we need to get a new page, we will need to acquire
		 * buffer locks on both old and new pages.	To avoid deadlock
		 * against some other backend trying to get the same two locks in
		 * the other order, we must be consistent about the order we get
		 * the locks in. We use the rule "lock the lower-numbered page of
		 * the relation first".  To implement this, we must do
		 * RelationGetBufferForTuple while not holding the lock on the old
		 * page, and we must rely on it to get the locks on both pages in
		 * the correct order.
		 */
		if (newtupsize > pagefree)
		{
			/* Assume there's no chance to put newtup on same page. */
			newbuf = RelationGetBufferForTuple(relation, newtup->t_len,
											   buffer);
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
				 * relock to avoid deadlock.  Fortunately, this path
				 * should seldom be taken.
				 */
				LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
				newbuf = RelationGetBufferForTuple(relation, newtup->t_len,
												   buffer);
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
	}

	pgstat_count_heap_update(&relation->pgstat_info);

	/*
	 * At this point newbuf and buffer are both pinned and locked, and
	 * newbuf has enough space for the new tuple.  If they are the same
	 * buffer, only one pin is held.
	 */

	/* NO EREPORT(ERROR) from here till changes are logged */
	START_CRIT_SECTION();

	RelationPutHeapTuple(relation, newbuf, newtup);		/* insert new tuple */

	if (already_marked)
	{
		oldtup.t_data->t_infomask &= ~HEAP_XMAX_UNLOGGED;
		XactPopRollback();
	}
	else
	{
		oldtup.t_data->t_infomask &= ~(HEAP_XMAX_COMMITTED |
									   HEAP_XMAX_INVALID |
									   HEAP_MARKED_FOR_UPDATE |
									   HEAP_MOVED);
		HeapTupleHeaderSetXmax(oldtup.t_data, GetCurrentTransactionId());
		HeapTupleHeaderSetCmax(oldtup.t_data, cid);
	}

	/* record address of new tuple in t_ctid of old one */
	oldtup.t_data->t_ctid = newtup->t_self;

	/* XLOG stuff */
	if (!relation->rd_istemp)
	{
		XLogRecPtr	recptr = log_heap_update(relation, buffer, oldtup.t_self,
											 newbuf, newtup, false);

		if (newbuf != buffer)
		{
			PageSetLSN(BufferGetPage(newbuf), recptr);
			PageSetSUI(BufferGetPage(newbuf), ThisStartUpID);
		}
		PageSetLSN(BufferGetPage(buffer), recptr);
		PageSetSUI(BufferGetPage(buffer), ThisStartUpID);
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
	 * boundary. We have to do this before WriteBuffer because we need to
	 * look at the contents of the tuple, so we need to hold our refcount.
	 */
	CacheInvalidateHeapTuple(relation, &oldtup);

	if (newbuf != buffer)
		WriteBuffer(newbuf);
	WriteBuffer(buffer);

	/*
	 * If new tuple is cachable, mark it for invalidation from the caches
	 * in case we abort.  Note it is OK to do this after WriteBuffer
	 * releases the buffer, because the "newtup" data structure is all in
	 * local memory, not in the shared buffer.
	 */
	CacheInvalidateHeapTuple(relation, newtup);

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
	int			result;
	ItemPointerData update_ctid;
	TransactionId update_xmax;

	result = heap_update(relation, otid, tup,
						 &update_ctid, &update_xmax,
						 GetCurrentCommandId(), SnapshotAny,
						 true /* wait for commit */);
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
 *	heap_mark4update		- mark a tuple for update
 *
 * Note that this acquires a buffer pin, which the caller must release.
 *
 * Input parameters:
 *	relation: relation containing tuple (caller must hold suitable lock)
 *	tuple->t_self: TID of tuple to lock (rest of struct need not be valid)
 *	cid: current command ID (used for visibility test, and stored into
 *		tuple's cmax if lock is successful)
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
 */
int
heap_mark4update(Relation relation, HeapTuple tuple, Buffer *buffer,
				 ItemPointer ctid, TransactionId *update_xmax,
				 CommandId cid)
{
	ItemPointer tid = &(tuple->t_self);
	ItemId		lp;
	PageHeader	dp;
	int			result;
	uint16		sv_infomask;

	*buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(tid));

	if (!BufferIsValid(*buffer))
		elog(ERROR, "ReadBuffer failed");

	LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);

	dp = (PageHeader) BufferGetPage(*buffer);
	lp = PageGetItemId(dp, ItemPointerGetOffsetNumber(tid));
	Assert(ItemIdIsUsed(lp));

	tuple->t_datamcxt = NULL;
	tuple->t_data = (HeapTupleHeader) PageGetItem((Page) dp, lp);
	tuple->t_len = ItemIdGetLength(lp);
	tuple->t_tableOid = RelationGetRelid(relation);

l3:
	sv_infomask = tuple->t_data->t_infomask;
	result = HeapTupleSatisfiesUpdate(tuple->t_data, cid);
	if (sv_infomask != tuple->t_data->t_infomask)
		SetBufferCommitInfoNeedsSave(*buffer);

	if (result == HeapTupleInvisible)
	{
		LockBuffer(*buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(*buffer);
		elog(ERROR, "attempted to mark4update invisible tuple");
	}
	else if (result == HeapTupleBeingUpdated)
	{
		TransactionId xwait = HeapTupleHeaderGetXmax(tuple->t_data);

		/* sleep untill concurrent transaction ends */
		LockBuffer(*buffer, BUFFER_LOCK_UNLOCK);
		XactLockTableWait(xwait);

		LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);
		if (!TransactionIdDidCommit(xwait))
			goto l3;

		/*
		 * xwait is committed but if xwait had just marked the tuple for
		 * update then some other xaction could update this tuple before
		 * we got to this point.
		 */
		if (!TransactionIdEquals(HeapTupleHeaderGetXmax(tuple->t_data), xwait))
			goto l3;
		if (!(tuple->t_data->t_infomask & HEAP_XMAX_COMMITTED))
		{
			tuple->t_data->t_infomask |= HEAP_XMAX_COMMITTED;
			SetBufferCommitInfoNeedsSave(*buffer);
		}
		/* if tuple was marked for update but not updated... */
		if (tuple->t_data->t_infomask & HEAP_MARKED_FOR_UPDATE)
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
		return result;
	}

	/*
	 * XLOG stuff: no logging is required as long as we have no
	 * savepoints. For savepoints private log could be used...
	 */
	((PageHeader) BufferGetPage(*buffer))->pd_sui = ThisStartUpID;

	/* store transaction information of xact marking the tuple */
	tuple->t_data->t_infomask &= ~(HEAP_XMAX_COMMITTED |
								   HEAP_XMAX_INVALID |
								   HEAP_MOVED);
	tuple->t_data->t_infomask |= HEAP_MARKED_FOR_UPDATE;
	HeapTupleHeaderSetXmax(tuple->t_data, GetCurrentTransactionId());
	HeapTupleHeaderSetCmax(tuple->t_data, cid);
	/* Make sure there is no forward chain link in t_ctid */
	tuple->t_data->t_ctid = *tid;

	LockBuffer(*buffer, BUFFER_LOCK_UNLOCK);

	WriteNoReleaseBuffer(*buffer);

	return HeapTupleMayBeUpdated;
}

/* ----------------
 *		heap_markpos	- mark scan position
 *
 *		Note:
 *				Should only one mark be maintained per scan at one time.
 *		Check if this can be done generally--say calls to get the
 *		next/previous tuple and NEVER pass struct scandesc to the
 *		user AM's.  Now, the mark is sent to the executor for safekeeping.
 *		Probably can store this info into a GENERAL scan structure.
 *
 *		May be best to change this call to store the marked position
 *		(up to 2?) in the scan structure itself.
 *		Fix to use the proper caching structure.
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
 *
 *		Note:  there are bad side effects here.  If we were past the end
 *		of a relation when heapmarkpos is called, then if the relation is
 *		extended via insert, then the next call to heaprestrpos will set
 *		cause the added tuples to be visible when the scan continues.
 *		Problems also arise if the TID's are rearranged!!!
 *
 * XXX	might be better to do direct access instead of
 *		using the generality of heapgettup().
 *
 * XXX It is very possible that when a scan is restored, that a tuple
 * XXX which previously qualified may fail for time range purposes, unless
 * XXX some form of locking exists (ie., portals currently can act funny.
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
				   (ScanKey) NULL);
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

	rdata[0].buffer = InvalidBuffer;
	rdata[0].data = (char *) &xlrec;
	rdata[0].len = SizeOfHeapClean;
	rdata[0].next = &(rdata[1]);

	/*
	 * The unused-offsets array is not actually in the buffer, but pretend
	 * that it is.	When XLogInsert stores the whole buffer, the offsets
	 * array need not be stored too.
	 */
	rdata[1].buffer = buffer;
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
	rdata[1].next = NULL;

	recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_CLEAN, rdata);

	return (recptr);
}

static XLogRecPtr
log_heap_update(Relation reln, Buffer oldbuf, ItemPointerData from,
				Buffer newbuf, HeapTuple newtup, bool move)
{
	/*
	 * Note: xlhdr is declared to have adequate size and correct alignment
	 * for an xl_heap_header.  However the two tids, if present at all,
	 * will be packed in with no wasted space after the xl_heap_header;
	 * they aren't necessarily aligned as implied by this struct
	 * declaration.
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
	rdata[0].buffer = InvalidBuffer;
	rdata[0].data = (char *) &xlrec;
	rdata[0].len = SizeOfHeapUpdate;
	rdata[0].next = &(rdata[1]);

	rdata[1].buffer = oldbuf;
	rdata[1].data = NULL;
	rdata[1].len = 0;
	rdata[1].next = &(rdata[2]);

	xlhdr.hdr.t_natts = newtup->t_data->t_natts;
	xlhdr.hdr.t_infomask = newtup->t_data->t_infomask;
	xlhdr.hdr.t_hoff = newtup->t_data->t_hoff;
	if (move)					/* remember xmax & xmin */
	{
		TransactionId xid[2];	/* xmax, xmin */

		if (newtup->t_data->t_infomask & (HEAP_XMAX_INVALID |
										  HEAP_MARKED_FOR_UPDATE))
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
	 * As with insert records, we need not store the rdata[2] segment if
	 * we decide to store the whole buffer instead.
	 */
	rdata[2].buffer = newbuf;
	rdata[2].data = (char *) &xlhdr;
	rdata[2].len = hsize;
	rdata[2].next = &(rdata[3]);

	rdata[3].buffer = newbuf;
	/* PG73FORMAT: write bitmap [+ padding] [+ oid] + data */
	rdata[3].data = (char *) newtup->t_data + offsetof(HeapTupleHeaderData, t_bits);
	rdata[3].len = newtup->t_len - offsetof(HeapTupleHeaderData, t_bits);
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
heap_xlog_clean(bool redo, XLogRecPtr lsn, XLogRecord *record)
{
	xl_heap_clean *xlrec = (xl_heap_clean *) XLogRecGetData(record);
	Relation	reln;
	Buffer		buffer;
	Page		page;

	if (!redo || (record->xl_info & XLR_BKP_BLOCK_1))
		return;

	reln = XLogOpenRelation(redo, RM_HEAP_ID, xlrec->node);
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
		UnlockAndReleaseBuffer(buffer);
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
	PageSetSUI(page, ThisStartUpID);	/* prev sui */
	UnlockAndWriteBuffer(buffer);
}

static void
heap_xlog_delete(bool redo, XLogRecPtr lsn, XLogRecord *record)
{
	xl_heap_delete *xlrec = (xl_heap_delete *) XLogRecGetData(record);
	Relation	reln;
	Buffer		buffer;
	Page		page;
	OffsetNumber offnum;
	ItemId		lp = NULL;
	HeapTupleHeader htup;

	if (redo && (record->xl_info & XLR_BKP_BLOCK_1))
		return;

	reln = XLogOpenRelation(redo, RM_HEAP_ID, xlrec->target.node);

	if (!RelationIsValid(reln))
		return;

	buffer = XLogReadBuffer(false, reln,
						ItemPointerGetBlockNumber(&(xlrec->target.tid)));
	if (!BufferIsValid(buffer))
		elog(PANIC, "heap_delete_%sdo: no block", (redo) ? "re" : "un");

	page = (Page) BufferGetPage(buffer);
	if (PageIsNew((PageHeader) page))
		elog(PANIC, "heap_delete_%sdo: uninitialized page", (redo) ? "re" : "un");

	if (redo)
	{
		if (XLByteLE(lsn, PageGetLSN(page)))	/* changes are applied */
		{
			UnlockAndReleaseBuffer(buffer);
			return;
		}
	}
	else if (XLByteLT(PageGetLSN(page), lsn))	/* changes are not applied
												 * ?! */
		elog(PANIC, "heap_delete_undo: bad page LSN");

	offnum = ItemPointerGetOffsetNumber(&(xlrec->target.tid));
	if (PageGetMaxOffsetNumber(page) >= offnum)
		lp = PageGetItemId(page, offnum);

	if (PageGetMaxOffsetNumber(page) < offnum || !ItemIdIsUsed(lp))
		elog(PANIC, "heap_delete_%sdo: invalid lp", (redo) ? "re" : "un");

	htup = (HeapTupleHeader) PageGetItem(page, lp);

	if (redo)
	{
		htup->t_infomask &= ~(HEAP_XMAX_COMMITTED |
							  HEAP_XMAX_INVALID |
							  HEAP_MARKED_FOR_UPDATE |
							  HEAP_MOVED);
		HeapTupleHeaderSetXmax(htup, record->xl_xid);
		HeapTupleHeaderSetCmax(htup, FirstCommandId);
		/* Make sure there is no forward chain link in t_ctid */
		htup->t_ctid = xlrec->target.tid;
		PageSetLSN(page, lsn);
		PageSetSUI(page, ThisStartUpID);
		UnlockAndWriteBuffer(buffer);
		return;
	}

	elog(PANIC, "heap_delete_undo: unimplemented");
}

static void
heap_xlog_insert(bool redo, XLogRecPtr lsn, XLogRecord *record)
{
	xl_heap_insert *xlrec = (xl_heap_insert *) XLogRecGetData(record);
	Relation	reln;
	Buffer		buffer;
	Page		page;
	OffsetNumber offnum;

	if (redo && (record->xl_info & XLR_BKP_BLOCK_1))
		return;

	reln = XLogOpenRelation(redo, RM_HEAP_ID, xlrec->target.node);

	if (!RelationIsValid(reln))
		return;

	buffer = XLogReadBuffer((redo) ? true : false, reln,
						ItemPointerGetBlockNumber(&(xlrec->target.tid)));
	if (!BufferIsValid(buffer))
		return;

	page = (Page) BufferGetPage(buffer);
	if (PageIsNew((PageHeader) page) &&
		(!redo || !(record->xl_info & XLOG_HEAP_INIT_PAGE)))
		elog(PANIC, "heap_insert_%sdo: uninitialized page", (redo) ? "re" : "un");

	if (redo)
	{
		struct
		{
			HeapTupleHeaderData hdr;
			char		data[MaxTupleSize];
		}			tbuf;
		HeapTupleHeader htup;
		xl_heap_header xlhdr;
		uint32		newlen;

		if (record->xl_info & XLOG_HEAP_INIT_PAGE)
			PageInit(page, BufferGetPageSize(buffer), 0);

		if (XLByteLE(lsn, PageGetLSN(page)))	/* changes are applied */
		{
			UnlockAndReleaseBuffer(buffer);
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
		PageSetSUI(page, ThisStartUpID);		/* prev sui */
		UnlockAndWriteBuffer(buffer);
		return;
	}

	/* undo insert */
	if (XLByteLT(PageGetLSN(page), lsn))		/* changes are not applied
												 * ?! */
		elog(PANIC, "heap_insert_undo: bad page LSN");

	elog(PANIC, "heap_insert_undo: unimplemented");
}

/*
 * Handles UPDATE & MOVE
 */
static void
heap_xlog_update(bool redo, XLogRecPtr lsn, XLogRecord *record, bool move)
{
	xl_heap_update *xlrec = (xl_heap_update *) XLogRecGetData(record);
	Relation	reln = XLogOpenRelation(redo, RM_HEAP_ID, xlrec->target.node);
	Buffer		buffer;
	bool		samepage =
	(ItemPointerGetBlockNumber(&(xlrec->newtid)) ==
	 ItemPointerGetBlockNumber(&(xlrec->target.tid)));
	Page		page;
	OffsetNumber offnum;
	ItemId		lp = NULL;
	HeapTupleHeader htup;

	if (!RelationIsValid(reln))
		return;

	if (redo && (record->xl_info & XLR_BKP_BLOCK_1))
		goto newt;

	/* Deal with old tuple version */

	buffer = XLogReadBuffer(false, reln,
						ItemPointerGetBlockNumber(&(xlrec->target.tid)));
	if (!BufferIsValid(buffer))
		elog(PANIC, "heap_update_%sdo: no block", (redo) ? "re" : "un");

	page = (Page) BufferGetPage(buffer);
	if (PageIsNew((PageHeader) page))
		elog(PANIC, "heap_update_%sdo: uninitialized old page", (redo) ? "re" : "un");

	if (redo)
	{
		if (XLByteLE(lsn, PageGetLSN(page)))	/* changes are applied */
		{
			UnlockAndReleaseBuffer(buffer);
			if (samepage)
				return;
			goto newt;
		}
	}
	else if (XLByteLT(PageGetLSN(page), lsn))	/* changes are not applied
												 * ?! */
		elog(PANIC, "heap_update_undo: bad old tuple page LSN");

	offnum = ItemPointerGetOffsetNumber(&(xlrec->target.tid));
	if (PageGetMaxOffsetNumber(page) >= offnum)
		lp = PageGetItemId(page, offnum);

	if (PageGetMaxOffsetNumber(page) < offnum || !ItemIdIsUsed(lp))
		elog(PANIC, "heap_update_%sdo: invalid lp", (redo) ? "re" : "un");

	htup = (HeapTupleHeader) PageGetItem(page, lp);

	if (redo)
	{
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
								  HEAP_MARKED_FOR_UPDATE |
								  HEAP_MOVED);
			HeapTupleHeaderSetXmax(htup, record->xl_xid);
			HeapTupleHeaderSetCmax(htup, FirstCommandId);
			/* Set forward chain link in t_ctid */
			htup->t_ctid = xlrec->newtid;
		}
		if (samepage)
			goto newsame;
		PageSetLSN(page, lsn);
		PageSetSUI(page, ThisStartUpID);
		UnlockAndWriteBuffer(buffer);
		goto newt;
	}

	elog(PANIC, "heap_update_undo: unimplemented");

	/* Deal with new tuple */

newt:;

	if (redo &&
		((record->xl_info & XLR_BKP_BLOCK_2) ||
		 ((record->xl_info & XLR_BKP_BLOCK_1) && samepage)))
		return;

	buffer = XLogReadBuffer((redo) ? true : false, reln,
							ItemPointerGetBlockNumber(&(xlrec->newtid)));
	if (!BufferIsValid(buffer))
		return;

	page = (Page) BufferGetPage(buffer);

newsame:;
	if (PageIsNew((PageHeader) page) &&
		(!redo || !(record->xl_info & XLOG_HEAP_INIT_PAGE)))
		elog(PANIC, "heap_update_%sdo: uninitialized page", (redo) ? "re" : "un");

	if (redo)
	{
		struct
		{
			HeapTupleHeaderData hdr;
			char		data[MaxTupleSize];
		}			tbuf;
		xl_heap_header xlhdr;
		int			hsize;
		uint32		newlen;

		if (record->xl_info & XLOG_HEAP_INIT_PAGE)
			PageInit(page, BufferGetPageSize(buffer), 0);

		if (XLByteLE(lsn, PageGetLSN(page)))	/* changes are applied */
		{
			UnlockAndReleaseBuffer(buffer);
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
			TransactionId xid[2];		/* xmax, xmin */

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
		PageSetSUI(page, ThisStartUpID);		/* prev sui */
		UnlockAndWriteBuffer(buffer);
		return;
	}

	/* undo */
	if (XLByteLT(PageGetLSN(page), lsn))		/* changes not applied?! */
		elog(PANIC, "heap_update_undo: bad new tuple page LSN");

	elog(PANIC, "heap_update_undo: unimplemented");

}

static void
_heap_unlock_tuple(void *data)
{
	xl_heaptid *xltid = (xl_heaptid *) data;
	Relation	reln = XLogOpenRelation(false, RM_HEAP_ID, xltid->node);
	Buffer		buffer;
	Page		page;
	OffsetNumber offnum;
	ItemId		lp;
	HeapTupleHeader htup;

	if (!RelationIsValid(reln))
		elog(PANIC, "_heap_unlock_tuple: can't open relation");

	buffer = XLogReadBuffer(false, reln,
							ItemPointerGetBlockNumber(&(xltid->tid)));
	if (!BufferIsValid(buffer))
		elog(PANIC, "_heap_unlock_tuple: can't read buffer");

	page = (Page) BufferGetPage(buffer);
	if (PageIsNew((PageHeader) page))
		elog(PANIC, "_heap_unlock_tuple: uninitialized page");

	offnum = ItemPointerGetOffsetNumber(&(xltid->tid));
	if (offnum > PageGetMaxOffsetNumber(page))
		elog(PANIC, "_heap_unlock_tuple: invalid itemid");
	lp = PageGetItemId(page, offnum);

	if (!ItemIdIsUsed(lp) || ItemIdDeleted(lp))
		elog(PANIC, "_heap_unlock_tuple: unused/deleted tuple in rollback");

	htup = (HeapTupleHeader) PageGetItem(page, lp);

	if (!TransactionIdEquals(HeapTupleHeaderGetXmax(htup), GetCurrentTransactionId()))
		elog(PANIC, "_heap_unlock_tuple: invalid xmax in rollback");
	htup->t_infomask &= ~HEAP_XMAX_UNLOGGED;
	htup->t_infomask |= HEAP_XMAX_INVALID;
	UnlockAndWriteBuffer(buffer);
	return;
}

void
heap_redo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;

	info &= XLOG_HEAP_OPMASK;
	if (info == XLOG_HEAP_INSERT)
		heap_xlog_insert(true, lsn, record);
	else if (info == XLOG_HEAP_DELETE)
		heap_xlog_delete(true, lsn, record);
	else if (info == XLOG_HEAP_UPDATE)
		heap_xlog_update(true, lsn, record, false);
	else if (info == XLOG_HEAP_MOVE)
		heap_xlog_update(true, lsn, record, true);
	else if (info == XLOG_HEAP_CLEAN)
		heap_xlog_clean(true, lsn, record);
	else
		elog(PANIC, "heap_redo: unknown op code %u", info);
}

void
heap_undo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;

	info &= XLOG_HEAP_OPMASK;
	if (info == XLOG_HEAP_INSERT)
		heap_xlog_insert(false, lsn, record);
	else if (info == XLOG_HEAP_DELETE)
		heap_xlog_delete(false, lsn, record);
	else if (info == XLOG_HEAP_UPDATE)
		heap_xlog_update(false, lsn, record, false);
	else if (info == XLOG_HEAP_MOVE)
		heap_xlog_update(false, lsn, record, true);
	else if (info == XLOG_HEAP_CLEAN)
		heap_xlog_clean(false, lsn, record);
	else
		elog(PANIC, "heap_undo: unknown op code %u", info);
}

static void
out_target(char *buf, xl_heaptid *target)
{
	sprintf(buf + strlen(buf), "node %u/%u; tid %u/%u",
			target->node.tblNode, target->node.relNode,
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

		sprintf(buf + strlen(buf), "clean: node %u/%u; blk %u",
				xlrec->node.tblNode, xlrec->node.relNode, xlrec->block);
	}
	else
		strcat(buf, "UNKNOWN");
}
