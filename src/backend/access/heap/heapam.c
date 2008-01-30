/*-------------------------------------------------------------------------
 *
 * heapam.c
 *	  heap access method code
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/heap/heapam.c,v 1.249 2008/01/30 18:35:55 tgl Exp $
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
 *		heap_sync		- sync heap, for when no WAL has been written
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
#include "access/transam.h"
#include "access/tuptoaster.h"
#include "access/valid.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "utils/datum.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/relcache.h"
#include "utils/syscache.h"


/* GUC variable */
bool	synchronize_seqscans = true;


static HeapScanDesc heap_beginscan_internal(Relation relation,
						Snapshot snapshot,
						int nkeys, ScanKey key,
						bool allow_strat, bool allow_sync,
						bool is_bitmapscan);
static XLogRecPtr log_heap_update(Relation reln, Buffer oldbuf,
		   ItemPointerData from, Buffer newbuf, HeapTuple newtup, bool move);
static bool HeapSatisfiesHOTUpdate(Relation relation, Bitmapset *hot_attrs,
					   HeapTuple oldtup, HeapTuple newtup);


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
	bool		allow_strat;
	bool		allow_sync;

	/*
	 * Determine the number of blocks we have to scan.
	 *
	 * It is sufficient to do this once at scan start, since any tuples added
	 * while the scan is in progress will be invisible to my snapshot anyway.
	 * (That is not true when using a non-MVCC snapshot.  However, we couldn't
	 * guarantee to return tuples added after scan start anyway, since they
	 * might go into pages we already scanned.	To guarantee consistent
	 * results for a non-MVCC snapshot, the caller must hold some higher-level
	 * lock that ensures the interesting tuple(s) won't change.)
	 */
	scan->rs_nblocks = RelationGetNumberOfBlocks(scan->rs_rd);

	/*
	 * If the table is large relative to NBuffers, use a bulk-read access
	 * strategy and enable synchronized scanning (see syncscan.c).	Although
	 * the thresholds for these features could be different, we make them the
	 * same so that there are only two behaviors to tune rather than four.
	 * (However, some callers need to be able to disable one or both of
	 * these behaviors, independently of the size of the table; also there
	 * is a GUC variable that can disable synchronized scanning.)
	 *
	 * During a rescan, don't make a new strategy object if we don't have to.
	 */
	if (!scan->rs_rd->rd_istemp &&
		scan->rs_nblocks > NBuffers / 4)
	{
		allow_strat = scan->rs_allow_strat;
		allow_sync = scan->rs_allow_sync;
	}
	else
		allow_strat = allow_sync = false;

	if (allow_strat)
	{
		if (scan->rs_strategy == NULL)
			scan->rs_strategy = GetAccessStrategy(BAS_BULKREAD);
	}
	else
	{
		if (scan->rs_strategy != NULL)
			FreeAccessStrategy(scan->rs_strategy);
		scan->rs_strategy = NULL;
	}

	if (allow_sync && synchronize_seqscans)
	{
		scan->rs_syncscan = true;
		scan->rs_startblock = ss_get_location(scan->rs_rd, scan->rs_nblocks);
	}
	else
	{
		scan->rs_syncscan = false;
		scan->rs_startblock = 0;
	}

	scan->rs_inited = false;
	scan->rs_ctup.t_data = NULL;
	ItemPointerSetInvalid(&scan->rs_ctup.t_self);
	scan->rs_cbuf = InvalidBuffer;
	scan->rs_cblock = InvalidBlockNumber;

	/* we don't have a marked position... */
	ItemPointerSetInvalid(&(scan->rs_mctid));

	/* page-at-a-time fields are always invalid when not rs_inited */

	/*
	 * copy the scan key, if appropriate
	 */
	if (key != NULL)
		memcpy(scan->rs_key, key, scan->rs_nkeys * sizeof(ScanKeyData));

	/*
	 * Currently, we don't have a stats counter for bitmap heap scans (but the
	 * underlying bitmap index scans will be counted).
	 */
	if (!scan->rs_bitmapscan)
		pgstat_count_heap_scan(scan->rs_rd);
}

/*
 * heapgetpage - subroutine for heapgettup()
 *
 * This routine reads and pins the specified page of the relation.
 * In page-at-a-time mode it performs additional work, namely determining
 * which tuples on the page are visible.
 */
static void
heapgetpage(HeapScanDesc scan, BlockNumber page)
{
	Buffer		buffer;
	Snapshot	snapshot;
	Page		dp;
	int			lines;
	int			ntup;
	OffsetNumber lineoff;
	ItemId		lpp;

	Assert(page < scan->rs_nblocks);

	/* release previous scan buffer, if any */
	if (BufferIsValid(scan->rs_cbuf))
	{
		ReleaseBuffer(scan->rs_cbuf);
		scan->rs_cbuf = InvalidBuffer;
	}

	/* read page using selected strategy */
	scan->rs_cbuf = ReadBufferWithStrategy(scan->rs_rd,
										   page,
										   scan->rs_strategy);
	scan->rs_cblock = page;

	if (!scan->rs_pageatatime)
		return;

	buffer = scan->rs_cbuf;
	snapshot = scan->rs_snapshot;

	/*
	 * Prune and repair fragmentation for the whole page, if possible.
	 */
	heap_page_prune_opt(scan->rs_rd, buffer, RecentGlobalXmin);

	/*
	 * We must hold share lock on the buffer content while examining tuple
	 * visibility.	Afterwards, however, the tuples we have found to be
	 * visible are guaranteed good as long as we hold the buffer pin.
	 */
	LockBuffer(buffer, BUFFER_LOCK_SHARE);

	dp = (Page) BufferGetPage(buffer);
	lines = PageGetMaxOffsetNumber(dp);
	ntup = 0;

	for (lineoff = FirstOffsetNumber, lpp = PageGetItemId(dp, lineoff);
		 lineoff <= lines;
		 lineoff++, lpp++)
	{
		if (ItemIdIsNormal(lpp))
		{
			HeapTupleData loctup;
			bool		valid;

			loctup.t_data = (HeapTupleHeader) PageGetItem((Page) dp, lpp);
			loctup.t_len = ItemIdGetLength(lpp);
			ItemPointerSet(&(loctup.t_self), page, lineoff);

			valid = HeapTupleSatisfiesVisibility(&loctup, snapshot, buffer);
			if (valid)
				scan->rs_vistuples[ntup++] = lineoff;
		}
	}

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

	Assert(ntup <= MaxHeapTuplesPerPage);
	scan->rs_ntuples = ntup;
}

/* ----------------
 *		heapgettup - fetch next heap tuple
 *
 *		Initialize the scan if not already done; then advance to the next
 *		tuple as indicated by "dir"; return the next tuple in scan->rs_ctup,
 *		or set scan->rs_ctup.t_data = NULL if no more tuples.
 *
 * dir == NoMovementScanDirection means "re-fetch the tuple indicated
 * by scan->rs_ctup".
 *
 * Note: the reason nkeys/key are passed separately, even though they are
 * kept in the scan descriptor, is that the caller may not want us to check
 * the scankeys.
 *
 * Note: when we fall off the end of the scan in either direction, we
 * reset rs_inited.  This means that a further request with the same
 * scan direction will restart the scan, which is a bit odd, but a
 * request with the opposite scan direction will start a fresh scan
 * in the proper direction.  The latter is required behavior for cursors,
 * while the former case is generally undefined behavior in Postgres
 * so we don't care too much.
 * ----------------
 */
static void
heapgettup(HeapScanDesc scan,
		   ScanDirection dir,
		   int nkeys,
		   ScanKey key)
{
	HeapTuple	tuple = &(scan->rs_ctup);
	Snapshot	snapshot = scan->rs_snapshot;
	bool		backward = ScanDirectionIsBackward(dir);
	BlockNumber page;
	bool		finished;
	Page		dp;
	int			lines;
	OffsetNumber lineoff;
	int			linesleft;
	ItemId		lpp;

	/*
	 * calculate next starting lineoff, given scan direction
	 */
	if (ScanDirectionIsForward(dir))
	{
		if (!scan->rs_inited)
		{
			/*
			 * return null immediately if relation is empty
			 */
			if (scan->rs_nblocks == 0)
			{
				Assert(!BufferIsValid(scan->rs_cbuf));
				tuple->t_data = NULL;
				return;
			}
			page = scan->rs_startblock; /* first page */
			heapgetpage(scan, page);
			lineoff = FirstOffsetNumber;		/* first offnum */
			scan->rs_inited = true;
		}
		else
		{
			/* continue from previously returned page/tuple */
			page = scan->rs_cblock;		/* current page */
			lineoff =			/* next offnum */
				OffsetNumberNext(ItemPointerGetOffsetNumber(&(tuple->t_self)));
		}

		LockBuffer(scan->rs_cbuf, BUFFER_LOCK_SHARE);

		dp = (Page) BufferGetPage(scan->rs_cbuf);
		lines = PageGetMaxOffsetNumber(dp);
		/* page and lineoff now reference the physically next tid */

		linesleft = lines - lineoff + 1;
	}
	else if (backward)
	{
		if (!scan->rs_inited)
		{
			/*
			 * return null immediately if relation is empty
			 */
			if (scan->rs_nblocks == 0)
			{
				Assert(!BufferIsValid(scan->rs_cbuf));
				tuple->t_data = NULL;
				return;
			}

			/*
			 * Disable reporting to syncscan logic in a backwards scan; it's
			 * not very likely anyone else is doing the same thing at the same
			 * time, and much more likely that we'll just bollix things for
			 * forward scanners.
			 */
			scan->rs_syncscan = false;
			/* start from last page of the scan */
			if (scan->rs_startblock > 0)
				page = scan->rs_startblock - 1;
			else
				page = scan->rs_nblocks - 1;
			heapgetpage(scan, page);
		}
		else
		{
			/* continue from previously returned page/tuple */
			page = scan->rs_cblock;		/* current page */
		}

		LockBuffer(scan->rs_cbuf, BUFFER_LOCK_SHARE);

		dp = (Page) BufferGetPage(scan->rs_cbuf);
		lines = PageGetMaxOffsetNumber(dp);

		if (!scan->rs_inited)
		{
			lineoff = lines;	/* final offnum */
			scan->rs_inited = true;
		}
		else
		{
			lineoff =			/* previous offnum */
				OffsetNumberPrev(ItemPointerGetOffsetNumber(&(tuple->t_self)));
		}
		/* page and lineoff now reference the physically previous tid */

		linesleft = lineoff;
	}
	else
	{
		/*
		 * ``no movement'' scan direction: refetch prior tuple
		 */
		if (!scan->rs_inited)
		{
			Assert(!BufferIsValid(scan->rs_cbuf));
			tuple->t_data = NULL;
			return;
		}

		page = ItemPointerGetBlockNumber(&(tuple->t_self));
		if (page != scan->rs_cblock)
			heapgetpage(scan, page);

		/* Since the tuple was previously fetched, needn't lock page here */
		dp = (Page) BufferGetPage(scan->rs_cbuf);
		lineoff = ItemPointerGetOffsetNumber(&(tuple->t_self));
		lpp = PageGetItemId(dp, lineoff);
		Assert(ItemIdIsNormal(lpp));

		tuple->t_data = (HeapTupleHeader) PageGetItem((Page) dp, lpp);
		tuple->t_len = ItemIdGetLength(lpp);

		return;
	}

	/*
	 * advance the scan until we find a qualifying tuple or run out of stuff
	 * to scan
	 */
	lpp = PageGetItemId(dp, lineoff);
	for (;;)
	{
		while (linesleft > 0)
		{
			if (ItemIdIsNormal(lpp))
			{
				bool		valid;

				tuple->t_data = (HeapTupleHeader) PageGetItem((Page) dp, lpp);
				tuple->t_len = ItemIdGetLength(lpp);
				ItemPointerSet(&(tuple->t_self), page, lineoff);

				/*
				 * if current tuple qualifies, return it.
				 */
				valid = HeapTupleSatisfiesVisibility(tuple,
													 snapshot,
													 scan->rs_cbuf);

				if (valid && key != NULL)
					HeapKeyTest(tuple, RelationGetDescr(scan->rs_rd),
								nkeys, key, valid);

				if (valid)
				{
					LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);
					return;
				}
			}

			/*
			 * otherwise move to the next item on the page
			 */
			--linesleft;
			if (backward)
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
		LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);

		/*
		 * advance to next/prior page and detect end of scan
		 */
		if (backward)
		{
			finished = (page == scan->rs_startblock);
			if (page == 0)
				page = scan->rs_nblocks;
			page--;
		}
		else
		{
			page++;
			if (page >= scan->rs_nblocks)
				page = 0;
			finished = (page == scan->rs_startblock);

			/*
			 * Report our new scan position for synchronization purposes. We
			 * don't do that when moving backwards, however. That would just
			 * mess up any other forward-moving scanners.
			 *
			 * Note: we do this before checking for end of scan so that the
			 * final state of the position hint is back at the start of the
			 * rel.  That's not strictly necessary, but otherwise when you run
			 * the same query multiple times the starting position would shift
			 * a little bit backwards on every invocation, which is confusing.
			 * We don't guarantee any specific ordering in general, though.
			 */
			if (scan->rs_syncscan)
				ss_report_location(scan->rs_rd, page);
		}

		/*
		 * return NULL if we've exhausted all the pages
		 */
		if (finished)
		{
			if (BufferIsValid(scan->rs_cbuf))
				ReleaseBuffer(scan->rs_cbuf);
			scan->rs_cbuf = InvalidBuffer;
			scan->rs_cblock = InvalidBlockNumber;
			tuple->t_data = NULL;
			scan->rs_inited = false;
			return;
		}

		heapgetpage(scan, page);

		LockBuffer(scan->rs_cbuf, BUFFER_LOCK_SHARE);

		dp = (Page) BufferGetPage(scan->rs_cbuf);
		lines = PageGetMaxOffsetNumber((Page) dp);
		linesleft = lines;
		if (backward)
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

/* ----------------
 *		heapgettup_pagemode - fetch next heap tuple in page-at-a-time mode
 *
 *		Same API as heapgettup, but used in page-at-a-time mode
 *
 * The internal logic is much the same as heapgettup's too, but there are some
 * differences: we do not take the buffer content lock (that only needs to
 * happen inside heapgetpage), and we iterate through just the tuples listed
 * in rs_vistuples[] rather than all tuples on the page.  Notice that
 * lineindex is 0-based, where the corresponding loop variable lineoff in
 * heapgettup is 1-based.
 * ----------------
 */
static void
heapgettup_pagemode(HeapScanDesc scan,
					ScanDirection dir,
					int nkeys,
					ScanKey key)
{
	HeapTuple	tuple = &(scan->rs_ctup);
	bool		backward = ScanDirectionIsBackward(dir);
	BlockNumber page;
	bool		finished;
	Page		dp;
	int			lines;
	int			lineindex;
	OffsetNumber lineoff;
	int			linesleft;
	ItemId		lpp;

	/*
	 * calculate next starting lineindex, given scan direction
	 */
	if (ScanDirectionIsForward(dir))
	{
		if (!scan->rs_inited)
		{
			/*
			 * return null immediately if relation is empty
			 */
			if (scan->rs_nblocks == 0)
			{
				Assert(!BufferIsValid(scan->rs_cbuf));
				tuple->t_data = NULL;
				return;
			}
			page = scan->rs_startblock; /* first page */
			heapgetpage(scan, page);
			lineindex = 0;
			scan->rs_inited = true;
		}
		else
		{
			/* continue from previously returned page/tuple */
			page = scan->rs_cblock;		/* current page */
			lineindex = scan->rs_cindex + 1;
		}

		dp = (Page) BufferGetPage(scan->rs_cbuf);
		lines = scan->rs_ntuples;
		/* page and lineindex now reference the next visible tid */

		linesleft = lines - lineindex;
	}
	else if (backward)
	{
		if (!scan->rs_inited)
		{
			/*
			 * return null immediately if relation is empty
			 */
			if (scan->rs_nblocks == 0)
			{
				Assert(!BufferIsValid(scan->rs_cbuf));
				tuple->t_data = NULL;
				return;
			}

			/*
			 * Disable reporting to syncscan logic in a backwards scan; it's
			 * not very likely anyone else is doing the same thing at the same
			 * time, and much more likely that we'll just bollix things for
			 * forward scanners.
			 */
			scan->rs_syncscan = false;
			/* start from last page of the scan */
			if (scan->rs_startblock > 0)
				page = scan->rs_startblock - 1;
			else
				page = scan->rs_nblocks - 1;
			heapgetpage(scan, page);
		}
		else
		{
			/* continue from previously returned page/tuple */
			page = scan->rs_cblock;		/* current page */
		}

		dp = (Page) BufferGetPage(scan->rs_cbuf);
		lines = scan->rs_ntuples;

		if (!scan->rs_inited)
		{
			lineindex = lines - 1;
			scan->rs_inited = true;
		}
		else
		{
			lineindex = scan->rs_cindex - 1;
		}
		/* page and lineindex now reference the previous visible tid */

		linesleft = lineindex + 1;
	}
	else
	{
		/*
		 * ``no movement'' scan direction: refetch prior tuple
		 */
		if (!scan->rs_inited)
		{
			Assert(!BufferIsValid(scan->rs_cbuf));
			tuple->t_data = NULL;
			return;
		}

		page = ItemPointerGetBlockNumber(&(tuple->t_self));
		if (page != scan->rs_cblock)
			heapgetpage(scan, page);

		/* Since the tuple was previously fetched, needn't lock page here */
		dp = (Page) BufferGetPage(scan->rs_cbuf);
		lineoff = ItemPointerGetOffsetNumber(&(tuple->t_self));
		lpp = PageGetItemId(dp, lineoff);
		Assert(ItemIdIsNormal(lpp));

		tuple->t_data = (HeapTupleHeader) PageGetItem((Page) dp, lpp);
		tuple->t_len = ItemIdGetLength(lpp);

		/* check that rs_cindex is in sync */
		Assert(scan->rs_cindex < scan->rs_ntuples);
		Assert(lineoff == scan->rs_vistuples[scan->rs_cindex]);

		return;
	}

	/*
	 * advance the scan until we find a qualifying tuple or run out of stuff
	 * to scan
	 */
	for (;;)
	{
		while (linesleft > 0)
		{
			lineoff = scan->rs_vistuples[lineindex];
			lpp = PageGetItemId(dp, lineoff);
			Assert(ItemIdIsNormal(lpp));

			tuple->t_data = (HeapTupleHeader) PageGetItem((Page) dp, lpp);
			tuple->t_len = ItemIdGetLength(lpp);
			ItemPointerSet(&(tuple->t_self), page, lineoff);

			/*
			 * if current tuple qualifies, return it.
			 */
			if (key != NULL)
			{
				bool		valid;

				HeapKeyTest(tuple, RelationGetDescr(scan->rs_rd),
							nkeys, key, valid);
				if (valid)
				{
					scan->rs_cindex = lineindex;
					return;
				}
			}
			else
			{
				scan->rs_cindex = lineindex;
				return;
			}

			/*
			 * otherwise move to the next item on the page
			 */
			--linesleft;
			if (backward)
				--lineindex;
			else
				++lineindex;
		}

		/*
		 * if we get here, it means we've exhausted the items on this page and
		 * it's time to move to the next.
		 */
		if (backward)
		{
			finished = (page == scan->rs_startblock);
			if (page == 0)
				page = scan->rs_nblocks;
			page--;
		}
		else
		{
			page++;
			if (page >= scan->rs_nblocks)
				page = 0;
			finished = (page == scan->rs_startblock);

			/*
			 * Report our new scan position for synchronization purposes. We
			 * don't do that when moving backwards, however. That would just
			 * mess up any other forward-moving scanners.
			 *
			 * Note: we do this before checking for end of scan so that the
			 * final state of the position hint is back at the start of the
			 * rel.  That's not strictly necessary, but otherwise when you run
			 * the same query multiple times the starting position would shift
			 * a little bit backwards on every invocation, which is confusing.
			 * We don't guarantee any specific ordering in general, though.
			 */
			if (scan->rs_syncscan)
				ss_report_location(scan->rs_rd, page);
		}

		/*
		 * return NULL if we've exhausted all the pages
		 */
		if (finished)
		{
			if (BufferIsValid(scan->rs_cbuf))
				ReleaseBuffer(scan->rs_cbuf);
			scan->rs_cbuf = InvalidBuffer;
			scan->rs_cblock = InvalidBlockNumber;
			tuple->t_data = NULL;
			scan->rs_inited = false;
			return;
		}

		heapgetpage(scan, page);

		dp = (Page) BufferGetPage(scan->rs_cbuf);
		lines = scan->rs_ntuples;
		linesleft = lines;
		if (backward)
			lineindex = lines - 1;
		else
			lineindex = 0;
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

	/* Get the lock before trying to open the relcache entry */
	if (lockmode != NoLock)
		LockRelationOid(relationId, lockmode);

	/* The relcache does all the real work... */
	r = RelationIdGetRelation(relationId);

	if (!RelationIsValid(r))
		elog(ERROR, "could not open relation with OID %u", relationId);

	pgstat_initstats(r);

	return r;
}

/* ----------------
 *		try_relation_open - open any relation by relation OID
 *
 *		Same as relation_open, except return NULL instead of failing
 *		if the relation does not exist.
 * ----------------
 */
Relation
try_relation_open(Oid relationId, LOCKMODE lockmode)
{
	Relation	r;

	Assert(lockmode >= NoLock && lockmode < MAX_LOCKMODES);

	/* Get the lock first */
	if (lockmode != NoLock)
		LockRelationOid(relationId, lockmode);

	/*
	 * Now that we have the lock, probe to see if the relation really exists
	 * or not.
	 */
	if (!SearchSysCacheExists(RELOID,
							  ObjectIdGetDatum(relationId),
							  0, 0, 0))
	{
		/* Release useless lock */
		if (lockmode != NoLock)
			UnlockRelationOid(relationId, lockmode);

		return NULL;
	}

	/* Should be safe to do a relcache load */
	r = RelationIdGetRelation(relationId);

	if (!RelationIsValid(r))
		elog(ERROR, "could not open relation with OID %u", relationId);

	pgstat_initstats(r);

	return r;
}

/* ----------------
 *		relation_open_nowait - open but don't wait for lock
 *
 *		Same as relation_open, except throw an error instead of waiting
 *		when the requested lock is not immediately obtainable.
 * ----------------
 */
Relation
relation_open_nowait(Oid relationId, LOCKMODE lockmode)
{
	Relation	r;

	Assert(lockmode >= NoLock && lockmode < MAX_LOCKMODES);

	/* Get the lock before trying to open the relcache entry */
	if (lockmode != NoLock)
	{
		if (!ConditionalLockRelationOid(relationId, lockmode))
		{
			/* try to throw error by name; relation could be deleted... */
			char	   *relname = get_rel_name(relationId);

			if (relname)
				ereport(ERROR,
						(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
						 errmsg("could not obtain lock on relation \"%s\"",
								relname)));
			else
				ereport(ERROR,
						(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
					  errmsg("could not obtain lock on relation with OID %u",
							 relationId)));
		}
	}

	/* The relcache does all the real work... */
	r = RelationIdGetRelation(relationId);

	if (!RelationIsValid(r))
		elog(ERROR, "could not open relation with OID %u", relationId);

	pgstat_initstats(r);

	return r;
}

/* ----------------
 *		relation_openrv - open any relation specified by a RangeVar
 *
 *		Same as relation_open, but the relation is specified by a RangeVar.
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
	 * onto that entry and suffer an error when we do RelationIdGetRelation.
	 * Note that relation_open does not need to do this, since a relation's
	 * OID never changes.
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
 *		If lockmode is not "NoLock", we then release the specified lock.
 *
 *		Note that it is often sensible to hold a lock beyond relation_close;
 *		in that case, the lock is released automatically at xact end.
 * ----------------
 */
void
relation_close(Relation relation, LOCKMODE lockmode)
{
	LockRelId	relid = relation->rd_lockInfo.lockRelId;

	Assert(lockmode >= NoLock && lockmode < MAX_LOCKMODES);

	/* The relcache does the real work... */
	RelationClose(relation);

	if (lockmode != NoLock)
		UnlockRelationId(&relid, lockmode);
}


/* ----------------
 *		heap_open - open a heap relation by relation OID
 *
 *		This is essentially relation_open plus check that the relation
 *		is not an index nor a composite type.  (The caller should also
 *		check that it's not a view before assuming it has storage.)
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
	else if (r->rd_rel->relkind == RELKIND_COMPOSITE_TYPE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is a composite type",
						RelationGetRelationName(r))));

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
	else if (r->rd_rel->relkind == RELKIND_COMPOSITE_TYPE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is a composite type",
						RelationGetRelationName(r))));

	return r;
}


/* ----------------
 *		heap_beginscan	- begin relation scan
 *
 * heap_beginscan_strat offers an extended API that lets the caller control
 * whether a nondefault buffer access strategy can be used, and whether
 * syncscan can be chosen (possibly resulting in the scan not starting from
 * block zero).  Both of these default to TRUE with plain heap_beginscan.
 *
 * heap_beginscan_bm is an alternative entry point for setting up a
 * HeapScanDesc for a bitmap heap scan.  Although that scan technology is
 * really quite unlike a standard seqscan, there is just enough commonality
 * to make it worth using the same data structure.
 * ----------------
 */
HeapScanDesc
heap_beginscan(Relation relation, Snapshot snapshot,
			   int nkeys, ScanKey key)
{
	return heap_beginscan_internal(relation, snapshot, nkeys, key,
								   true, true, false);
}

HeapScanDesc
heap_beginscan_strat(Relation relation, Snapshot snapshot,
					 int nkeys, ScanKey key,
					 bool allow_strat, bool allow_sync)
{
	return heap_beginscan_internal(relation, snapshot, nkeys, key,
								   allow_strat, allow_sync, false);
}

HeapScanDesc
heap_beginscan_bm(Relation relation, Snapshot snapshot,
				  int nkeys, ScanKey key)
{
	return heap_beginscan_internal(relation, snapshot, nkeys, key,
								   false, false, true);
}

static HeapScanDesc
heap_beginscan_internal(Relation relation, Snapshot snapshot,
						int nkeys, ScanKey key,
						bool allow_strat, bool allow_sync,
						bool is_bitmapscan)
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
	scan->rs_bitmapscan = is_bitmapscan;
	scan->rs_strategy = NULL;	/* set in initscan */
	scan->rs_allow_strat = allow_strat;
	scan->rs_allow_sync = allow_sync;

	/*
	 * we can use page-at-a-time mode if it's an MVCC-safe snapshot
	 */
	scan->rs_pageatatime = IsMVCCSnapshot(snapshot);

	/* we only need to set this up once */
	scan->rs_ctup.t_tableOid = RelationGetRelid(relation);

	/*
	 * we do this here instead of in initscan() because heap_rescan also calls
	 * initscan() and we don't want to allocate memory again
	 */
	if (nkeys > 0)
		scan->rs_key = (ScanKey) palloc(sizeof(ScanKeyData) * nkeys);
	else
		scan->rs_key = NULL;

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

	if (scan->rs_strategy != NULL)
		FreeAccessStrategy(scan->rs_strategy);

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

	if (scan->rs_pageatatime)
		heapgettup_pagemode(scan, direction,
							scan->rs_nkeys, scan->rs_key);
	else
		heapgettup(scan, direction, scan->rs_nkeys, scan->rs_key);

	if (scan->rs_ctup.t_data == NULL)
	{
		HEAPDEBUG_2;			/* heap_getnext returning EOS */
		return NULL;
	}

	/*
	 * if we get here it means we have a new current scan tuple, so point to
	 * the proper return buffer and return the tuple.
	 */
	HEAPDEBUG_3;				/* heap_getnext returning tuple */

	pgstat_count_heap_getnext(scan->rs_rd);

	return &(scan->rs_ctup);
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
 * stats_relation is the relation to charge the heap_fetch operation against
 * for statistical purposes.  (This could be the heap rel itself, an
 * associated index, or NULL to not count the fetch at all.)
 *
 * heap_fetch does not follow HOT chains: only the exact TID requested will
 * be fetched.
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
		   Relation stats_relation)
{
	/* Assume *userbuf is undefined on entry */
	*userbuf = InvalidBuffer;
	return heap_release_fetch(relation, snapshot, tuple,
							  userbuf, keep_buf, stats_relation);
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
				   Relation stats_relation)
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
	if (!ItemIdIsNormal(lp))
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		if (keep_buf)
			*userbuf = buffer;
		else
		{
			ReleaseBuffer(buffer);
			*userbuf = InvalidBuffer;
		}
		tuple->t_data = NULL;
		return false;
	}

	/*
	 * fill in *tuple fields
	 */
	tuple->t_data = (HeapTupleHeader) PageGetItem((Page) dp, lp);
	tuple->t_len = ItemIdGetLength(lp);
	tuple->t_tableOid = RelationGetRelid(relation);

	/*
	 * check time qualification of tuple, then release lock
	 */
	valid = HeapTupleSatisfiesVisibility(tuple, snapshot, buffer);

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

	if (valid)
	{
		/*
		 * All checks passed, so return the tuple as valid. Caller is now
		 * responsible for releasing the buffer.
		 */
		*userbuf = buffer;

		/* Count the successful fetch against appropriate rel, if any */
		if (stats_relation != NULL)
			pgstat_count_heap_fetch(stats_relation);

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
 *	heap_hot_search_buffer	- search HOT chain for tuple satisfying snapshot
 *
 * On entry, *tid is the TID of a tuple (either a simple tuple, or the root
 * of a HOT chain), and buffer is the buffer holding this tuple.  We search
 * for the first chain member satisfying the given snapshot.  If one is
 * found, we update *tid to reference that tuple's offset number, and
 * return TRUE.  If no match, return FALSE without modifying *tid.
 *
 * If all_dead is not NULL, we check non-visible tuples to see if they are
 * globally dead; *all_dead is set TRUE if all members of the HOT chain
 * are vacuumable, FALSE if not.
 *
 * Unlike heap_fetch, the caller must already have pin and (at least) share
 * lock on the buffer; it is still pinned/locked at exit.  Also unlike
 * heap_fetch, we do not report any pgstats count; caller may do so if wanted.
 */
bool
heap_hot_search_buffer(ItemPointer tid, Buffer buffer, Snapshot snapshot,
					   bool *all_dead)
{
	Page		dp = (Page) BufferGetPage(buffer);
	TransactionId prev_xmax = InvalidTransactionId;
	OffsetNumber offnum;
	bool		at_chain_start;

	if (all_dead)
		*all_dead = true;

	Assert(ItemPointerGetBlockNumber(tid) == BufferGetBlockNumber(buffer));
	offnum = ItemPointerGetOffsetNumber(tid);
	at_chain_start = true;

	/* Scan through possible multiple members of HOT-chain */
	for (;;)
	{
		ItemId		lp;
		HeapTupleData heapTuple;

		/* check for bogus TID */
		if (offnum < FirstOffsetNumber || offnum > PageGetMaxOffsetNumber(dp))
			break;

		lp = PageGetItemId(dp, offnum);

		/* check for unused, dead, or redirected items */
		if (!ItemIdIsNormal(lp))
		{
			/* We should only see a redirect at start of chain */
			if (ItemIdIsRedirected(lp) && at_chain_start)
			{
				/* Follow the redirect */
				offnum = ItemIdGetRedirect(lp);
				at_chain_start = false;
				continue;
			}
			/* else must be end of chain */
			break;
		}

		heapTuple.t_data = (HeapTupleHeader) PageGetItem(dp, lp);
		heapTuple.t_len = ItemIdGetLength(lp);

		/*
		 * Shouldn't see a HEAP_ONLY tuple at chain start.
		 */
		if (at_chain_start && HeapTupleIsHeapOnly(&heapTuple))
			break;

		/*
		 * The xmin should match the previous xmax value, else chain is
		 * broken.
		 */
		if (TransactionIdIsValid(prev_xmax) &&
			!TransactionIdEquals(prev_xmax,
								 HeapTupleHeaderGetXmin(heapTuple.t_data)))
			break;

		/* If it's visible per the snapshot, we must return it */
		if (HeapTupleSatisfiesVisibility(&heapTuple, snapshot, buffer))
		{
			ItemPointerSetOffsetNumber(tid, offnum);
			if (all_dead)
				*all_dead = false;
			return true;
		}

		/*
		 * If we can't see it, maybe no one else can either.  At caller
		 * request, check whether all chain members are dead to all
		 * transactions.
		 */
		if (all_dead && *all_dead &&
			HeapTupleSatisfiesVacuum(heapTuple.t_data, RecentGlobalXmin,
									 buffer) != HEAPTUPLE_DEAD)
			*all_dead = false;

		/*
		 * Check to see if HOT chain continues past this tuple; if so fetch
		 * the next offnum and loop around.
		 */
		if (HeapTupleIsHotUpdated(&heapTuple))
		{
			Assert(ItemPointerGetBlockNumber(&heapTuple.t_data->t_ctid) ==
				   ItemPointerGetBlockNumber(tid));
			offnum = ItemPointerGetOffsetNumber(&heapTuple.t_data->t_ctid);
			at_chain_start = false;
			prev_xmax = HeapTupleHeaderGetXmax(heapTuple.t_data);
		}
		else
			break;				/* end of chain */
	}

	return false;
}

/*
 *	heap_hot_search		- search HOT chain for tuple satisfying snapshot
 *
 * This has the same API as heap_hot_search_buffer, except that the caller
 * does not provide the buffer containing the page, rather we access it
 * locally.
 */
bool
heap_hot_search(ItemPointer tid, Relation relation, Snapshot snapshot,
				bool *all_dead)
{
	bool		result;
	Buffer		buffer;

	buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(tid));
	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	result = heap_hot_search_buffer(tid, buffer, snapshot, all_dead);
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(buffer);
	return result;
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
			UnlockReleaseBuffer(buffer);
			break;
		}
		lp = PageGetItemId(dp, offnum);
		if (!ItemIdIsNormal(lp))
		{
			UnlockReleaseBuffer(buffer);
			break;
		}

		/* OK to access the tuple */
		tp.t_self = ctid;
		tp.t_data = (HeapTupleHeader) PageGetItem(dp, lp);
		tp.t_len = ItemIdGetLength(lp);

		/*
		 * After following a t_ctid link, we might arrive at an unrelated
		 * tuple.  Check for XMIN match.
		 */
		if (TransactionIdIsValid(priorXmax) &&
		  !TransactionIdEquals(priorXmax, HeapTupleHeaderGetXmin(tp.t_data)))
		{
			UnlockReleaseBuffer(buffer);
			break;
		}

		/*
		 * Check time qualification of tuple; if visible, set it as the new
		 * result candidate.
		 */
		valid = HeapTupleSatisfiesVisibility(&tp, snapshot, buffer);
		if (valid)
			*tid = ctid;

		/*
		 * If there's a valid t_ctid link, follow it, else we're done.
		 */
		if ((tp.t_data->t_infomask & (HEAP_XMAX_INVALID | HEAP_IS_LOCKED)) ||
			ItemPointerEquals(&tp.t_self, &tp.t_data->t_ctid))
		{
			UnlockReleaseBuffer(buffer);
			break;
		}

		ctid = tp.t_data->t_ctid;
		priorXmax = HeapTupleHeaderGetXmax(tp.t_data);
		UnlockReleaseBuffer(buffer);
	}							/* end of loop */
}


/*
 * UpdateXmaxHintBits - update tuple hint bits after xmax transaction ends
 *
 * This is called after we have waited for the XMAX transaction to terminate.
 * If the transaction aborted, we guarantee the XMAX_INVALID hint bit will
 * be set on exit.	If the transaction committed, we set the XMAX_COMMITTED
 * hint bit if possible --- but beware that that may not yet be possible,
 * if the transaction committed asynchronously.  Hence callers should look
 * only at XMAX_INVALID.
 */
static void
UpdateXmaxHintBits(HeapTupleHeader tuple, Buffer buffer, TransactionId xid)
{
	Assert(TransactionIdEquals(HeapTupleHeaderGetXmax(tuple), xid));

	if (!(tuple->t_infomask & (HEAP_XMAX_COMMITTED | HEAP_XMAX_INVALID)))
	{
		if (TransactionIdDidCommit(xid))
			HeapTupleSetHintBits(tuple, buffer, HEAP_XMAX_COMMITTED,
								 xid);
		else
			HeapTupleSetHintBits(tuple, buffer, HEAP_XMAX_INVALID,
								 InvalidTransactionId);
	}
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
 * transactions, and that the relation gets fsync'd before commit.
 * (See also heap_sync() comments)
 *
 * use_fsm is passed directly to RelationGetBufferForTuple, which see for
 * more info.
 *
 * Note that use_wal and use_fsm will be applied when inserting into the
 * heap's TOAST table, too, if the tuple requires any out-of-line data.
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
	tup->t_data->t_infomask2 &= ~(HEAP2_XACT_MASK);
	tup->t_data->t_infomask |= HEAP_XMAX_INVALID;
	HeapTupleHeaderSetXmin(tup->t_data, xid);
	HeapTupleHeaderSetCmin(tup->t_data, cid);
	HeapTupleHeaderSetXmax(tup->t_data, 0);		/* for cleanliness */
	tup->t_tableOid = RelationGetRelid(relation);

	/*
	 * If the new tuple is too big for storage or contains already toasted
	 * out-of-line attributes from some other relation, invoke the toaster.
	 *
	 * Note: below this point, heaptup is the data we actually intend to store
	 * into the relation; tup is the caller's original untoasted data.
	 */
	if (relation->rd_rel->relkind != RELKIND_RELATION)
	{
		/* toast table entries should never be recursively toasted */
		Assert(!HeapTupleHasExternal(tup));
		heaptup = tup;
	}
	else if (HeapTupleHasExternal(tup) || tup->t_len > TOAST_TUPLE_THRESHOLD)
		heaptup = toast_insert_or_update(relation, tup, NULL,
										 use_wal, use_fsm);
	else
		heaptup = tup;

	/* Find buffer to insert this tuple into */
	buffer = RelationGetBufferForTuple(relation, heaptup->t_len,
									   InvalidBuffer, use_fsm);

	/* NO EREPORT(ERROR) from here till changes are logged */
	START_CRIT_SECTION();

	RelationPutHeapTuple(relation, buffer, heaptup);

	/*
	 * XXX Should we set PageSetPrunable on this page ?
	 *
	 * The inserting transaction may eventually abort thus making this tuple
	 * DEAD and hence available for pruning. Though we don't want to optimize
	 * for aborts, if no other tuple in this page is UPDATEd/DELETEd, the
	 * aborted tuple will never be pruned until next vacuum is triggered.
	 *
	 * If you do add PageSetPrunable here, add it in heap_xlog_insert too.
	 */

	MarkBufferDirty(buffer);

	/* XLOG stuff */
	if (use_wal && !relation->rd_istemp)
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

		xlhdr.t_infomask2 = heaptup->t_data->t_infomask2;
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

	UnlockReleaseBuffer(buffer);

	/*
	 * If tuple is cachable, mark it for invalidation from the caches in case
	 * we abort.  Note it is OK to do this after releasing the buffer, because
	 * the heaptup data structure is all in local memory, not in the shared
	 * buffer.
	 */
	CacheInvalidateHeapTuple(relation, heaptup);

	pgstat_count_heap_insert(relation);

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
 * a default command ID and not allowing access to the speedup options.
 *
 * This should be used rather than using heap_insert directly in most places
 * where we are modifying system catalogs.
 */
Oid
simple_heap_insert(Relation relation, HeapTuple tup)
{
	return heap_insert(relation, tup, GetCurrentCommandId(true), true, true);
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
	bool		iscombo;

	Assert(ItemPointerIsValid(tid));

	buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(tid));
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	dp = (PageHeader) BufferGetPage(buffer);
	lp = PageGetItemId(dp, ItemPointerGetOffsetNumber(tid));
	Assert(ItemIdIsNormal(lp));

	tp.t_data = (HeapTupleHeader) PageGetItem(dp, lp);
	tp.t_len = ItemIdGetLength(lp);
	tp.t_self = *tid;

l1:
	result = HeapTupleSatisfiesUpdate(tp.t_data, cid, buffer);

	if (result == HeapTupleInvisible)
	{
		UnlockReleaseBuffer(buffer);
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

			/* Otherwise check if it committed or aborted */
			UpdateXmaxHintBits(tp.t_data, buffer, xwait);
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
		if (!HeapTupleSatisfiesVisibility(&tp, crosscheck, buffer))
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
		UnlockReleaseBuffer(buffer);
		if (have_tuple_lock)
			UnlockTuple(relation, &(tp.t_self), ExclusiveLock);
		return result;
	}

	/* replace cid with a combo cid if necessary */
	HeapTupleHeaderAdjustCmax(tp.t_data, &cid, &iscombo);

	START_CRIT_SECTION();

	/*
	 * If this transaction commits, the tuple will become DEAD sooner or
	 * later.  Set flag that this page is a candidate for pruning once our xid
	 * falls below the OldestXmin horizon.	If the transaction finally aborts,
	 * the subsequent page pruning will be a no-op and the hint will be
	 * cleared.
	 */
	PageSetPrunable(dp, xid);

	/* store transaction information of xact deleting the tuple */
	tp.t_data->t_infomask &= ~(HEAP_XMAX_COMMITTED |
							   HEAP_XMAX_INVALID |
							   HEAP_XMAX_IS_MULTI |
							   HEAP_IS_LOCKED |
							   HEAP_MOVED);
	HeapTupleHeaderClearHotUpdated(tp.t_data);
	HeapTupleHeaderSetXmax(tp.t_data, xid);
	HeapTupleHeaderSetCmax(tp.t_data, cid, iscombo);
	/* Make sure there is no forward chain link in t_ctid */
	tp.t_data->t_ctid = tp.t_self;

	MarkBufferDirty(buffer);

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

	END_CRIT_SECTION();

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

	/*
	 * If the tuple has toasted out-of-line attributes, we need to delete
	 * those items too.  We have to do this before releasing the buffer
	 * because we need to look at the contents of the tuple, but it's OK to
	 * release the content lock on the buffer first.
	 */
	if (relation->rd_rel->relkind != RELKIND_RELATION)
	{
		/* toast table entries should never be recursively toasted */
		Assert(!HeapTupleHasExternal(&tp));
	}
	else if (HeapTupleHasExternal(&tp))
		toast_delete(relation, &tp);

	/*
	 * Mark tuple for invalidation from system caches at next command
	 * boundary. We have to do this before releasing the buffer because we
	 * need to look at the contents of the tuple.
	 */
	CacheInvalidateHeapTuple(relation, &tp);

	/* Now we can release the buffer */
	ReleaseBuffer(buffer);

	/*
	 * Release the lmgr tuple lock, if we had it.
	 */
	if (have_tuple_lock)
		UnlockTuple(relation, &(tp.t_self), ExclusiveLock);

	pgstat_count_heap_delete(relation);

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
						 GetCurrentCommandId(true), InvalidSnapshot,
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
 * new tuple was inserted, and its HEAP_ONLY_TUPLE flag is set iff a HOT
 * update was done.  However, any TOAST changes in the new tuple's
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
	Bitmapset  *hot_attrs;
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
	bool		iscombo;
	bool		use_hot_update = false;

	Assert(ItemPointerIsValid(otid));

	/*
	 * Fetch the list of attributes to be checked for HOT update.  This is
	 * wasted effort if we fail to update or have to put the new tuple on a
	 * different page.	But we must compute the list before obtaining buffer
	 * lock --- in the worst case, if we are doing an update on one of the
	 * relevant system catalogs, we could deadlock if we try to fetch the list
	 * later.  In any case, the relcache caches the data so this is usually
	 * pretty cheap.
	 *
	 * Note that we get a copy here, so we need not worry about relcache flush
	 * happening midway through.
	 */
	hot_attrs = RelationGetIndexAttrBitmap(relation);

	buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(otid));
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	dp = (PageHeader) BufferGetPage(buffer);
	lp = PageGetItemId(dp, ItemPointerGetOffsetNumber(otid));
	Assert(ItemIdIsNormal(lp));

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
		UnlockReleaseBuffer(buffer);
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

			/* Otherwise check if it committed or aborted */
			UpdateXmaxHintBits(oldtup.t_data, buffer, xwait);
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
		if (!HeapTupleSatisfiesVisibility(&oldtup, crosscheck, buffer))
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
		UnlockReleaseBuffer(buffer);
		if (have_tuple_lock)
			UnlockTuple(relation, &(oldtup.t_self), ExclusiveLock);
		bms_free(hot_attrs);
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
	newtup->t_data->t_infomask2 &= ~(HEAP2_XACT_MASK);
	newtup->t_data->t_infomask |= (HEAP_XMAX_INVALID | HEAP_UPDATED);
	HeapTupleHeaderSetXmin(newtup->t_data, xid);
	HeapTupleHeaderSetCmin(newtup->t_data, cid);
	HeapTupleHeaderSetXmax(newtup->t_data, 0);	/* for cleanliness */

	/*
	 * Replace cid with a combo cid if necessary.  Note that we already put
	 * the plain cid into the new tuple.
	 */
	HeapTupleHeaderAdjustCmax(oldtup.t_data, &cid, &iscombo);

	/*
	 * If the toaster needs to be activated, OR if the new tuple will not fit
	 * on the same page as the old, then we need to release the content lock
	 * (but not the pin!) on the old tuple's buffer while we are off doing
	 * TOAST and/or table-file-extension work.	We must mark the old tuple to
	 * show that it's already being updated, else other processes may try to
	 * update it themselves.
	 *
	 * We need to invoke the toaster if there are already any out-of-line
	 * toasted values present, or if the new tuple is over-threshold.
	 */
	if (relation->rd_rel->relkind != RELKIND_RELATION)
	{
		/* toast table entries should never be recursively toasted */
		Assert(!HeapTupleHasExternal(&oldtup));
		Assert(!HeapTupleHasExternal(newtup));
		need_toast = false;
	}
	else
		need_toast = (HeapTupleHasExternal(&oldtup) ||
					  HeapTupleHasExternal(newtup) ||
					  newtup->t_len > TOAST_TUPLE_THRESHOLD);

	pagefree = PageGetHeapFreeSpace((Page) dp);

	newtupsize = MAXALIGN(newtup->t_len);

	if (need_toast || newtupsize > pagefree)
	{
		/* Clear obsolete visibility flags ... */
		oldtup.t_data->t_infomask &= ~(HEAP_XMAX_COMMITTED |
									   HEAP_XMAX_INVALID |
									   HEAP_XMAX_IS_MULTI |
									   HEAP_IS_LOCKED |
									   HEAP_MOVED);
		HeapTupleClearHotUpdated(&oldtup);
		/* ... and store info about transaction updating this tuple */
		HeapTupleHeaderSetXmax(oldtup.t_data, xid);
		HeapTupleHeaderSetCmax(oldtup.t_data, cid, iscombo);
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
			/* Note we always use WAL and FSM during updates */
			heaptup = toast_insert_or_update(relation, newtup, &oldtup,
											 true, true);
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
			pagefree = PageGetHeapFreeSpace((Page) dp);
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

	if (newbuf == buffer)
	{
		/*
		 * Since the new tuple is going into the same page, we might be able
		 * to do a HOT update.	Check if any of the index columns have been
		 * changed.  If not, then HOT update is possible.
		 */
		if (HeapSatisfiesHOTUpdate(relation, hot_attrs, &oldtup, heaptup))
			use_hot_update = true;
	}
	else
	{
		/* Set a hint that the old page could use prune/defrag */
		PageSetFull(dp);
	}

	/* NO EREPORT(ERROR) from here till changes are logged */
	START_CRIT_SECTION();

	/*
	 * If this transaction commits, the old tuple will become DEAD sooner or
	 * later.  Set flag that this page is a candidate for pruning once our xid
	 * falls below the OldestXmin horizon.	If the transaction finally aborts,
	 * the subsequent page pruning will be a no-op and the hint will be
	 * cleared.
	 *
	 * XXX Should we set hint on newbuf as well?  If the transaction aborts,
	 * there would be a prunable tuple in the newbuf; but for now we choose
	 * not to optimize for aborts.	Note that heap_xlog_update must be kept in
	 * sync if this decision changes.
	 */
	PageSetPrunable(dp, xid);

	if (use_hot_update)
	{
		/* Mark the old tuple as HOT-updated */
		HeapTupleSetHotUpdated(&oldtup);
		/* And mark the new tuple as heap-only */
		HeapTupleSetHeapOnly(heaptup);
		/* Mark the caller's copy too, in case different from heaptup */
		HeapTupleSetHeapOnly(newtup);
	}
	else
	{
		/* Make sure tuples are correctly marked as not-HOT */
		HeapTupleClearHotUpdated(&oldtup);
		HeapTupleClearHeapOnly(heaptup);
		HeapTupleClearHeapOnly(newtup);
	}

	RelationPutHeapTuple(relation, newbuf, heaptup);	/* insert new tuple */

	if (!already_marked)
	{
		/* Clear obsolete visibility flags ... */
		oldtup.t_data->t_infomask &= ~(HEAP_XMAX_COMMITTED |
									   HEAP_XMAX_INVALID |
									   HEAP_XMAX_IS_MULTI |
									   HEAP_IS_LOCKED |
									   HEAP_MOVED);
		/* ... and store info about transaction updating this tuple */
		HeapTupleHeaderSetXmax(oldtup.t_data, xid);
		HeapTupleHeaderSetCmax(oldtup.t_data, cid, iscombo);
	}

	/* record address of new tuple in t_ctid of old one */
	oldtup.t_data->t_ctid = heaptup->t_self;

	if (newbuf != buffer)
		MarkBufferDirty(newbuf);
	MarkBufferDirty(buffer);

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

	END_CRIT_SECTION();

	if (newbuf != buffer)
		LockBuffer(newbuf, BUFFER_LOCK_UNLOCK);
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

	/*
	 * Mark old tuple for invalidation from system caches at next command
	 * boundary. We have to do this before releasing the buffer because we
	 * need to look at the contents of the tuple.
	 */
	CacheInvalidateHeapTuple(relation, &oldtup);

	/* Now we can release the buffer(s) */
	if (newbuf != buffer)
		ReleaseBuffer(newbuf);
	ReleaseBuffer(buffer);

	/*
	 * If new tuple is cachable, mark it for invalidation from the caches in
	 * case we abort.  Note it is OK to do this after releasing the buffer,
	 * because the heaptup data structure is all in local memory, not in the
	 * shared buffer.
	 */
	CacheInvalidateHeapTuple(relation, heaptup);

	/*
	 * Release the lmgr tuple lock, if we had it.
	 */
	if (have_tuple_lock)
		UnlockTuple(relation, &(oldtup.t_self), ExclusiveLock);

	pgstat_count_heap_update(relation, use_hot_update);

	/*
	 * If heaptup is a private copy, release it.  Don't forget to copy t_self
	 * back to the caller's image, too.
	 */
	if (heaptup != newtup)
	{
		newtup->t_self = heaptup->t_self;
		heap_freetuple(heaptup);
	}

	bms_free(hot_attrs);

	return HeapTupleMayBeUpdated;
}

/*
 * Check if the specified attribute's value is same in both given tuples.
 * Subroutine for HeapSatisfiesHOTUpdate.
 */
static bool
heap_tuple_attr_equals(TupleDesc tupdesc, int attrnum,
					   HeapTuple tup1, HeapTuple tup2)
{
	Datum		value1,
				value2;
	bool		isnull1,
				isnull2;
	Form_pg_attribute att;

	/*
	 * If it's a whole-tuple reference, say "not equal".  It's not really
	 * worth supporting this case, since it could only succeed after a no-op
	 * update, which is hardly a case worth optimizing for.
	 */
	if (attrnum == 0)
		return false;

	/*
	 * Likewise, automatically say "not equal" for any system attribute other
	 * than OID and tableOID; we cannot expect these to be consistent in a HOT
	 * chain, or even to be set correctly yet in the new tuple.
	 */
	if (attrnum < 0)
	{
		if (attrnum != ObjectIdAttributeNumber &&
			attrnum != TableOidAttributeNumber)
			return false;
	}

	/*
	 * Extract the corresponding values.  XXX this is pretty inefficient if
	 * there are many indexed columns.	Should HeapSatisfiesHOTUpdate do a
	 * single heap_deform_tuple call on each tuple, instead?  But that doesn't
	 * work for system columns ...
	 */
	value1 = heap_getattr(tup1, attrnum, tupdesc, &isnull1);
	value2 = heap_getattr(tup2, attrnum, tupdesc, &isnull2);

	/*
	 * If one value is NULL and other is not, then they are certainly not
	 * equal
	 */
	if (isnull1 != isnull2)
		return false;

	/*
	 * If both are NULL, they can be considered equal.
	 */
	if (isnull1)
		return true;

	/*
	 * We do simple binary comparison of the two datums.  This may be overly
	 * strict because there can be multiple binary representations for the
	 * same logical value.	But we should be OK as long as there are no false
	 * positives.  Using a type-specific equality operator is messy because
	 * there could be multiple notions of equality in different operator
	 * classes; furthermore, we cannot safely invoke user-defined functions
	 * while holding exclusive buffer lock.
	 */
	if (attrnum <= 0)
	{
		/* The only allowed system columns are OIDs, so do this */
		return (DatumGetObjectId(value1) == DatumGetObjectId(value2));
	}
	else
	{
		Assert(attrnum <= tupdesc->natts);
		att = tupdesc->attrs[attrnum - 1];
		return datumIsEqual(value1, value2, att->attbyval, att->attlen);
	}
}

/*
 * Check if the old and new tuples represent a HOT-safe update. To be able
 * to do a HOT update, we must not have changed any columns used in index
 * definitions.
 *
 * The set of attributes to be checked is passed in (we dare not try to
 * compute it while holding exclusive buffer lock...)  NOTE that hot_attrs
 * is destructively modified!  That is OK since this is invoked at most once
 * by heap_update().
 *
 * Returns true if safe to do HOT update.
 */
static bool
HeapSatisfiesHOTUpdate(Relation relation, Bitmapset *hot_attrs,
					   HeapTuple oldtup, HeapTuple newtup)
{
	int			attrnum;

	while ((attrnum = bms_first_member(hot_attrs)) >= 0)
	{
		/* Adjust for system attributes */
		attrnum += FirstLowInvalidHeapAttributeNumber;

		/* If the attribute value has changed, we can't do HOT update */
		if (!heap_tuple_attr_equals(RelationGetDescr(relation), attrnum,
									oldtup, newtup))
			return false;
	}

	return true;
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
						 GetCurrentCommandId(true), InvalidSnapshot,
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
	TransactionId xmax;
	uint16		old_infomask;
	uint16		new_infomask;
	LOCKMODE	tuple_lock_type;
	bool		have_tuple_lock = false;

	tuple_lock_type = (mode == LockTupleShared) ? ShareLock : ExclusiveLock;

	*buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(tid));
	LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);

	dp = (PageHeader) BufferGetPage(*buffer);
	lp = PageGetItemId(dp, ItemPointerGetOffsetNumber(tid));
	Assert(ItemIdIsNormal(lp));

	tuple->t_data = (HeapTupleHeader) PageGetItem((Page) dp, lp);
	tuple->t_len = ItemIdGetLength(lp);
	tuple->t_tableOid = RelationGetRelid(relation);

l3:
	result = HeapTupleSatisfiesUpdate(tuple->t_data, cid, *buffer);

	if (result == HeapTupleInvisible)
	{
		UnlockReleaseBuffer(*buffer);
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
		 * If we wish to acquire share lock, and the tuple is already
		 * share-locked by a multixact that includes any subtransaction of the
		 * current top transaction, then we effectively hold the desired lock
		 * already.  We *must* succeed without trying to take the tuple lock,
		 * else we will deadlock against anyone waiting to acquire exclusive
		 * lock.  We don't need to make any state changes in this case.
		 */
		if (mode == LockTupleShared &&
			(infomask & HEAP_XMAX_IS_MULTI) &&
			MultiXactIdIsCurrent((MultiXactId) xwait))
		{
			Assert(infomask & HEAP_XMAX_SHARED_LOCK);
			/* Probably can't hold tuple lock here, but may as well check */
			if (have_tuple_lock)
				UnlockTuple(relation, tid, tuple_lock_type);
			return HeapTupleMayBeUpdated;
		}

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

			/* Otherwise check if it committed or aborted */
			UpdateXmaxHintBits(tuple->t_data, *buffer, xwait);
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
	 * We might already hold the desired lock (or stronger), possibly under a
	 * different subtransaction of the current top transaction.  If so, there
	 * is no need to change state or issue a WAL record.  We already handled
	 * the case where this is true for xmax being a MultiXactId, so now check
	 * for cases where it is a plain TransactionId.
	 *
	 * Note in particular that this covers the case where we already hold
	 * exclusive lock on the tuple and the caller only wants shared lock. It
	 * would certainly not do to give up the exclusive lock.
	 */
	xmax = HeapTupleHeaderGetXmax(tuple->t_data);
	old_infomask = tuple->t_data->t_infomask;

	if (!(old_infomask & (HEAP_XMAX_INVALID |
						  HEAP_XMAX_COMMITTED |
						  HEAP_XMAX_IS_MULTI)) &&
		(mode == LockTupleShared ?
		 (old_infomask & HEAP_IS_LOCKED) :
		 (old_infomask & HEAP_XMAX_EXCL_LOCK)) &&
		TransactionIdIsCurrentTransactionId(xmax))
	{
		LockBuffer(*buffer, BUFFER_LOCK_UNLOCK);
		/* Probably can't hold tuple lock here, but may as well check */
		if (have_tuple_lock)
			UnlockTuple(relation, tid, tuple_lock_type);
		return HeapTupleMayBeUpdated;
	}

	/*
	 * Compute the new xmax and infomask to store into the tuple.  Note we do
	 * not modify the tuple just yet, because that would leave it in the wrong
	 * state if multixact.c elogs.
	 */
	xid = GetCurrentTransactionId();

	new_infomask = old_infomask & ~(HEAP_XMAX_COMMITTED |
									HEAP_XMAX_INVALID |
									HEAP_XMAX_IS_MULTI |
									HEAP_IS_LOCKED |
									HEAP_MOVED);

	if (mode == LockTupleShared)
	{
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
				/*
				 * If the XMAX is a valid TransactionId, then we need to
				 * create a new MultiXactId that includes both the old locker
				 * and our own TransactionId.
				 */
				xid = MultiXactIdCreate(xmax, xid);
				new_infomask |= HEAP_XMAX_IS_MULTI;
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
	 * Note: Cmax is meaningless in this context, so don't set it; this avoids
	 * possibly generating a useless combo CID.
	 */
	tuple->t_data->t_infomask = new_infomask;
	HeapTupleHeaderClearHotUpdated(tuple->t_data);
	HeapTupleHeaderSetXmax(tuple->t_data, xid);
	/* Make sure there is no forward chain link in t_ctid */
	tuple->t_data->t_ctid = *tid;

	MarkBufferDirty(*buffer);

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

	END_CRIT_SECTION();

	LockBuffer(*buffer, BUFFER_LOCK_UNLOCK);

	/*
	 * Now that we have successfully marked the tuple as locked, we can
	 * release the lmgr tuple lock, if we had it.
	 */
	if (have_tuple_lock)
		UnlockTuple(relation, tid, tuple_lock_type);

	return HeapTupleMayBeUpdated;
}


/*
 * heap_inplace_update - update a tuple "in place" (ie, overwrite it)
 *
 * Overwriting violates both MVCC and transactional safety, so the uses
 * of this function in Postgres are extremely limited.	Nonetheless we
 * find some places to use it.
 *
 * The tuple cannot change size, and therefore it's reasonable to assume
 * that its null bitmap (if any) doesn't change either.  So we just
 * overwrite the data portion of the tuple without touching the null
 * bitmap or any of the header fields.
 *
 * tuple is an in-memory tuple structure containing the data to be written
 * over the target tuple.  Also, tuple->t_self identifies the target tuple.
 */
void
heap_inplace_update(Relation relation, HeapTuple tuple)
{
	Buffer		buffer;
	Page		page;
	OffsetNumber offnum;
	ItemId		lp = NULL;
	HeapTupleHeader htup;
	uint32		oldlen;
	uint32		newlen;

	buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(&(tuple->t_self)));
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	page = (Page) BufferGetPage(buffer);

	offnum = ItemPointerGetOffsetNumber(&(tuple->t_self));
	if (PageGetMaxOffsetNumber(page) >= offnum)
		lp = PageGetItemId(page, offnum);

	if (PageGetMaxOffsetNumber(page) < offnum || !ItemIdIsNormal(lp))
		elog(ERROR, "heap_inplace_update: invalid lp");

	htup = (HeapTupleHeader) PageGetItem(page, lp);

	oldlen = ItemIdGetLength(lp) - htup->t_hoff;
	newlen = tuple->t_len - tuple->t_data->t_hoff;
	if (oldlen != newlen || htup->t_hoff != tuple->t_data->t_hoff)
		elog(ERROR, "heap_inplace_update: wrong tuple length");

	/* NO EREPORT(ERROR) from here till changes are logged */
	START_CRIT_SECTION();

	memcpy((char *) htup + htup->t_hoff,
		   (char *) tuple->t_data + tuple->t_data->t_hoff,
		   newlen);

	MarkBufferDirty(buffer);

	/* XLOG stuff */
	if (!relation->rd_istemp)
	{
		xl_heap_inplace xlrec;
		XLogRecPtr	recptr;
		XLogRecData rdata[2];

		xlrec.target.node = relation->rd_node;
		xlrec.target.tid = tuple->t_self;

		rdata[0].data = (char *) &xlrec;
		rdata[0].len = SizeOfHeapInplace;
		rdata[0].buffer = InvalidBuffer;
		rdata[0].next = &(rdata[1]);

		rdata[1].data = (char *) htup + htup->t_hoff;
		rdata[1].len = newlen;
		rdata[1].buffer = buffer;
		rdata[1].buffer_std = true;
		rdata[1].next = NULL;

		recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_INPLACE, rdata);

		PageSetLSN(page, recptr);
		PageSetTLI(page, ThisTimeLineID);
	}

	END_CRIT_SECTION();

	UnlockReleaseBuffer(buffer);

	/* Send out shared cache inval if necessary */
	if (!IsBootstrapProcessingMode())
		CacheInvalidateHeapTuple(relation, tuple);
}


/*
 * heap_freeze_tuple
 *
 * Check to see whether any of the XID fields of a tuple (xmin, xmax, xvac)
 * are older than the specified cutoff XID.  If so, replace them with
 * FrozenTransactionId or InvalidTransactionId as appropriate, and return
 * TRUE.  Return FALSE if nothing was changed.
 *
 * It is assumed that the caller has checked the tuple with
 * HeapTupleSatisfiesVacuum() and determined that it is not HEAPTUPLE_DEAD
 * (else we should be removing the tuple, not freezing it).
 *
 * NB: cutoff_xid *must* be <= the current global xmin, to ensure that any
 * XID older than it could neither be running nor seen as running by any
 * open transaction.  This ensures that the replacement will not change
 * anyone's idea of the tuple state.  Also, since we assume the tuple is
 * not HEAPTUPLE_DEAD, the fact that an XID is not still running allows us
 * to assume that it is either committed good or aborted, as appropriate;
 * so we need no external state checks to decide what to do.  (This is good
 * because this function is applied during WAL recovery, when we don't have
 * access to any such state, and can't depend on the hint bits to be set.)
 *
 * In lazy VACUUM, we call this while initially holding only a shared lock
 * on the tuple's buffer.  If any change is needed, we trade that in for an
 * exclusive lock before making the change.  Caller should pass the buffer ID
 * if shared lock is held, InvalidBuffer if exclusive lock is already held.
 *
 * Note: it might seem we could make the changes without exclusive lock, since
 * TransactionId read/write is assumed atomic anyway.  However there is a race
 * condition: someone who just fetched an old XID that we overwrite here could
 * conceivably not finish checking the XID against pg_clog before we finish
 * the VACUUM and perhaps truncate off the part of pg_clog he needs.  Getting
 * exclusive lock ensures no other backend is in process of checking the
 * tuple status.  Also, getting exclusive lock makes it safe to adjust the
 * infomask bits.
 */
bool
heap_freeze_tuple(HeapTupleHeader tuple, TransactionId cutoff_xid,
				  Buffer buf)
{
	bool		changed = false;
	TransactionId xid;

	xid = HeapTupleHeaderGetXmin(tuple);
	if (TransactionIdIsNormal(xid) &&
		TransactionIdPrecedes(xid, cutoff_xid))
	{
		if (buf != InvalidBuffer)
		{
			/* trade in share lock for exclusive lock */
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
			buf = InvalidBuffer;
		}
		HeapTupleHeaderSetXmin(tuple, FrozenTransactionId);

		/*
		 * Might as well fix the hint bits too; usually XMIN_COMMITTED will
		 * already be set here, but there's a small chance not.
		 */
		Assert(!(tuple->t_infomask & HEAP_XMIN_INVALID));
		tuple->t_infomask |= HEAP_XMIN_COMMITTED;
		changed = true;
	}

	/*
	 * When we release shared lock, it's possible for someone else to change
	 * xmax before we get the lock back, so repeat the check after acquiring
	 * exclusive lock.	(We don't need this pushup for xmin, because only
	 * VACUUM could be interested in changing an existing tuple's xmin, and
	 * there's only one VACUUM allowed on a table at a time.)
	 */
recheck_xmax:
	if (!(tuple->t_infomask & HEAP_XMAX_IS_MULTI))
	{
		xid = HeapTupleHeaderGetXmax(tuple);
		if (TransactionIdIsNormal(xid) &&
			TransactionIdPrecedes(xid, cutoff_xid))
		{
			if (buf != InvalidBuffer)
			{
				/* trade in share lock for exclusive lock */
				LockBuffer(buf, BUFFER_LOCK_UNLOCK);
				LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
				buf = InvalidBuffer;
				goto recheck_xmax;		/* see comment above */
			}
			HeapTupleHeaderSetXmax(tuple, InvalidTransactionId);

			/*
			 * The tuple might be marked either XMAX_INVALID or XMAX_COMMITTED
			 * + LOCKED.  Normalize to INVALID just to be sure no one gets
			 * confused.
			 */
			tuple->t_infomask &= ~HEAP_XMAX_COMMITTED;
			tuple->t_infomask |= HEAP_XMAX_INVALID;
			HeapTupleHeaderClearHotUpdated(tuple);
			changed = true;
		}
	}
	else
	{
		/*----------
		 * XXX perhaps someday we should zero out very old MultiXactIds here?
		 *
		 * The only way a stale MultiXactId could pose a problem is if a
		 * tuple, having once been multiply-share-locked, is not touched by
		 * any vacuum or attempted lock or deletion for just over 4G MultiXact
		 * creations, and then in the probably-narrow window where its xmax
		 * is again a live MultiXactId, someone tries to lock or delete it.
		 * Even then, another share-lock attempt would work fine.  An
		 * exclusive-lock or delete attempt would face unexpected delay, or
		 * in the very worst case get a deadlock error.  This seems an
		 * extremely low-probability scenario with minimal downside even if
		 * it does happen, so for now we don't do the extra bookkeeping that
		 * would be needed to clean out MultiXactIds.
		 *----------
		 */
	}

	/*
	 * Although xvac per se could only be set by VACUUM, it shares physical
	 * storage space with cmax, and so could be wiped out by someone setting
	 * xmax.  Hence recheck after changing lock, same as for xmax itself.
	 */
recheck_xvac:
	if (tuple->t_infomask & HEAP_MOVED)
	{
		xid = HeapTupleHeaderGetXvac(tuple);
		if (TransactionIdIsNormal(xid) &&
			TransactionIdPrecedes(xid, cutoff_xid))
		{
			if (buf != InvalidBuffer)
			{
				/* trade in share lock for exclusive lock */
				LockBuffer(buf, BUFFER_LOCK_UNLOCK);
				LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
				buf = InvalidBuffer;
				goto recheck_xvac;		/* see comment above */
			}

			/*
			 * If a MOVED_OFF tuple is not dead, the xvac transaction must
			 * have failed; whereas a non-dead MOVED_IN tuple must mean the
			 * xvac transaction succeeded.
			 */
			if (tuple->t_infomask & HEAP_MOVED_OFF)
				HeapTupleHeaderSetXvac(tuple, InvalidTransactionId);
			else
				HeapTupleHeaderSetXvac(tuple, FrozenTransactionId);

			/*
			 * Might as well fix the hint bits too; usually XMIN_COMMITTED
			 * will already be set here, but there's a small chance not.
			 */
			Assert(!(tuple->t_infomask & HEAP_XMIN_INVALID));
			tuple->t_infomask |= HEAP_XMIN_COMMITTED;
			changed = true;
		}
	}

	return changed;
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
	{
		scan->rs_mctid = scan->rs_ctup.t_self;
		if (scan->rs_pageatatime)
			scan->rs_mindex = scan->rs_cindex;
	}
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

	if (!ItemPointerIsValid(&scan->rs_mctid))
	{
		scan->rs_ctup.t_data = NULL;

		/*
		 * unpin scan buffers
		 */
		if (BufferIsValid(scan->rs_cbuf))
			ReleaseBuffer(scan->rs_cbuf);
		scan->rs_cbuf = InvalidBuffer;
		scan->rs_cblock = InvalidBlockNumber;
		scan->rs_inited = false;
	}
	else
	{
		/*
		 * If we reached end of scan, rs_inited will now be false.	We must
		 * reset it to true to keep heapgettup from doing the wrong thing.
		 */
		scan->rs_inited = true;
		scan->rs_ctup.t_self = scan->rs_mctid;
		if (scan->rs_pageatatime)
		{
			scan->rs_cindex = scan->rs_mindex;
			heapgettup_pagemode(scan,
								NoMovementScanDirection,
								0,		/* needn't recheck scan keys */
								NULL);
		}
		else
			heapgettup(scan,
					   NoMovementScanDirection,
					   0,		/* needn't recheck scan keys */
					   NULL);
	}
}

/*
 * Perform XLogInsert for a heap-clean operation.  Caller must already
 * have modified the buffer and marked it dirty.
 *
 * Note: prior to Postgres 8.3, the entries in the nowunused[] array were
 * zero-based tuple indexes.  Now they are one-based like other uses
 * of OffsetNumber.
 */
XLogRecPtr
log_heap_clean(Relation reln, Buffer buffer,
			   OffsetNumber *redirected, int nredirected,
			   OffsetNumber *nowdead, int ndead,
			   OffsetNumber *nowunused, int nunused,
			   bool redirect_move)
{
	xl_heap_clean xlrec;
	uint8		info;
	XLogRecPtr	recptr;
	XLogRecData rdata[4];

	/* Caller should not call me on a temp relation */
	Assert(!reln->rd_istemp);

	xlrec.node = reln->rd_node;
	xlrec.block = BufferGetBlockNumber(buffer);
	xlrec.nredirected = nredirected;
	xlrec.ndead = ndead;

	rdata[0].data = (char *) &xlrec;
	rdata[0].len = SizeOfHeapClean;
	rdata[0].buffer = InvalidBuffer;
	rdata[0].next = &(rdata[1]);

	/*
	 * The OffsetNumber arrays are not actually in the buffer, but we pretend
	 * that they are.  When XLogInsert stores the whole buffer, the offset
	 * arrays need not be stored too.  Note that even if all three arrays are
	 * empty, we want to expose the buffer as a candidate for whole-page
	 * storage, since this record type implies a defragmentation operation
	 * even if no item pointers changed state.
	 */
	if (nredirected > 0)
	{
		rdata[1].data = (char *) redirected;
		rdata[1].len = nredirected * sizeof(OffsetNumber) * 2;
	}
	else
	{
		rdata[1].data = NULL;
		rdata[1].len = 0;
	}
	rdata[1].buffer = buffer;
	rdata[1].buffer_std = true;
	rdata[1].next = &(rdata[2]);

	if (ndead > 0)
	{
		rdata[2].data = (char *) nowdead;
		rdata[2].len = ndead * sizeof(OffsetNumber);
	}
	else
	{
		rdata[2].data = NULL;
		rdata[2].len = 0;
	}
	rdata[2].buffer = buffer;
	rdata[2].buffer_std = true;
	rdata[2].next = &(rdata[3]);

	if (nunused > 0)
	{
		rdata[3].data = (char *) nowunused;
		rdata[3].len = nunused * sizeof(OffsetNumber);
	}
	else
	{
		rdata[3].data = NULL;
		rdata[3].len = 0;
	}
	rdata[3].buffer = buffer;
	rdata[3].buffer_std = true;
	rdata[3].next = NULL;

	info = redirect_move ? XLOG_HEAP2_CLEAN_MOVE : XLOG_HEAP2_CLEAN;
	recptr = XLogInsert(RM_HEAP2_ID, info, rdata);

	return recptr;
}

/*
 * Perform XLogInsert for a heap-freeze operation.	Caller must already
 * have modified the buffer and marked it dirty.
 */
XLogRecPtr
log_heap_freeze(Relation reln, Buffer buffer,
				TransactionId cutoff_xid,
				OffsetNumber *offsets, int offcnt)
{
	xl_heap_freeze xlrec;
	XLogRecPtr	recptr;
	XLogRecData rdata[2];

	/* Caller should not call me on a temp relation */
	Assert(!reln->rd_istemp);

	xlrec.node = reln->rd_node;
	xlrec.block = BufferGetBlockNumber(buffer);
	xlrec.cutoff_xid = cutoff_xid;

	rdata[0].data = (char *) &xlrec;
	rdata[0].len = SizeOfHeapFreeze;
	rdata[0].buffer = InvalidBuffer;
	rdata[0].next = &(rdata[1]);

	/*
	 * The tuple-offsets array is not actually in the buffer, but pretend that
	 * it is.  When XLogInsert stores the whole buffer, the offsets array need
	 * not be stored too.
	 */
	if (offcnt > 0)
	{
		rdata[1].data = (char *) offsets;
		rdata[1].len = offcnt * sizeof(OffsetNumber);
	}
	else
	{
		rdata[1].data = NULL;
		rdata[1].len = 0;
	}
	rdata[1].buffer = buffer;
	rdata[1].buffer_std = true;
	rdata[1].next = NULL;

	recptr = XLogInsert(RM_HEAP2_ID, XLOG_HEAP2_FREEZE, rdata);

	return recptr;
}

/*
 * Perform XLogInsert for a heap-update operation.	Caller must already
 * have modified the buffer(s) and marked them dirty.
 */
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
	uint8		info;
	XLogRecPtr	recptr;
	XLogRecData rdata[4];
	Page		page = BufferGetPage(newbuf);

	/* Caller should not call me on a temp relation */
	Assert(!reln->rd_istemp);

	if (move)
	{
		Assert(!HeapTupleIsHeapOnly(newtup));
		info = XLOG_HEAP_MOVE;
	}
	else if (HeapTupleIsHeapOnly(newtup))
		info = XLOG_HEAP_HOT_UPDATE;
	else
		info = XLOG_HEAP_UPDATE;

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

	xlhdr.hdr.t_infomask2 = newtup->t_data->t_infomask2;
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

	return recptr;
}

/*
 * Perform XLogInsert for a heap-move operation.  Caller must already
 * have modified the buffers and marked them dirty.
 */
XLogRecPtr
log_heap_move(Relation reln, Buffer oldbuf, ItemPointerData from,
			  Buffer newbuf, HeapTuple newtup)
{
	return log_heap_update(reln, oldbuf, from, newbuf, newtup, true);
}

/*
 * Perform XLogInsert of a HEAP_NEWPAGE record to WAL. Caller is responsible
 * for writing the page to disk after calling this routine.
 *
 * Note: all current callers build pages in private memory and write them
 * directly to smgr, rather than using bufmgr.	Therefore there is no need
 * to pass a buffer ID to XLogInsert, nor to perform MarkBufferDirty within
 * the critical section.
 *
 * Note: the NEWPAGE log record is used for both heaps and indexes, so do
 * not do anything that assumes we are touching a heap.
 */
XLogRecPtr
log_newpage(RelFileNode *rnode, BlockNumber blkno, Page page)
{
	xl_heap_newpage xlrec;
	XLogRecPtr	recptr;
	XLogRecData rdata[2];

	/* NO ELOG(ERROR) from here till newpage op is logged */
	START_CRIT_SECTION();

	xlrec.node = *rnode;
	xlrec.blkno = blkno;

	rdata[0].data = (char *) &xlrec;
	rdata[0].len = SizeOfHeapNewpage;
	rdata[0].buffer = InvalidBuffer;
	rdata[0].next = &(rdata[1]);

	rdata[1].data = (char *) page;
	rdata[1].len = BLCKSZ;
	rdata[1].buffer = InvalidBuffer;
	rdata[1].next = NULL;

	recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_NEWPAGE, rdata);

	PageSetLSN(page, recptr);
	PageSetTLI(page, ThisTimeLineID);

	END_CRIT_SECTION();

	return recptr;
}

/*
 * Handles CLEAN and CLEAN_MOVE record types
 */
static void
heap_xlog_clean(XLogRecPtr lsn, XLogRecord *record, bool clean_move)
{
	xl_heap_clean *xlrec = (xl_heap_clean *) XLogRecGetData(record);
	Relation	reln;
	Buffer		buffer;
	Page		page;
	OffsetNumber *offnum;
	OffsetNumber *end;
	int			nredirected;
	int			ndead;
	int			i;

	if (record->xl_info & XLR_BKP_BLOCK_1)
		return;

	reln = XLogOpenRelation(xlrec->node);
	buffer = XLogReadBuffer(reln, xlrec->block, false);
	if (!BufferIsValid(buffer))
		return;
	page = (Page) BufferGetPage(buffer);

	if (XLByteLE(lsn, PageGetLSN(page)))
	{
		UnlockReleaseBuffer(buffer);
		return;
	}

	nredirected = xlrec->nredirected;
	ndead = xlrec->ndead;
	offnum = (OffsetNumber *) ((char *) xlrec + SizeOfHeapClean);
	end = (OffsetNumber *) ((char *) xlrec + record->xl_len);

	/* Update all redirected or moved line pointers */
	for (i = 0; i < nredirected; i++)
	{
		OffsetNumber fromoff = *offnum++;
		OffsetNumber tooff = *offnum++;
		ItemId		fromlp = PageGetItemId(page, fromoff);

		if (clean_move)
		{
			/* Physically move the "to" item to the "from" slot */
			ItemId		tolp = PageGetItemId(page, tooff);
			HeapTupleHeader htup;

			*fromlp = *tolp;
			ItemIdSetUnused(tolp);

			/* We also have to clear the tuple's heap-only bit */
			Assert(ItemIdIsNormal(fromlp));
			htup = (HeapTupleHeader) PageGetItem(page, fromlp);
			Assert(HeapTupleHeaderIsHeapOnly(htup));
			HeapTupleHeaderClearHeapOnly(htup);
		}
		else
		{
			/* Just insert a REDIRECT link at fromoff */
			ItemIdSetRedirect(fromlp, tooff);
		}
	}

	/* Update all now-dead line pointers */
	for (i = 0; i < ndead; i++)
	{
		OffsetNumber off = *offnum++;
		ItemId		lp = PageGetItemId(page, off);

		ItemIdSetDead(lp);
	}

	/* Update all now-unused line pointers */
	while (offnum < end)
	{
		OffsetNumber off = *offnum++;
		ItemId		lp = PageGetItemId(page, off);

		ItemIdSetUnused(lp);
	}

	/*
	 * Finally, repair any fragmentation, and update the page's hint bit about
	 * whether it has free pointers.
	 */
	PageRepairFragmentation(page);

	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);
	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);
}

static void
heap_xlog_freeze(XLogRecPtr lsn, XLogRecord *record)
{
	xl_heap_freeze *xlrec = (xl_heap_freeze *) XLogRecGetData(record);
	TransactionId cutoff_xid = xlrec->cutoff_xid;
	Relation	reln;
	Buffer		buffer;
	Page		page;

	if (record->xl_info & XLR_BKP_BLOCK_1)
		return;

	reln = XLogOpenRelation(xlrec->node);
	buffer = XLogReadBuffer(reln, xlrec->block, false);
	if (!BufferIsValid(buffer))
		return;
	page = (Page) BufferGetPage(buffer);

	if (XLByteLE(lsn, PageGetLSN(page)))
	{
		UnlockReleaseBuffer(buffer);
		return;
	}

	if (record->xl_len > SizeOfHeapFreeze)
	{
		OffsetNumber *offsets;
		OffsetNumber *offsets_end;

		offsets = (OffsetNumber *) ((char *) xlrec + SizeOfHeapFreeze);
		offsets_end = (OffsetNumber *) ((char *) xlrec + record->xl_len);

		while (offsets < offsets_end)
		{
			/* offsets[] entries are one-based */
			ItemId		lp = PageGetItemId(page, *offsets);
			HeapTupleHeader tuple = (HeapTupleHeader) PageGetItem(page, lp);

			(void) heap_freeze_tuple(tuple, cutoff_xid, InvalidBuffer);
			offsets++;
		}
	}

	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);
	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);
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
	reln = XLogOpenRelation(xlrec->node);
	buffer = XLogReadBuffer(reln, xlrec->blkno, true);
	Assert(BufferIsValid(buffer));
	page = (Page) BufferGetPage(buffer);

	Assert(record->xl_len == SizeOfHeapNewpage + BLCKSZ);
	memcpy(page, (char *) xlrec + SizeOfHeapNewpage, BLCKSZ);

	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);
	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);
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
	buffer = XLogReadBuffer(reln,
							ItemPointerGetBlockNumber(&(xlrec->target.tid)),
							false);
	if (!BufferIsValid(buffer))
		return;
	page = (Page) BufferGetPage(buffer);

	if (XLByteLE(lsn, PageGetLSN(page)))		/* changes are applied */
	{
		UnlockReleaseBuffer(buffer);
		return;
	}

	offnum = ItemPointerGetOffsetNumber(&(xlrec->target.tid));
	if (PageGetMaxOffsetNumber(page) >= offnum)
		lp = PageGetItemId(page, offnum);

	if (PageGetMaxOffsetNumber(page) < offnum || !ItemIdIsNormal(lp))
		elog(PANIC, "heap_delete_redo: invalid lp");

	htup = (HeapTupleHeader) PageGetItem(page, lp);

	htup->t_infomask &= ~(HEAP_XMAX_COMMITTED |
						  HEAP_XMAX_INVALID |
						  HEAP_XMAX_IS_MULTI |
						  HEAP_IS_LOCKED |
						  HEAP_MOVED);
	HeapTupleHeaderClearHotUpdated(htup);
	HeapTupleHeaderSetXmax(htup, record->xl_xid);
	HeapTupleHeaderSetCmax(htup, FirstCommandId, false);

	/* Mark the page as a candidate for pruning */
	PageSetPrunable(page, record->xl_xid);

	/* Make sure there is no forward chain link in t_ctid */
	htup->t_ctid = xlrec->target.tid;
	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);
	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);
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
		char		data[MaxHeapTupleSize];
	}			tbuf;
	HeapTupleHeader htup;
	xl_heap_header xlhdr;
	uint32		newlen;

	if (record->xl_info & XLR_BKP_BLOCK_1)
		return;

	reln = XLogOpenRelation(xlrec->target.node);

	if (record->xl_info & XLOG_HEAP_INIT_PAGE)
	{
		buffer = XLogReadBuffer(reln,
							 ItemPointerGetBlockNumber(&(xlrec->target.tid)),
								true);
		Assert(BufferIsValid(buffer));
		page = (Page) BufferGetPage(buffer);

		PageInit(page, BufferGetPageSize(buffer), 0);
	}
	else
	{
		buffer = XLogReadBuffer(reln,
							 ItemPointerGetBlockNumber(&(xlrec->target.tid)),
								false);
		if (!BufferIsValid(buffer))
			return;
		page = (Page) BufferGetPage(buffer);

		if (XLByteLE(lsn, PageGetLSN(page)))	/* changes are applied */
		{
			UnlockReleaseBuffer(buffer);
			return;
		}
	}

	offnum = ItemPointerGetOffsetNumber(&(xlrec->target.tid));
	if (PageGetMaxOffsetNumber(page) + 1 < offnum)
		elog(PANIC, "heap_insert_redo: invalid max offset number");

	newlen = record->xl_len - SizeOfHeapInsert - SizeOfHeapHeader;
	Assert(newlen <= MaxHeapTupleSize);
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
	htup->t_infomask2 = xlhdr.t_infomask2;
	htup->t_infomask = xlhdr.t_infomask;
	htup->t_hoff = xlhdr.t_hoff;
	HeapTupleHeaderSetXmin(htup, record->xl_xid);
	HeapTupleHeaderSetCmin(htup, FirstCommandId);
	htup->t_ctid = xlrec->target.tid;

	offnum = PageAddItem(page, (Item) htup, newlen, offnum, true, true);
	if (offnum == InvalidOffsetNumber)
		elog(PANIC, "heap_insert_redo: failed to add tuple");
	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);
	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);
}

/*
 * Handles UPDATE, HOT_UPDATE & MOVE
 */
static void
heap_xlog_update(XLogRecPtr lsn, XLogRecord *record, bool move, bool hot_update)
{
	xl_heap_update *xlrec = (xl_heap_update *) XLogRecGetData(record);
	Relation	reln = XLogOpenRelation(xlrec->target.node);
	Buffer		buffer;
	bool		samepage = (ItemPointerGetBlockNumber(&(xlrec->newtid)) ==
							ItemPointerGetBlockNumber(&(xlrec->target.tid)));
	Page		page;
	OffsetNumber offnum;
	ItemId		lp = NULL;
	HeapTupleHeader htup;
	struct
	{
		HeapTupleHeaderData hdr;
		char		data[MaxHeapTupleSize];
	}			tbuf;
	xl_heap_header xlhdr;
	int			hsize;
	uint32		newlen;

	if (record->xl_info & XLR_BKP_BLOCK_1)
	{
		if (samepage)
			return;				/* backup block covered both changes */
		goto newt;
	}

	/* Deal with old tuple version */

	buffer = XLogReadBuffer(reln,
							ItemPointerGetBlockNumber(&(xlrec->target.tid)),
							false);
	if (!BufferIsValid(buffer))
		goto newt;
	page = (Page) BufferGetPage(buffer);

	if (XLByteLE(lsn, PageGetLSN(page)))		/* changes are applied */
	{
		UnlockReleaseBuffer(buffer);
		if (samepage)
			return;
		goto newt;
	}

	offnum = ItemPointerGetOffsetNumber(&(xlrec->target.tid));
	if (PageGetMaxOffsetNumber(page) >= offnum)
		lp = PageGetItemId(page, offnum);

	if (PageGetMaxOffsetNumber(page) < offnum || !ItemIdIsNormal(lp))
		elog(PANIC, "heap_update_redo: invalid lp");

	htup = (HeapTupleHeader) PageGetItem(page, lp);

	if (move)
	{
		htup->t_infomask &= ~(HEAP_XMIN_COMMITTED |
							  HEAP_XMIN_INVALID |
							  HEAP_MOVED_IN);
		htup->t_infomask |= HEAP_MOVED_OFF;
		HeapTupleHeaderClearHotUpdated(htup);
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
		if (hot_update)
			HeapTupleHeaderSetHotUpdated(htup);
		else
			HeapTupleHeaderClearHotUpdated(htup);
		HeapTupleHeaderSetXmax(htup, record->xl_xid);
		HeapTupleHeaderSetCmax(htup, FirstCommandId, false);
		/* Set forward chain link in t_ctid */
		htup->t_ctid = xlrec->newtid;
	}

	/* Mark the page as a candidate for pruning */
	PageSetPrunable(page, record->xl_xid);

	/*
	 * this test is ugly, but necessary to avoid thinking that insert change
	 * is already applied
	 */
	if (samepage)
		goto newsame;
	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);
	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);

	/* Deal with new tuple */

newt:;

	if (record->xl_info & XLR_BKP_BLOCK_2)
		return;

	if (record->xl_info & XLOG_HEAP_INIT_PAGE)
	{
		buffer = XLogReadBuffer(reln,
								ItemPointerGetBlockNumber(&(xlrec->newtid)),
								true);
		Assert(BufferIsValid(buffer));
		page = (Page) BufferGetPage(buffer);

		PageInit(page, BufferGetPageSize(buffer), 0);
	}
	else
	{
		buffer = XLogReadBuffer(reln,
								ItemPointerGetBlockNumber(&(xlrec->newtid)),
								false);
		if (!BufferIsValid(buffer))
			return;
		page = (Page) BufferGetPage(buffer);

		if (XLByteLE(lsn, PageGetLSN(page)))	/* changes are applied */
		{
			UnlockReleaseBuffer(buffer);
			return;
		}
	}

newsame:;

	offnum = ItemPointerGetOffsetNumber(&(xlrec->newtid));
	if (PageGetMaxOffsetNumber(page) + 1 < offnum)
		elog(PANIC, "heap_update_redo: invalid max offset number");

	hsize = SizeOfHeapUpdate + SizeOfHeapHeader;
	if (move)
		hsize += (2 * sizeof(TransactionId));

	newlen = record->xl_len - hsize;
	Assert(newlen <= MaxHeapTupleSize);
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
	htup->t_infomask2 = xlhdr.t_infomask2;
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

	offnum = PageAddItem(page, (Item) htup, newlen, offnum, true, true);
	if (offnum == InvalidOffsetNumber)
		elog(PANIC, "heap_update_redo: failed to add tuple");
	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);
	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);
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
	buffer = XLogReadBuffer(reln,
							ItemPointerGetBlockNumber(&(xlrec->target.tid)),
							false);
	if (!BufferIsValid(buffer))
		return;
	page = (Page) BufferGetPage(buffer);

	if (XLByteLE(lsn, PageGetLSN(page)))		/* changes are applied */
	{
		UnlockReleaseBuffer(buffer);
		return;
	}

	offnum = ItemPointerGetOffsetNumber(&(xlrec->target.tid));
	if (PageGetMaxOffsetNumber(page) >= offnum)
		lp = PageGetItemId(page, offnum);

	if (PageGetMaxOffsetNumber(page) < offnum || !ItemIdIsNormal(lp))
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
	HeapTupleHeaderClearHotUpdated(htup);
	HeapTupleHeaderSetXmax(htup, xlrec->locking_xid);
	HeapTupleHeaderSetCmax(htup, FirstCommandId, false);
	/* Make sure there is no forward chain link in t_ctid */
	htup->t_ctid = xlrec->target.tid;
	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);
	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);
}

static void
heap_xlog_inplace(XLogRecPtr lsn, XLogRecord *record)
{
	xl_heap_inplace *xlrec = (xl_heap_inplace *) XLogRecGetData(record);
	Relation	reln = XLogOpenRelation(xlrec->target.node);
	Buffer		buffer;
	Page		page;
	OffsetNumber offnum;
	ItemId		lp = NULL;
	HeapTupleHeader htup;
	uint32		oldlen;
	uint32		newlen;

	if (record->xl_info & XLR_BKP_BLOCK_1)
		return;

	buffer = XLogReadBuffer(reln,
							ItemPointerGetBlockNumber(&(xlrec->target.tid)),
							false);
	if (!BufferIsValid(buffer))
		return;
	page = (Page) BufferGetPage(buffer);

	if (XLByteLE(lsn, PageGetLSN(page)))		/* changes are applied */
	{
		UnlockReleaseBuffer(buffer);
		return;
	}

	offnum = ItemPointerGetOffsetNumber(&(xlrec->target.tid));
	if (PageGetMaxOffsetNumber(page) >= offnum)
		lp = PageGetItemId(page, offnum);

	if (PageGetMaxOffsetNumber(page) < offnum || !ItemIdIsNormal(lp))
		elog(PANIC, "heap_inplace_redo: invalid lp");

	htup = (HeapTupleHeader) PageGetItem(page, lp);

	oldlen = ItemIdGetLength(lp) - htup->t_hoff;
	newlen = record->xl_len - SizeOfHeapInplace;
	if (oldlen != newlen)
		elog(PANIC, "heap_inplace_redo: wrong tuple length");

	memcpy((char *) htup + htup->t_hoff,
		   (char *) xlrec + SizeOfHeapInplace,
		   newlen);

	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);
	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);
}

void
heap_redo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;

	switch (info & XLOG_HEAP_OPMASK)
	{
		case XLOG_HEAP_INSERT:
			heap_xlog_insert(lsn, record);
			break;
		case XLOG_HEAP_DELETE:
			heap_xlog_delete(lsn, record);
			break;
		case XLOG_HEAP_UPDATE:
			heap_xlog_update(lsn, record, false, false);
			break;
		case XLOG_HEAP_MOVE:
			heap_xlog_update(lsn, record, true, false);
			break;
		case XLOG_HEAP_HOT_UPDATE:
			heap_xlog_update(lsn, record, false, true);
			break;
		case XLOG_HEAP_NEWPAGE:
			heap_xlog_newpage(lsn, record);
			break;
		case XLOG_HEAP_LOCK:
			heap_xlog_lock(lsn, record);
			break;
		case XLOG_HEAP_INPLACE:
			heap_xlog_inplace(lsn, record);
			break;
		default:
			elog(PANIC, "heap_redo: unknown op code %u", info);
	}
}

void
heap2_redo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;

	switch (info & XLOG_HEAP_OPMASK)
	{
		case XLOG_HEAP2_FREEZE:
			heap_xlog_freeze(lsn, record);
			break;
		case XLOG_HEAP2_CLEAN:
			heap_xlog_clean(lsn, record, false);
			break;
		case XLOG_HEAP2_CLEAN_MOVE:
			heap_xlog_clean(lsn, record, true);
			break;
		default:
			elog(PANIC, "heap2_redo: unknown op code %u", info);
	}
}

static void
out_target(StringInfo buf, xl_heaptid *target)
{
	appendStringInfo(buf, "rel %u/%u/%u; tid %u/%u",
			 target->node.spcNode, target->node.dbNode, target->node.relNode,
					 ItemPointerGetBlockNumber(&(target->tid)),
					 ItemPointerGetOffsetNumber(&(target->tid)));
}

void
heap_desc(StringInfo buf, uint8 xl_info, char *rec)
{
	uint8		info = xl_info & ~XLR_INFO_MASK;

	info &= XLOG_HEAP_OPMASK;
	if (info == XLOG_HEAP_INSERT)
	{
		xl_heap_insert *xlrec = (xl_heap_insert *) rec;

		if (xl_info & XLOG_HEAP_INIT_PAGE)
			appendStringInfo(buf, "insert(init): ");
		else
			appendStringInfo(buf, "insert: ");
		out_target(buf, &(xlrec->target));
	}
	else if (info == XLOG_HEAP_DELETE)
	{
		xl_heap_delete *xlrec = (xl_heap_delete *) rec;

		appendStringInfo(buf, "delete: ");
		out_target(buf, &(xlrec->target));
	}
	else if (info == XLOG_HEAP_UPDATE)
	{
		xl_heap_update *xlrec = (xl_heap_update *) rec;

		if (xl_info & XLOG_HEAP_INIT_PAGE)
			appendStringInfo(buf, "update(init): ");
		else
			appendStringInfo(buf, "update: ");
		out_target(buf, &(xlrec->target));
		appendStringInfo(buf, "; new %u/%u",
						 ItemPointerGetBlockNumber(&(xlrec->newtid)),
						 ItemPointerGetOffsetNumber(&(xlrec->newtid)));
	}
	else if (info == XLOG_HEAP_MOVE)
	{
		xl_heap_update *xlrec = (xl_heap_update *) rec;

		if (xl_info & XLOG_HEAP_INIT_PAGE)
			appendStringInfo(buf, "move(init): ");
		else
			appendStringInfo(buf, "move: ");
		out_target(buf, &(xlrec->target));
		appendStringInfo(buf, "; new %u/%u",
						 ItemPointerGetBlockNumber(&(xlrec->newtid)),
						 ItemPointerGetOffsetNumber(&(xlrec->newtid)));
	}
	else if (info == XLOG_HEAP_HOT_UPDATE)
	{
		xl_heap_update *xlrec = (xl_heap_update *) rec;

		if (xl_info & XLOG_HEAP_INIT_PAGE)		/* can this case happen? */
			appendStringInfo(buf, "hot_update(init): ");
		else
			appendStringInfo(buf, "hot_update: ");
		out_target(buf, &(xlrec->target));
		appendStringInfo(buf, "; new %u/%u",
						 ItemPointerGetBlockNumber(&(xlrec->newtid)),
						 ItemPointerGetOffsetNumber(&(xlrec->newtid)));
	}
	else if (info == XLOG_HEAP_NEWPAGE)
	{
		xl_heap_newpage *xlrec = (xl_heap_newpage *) rec;

		appendStringInfo(buf, "newpage: rel %u/%u/%u; blk %u",
						 xlrec->node.spcNode, xlrec->node.dbNode,
						 xlrec->node.relNode, xlrec->blkno);
	}
	else if (info == XLOG_HEAP_LOCK)
	{
		xl_heap_lock *xlrec = (xl_heap_lock *) rec;

		if (xlrec->shared_lock)
			appendStringInfo(buf, "shared_lock: ");
		else
			appendStringInfo(buf, "exclusive_lock: ");
		if (xlrec->xid_is_mxact)
			appendStringInfo(buf, "mxid ");
		else
			appendStringInfo(buf, "xid ");
		appendStringInfo(buf, "%u ", xlrec->locking_xid);
		out_target(buf, &(xlrec->target));
	}
	else if (info == XLOG_HEAP_INPLACE)
	{
		xl_heap_inplace *xlrec = (xl_heap_inplace *) rec;

		appendStringInfo(buf, "inplace: ");
		out_target(buf, &(xlrec->target));
	}
	else
		appendStringInfo(buf, "UNKNOWN");
}

void
heap2_desc(StringInfo buf, uint8 xl_info, char *rec)
{
	uint8		info = xl_info & ~XLR_INFO_MASK;

	info &= XLOG_HEAP_OPMASK;
	if (info == XLOG_HEAP2_FREEZE)
	{
		xl_heap_freeze *xlrec = (xl_heap_freeze *) rec;

		appendStringInfo(buf, "freeze: rel %u/%u/%u; blk %u; cutoff %u",
						 xlrec->node.spcNode, xlrec->node.dbNode,
						 xlrec->node.relNode, xlrec->block,
						 xlrec->cutoff_xid);
	}
	else if (info == XLOG_HEAP2_CLEAN)
	{
		xl_heap_clean *xlrec = (xl_heap_clean *) rec;

		appendStringInfo(buf, "clean: rel %u/%u/%u; blk %u",
						 xlrec->node.spcNode, xlrec->node.dbNode,
						 xlrec->node.relNode, xlrec->block);
	}
	else if (info == XLOG_HEAP2_CLEAN_MOVE)
	{
		xl_heap_clean *xlrec = (xl_heap_clean *) rec;

		appendStringInfo(buf, "clean_move: rel %u/%u/%u; blk %u",
						 xlrec->node.spcNode, xlrec->node.dbNode,
						 xlrec->node.relNode, xlrec->block);
	}
	else
		appendStringInfo(buf, "UNKNOWN");
}

/*
 *	heap_sync		- sync a heap, for use when no WAL has been written
 *
 * This forces the heap contents (including TOAST heap if any) down to disk.
 * If we skipped using WAL, and it's not a temp relation, we must force the
 * relation down to disk before it's safe to commit the transaction.  This
 * requires writing out any dirty buffers and then doing a forced fsync.
 *
 * Indexes are not touched.  (Currently, index operations associated with
 * the commands that use this are WAL-logged and so do not need fsync.
 * That behavior might change someday, but in any case it's likely that
 * any fsync decisions required would be per-index and hence not appropriate
 * to be done here.)
 */
void
heap_sync(Relation rel)
{
	/* temp tables never need fsync */
	if (rel->rd_istemp)
		return;

	/* main heap */
	FlushRelationBuffers(rel);
	/* FlushRelationBuffers will have opened rd_smgr */
	smgrimmedsync(rel->rd_smgr);

	/* toast heap, if any */
	if (OidIsValid(rel->rd_rel->reltoastrelid))
	{
		Relation	toastrel;

		toastrel = heap_open(rel->rd_rel->reltoastrelid, AccessShareLock);
		FlushRelationBuffers(toastrel);
		smgrimmedsync(toastrel->rd_smgr);
		heap_close(toastrel, AccessShareLock);
	}
}
