/*-------------------------------------------------------------------------
 *
 * heapam.c
 *	  heap access method code
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/heap/heapam.c
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
 *		heap_multi_insert - insert multiple tuples into a relation
 *		heap_delete		- delete a tuple from a relation
 *		heap_update		- replace a tuple in a relation with another tuple
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
#include "access/heapam_xlog.h"
#include "access/hio.h"
#include "access/multixact.h"
#include "access/parallel.h"
#include "access/relscan.h"
#include "access/sysattr.h"
#include "access/transam.h"
#include "access/tuptoaster.h"
#include "access/valid.h"
#include "access/visibilitymap.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/freespace.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "storage/spin.h"
#include "storage/standby.h"
#include "utils/datum.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/relcache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/tqual.h"


/* GUC variable */
bool		synchronize_seqscans = true;


static HeapScanDesc heap_beginscan_internal(Relation relation,
						Snapshot snapshot,
						int nkeys, ScanKey key,
						ParallelHeapScanDesc parallel_scan,
						bool allow_strat,
						bool allow_sync,
						bool allow_pagemode,
						bool is_bitmapscan,
						bool is_samplescan,
						bool temp_snap);
static BlockNumber heap_parallelscan_nextpage(HeapScanDesc scan);
static HeapTuple heap_prepare_insert(Relation relation, HeapTuple tup,
					TransactionId xid, CommandId cid, int options);
static XLogRecPtr log_heap_update(Relation reln, Buffer oldbuf,
				Buffer newbuf, HeapTuple oldtup,
				HeapTuple newtup, HeapTuple old_key_tup,
				bool all_visible_cleared, bool new_all_visible_cleared);
static void HeapSatisfiesHOTandKeyUpdate(Relation relation,
							 Bitmapset *hot_attrs,
							 Bitmapset *key_attrs, Bitmapset *id_attrs,
							 bool *satisfies_hot, bool *satisfies_key,
							 bool *satisfies_id,
							 HeapTuple oldtup, HeapTuple newtup);
static bool heap_acquire_tuplock(Relation relation, ItemPointer tid,
					 LockTupleMode mode, LockWaitPolicy wait_policy,
					 bool *have_tuple_lock);
static void compute_new_xmax_infomask(TransactionId xmax, uint16 old_infomask,
						  uint16 old_infomask2, TransactionId add_to_xmax,
						  LockTupleMode mode, bool is_update,
						  TransactionId *result_xmax, uint16 *result_infomask,
						  uint16 *result_infomask2);
static HTSU_Result heap_lock_updated_tuple(Relation rel, HeapTuple tuple,
						ItemPointer ctid, TransactionId xid,
						LockTupleMode mode);
static void GetMultiXactIdHintBits(MultiXactId multi, uint16 *new_infomask,
					   uint16 *new_infomask2);
static TransactionId MultiXactIdGetUpdateXid(TransactionId xmax,
						uint16 t_infomask);
static bool DoesMultiXactIdConflict(MultiXactId multi, uint16 infomask,
						LockTupleMode lockmode);
static void MultiXactIdWait(MultiXactId multi, MultiXactStatus status, uint16 infomask,
				Relation rel, ItemPointer ctid, XLTW_Oper oper,
				int *remaining);
static bool ConditionalMultiXactIdWait(MultiXactId multi, MultiXactStatus status,
						   uint16 infomask, Relation rel, int *remaining);
static XLogRecPtr log_heap_new_cid(Relation relation, HeapTuple tup);
static HeapTuple ExtractReplicaIdentity(Relation rel, HeapTuple tup, bool key_modified,
					   bool *copy);


/*
 * Each tuple lock mode has a corresponding heavyweight lock, and one or two
 * corresponding MultiXactStatuses (one to merely lock tuples, another one to
 * update them).  This table (and the macros below) helps us determine the
 * heavyweight lock mode and MultiXactStatus values to use for any particular
 * tuple lock strength.
 *
 * Don't look at lockstatus/updstatus directly!  Use get_mxact_status_for_lock
 * instead.
 */
static const struct
{
	LOCKMODE	hwlock;
	int			lockstatus;
	int			updstatus;
}

			tupleLockExtraInfo[MaxLockTupleMode + 1] =
{
	{							/* LockTupleKeyShare */
		AccessShareLock,
		MultiXactStatusForKeyShare,
		-1						/* KeyShare does not allow updating tuples */
	},
	{							/* LockTupleShare */
		RowShareLock,
		MultiXactStatusForShare,
		-1						/* Share does not allow updating tuples */
	},
	{							/* LockTupleNoKeyExclusive */
		ExclusiveLock,
		MultiXactStatusForNoKeyUpdate,
		MultiXactStatusNoKeyUpdate
	},
	{							/* LockTupleExclusive */
		AccessExclusiveLock,
		MultiXactStatusForUpdate,
		MultiXactStatusUpdate
	}
};

/* Get the LOCKMODE for a given MultiXactStatus */
#define LOCKMODE_from_mxstatus(status) \
			(tupleLockExtraInfo[TUPLOCK_from_mxstatus((status))].hwlock)

/*
 * Acquire heavyweight locks on tuples, using a LockTupleMode strength value.
 * This is more readable than having every caller translate it to lock.h's
 * LOCKMODE.
 */
#define LockTupleTuplock(rel, tup, mode) \
	LockTuple((rel), (tup), tupleLockExtraInfo[mode].hwlock)
#define UnlockTupleTuplock(rel, tup, mode) \
	UnlockTuple((rel), (tup), tupleLockExtraInfo[mode].hwlock)
#define ConditionalLockTupleTuplock(rel, tup, mode) \
	ConditionalLockTuple((rel), (tup), tupleLockExtraInfo[mode].hwlock)

/*
 * This table maps tuple lock strength values for each particular
 * MultiXactStatus value.
 */
static const int MultiXactStatusLock[MaxMultiXactStatus + 1] =
{
	LockTupleKeyShare,			/* ForKeyShare */
	LockTupleShare,				/* ForShare */
	LockTupleNoKeyExclusive,	/* ForNoKeyUpdate */
	LockTupleExclusive,			/* ForUpdate */
	LockTupleNoKeyExclusive,	/* NoKeyUpdate */
	LockTupleExclusive			/* Update */
};

/* Get the LockTupleMode for a given MultiXactStatus */
#define TUPLOCK_from_mxstatus(status) \
			(MultiXactStatusLock[(status)])

/* ----------------------------------------------------------------
 *						 heap support routines
 * ----------------------------------------------------------------
 */

/* ----------------
 *		initscan - scan code common to heap_beginscan and heap_rescan
 * ----------------
 */
static void
initscan(HeapScanDesc scan, ScanKey key, bool keep_startblock)
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
	 * might go into pages we already scanned.  To guarantee consistent
	 * results for a non-MVCC snapshot, the caller must hold some higher-level
	 * lock that ensures the interesting tuple(s) won't change.)
	 */
	if (scan->rs_parallel != NULL)
		scan->rs_nblocks = scan->rs_parallel->phs_nblocks;
	else
		scan->rs_nblocks = RelationGetNumberOfBlocks(scan->rs_rd);

	/*
	 * If the table is large relative to NBuffers, use a bulk-read access
	 * strategy and enable synchronized scanning (see syncscan.c).  Although
	 * the thresholds for these features could be different, we make them the
	 * same so that there are only two behaviors to tune rather than four.
	 * (However, some callers need to be able to disable one or both of these
	 * behaviors, independently of the size of the table; also there is a GUC
	 * variable that can disable synchronized scanning.)
	 *
	 * Note that heap_parallelscan_initialize has a very similar test; if you
	 * change this, consider changing that one, too.
	 */
	if (!RelationUsesLocalBuffers(scan->rs_rd) &&
		scan->rs_nblocks > NBuffers / 4)
	{
		allow_strat = scan->rs_allow_strat;
		allow_sync = scan->rs_allow_sync;
	}
	else
		allow_strat = allow_sync = false;

	if (allow_strat)
	{
		/* During a rescan, keep the previous strategy object. */
		if (scan->rs_strategy == NULL)
			scan->rs_strategy = GetAccessStrategy(BAS_BULKREAD);
	}
	else
	{
		if (scan->rs_strategy != NULL)
			FreeAccessStrategy(scan->rs_strategy);
		scan->rs_strategy = NULL;
	}

	if (scan->rs_parallel != NULL)
	{
		/* For parallel scan, believe whatever ParallelHeapScanDesc says. */
		scan->rs_syncscan = scan->rs_parallel->phs_syncscan;
	}
	else if (keep_startblock)
	{
		/*
		 * When rescanning, we want to keep the previous startblock setting,
		 * so that rewinding a cursor doesn't generate surprising results.
		 * Reset the active syncscan setting, though.
		 */
		scan->rs_syncscan = (allow_sync && synchronize_seqscans);
	}
	else if (allow_sync && synchronize_seqscans)
	{
		scan->rs_syncscan = true;
		scan->rs_startblock = ss_get_location(scan->rs_rd, scan->rs_nblocks);
	}
	else
	{
		scan->rs_syncscan = false;
		scan->rs_startblock = 0;
	}

	scan->rs_numblocks = InvalidBlockNumber;
	scan->rs_inited = false;
	scan->rs_ctup.t_data = NULL;
	ItemPointerSetInvalid(&scan->rs_ctup.t_self);
	scan->rs_cbuf = InvalidBuffer;
	scan->rs_cblock = InvalidBlockNumber;

	/* page-at-a-time fields are always invalid when not rs_inited */

	/*
	 * copy the scan key, if appropriate
	 */
	if (key != NULL)
		memcpy(scan->rs_key, key, scan->rs_nkeys * sizeof(ScanKeyData));

	/*
	 * Currently, we don't have a stats counter for bitmap heap scans (but the
	 * underlying bitmap index scans will be counted) or sample scans (we only
	 * update stats for tuple fetches there)
	 */
	if (!scan->rs_bitmapscan && !scan->rs_samplescan)
		pgstat_count_heap_scan(scan->rs_rd);
}

/*
 * heap_setscanlimits - restrict range of a heapscan
 *
 * startBlk is the page to start at
 * numBlks is number of pages to scan (InvalidBlockNumber means "all")
 */
void
heap_setscanlimits(HeapScanDesc scan, BlockNumber startBlk, BlockNumber numBlks)
{
	Assert(!scan->rs_inited);	/* else too late to change */
	Assert(!scan->rs_syncscan); /* else rs_startblock is significant */

	/* Check startBlk is valid (but allow case of zero blocks...) */
	Assert(startBlk == 0 || startBlk < scan->rs_nblocks);

	scan->rs_startblock = startBlk;
	scan->rs_numblocks = numBlks;
}

/*
 * heapgetpage - subroutine for heapgettup()
 *
 * This routine reads and pins the specified page of the relation.
 * In page-at-a-time mode it performs additional work, namely determining
 * which tuples on the page are visible.
 */
void
heapgetpage(HeapScanDesc scan, BlockNumber page)
{
	Buffer		buffer;
	Snapshot	snapshot;
	Page		dp;
	int			lines;
	int			ntup;
	OffsetNumber lineoff;
	ItemId		lpp;
	bool		all_visible;

	Assert(page < scan->rs_nblocks);

	/* release previous scan buffer, if any */
	if (BufferIsValid(scan->rs_cbuf))
	{
		ReleaseBuffer(scan->rs_cbuf);
		scan->rs_cbuf = InvalidBuffer;
	}

	/*
	 * Be sure to check for interrupts at least once per page.  Checks at
	 * higher code levels won't be able to stop a seqscan that encounters many
	 * pages' worth of consecutive dead tuples.
	 */
	CHECK_FOR_INTERRUPTS();

	/* read page using selected strategy */
	scan->rs_cbuf = ReadBufferExtended(scan->rs_rd, MAIN_FORKNUM, page,
									   RBM_NORMAL, scan->rs_strategy);
	scan->rs_cblock = page;

	if (!scan->rs_pageatatime)
		return;

	buffer = scan->rs_cbuf;
	snapshot = scan->rs_snapshot;

	/*
	 * Prune and repair fragmentation for the whole page, if possible.
	 */
	heap_page_prune_opt(scan->rs_rd, buffer);

	/*
	 * We must hold share lock on the buffer content while examining tuple
	 * visibility.  Afterwards, however, the tuples we have found to be
	 * visible are guaranteed good as long as we hold the buffer pin.
	 */
	LockBuffer(buffer, BUFFER_LOCK_SHARE);

	dp = (Page) BufferGetPage(buffer);
	lines = PageGetMaxOffsetNumber(dp);
	ntup = 0;

	/*
	 * If the all-visible flag indicates that all tuples on the page are
	 * visible to everyone, we can skip the per-tuple visibility tests.
	 *
	 * Note: In hot standby, a tuple that's already visible to all
	 * transactions in the master might still be invisible to a read-only
	 * transaction in the standby. We partly handle this problem by tracking
	 * the minimum xmin of visible tuples as the cut-off XID while marking a
	 * page all-visible on master and WAL log that along with the visibility
	 * map SET operation. In hot standby, we wait for (or abort) all
	 * transactions that can potentially may not see one or more tuples on the
	 * page. That's how index-only scans work fine in hot standby. A crucial
	 * difference between index-only scans and heap scans is that the
	 * index-only scan completely relies on the visibility map where as heap
	 * scan looks at the page-level PD_ALL_VISIBLE flag. We are not sure if
	 * the page-level flag can be trusted in the same way, because it might
	 * get propagated somehow without being explicitly WAL-logged, e.g. via a
	 * full page write. Until we can prove that beyond doubt, let's check each
	 * tuple for visibility the hard way.
	 */
	all_visible = PageIsAllVisible(dp) && !snapshot->takenDuringRecovery;

	for (lineoff = FirstOffsetNumber, lpp = PageGetItemId(dp, lineoff);
		 lineoff <= lines;
		 lineoff++, lpp++)
	{
		if (ItemIdIsNormal(lpp))
		{
			HeapTupleData loctup;
			bool		valid;

			loctup.t_tableOid = RelationGetRelid(scan->rs_rd);
			loctup.t_data = (HeapTupleHeader) PageGetItem((Page) dp, lpp);
			loctup.t_len = ItemIdGetLength(lpp);
			ItemPointerSet(&(loctup.t_self), page, lineoff);

			if (all_visible)
				valid = true;
			else
				valid = HeapTupleSatisfiesVisibility(&loctup, snapshot, buffer);

			CheckForSerializableConflictOut(valid, scan->rs_rd, &loctup,
											buffer, snapshot);

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
			if (scan->rs_nblocks == 0 || scan->rs_numblocks == 0)
			{
				Assert(!BufferIsValid(scan->rs_cbuf));
				tuple->t_data = NULL;
				return;
			}
			if (scan->rs_parallel != NULL)
			{
				page = heap_parallelscan_nextpage(scan);

				/* Other processes might have already finished the scan. */
				if (page == InvalidBlockNumber)
				{
					Assert(!BufferIsValid(scan->rs_cbuf));
					tuple->t_data = NULL;
					return;
				}
			}
			else
				page = scan->rs_startblock;		/* first page */
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
		/* backward parallel scan not supported */
		Assert(scan->rs_parallel == NULL);

		if (!scan->rs_inited)
		{
			/*
			 * return null immediately if relation is empty
			 */
			if (scan->rs_nblocks == 0 || scan->rs_numblocks == 0)
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

				CheckForSerializableConflictOut(valid, scan->rs_rd, tuple,
												scan->rs_cbuf, snapshot);

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
			finished = (page == scan->rs_startblock) ||
				(scan->rs_numblocks != InvalidBlockNumber ? --scan->rs_numblocks == 0 : false);
			if (page == 0)
				page = scan->rs_nblocks;
			page--;
		}
		else if (scan->rs_parallel != NULL)
		{
			page = heap_parallelscan_nextpage(scan);
			finished = (page == InvalidBlockNumber);
		}
		else
		{
			page++;
			if (page >= scan->rs_nblocks)
				page = 0;
			finished = (page == scan->rs_startblock) ||
				(scan->rs_numblocks != InvalidBlockNumber ? --scan->rs_numblocks == 0 : false);

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
			if (scan->rs_nblocks == 0 || scan->rs_numblocks == 0)
			{
				Assert(!BufferIsValid(scan->rs_cbuf));
				tuple->t_data = NULL;
				return;
			}
			if (scan->rs_parallel != NULL)
			{
				page = heap_parallelscan_nextpage(scan);

				/* Other processes might have already finished the scan. */
				if (page == InvalidBlockNumber)
				{
					Assert(!BufferIsValid(scan->rs_cbuf));
					tuple->t_data = NULL;
					return;
				}
			}
			else
				page = scan->rs_startblock;		/* first page */
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
		/* backward parallel scan not supported */
		Assert(scan->rs_parallel == NULL);

		if (!scan->rs_inited)
		{
			/*
			 * return null immediately if relation is empty
			 */
			if (scan->rs_nblocks == 0 || scan->rs_numblocks == 0)
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
			finished = (page == scan->rs_startblock) ||
				(scan->rs_numblocks != InvalidBlockNumber ? --scan->rs_numblocks == 0 : false);
			if (page == 0)
				page = scan->rs_nblocks;
			page--;
		}
		else if (scan->rs_parallel != NULL)
		{
			page = heap_parallelscan_nextpage(scan);
			finished = (page == InvalidBlockNumber);
		}
		else
		{
			page++;
			if (page >= scan->rs_nblocks)
				page = 0;
			finished = (page == scan->rs_startblock) ||
				(scan->rs_numblocks != InvalidBlockNumber ? --scan->rs_numblocks == 0 : false);

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
 * definition in access/htup_details.h is maintained.
 */
Datum
fastgetattr(HeapTuple tup, int attnum, TupleDesc tupleDesc,
			bool *isnull)
{
	return (
			(attnum) > 0 ?
			(
			 (*(isnull) = false),
			 HeapTupleNoNulls(tup) ?
			 (
			  (tupleDesc)->attrs[(attnum) - 1]->attcacheoff >= 0 ?
			  (
			   fetchatt((tupleDesc)->attrs[(attnum) - 1],
						(char *) (tup)->t_data + (tup)->t_data->t_hoff +
						(tupleDesc)->attrs[(attnum) - 1]->attcacheoff)
			   )
			  :
			  nocachegetattr((tup), (attnum), (tupleDesc))
			  )
			 :
			 (
			  att_isnull((attnum) - 1, (tup)->t_data->t_bits) ?
			  (
			   (*(isnull) = true),
			   (Datum) NULL
			   )
			  :
			  (
			   nocachegetattr((tup), (attnum), (tupleDesc))
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

	/* Make note that we've accessed a temporary relation */
	if (RelationUsesLocalBuffers(r))
	{
		if (IsParallelWorker())
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
					 errmsg("cannot access temporary tables during a parallel operation")));
		MyXactAccessedTempRel = true;
	}

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
	if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(relationId)))
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

	/* Make note that we've accessed a temporary relation */
	if (RelationUsesLocalBuffers(r))
	{
		if (IsParallelWorker())
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
					 errmsg("cannot access temporary tables during a parallel operation")));
		MyXactAccessedTempRel = true;
	}

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
	 * relation.  This is needed even if we already hold a lock on the
	 * relation, because GRANT/REVOKE are executed without taking any lock on
	 * the target relation, and we want to be sure we see current ACL
	 * information.  We can skip this if asked for NoLock, on the assumption
	 * that such a call is not the first one in the current command, and so we
	 * should be reasonably up-to-date already.  (XXX this all could stand to
	 * be redesigned, but for the moment we'll keep doing this like it's been
	 * done historically.)
	 */
	if (lockmode != NoLock)
		AcceptInvalidationMessages();

	/* Look up and lock the appropriate relation using namespace search */
	relOid = RangeVarGetRelid(relation, lockmode, false);

	/* Let relation_open do the rest */
	return relation_open(relOid, NoLock);
}

/* ----------------
 *		relation_openrv_extended - open any relation specified by a RangeVar
 *
 *		Same as relation_openrv, but with an additional missing_ok argument
 *		allowing a NULL return rather than an error if the relation is not
 *		found.  (Note that some other causes, such as permissions problems,
 *		will still result in an ereport.)
 * ----------------
 */
Relation
relation_openrv_extended(const RangeVar *relation, LOCKMODE lockmode,
						 bool missing_ok)
{
	Oid			relOid;

	/*
	 * Check for shared-cache-inval messages before trying to open the
	 * relation.  See comments in relation_openrv().
	 */
	if (lockmode != NoLock)
		AcceptInvalidationMessages();

	/* Look up and lock the appropriate relation using namespace search */
	relOid = RangeVarGetRelid(relation, lockmode, missing_ok);

	/* Return NULL on not-found */
	if (!OidIsValid(relOid))
		return NULL;

	/* Let relation_open do the rest */
	return relation_open(relOid, NoLock);
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
 *		check that it's not a view or foreign table before assuming it has
 *		storage.)
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
 *		heap_openrv_extended - open a heap relation specified
 *		by a RangeVar node
 *
 *		As above, but optionally return NULL instead of failing for
 *		relation-not-found.
 * ----------------
 */
Relation
heap_openrv_extended(const RangeVar *relation, LOCKMODE lockmode,
					 bool missing_ok)
{
	Relation	r;

	r = relation_openrv_extended(relation, lockmode, missing_ok);

	if (r)
	{
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
	}

	return r;
}


/* ----------------
 *		heap_beginscan	- begin relation scan
 *
 * heap_beginscan is the "standard" case.
 *
 * heap_beginscan_catalog differs in setting up its own temporary snapshot.
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
 *
 * heap_beginscan_sampling is an alternative entry point for setting up a
 * HeapScanDesc for a TABLESAMPLE scan.  As with bitmap scans, it's worth
 * using the same data structure although the behavior is rather different.
 * In addition to the options offered by heap_beginscan_strat, this call
 * also allows control of whether page-mode visibility checking is used.
 * ----------------
 */
HeapScanDesc
heap_beginscan(Relation relation, Snapshot snapshot,
			   int nkeys, ScanKey key)
{
	return heap_beginscan_internal(relation, snapshot, nkeys, key, NULL,
								   true, true, true, false, false, false);
}

HeapScanDesc
heap_beginscan_catalog(Relation relation, int nkeys, ScanKey key)
{
	Oid			relid = RelationGetRelid(relation);
	Snapshot	snapshot = RegisterSnapshot(GetCatalogSnapshot(relid));

	return heap_beginscan_internal(relation, snapshot, nkeys, key, NULL,
								   true, true, true, false, false, true);
}

HeapScanDesc
heap_beginscan_strat(Relation relation, Snapshot snapshot,
					 int nkeys, ScanKey key,
					 bool allow_strat, bool allow_sync)
{
	return heap_beginscan_internal(relation, snapshot, nkeys, key, NULL,
								   allow_strat, allow_sync, true,
								   false, false, false);
}

HeapScanDesc
heap_beginscan_bm(Relation relation, Snapshot snapshot,
				  int nkeys, ScanKey key)
{
	return heap_beginscan_internal(relation, snapshot, nkeys, key, NULL,
								   false, false, true, true, false, false);
}

HeapScanDesc
heap_beginscan_sampling(Relation relation, Snapshot snapshot,
						int nkeys, ScanKey key,
					  bool allow_strat, bool allow_sync, bool allow_pagemode)
{
	return heap_beginscan_internal(relation, snapshot, nkeys, key, NULL,
								   allow_strat, allow_sync, allow_pagemode,
								   false, true, false);
}

static HeapScanDesc
heap_beginscan_internal(Relation relation, Snapshot snapshot,
						int nkeys, ScanKey key,
						ParallelHeapScanDesc parallel_scan,
						bool allow_strat,
						bool allow_sync,
						bool allow_pagemode,
						bool is_bitmapscan,
						bool is_samplescan,
						bool temp_snap)
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
	scan->rs_samplescan = is_samplescan;
	scan->rs_strategy = NULL;	/* set in initscan */
	scan->rs_allow_strat = allow_strat;
	scan->rs_allow_sync = allow_sync;
	scan->rs_temp_snap = temp_snap;
	scan->rs_parallel = parallel_scan;

	/*
	 * we can use page-at-a-time mode if it's an MVCC-safe snapshot
	 */
	scan->rs_pageatatime = allow_pagemode && IsMVCCSnapshot(snapshot);

	/*
	 * For a seqscan in a serializable transaction, acquire a predicate lock
	 * on the entire relation. This is required not only to lock all the
	 * matching tuples, but also to conflict with new insertions into the
	 * table. In an indexscan, we take page locks on the index pages covering
	 * the range specified in the scan qual, but in a heap scan there is
	 * nothing more fine-grained to lock. A bitmap scan is a different story,
	 * there we have already scanned the index and locked the index pages
	 * covering the predicate. But in that case we still have to lock any
	 * matching heap tuples.
	 */
	if (!is_bitmapscan)
		PredicateLockRelation(relation, snapshot);

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

	initscan(scan, key, false);

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
	initscan(scan, key, true);

	/*
	 * reset parallel scan, if present
	 */
	if (scan->rs_parallel != NULL)
	{
		ParallelHeapScanDesc parallel_scan;

		/*
		 * Caller is responsible for making sure that all workers have
		 * finished the scan before calling this, so it really shouldn't be
		 * necessary to acquire the mutex at all.  We acquire it anyway, just
		 * to be tidy.
		 */
		parallel_scan = scan->rs_parallel;
		SpinLockAcquire(&parallel_scan->phs_mutex);
		parallel_scan->phs_cblock = parallel_scan->phs_startblock;
		SpinLockRelease(&parallel_scan->phs_mutex);
	}
}

/* ----------------
 *		heap_rescan_set_params	- restart a relation scan after changing params
 *
 * This call allows changing the buffer strategy, syncscan, and pagemode
 * options before starting a fresh scan.  Note that although the actual use
 * of syncscan might change (effectively, enabling or disabling reporting),
 * the previously selected startblock will be kept.
 * ----------------
 */
void
heap_rescan_set_params(HeapScanDesc scan, ScanKey key,
					   bool allow_strat, bool allow_sync, bool allow_pagemode)
{
	/* adjust parameters */
	scan->rs_allow_strat = allow_strat;
	scan->rs_allow_sync = allow_sync;
	scan->rs_pageatatime = allow_pagemode && IsMVCCSnapshot(scan->rs_snapshot);
	/* ... and rescan */
	heap_rescan(scan, key);
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

	if (scan->rs_temp_snap)
		UnregisterSnapshot(scan->rs_snapshot);

	pfree(scan);
}

/* ----------------
 *		heap_parallelscan_estimate - estimate storage for ParallelHeapScanDesc
 *
 *		Sadly, this doesn't reduce to a constant, because the size required
 *		to serialize the snapshot can vary.
 * ----------------
 */
Size
heap_parallelscan_estimate(Snapshot snapshot)
{
	return add_size(offsetof(ParallelHeapScanDescData, phs_snapshot_data),
					EstimateSnapshotSpace(snapshot));
}

/* ----------------
 *		heap_parallelscan_initialize - initialize ParallelHeapScanDesc
 *
 *		Must allow as many bytes of shared memory as returned by
 *		heap_parallelscan_estimate.  Call this just once in the leader
 *		process; then, individual workers attach via heap_beginscan_parallel.
 * ----------------
 */
void
heap_parallelscan_initialize(ParallelHeapScanDesc target, Relation relation,
							 Snapshot snapshot)
{
	target->phs_relid = RelationGetRelid(relation);
	target->phs_nblocks = RelationGetNumberOfBlocks(relation);
	/* compare phs_syncscan initialization to similar logic in initscan */
	target->phs_syncscan = synchronize_seqscans &&
		!RelationUsesLocalBuffers(relation) &&
		target->phs_nblocks > NBuffers / 4;
	SpinLockInit(&target->phs_mutex);
	target->phs_cblock = InvalidBlockNumber;
	target->phs_startblock = InvalidBlockNumber;
	SerializeSnapshot(snapshot, target->phs_snapshot_data);
}

/* ----------------
 *		heap_beginscan_parallel - join a parallel scan
 *
 *		Caller must hold a suitable lock on the correct relation.
 * ----------------
 */
HeapScanDesc
heap_beginscan_parallel(Relation relation, ParallelHeapScanDesc parallel_scan)
{
	Snapshot	snapshot;

	Assert(RelationGetRelid(relation) == parallel_scan->phs_relid);
	snapshot = RestoreSnapshot(parallel_scan->phs_snapshot_data);
	RegisterSnapshot(snapshot);

	return heap_beginscan_internal(relation, snapshot, 0, NULL, parallel_scan,
								   true, true, true, false, false, true);
}

/* ----------------
 *		heap_parallelscan_nextpage - get the next page to scan
 *
 *		Get the next page to scan.  Even if there are no pages left to scan,
 *		another backend could have grabbed a page to scan and not yet finished
 *		looking at it, so it doesn't follow that the scan is done when the
 *		first backend gets an InvalidBlockNumber return.
 * ----------------
 */
static BlockNumber
heap_parallelscan_nextpage(HeapScanDesc scan)
{
	BlockNumber page = InvalidBlockNumber;
	BlockNumber sync_startpage = InvalidBlockNumber;
	BlockNumber	report_page = InvalidBlockNumber;
	ParallelHeapScanDesc parallel_scan;

	Assert(scan->rs_parallel);
	parallel_scan = scan->rs_parallel;

retry:
	/* Grab the spinlock. */
	SpinLockAcquire(&parallel_scan->phs_mutex);

	/*
	 * If the scan's startblock has not yet been initialized, we must do so
	 * now.  If this is not a synchronized scan, we just start at block 0, but
	 * if it is a synchronized scan, we must get the starting position from
	 * the synchronized scan machinery.  We can't hold the spinlock while
	 * doing that, though, so release the spinlock, get the information we
	 * need, and retry.  If nobody else has initialized the scan in the
	 * meantime, we'll fill in the value we fetched on the second time
	 * through.
	 */
	if (parallel_scan->phs_startblock == InvalidBlockNumber)
	{
		if (!parallel_scan->phs_syncscan)
			parallel_scan->phs_startblock = 0;
		else if (sync_startpage != InvalidBlockNumber)
			parallel_scan->phs_startblock = sync_startpage;
		else
		{
			SpinLockRelease(&parallel_scan->phs_mutex);
			sync_startpage = ss_get_location(scan->rs_rd, scan->rs_nblocks);
			goto retry;
		}
		parallel_scan->phs_cblock = parallel_scan->phs_startblock;
	}

	/*
	 * The current block number is the next one that needs to be scanned,
	 * unless it's InvalidBlockNumber already, in which case there are no more
	 * blocks to scan.  After remembering the current value, we must advance
	 * it so that the next call to this function returns the next block to be
	 * scanned.
	 */
	page = parallel_scan->phs_cblock;
	if (page != InvalidBlockNumber)
	{
		parallel_scan->phs_cblock++;
		if (parallel_scan->phs_cblock >= scan->rs_nblocks)
			parallel_scan->phs_cblock = 0;
		if (parallel_scan->phs_cblock == parallel_scan->phs_startblock)
		{
			parallel_scan->phs_cblock = InvalidBlockNumber;
			report_page = parallel_scan->phs_startblock;
		}
	}

	/* Release the lock. */
	SpinLockRelease(&parallel_scan->phs_mutex);

	/*
	 * Report scan location.  Normally, we report the current page number.
	 * When we reach the end of the scan, though, we report the starting page,
	 * not the ending page, just so the starting positions for later scans
	 * doesn't slew backwards.  We only report the position at the end of the
	 * scan once, though: subsequent callers will have report nothing, since
	 * they will have page == InvalidBlockNumber.
	 */
	if (scan->rs_syncscan)
	{
		if (report_page == InvalidBlockNumber)
			report_page = page;
		if (report_page != InvalidBlockNumber)
			ss_report_location(scan->rs_rd, report_page);
	}

	return page;
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
	ItemPointer tid = &(tuple->t_self);
	ItemId		lp;
	Buffer		buffer;
	Page		page;
	OffsetNumber offnum;
	bool		valid;

	/*
	 * Fetch and pin the appropriate page of the relation.
	 */
	buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(tid));

	/*
	 * Need share lock on buffer to examine tuple commit status.
	 */
	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buffer);

	/*
	 * We'd better check for out-of-range offnum in case of VACUUM since the
	 * TID was obtained.
	 */
	offnum = ItemPointerGetOffsetNumber(tid);
	if (offnum < FirstOffsetNumber || offnum > PageGetMaxOffsetNumber(page))
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
	lp = PageGetItemId(page, offnum);

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
	tuple->t_data = (HeapTupleHeader) PageGetItem(page, lp);
	tuple->t_len = ItemIdGetLength(lp);
	tuple->t_tableOid = RelationGetRelid(relation);

	/*
	 * check time qualification of tuple, then release lock
	 */
	valid = HeapTupleSatisfiesVisibility(tuple, snapshot, buffer);

	if (valid)
		PredicateLockTuple(relation, tuple, snapshot);

	CheckForSerializableConflictOut(valid, relation, tuple, buffer, snapshot);

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
 * heapTuple is a caller-supplied buffer.  When a match is found, we return
 * the tuple here, in addition to updating *tid.  If no match is found, the
 * contents of this buffer on return are undefined.
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
heap_hot_search_buffer(ItemPointer tid, Relation relation, Buffer buffer,
					   Snapshot snapshot, HeapTuple heapTuple,
					   bool *all_dead, bool first_call)
{
	Page		dp = (Page) BufferGetPage(buffer);
	TransactionId prev_xmax = InvalidTransactionId;
	OffsetNumber offnum;
	bool		at_chain_start;
	bool		valid;
	bool		skip;

	/* If this is not the first call, previous call returned a (live!) tuple */
	if (all_dead)
		*all_dead = first_call;

	Assert(TransactionIdIsValid(RecentGlobalXmin));

	Assert(ItemPointerGetBlockNumber(tid) == BufferGetBlockNumber(buffer));
	offnum = ItemPointerGetOffsetNumber(tid);
	at_chain_start = first_call;
	skip = !first_call;

	heapTuple->t_self = *tid;

	/* Scan through possible multiple members of HOT-chain */
	for (;;)
	{
		ItemId		lp;

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

		heapTuple->t_data = (HeapTupleHeader) PageGetItem(dp, lp);
		heapTuple->t_len = ItemIdGetLength(lp);
		heapTuple->t_tableOid = RelationGetRelid(relation);
		ItemPointerSetOffsetNumber(&heapTuple->t_self, offnum);

		/*
		 * Shouldn't see a HEAP_ONLY tuple at chain start.
		 */
		if (at_chain_start && HeapTupleIsHeapOnly(heapTuple))
			break;

		/*
		 * The xmin should match the previous xmax value, else chain is
		 * broken.
		 */
		if (TransactionIdIsValid(prev_xmax) &&
			!TransactionIdEquals(prev_xmax,
								 HeapTupleHeaderGetXmin(heapTuple->t_data)))
			break;

		/*
		 * When first_call is true (and thus, skip is initially false) we'll
		 * return the first tuple we find.  But on later passes, heapTuple
		 * will initially be pointing to the tuple we returned last time.
		 * Returning it again would be incorrect (and would loop forever), so
		 * we skip it and return the next match we find.
		 */
		if (!skip)
		{
			/*
			 * For the benefit of logical decoding, have t_self point at the
			 * element of the HOT chain we're currently investigating instead
			 * of the root tuple of the HOT chain. This is important because
			 * the *Satisfies routine for historical mvcc snapshots needs the
			 * correct tid to decide about the visibility in some cases.
			 */
			ItemPointerSet(&(heapTuple->t_self), BufferGetBlockNumber(buffer), offnum);

			/* If it's visible per the snapshot, we must return it */
			valid = HeapTupleSatisfiesVisibility(heapTuple, snapshot, buffer);
			CheckForSerializableConflictOut(valid, relation, heapTuple,
											buffer, snapshot);
			/* reset to original, non-redirected, tid */
			heapTuple->t_self = *tid;

			if (valid)
			{
				ItemPointerSetOffsetNumber(tid, offnum);
				PredicateLockTuple(relation, heapTuple, snapshot);
				if (all_dead)
					*all_dead = false;
				return true;
			}
		}
		skip = false;

		/*
		 * If we can't see it, maybe no one else can either.  At caller
		 * request, check whether all chain members are dead to all
		 * transactions.
		 */
		if (all_dead && *all_dead &&
			!HeapTupleIsSurelyDead(heapTuple, RecentGlobalXmin))
			*all_dead = false;

		/*
		 * Check to see if HOT chain continues past this tuple; if so fetch
		 * the next offnum and loop around.
		 */
		if (HeapTupleIsHotUpdated(heapTuple))
		{
			Assert(ItemPointerGetBlockNumber(&heapTuple->t_data->t_ctid) ==
				   ItemPointerGetBlockNumber(tid));
			offnum = ItemPointerGetOffsetNumber(&heapTuple->t_data->t_ctid);
			at_chain_start = false;
			prev_xmax = HeapTupleHeaderGetUpdateXid(heapTuple->t_data);
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
	HeapTupleData heapTuple;

	buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(tid));
	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	result = heap_hot_search_buffer(tid, relation, buffer, snapshot,
									&heapTuple, all_dead, true);
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
 * show the latest version of the row.  Note that it will not be changed
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
		Page		page;
		OffsetNumber offnum;
		ItemId		lp;
		HeapTupleData tp;
		bool		valid;

		/*
		 * Read, pin, and lock the page.
		 */
		buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(&ctid));
		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buffer);

		/*
		 * Check for bogus item number.  This is not treated as an error
		 * condition because it can happen while following a t_ctid link. We
		 * just assume that the prior tid is OK and return it unchanged.
		 */
		offnum = ItemPointerGetOffsetNumber(&ctid);
		if (offnum < FirstOffsetNumber || offnum > PageGetMaxOffsetNumber(page))
		{
			UnlockReleaseBuffer(buffer);
			break;
		}
		lp = PageGetItemId(page, offnum);
		if (!ItemIdIsNormal(lp))
		{
			UnlockReleaseBuffer(buffer);
			break;
		}

		/* OK to access the tuple */
		tp.t_self = ctid;
		tp.t_data = (HeapTupleHeader) PageGetItem(page, lp);
		tp.t_len = ItemIdGetLength(lp);
		tp.t_tableOid = RelationGetRelid(relation);

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
		CheckForSerializableConflictOut(valid, relation, &tp, buffer, snapshot);
		if (valid)
			*tid = ctid;

		/*
		 * If there's a valid t_ctid link, follow it, else we're done.
		 */
		if ((tp.t_data->t_infomask & HEAP_XMAX_INVALID) ||
			HeapTupleHeaderIsOnlyLocked(tp.t_data) ||
			ItemPointerEquals(&tp.t_self, &tp.t_data->t_ctid))
		{
			UnlockReleaseBuffer(buffer);
			break;
		}

		ctid = tp.t_data->t_ctid;
		priorXmax = HeapTupleHeaderGetUpdateXid(tp.t_data);
		UnlockReleaseBuffer(buffer);
	}							/* end of loop */
}


/*
 * UpdateXmaxHintBits - update tuple hint bits after xmax transaction ends
 *
 * This is called after we have waited for the XMAX transaction to terminate.
 * If the transaction aborted, we guarantee the XMAX_INVALID hint bit will
 * be set on exit.  If the transaction committed, we set the XMAX_COMMITTED
 * hint bit if possible --- but beware that that may not yet be possible,
 * if the transaction committed asynchronously.
 *
 * Note that if the transaction was a locker only, we set HEAP_XMAX_INVALID
 * even if it commits.
 *
 * Hence callers should look only at XMAX_INVALID.
 *
 * Note this is not allowed for tuples whose xmax is a multixact.
 */
static void
UpdateXmaxHintBits(HeapTupleHeader tuple, Buffer buffer, TransactionId xid)
{
	Assert(TransactionIdEquals(HeapTupleHeaderGetRawXmax(tuple), xid));
	Assert(!(tuple->t_infomask & HEAP_XMAX_IS_MULTI));

	if (!(tuple->t_infomask & (HEAP_XMAX_COMMITTED | HEAP_XMAX_INVALID)))
	{
		if (!HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask) &&
			TransactionIdDidCommit(xid))
			HeapTupleSetHintBits(tuple, buffer, HEAP_XMAX_COMMITTED,
								 xid);
		else
			HeapTupleSetHintBits(tuple, buffer, HEAP_XMAX_INVALID,
								 InvalidTransactionId);
	}
}


/*
 * GetBulkInsertState - prepare status object for a bulk insert
 */
BulkInsertState
GetBulkInsertState(void)
{
	BulkInsertState bistate;

	bistate = (BulkInsertState) palloc(sizeof(BulkInsertStateData));
	bistate->strategy = GetAccessStrategy(BAS_BULKWRITE);
	bistate->current_buf = InvalidBuffer;
	return bistate;
}

/*
 * FreeBulkInsertState - clean up after finishing a bulk insert
 */
void
FreeBulkInsertState(BulkInsertState bistate)
{
	if (bistate->current_buf != InvalidBuffer)
		ReleaseBuffer(bistate->current_buf);
	FreeAccessStrategy(bistate->strategy);
	pfree(bistate);
}


/*
 *	heap_insert		- insert tuple into a heap
 *
 * The new tuple is stamped with current transaction ID and the specified
 * command ID.
 *
 * If the HEAP_INSERT_SKIP_WAL option is specified, the new tuple is not
 * logged in WAL, even for a non-temp relation.  Safe usage of this behavior
 * requires that we arrange that all new tuples go into new pages not
 * containing any tuples from other transactions, and that the relation gets
 * fsync'd before commit.  (See also heap_sync() comments)
 *
 * The HEAP_INSERT_SKIP_FSM option is passed directly to
 * RelationGetBufferForTuple, which see for more info.
 *
 * HEAP_INSERT_FROZEN should only be specified for inserts into
 * relfilenodes created during the current subtransaction and when
 * there are no prior snapshots or pre-existing portals open.
 * This causes rows to be frozen, which is an MVCC violation and
 * requires explicit options chosen by user.
 *
 * HEAP_INSERT_IS_SPECULATIVE is used on so-called "speculative insertions",
 * which can be backed out afterwards without aborting the whole transaction.
 * Other sessions can wait for the speculative insertion to be confirmed,
 * turning it into a regular tuple, or aborted, as if it never existed.
 * Speculatively inserted tuples behave as "value locks" of short duration,
 * used to implement INSERT .. ON CONFLICT.
 *
 * Note that most of these options will be applied when inserting into the
 * heap's TOAST table, too, if the tuple requires any out-of-line data.  Only
 * HEAP_INSERT_IS_SPECULATIVE is explicitly ignored, as the toast data does
 * not partake in speculative insertion.
 *
 * The BulkInsertState object (if any; bistate can be NULL for default
 * behavior) is also just passed through to RelationGetBufferForTuple.
 *
 * The return value is the OID assigned to the tuple (either here or by the
 * caller), or InvalidOid if no OID.  The header fields of *tup are updated
 * to match the stored tuple; in particular tup->t_self receives the actual
 * TID where the tuple was stored.  But note that any toasting of fields
 * within the tuple data is NOT reflected into *tup.
 */
Oid
heap_insert(Relation relation, HeapTuple tup, CommandId cid,
			int options, BulkInsertState bistate)
{
	TransactionId xid = GetCurrentTransactionId();
	HeapTuple	heaptup;
	Buffer		buffer;
	Buffer		vmbuffer = InvalidBuffer;
	bool		all_visible_cleared = false;

	/*
	 * Fill in tuple header fields, assign an OID, and toast the tuple if
	 * necessary.
	 *
	 * Note: below this point, heaptup is the data we actually intend to store
	 * into the relation; tup is the caller's original untoasted data.
	 */
	heaptup = heap_prepare_insert(relation, tup, xid, cid, options);

	/*
	 * Find buffer to insert this tuple into.  If the page is all visible,
	 * this will also pin the requisite visibility map page.
	 */
	buffer = RelationGetBufferForTuple(relation, heaptup->t_len,
									   InvalidBuffer, options, bistate,
									   &vmbuffer, NULL);

	/*
	 * We're about to do the actual insert -- but check for conflict first, to
	 * avoid possibly having to roll back work we've just done.
	 *
	 * This is safe without a recheck as long as there is no possibility of
	 * another process scanning the page between this check and the insert
	 * being visible to the scan (i.e., an exclusive buffer content lock is
	 * continuously held from this point until the tuple insert is visible).
	 *
	 * For a heap insert, we only need to check for table-level SSI locks. Our
	 * new tuple can't possibly conflict with existing tuple locks, and heap
	 * page locks are only consolidated versions of tuple locks; they do not
	 * lock "gaps" as index page locks do.  So we don't need to specify a
	 * buffer when making the call, which makes for a faster check.
	 */
	CheckForSerializableConflictIn(relation, NULL, InvalidBuffer);

	/* NO EREPORT(ERROR) from here till changes are logged */
	START_CRIT_SECTION();

	RelationPutHeapTuple(relation, buffer, heaptup,
						 (options & HEAP_INSERT_SPECULATIVE) != 0);

	if (PageIsAllVisible(BufferGetPage(buffer)))
	{
		all_visible_cleared = true;
		PageClearAllVisible(BufferGetPage(buffer));
		visibilitymap_clear(relation,
							ItemPointerGetBlockNumber(&(heaptup->t_self)),
							vmbuffer);
	}

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
	if (!(options & HEAP_INSERT_SKIP_WAL) && RelationNeedsWAL(relation))
	{
		xl_heap_insert xlrec;
		xl_heap_header xlhdr;
		XLogRecPtr	recptr;
		Page		page = BufferGetPage(buffer);
		uint8		info = XLOG_HEAP_INSERT;
		int			bufflags = 0;

		/*
		 * If this is a catalog, we need to transmit combocids to properly
		 * decode, so log that as well.
		 */
		if (RelationIsAccessibleInLogicalDecoding(relation))
			log_heap_new_cid(relation, heaptup);

		/*
		 * If this is the single and first tuple on page, we can reinit the
		 * page instead of restoring the whole thing.  Set flag, and hide
		 * buffer references from XLogInsert.
		 */
		if (ItemPointerGetOffsetNumber(&(heaptup->t_self)) == FirstOffsetNumber &&
			PageGetMaxOffsetNumber(page) == FirstOffsetNumber)
		{
			info |= XLOG_HEAP_INIT_PAGE;
			bufflags |= REGBUF_WILL_INIT;
		}

		xlrec.offnum = ItemPointerGetOffsetNumber(&heaptup->t_self);
		xlrec.flags = 0;
		if (all_visible_cleared)
			xlrec.flags |= XLH_INSERT_ALL_VISIBLE_CLEARED;
		if (options & HEAP_INSERT_SPECULATIVE)
			xlrec.flags |= XLH_INSERT_IS_SPECULATIVE;
		Assert(ItemPointerGetBlockNumber(&heaptup->t_self) == BufferGetBlockNumber(buffer));

		/*
		 * For logical decoding, we need the tuple even if we're doing a full
		 * page write, so make sure it's included even if we take a full-page
		 * image. (XXX We could alternatively store a pointer into the FPW).
		 */
		if (RelationIsLogicallyLogged(relation))
		{
			xlrec.flags |= XLH_INSERT_CONTAINS_NEW_TUPLE;
			bufflags |= REGBUF_KEEP_DATA;
		}

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfHeapInsert);

		xlhdr.t_infomask2 = heaptup->t_data->t_infomask2;
		xlhdr.t_infomask = heaptup->t_data->t_infomask;
		xlhdr.t_hoff = heaptup->t_data->t_hoff;

		/*
		 * note we mark xlhdr as belonging to buffer; if XLogInsert decides to
		 * write the whole page to the xlog, we don't need to store
		 * xl_heap_header in the xlog.
		 */
		XLogRegisterBuffer(0, buffer, REGBUF_STANDARD | bufflags);
		XLogRegisterBufData(0, (char *) &xlhdr, SizeOfHeapHeader);
		/* PG73FORMAT: write bitmap [+ padding] [+ oid] + data */
		XLogRegisterBufData(0,
							(char *) heaptup->t_data + SizeofHeapTupleHeader,
							heaptup->t_len - SizeofHeapTupleHeader);

		/* filtering by origin on a row level is much more efficient */
		XLogIncludeOrigin();

		recptr = XLogInsert(RM_HEAP_ID, info);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	UnlockReleaseBuffer(buffer);
	if (vmbuffer != InvalidBuffer)
		ReleaseBuffer(vmbuffer);

	/*
	 * If tuple is cachable, mark it for invalidation from the caches in case
	 * we abort.  Note it is OK to do this after releasing the buffer, because
	 * the heaptup data structure is all in local memory, not in the shared
	 * buffer.
	 */
	CacheInvalidateHeapTuple(relation, heaptup, NULL);

	/* Note: speculative insertions are counted too, even if aborted later */
	pgstat_count_heap_insert(relation, 1);

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
 * Subroutine for heap_insert(). Prepares a tuple for insertion. This sets the
 * tuple header fields, assigns an OID, and toasts the tuple if necessary.
 * Returns a toasted version of the tuple if it was toasted, or the original
 * tuple if not. Note that in any case, the header fields are also set in
 * the original tuple.
 */
static HeapTuple
heap_prepare_insert(Relation relation, HeapTuple tup, TransactionId xid,
					CommandId cid, int options)
{
	/*
	 * For now, parallel operations are required to be strictly read-only.
	 * Unlike heap_update() and heap_delete(), an insert should never create a
	 * combo CID, so it might be possible to relax this restriction, but not
	 * without more thought and testing.
	 */
	if (IsInParallelMode())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("cannot insert tuples during a parallel operation")));

	if (relation->rd_rel->relhasoids)
	{
#ifdef NOT_USED
		/* this is redundant with an Assert in HeapTupleSetOid */
		Assert(tup->t_data->t_infomask & HEAP_HASOID);
#endif

		/*
		 * If the object id of this tuple has already been assigned, trust the
		 * caller.  There are a couple of ways this can happen.  At initial db
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
	if (options & HEAP_INSERT_FROZEN)
		HeapTupleHeaderSetXminFrozen(tup->t_data);

	HeapTupleHeaderSetCmin(tup->t_data, cid);
	HeapTupleHeaderSetXmax(tup->t_data, 0);		/* for cleanliness */
	tup->t_tableOid = RelationGetRelid(relation);

	/*
	 * If the new tuple is too big for storage or contains already toasted
	 * out-of-line attributes from some other relation, invoke the toaster.
	 */
	if (relation->rd_rel->relkind != RELKIND_RELATION &&
		relation->rd_rel->relkind != RELKIND_MATVIEW)
	{
		/* toast table entries should never be recursively toasted */
		Assert(!HeapTupleHasExternal(tup));
		return tup;
	}
	else if (HeapTupleHasExternal(tup) || tup->t_len > TOAST_TUPLE_THRESHOLD)
		return toast_insert_or_update(relation, tup, NULL, options);
	else
		return tup;
}

/*
 *	heap_multi_insert	- insert multiple tuple into a heap
 *
 * This is like heap_insert(), but inserts multiple tuples in one operation.
 * That's faster than calling heap_insert() in a loop, because when multiple
 * tuples can be inserted on a single page, we can write just a single WAL
 * record covering all of them, and only need to lock/unlock the page once.
 *
 * Note: this leaks memory into the current memory context. You can create a
 * temporary context before calling this, if that's a problem.
 */
void
heap_multi_insert(Relation relation, HeapTuple *tuples, int ntuples,
				  CommandId cid, int options, BulkInsertState bistate)
{
	TransactionId xid = GetCurrentTransactionId();
	HeapTuple  *heaptuples;
	int			i;
	int			ndone;
	char	   *scratch = NULL;
	Page		page;
	bool		needwal;
	Size		saveFreeSpace;
	bool		need_tuple_data = RelationIsLogicallyLogged(relation);
	bool		need_cids = RelationIsAccessibleInLogicalDecoding(relation);

	needwal = !(options & HEAP_INSERT_SKIP_WAL) && RelationNeedsWAL(relation);
	saveFreeSpace = RelationGetTargetPageFreeSpace(relation,
												   HEAP_DEFAULT_FILLFACTOR);

	/* Toast and set header data in all the tuples */
	heaptuples = palloc(ntuples * sizeof(HeapTuple));
	for (i = 0; i < ntuples; i++)
		heaptuples[i] = heap_prepare_insert(relation, tuples[i],
											xid, cid, options);

	/*
	 * Allocate some memory to use for constructing the WAL record. Using
	 * palloc() within a critical section is not safe, so we allocate this
	 * beforehand.
	 */
	if (needwal)
		scratch = palloc(BLCKSZ);

	/*
	 * We're about to do the actual inserts -- but check for conflict first,
	 * to minimize the possibility of having to roll back work we've just
	 * done.
	 *
	 * A check here does not definitively prevent a serialization anomaly;
	 * that check MUST be done at least past the point of acquiring an
	 * exclusive buffer content lock on every buffer that will be affected,
	 * and MAY be done after all inserts are reflected in the buffers and
	 * those locks are released; otherwise there race condition.  Since
	 * multiple buffers can be locked and unlocked in the loop below, and it
	 * would not be feasible to identify and lock all of those buffers before
	 * the loop, we must do a final check at the end.
	 *
	 * The check here could be omitted with no loss of correctness; it is
	 * present strictly as an optimization.
	 *
	 * For heap inserts, we only need to check for table-level SSI locks. Our
	 * new tuples can't possibly conflict with existing tuple locks, and heap
	 * page locks are only consolidated versions of tuple locks; they do not
	 * lock "gaps" as index page locks do.  So we don't need to specify a
	 * buffer when making the call, which makes for a faster check.
	 */
	CheckForSerializableConflictIn(relation, NULL, InvalidBuffer);

	ndone = 0;
	while (ndone < ntuples)
	{
		Buffer		buffer;
		Buffer		vmbuffer = InvalidBuffer;
		bool		all_visible_cleared = false;
		int			nthispage;

		CHECK_FOR_INTERRUPTS();

		/*
		 * Find buffer where at least the next tuple will fit.  If the page is
		 * all-visible, this will also pin the requisite visibility map page.
		 */
		buffer = RelationGetBufferForTuple(relation, heaptuples[ndone]->t_len,
										   InvalidBuffer, options, bistate,
										   &vmbuffer, NULL);
		page = BufferGetPage(buffer);

		/* NO EREPORT(ERROR) from here till changes are logged */
		START_CRIT_SECTION();

		/*
		 * RelationGetBufferForTuple has ensured that the first tuple fits.
		 * Put that on the page, and then as many other tuples as fit.
		 */
		RelationPutHeapTuple(relation, buffer, heaptuples[ndone], false);
		for (nthispage = 1; ndone + nthispage < ntuples; nthispage++)
		{
			HeapTuple	heaptup = heaptuples[ndone + nthispage];

			if (PageGetHeapFreeSpace(page) < MAXALIGN(heaptup->t_len) + saveFreeSpace)
				break;

			RelationPutHeapTuple(relation, buffer, heaptup, false);

			/*
			 * We don't use heap_multi_insert for catalog tuples yet, but
			 * better be prepared...
			 */
			if (needwal && need_cids)
				log_heap_new_cid(relation, heaptup);
		}

		if (PageIsAllVisible(page))
		{
			all_visible_cleared = true;
			PageClearAllVisible(page);
			visibilitymap_clear(relation,
								BufferGetBlockNumber(buffer),
								vmbuffer);
		}

		/*
		 * XXX Should we set PageSetPrunable on this page ? See heap_insert()
		 */

		MarkBufferDirty(buffer);

		/* XLOG stuff */
		if (needwal)
		{
			XLogRecPtr	recptr;
			xl_heap_multi_insert *xlrec;
			uint8		info = XLOG_HEAP2_MULTI_INSERT;
			char	   *tupledata;
			int			totaldatalen;
			char	   *scratchptr = scratch;
			bool		init;
			int			bufflags = 0;

			/*
			 * If the page was previously empty, we can reinit the page
			 * instead of restoring the whole thing.
			 */
			init = (ItemPointerGetOffsetNumber(&(heaptuples[ndone]->t_self)) == FirstOffsetNumber &&
					PageGetMaxOffsetNumber(page) == FirstOffsetNumber + nthispage - 1);

			/* allocate xl_heap_multi_insert struct from the scratch area */
			xlrec = (xl_heap_multi_insert *) scratchptr;
			scratchptr += SizeOfHeapMultiInsert;

			/*
			 * Allocate offsets array. Unless we're reinitializing the page,
			 * in that case the tuples are stored in order starting at
			 * FirstOffsetNumber and we don't need to store the offsets
			 * explicitly.
			 */
			if (!init)
				scratchptr += nthispage * sizeof(OffsetNumber);

			/* the rest of the scratch space is used for tuple data */
			tupledata = scratchptr;

			xlrec->flags = all_visible_cleared ? XLH_INSERT_ALL_VISIBLE_CLEARED : 0;
			xlrec->ntuples = nthispage;

			/*
			 * Write out an xl_multi_insert_tuple and the tuple data itself
			 * for each tuple.
			 */
			for (i = 0; i < nthispage; i++)
			{
				HeapTuple	heaptup = heaptuples[ndone + i];
				xl_multi_insert_tuple *tuphdr;
				int			datalen;

				if (!init)
					xlrec->offsets[i] = ItemPointerGetOffsetNumber(&heaptup->t_self);
				/* xl_multi_insert_tuple needs two-byte alignment. */
				tuphdr = (xl_multi_insert_tuple *) SHORTALIGN(scratchptr);
				scratchptr = ((char *) tuphdr) + SizeOfMultiInsertTuple;

				tuphdr->t_infomask2 = heaptup->t_data->t_infomask2;
				tuphdr->t_infomask = heaptup->t_data->t_infomask;
				tuphdr->t_hoff = heaptup->t_data->t_hoff;

				/* write bitmap [+ padding] [+ oid] + data */
				datalen = heaptup->t_len - SizeofHeapTupleHeader;
				memcpy(scratchptr,
					   (char *) heaptup->t_data + SizeofHeapTupleHeader,
					   datalen);
				tuphdr->datalen = datalen;
				scratchptr += datalen;
			}
			totaldatalen = scratchptr - tupledata;
			Assert((scratchptr - scratch) < BLCKSZ);

			if (need_tuple_data)
				xlrec->flags |= XLH_INSERT_CONTAINS_NEW_TUPLE;

			/*
			 * Signal that this is the last xl_heap_multi_insert record
			 * emitted by this call to heap_multi_insert(). Needed for logical
			 * decoding so it knows when to cleanup temporary data.
			 */
			if (ndone + nthispage == ntuples)
				xlrec->flags |= XLH_INSERT_LAST_IN_MULTI;

			if (init)
			{
				info |= XLOG_HEAP_INIT_PAGE;
				bufflags |= REGBUF_WILL_INIT;
			}

			/*
			 * If we're doing logical decoding, include the new tuple data
			 * even if we take a full-page image of the page.
			 */
			if (need_tuple_data)
				bufflags |= REGBUF_KEEP_DATA;

			XLogBeginInsert();
			XLogRegisterData((char *) xlrec, tupledata - scratch);
			XLogRegisterBuffer(0, buffer, REGBUF_STANDARD | bufflags);

			XLogRegisterBufData(0, tupledata, totaldatalen);

			/* filtering by origin on a row level is much more efficient */
			XLogIncludeOrigin();

			recptr = XLogInsert(RM_HEAP2_ID, info);

			PageSetLSN(page, recptr);
		}

		END_CRIT_SECTION();

		UnlockReleaseBuffer(buffer);
		if (vmbuffer != InvalidBuffer)
			ReleaseBuffer(vmbuffer);

		ndone += nthispage;
	}

	/*
	 * We're done with the actual inserts.  Check for conflicts again, to
	 * ensure that all rw-conflicts in to these inserts are detected.  Without
	 * this final check, a sequential scan of the heap may have locked the
	 * table after the "before" check, missing one opportunity to detect the
	 * conflict, and then scanned the table before the new tuples were there,
	 * missing the other chance to detect the conflict.
	 *
	 * For heap inserts, we only need to check for table-level SSI locks. Our
	 * new tuples can't possibly conflict with existing tuple locks, and heap
	 * page locks are only consolidated versions of tuple locks; they do not
	 * lock "gaps" as index page locks do.  So we don't need to specify a
	 * buffer when making the call.
	 */
	CheckForSerializableConflictIn(relation, NULL, InvalidBuffer);

	/*
	 * If tuples are cachable, mark them for invalidation from the caches in
	 * case we abort.  Note it is OK to do this after releasing the buffer,
	 * because the heaptuples data structure is all in local memory, not in
	 * the shared buffer.
	 */
	if (IsCatalogRelation(relation))
	{
		for (i = 0; i < ntuples; i++)
			CacheInvalidateHeapTuple(relation, heaptuples[i], NULL);
	}

	/*
	 * Copy t_self fields back to the caller's original tuples. This does
	 * nothing for untoasted tuples (tuples[i] == heaptuples[i)], but it's
	 * probably faster to always copy than check.
	 */
	for (i = 0; i < ntuples; i++)
		tuples[i]->t_self = heaptuples[i]->t_self;

	pgstat_count_heap_insert(relation, ntuples);
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
	return heap_insert(relation, tup, GetCurrentCommandId(true), 0, NULL);
}

/*
 * Given infomask/infomask2, compute the bits that must be saved in the
 * "infobits" field of xl_heap_delete, xl_heap_update, xl_heap_lock,
 * xl_heap_lock_updated WAL records.
 *
 * See fix_infomask_from_infobits.
 */
static uint8
compute_infobits(uint16 infomask, uint16 infomask2)
{
	return
		((infomask & HEAP_XMAX_IS_MULTI) != 0 ? XLHL_XMAX_IS_MULTI : 0) |
		((infomask & HEAP_XMAX_LOCK_ONLY) != 0 ? XLHL_XMAX_LOCK_ONLY : 0) |
		((infomask & HEAP_XMAX_EXCL_LOCK) != 0 ? XLHL_XMAX_EXCL_LOCK : 0) |
	/* note we ignore HEAP_XMAX_SHR_LOCK here */
		((infomask & HEAP_XMAX_KEYSHR_LOCK) != 0 ? XLHL_XMAX_KEYSHR_LOCK : 0) |
		((infomask2 & HEAP_KEYS_UPDATED) != 0 ?
		 XLHL_KEYS_UPDATED : 0);
}

/*
 * Given two versions of the same t_infomask for a tuple, compare them and
 * return whether the relevant status for a tuple Xmax has changed.  This is
 * used after a buffer lock has been released and reacquired: we want to ensure
 * that the tuple state continues to be the same it was when we previously
 * examined it.
 *
 * Note the Xmax field itself must be compared separately.
 */
static inline bool
xmax_infomask_changed(uint16 new_infomask, uint16 old_infomask)
{
	const uint16 interesting =
	HEAP_XMAX_IS_MULTI | HEAP_XMAX_LOCK_ONLY | HEAP_LOCK_MASK;

	if ((new_infomask & interesting) != (old_infomask & interesting))
		return true;

	return false;
}

/*
 *	heap_delete - delete a tuple
 *
 * NB: do not call this directly unless you are prepared to deal with
 * concurrent-update conditions.  Use simple_heap_delete instead.
 *
 *	relation - table to be modified (caller must hold suitable lock)
 *	tid - TID of tuple to be deleted
 *	cid - delete command ID (used for visibility test, and stored into
 *		cmax if successful)
 *	crosscheck - if not InvalidSnapshot, also check tuple against this
 *	wait - true if should wait for any conflicting update to commit/abort
 *	hufd - output parameter, filled in failure cases (see below)
 *
 * Normal, successful return value is HeapTupleMayBeUpdated, which
 * actually means we did delete it.  Failure return codes are
 * HeapTupleSelfUpdated, HeapTupleUpdated, or HeapTupleBeingUpdated
 * (the last only possible if wait == false).
 *
 * In the failure cases, the routine fills *hufd with the tuple's t_ctid,
 * t_xmax (resolving a possible MultiXact, if necessary), and t_cmax
 * (the last only for HeapTupleSelfUpdated, since we
 * cannot obtain cmax from a combocid generated by another transaction).
 * See comments for struct HeapUpdateFailureData for additional info.
 */
HTSU_Result
heap_delete(Relation relation, ItemPointer tid,
			CommandId cid, Snapshot crosscheck, bool wait,
			HeapUpdateFailureData *hufd)
{
	HTSU_Result result;
	TransactionId xid = GetCurrentTransactionId();
	ItemId		lp;
	HeapTupleData tp;
	Page		page;
	BlockNumber block;
	Buffer		buffer;
	Buffer		vmbuffer = InvalidBuffer;
	TransactionId new_xmax;
	uint16		new_infomask,
				new_infomask2;
	bool		have_tuple_lock = false;
	bool		iscombo;
	bool		all_visible_cleared = false;
	HeapTuple	old_key_tuple = NULL;	/* replica identity of the tuple */
	bool		old_key_copied = false;

	Assert(ItemPointerIsValid(tid));

	/*
	 * Forbid this during a parallel operation, lets it allocate a combocid.
	 * Other workers might need that combocid for visibility checks, and we
	 * have no provision for broadcasting it to them.
	 */
	if (IsInParallelMode())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("cannot delete tuples during a parallel operation")));

	block = ItemPointerGetBlockNumber(tid);
	buffer = ReadBuffer(relation, block);
	page = BufferGetPage(buffer);

	/*
	 * Before locking the buffer, pin the visibility map page if it appears to
	 * be necessary.  Since we haven't got the lock yet, someone else might be
	 * in the middle of changing this, so we'll need to recheck after we have
	 * the lock.
	 */
	if (PageIsAllVisible(page))
		visibilitymap_pin(relation, block, &vmbuffer);

	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	/*
	 * If we didn't pin the visibility map page and the page has become all
	 * visible while we were busy locking the buffer, we'll have to unlock and
	 * re-lock, to avoid holding the buffer lock across an I/O.  That's a bit
	 * unfortunate, but hopefully shouldn't happen often.
	 */
	if (vmbuffer == InvalidBuffer && PageIsAllVisible(page))
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		visibilitymap_pin(relation, block, &vmbuffer);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	}

	lp = PageGetItemId(page, ItemPointerGetOffsetNumber(tid));
	Assert(ItemIdIsNormal(lp));

	tp.t_tableOid = RelationGetRelid(relation);
	tp.t_data = (HeapTupleHeader) PageGetItem(page, lp);
	tp.t_len = ItemIdGetLength(lp);
	tp.t_self = *tid;

l1:
	result = HeapTupleSatisfiesUpdate(&tp, cid, buffer);

	if (result == HeapTupleInvisible)
	{
		UnlockReleaseBuffer(buffer);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("attempted to delete invisible tuple")));
	}
	else if (result == HeapTupleBeingUpdated && wait)
	{
		TransactionId xwait;
		uint16		infomask;

		/* must copy state data before unlocking buffer */
		xwait = HeapTupleHeaderGetRawXmax(tp.t_data);
		infomask = tp.t_data->t_infomask;

		/*
		 * Sleep until concurrent transaction ends -- except when there's a
		 * single locker and it's our own transaction.  Note we don't care
		 * which lock mode the locker has, because we need the strongest one.
		 *
		 * Before sleeping, we need to acquire tuple lock to establish our
		 * priority for the tuple (see heap_lock_tuple).  LockTuple will
		 * release us when we are next-in-line for the tuple.
		 *
		 * If we are forced to "start over" below, we keep the tuple lock;
		 * this arranges that we stay at the head of the line while rechecking
		 * tuple state.
		 */
		if (infomask & HEAP_XMAX_IS_MULTI)
		{
			/* wait for multixact */
			if (DoesMultiXactIdConflict((MultiXactId) xwait, infomask,
										LockTupleExclusive))
			{
				LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

				/* acquire tuple lock, if necessary */
				heap_acquire_tuplock(relation, &(tp.t_self), LockTupleExclusive,
									 LockWaitBlock, &have_tuple_lock);

				/* wait for multixact */
				MultiXactIdWait((MultiXactId) xwait, MultiXactStatusUpdate, infomask,
								relation, &(tp.t_self), XLTW_Delete,
								NULL);
				LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

				/*
				 * If xwait had just locked the tuple then some other xact
				 * could update this tuple before we get to this point.  Check
				 * for xmax change, and start over if so.
				 */
				if (xmax_infomask_changed(tp.t_data->t_infomask, infomask) ||
					!TransactionIdEquals(HeapTupleHeaderGetRawXmax(tp.t_data),
										 xwait))
					goto l1;
			}

			/*
			 * You might think the multixact is necessarily done here, but not
			 * so: it could have surviving members, namely our own xact or
			 * other subxacts of this backend.  It is legal for us to delete
			 * the tuple in either case, however (the latter case is
			 * essentially a situation of upgrading our former shared lock to
			 * exclusive).  We don't bother changing the on-disk hint bits
			 * since we are about to overwrite the xmax altogether.
			 */
		}
		else if (!TransactionIdIsCurrentTransactionId(xwait))
		{
			/*
			 * Wait for regular transaction to end; but first, acquire tuple
			 * lock.
			 */
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			heap_acquire_tuplock(relation, &(tp.t_self), LockTupleExclusive,
								 LockWaitBlock, &have_tuple_lock);
			XactLockTableWait(xwait, relation, &(tp.t_self), XLTW_Delete);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

			/*
			 * xwait is done, but if xwait had just locked the tuple then some
			 * other xact could update this tuple before we get to this point.
			 * Check for xmax change, and start over if so.
			 */
			if (xmax_infomask_changed(tp.t_data->t_infomask, infomask) ||
				!TransactionIdEquals(HeapTupleHeaderGetRawXmax(tp.t_data),
									 xwait))
				goto l1;

			/* Otherwise check if it committed or aborted */
			UpdateXmaxHintBits(tp.t_data, buffer, xwait);
		}

		/*
		 * We may overwrite if previous xmax aborted, or if it committed but
		 * only locked the tuple without updating it.
		 */
		if ((tp.t_data->t_infomask & HEAP_XMAX_INVALID) ||
			HEAP_XMAX_IS_LOCKED_ONLY(tp.t_data->t_infomask) ||
			HeapTupleHeaderIsOnlyLocked(tp.t_data))
			result = HeapTupleMayBeUpdated;
		else
			result = HeapTupleUpdated;
	}

	if (crosscheck != InvalidSnapshot && result == HeapTupleMayBeUpdated)
	{
		/* Perform additional check for transaction-snapshot mode RI updates */
		if (!HeapTupleSatisfiesVisibility(&tp, crosscheck, buffer))
			result = HeapTupleUpdated;
	}

	if (result != HeapTupleMayBeUpdated)
	{
		Assert(result == HeapTupleSelfUpdated ||
			   result == HeapTupleUpdated ||
			   result == HeapTupleBeingUpdated);
		Assert(!(tp.t_data->t_infomask & HEAP_XMAX_INVALID));
		hufd->ctid = tp.t_data->t_ctid;
		hufd->xmax = HeapTupleHeaderGetUpdateXid(tp.t_data);
		if (result == HeapTupleSelfUpdated)
			hufd->cmax = HeapTupleHeaderGetCmax(tp.t_data);
		else
			hufd->cmax = InvalidCommandId;
		UnlockReleaseBuffer(buffer);
		if (have_tuple_lock)
			UnlockTupleTuplock(relation, &(tp.t_self), LockTupleExclusive);
		if (vmbuffer != InvalidBuffer)
			ReleaseBuffer(vmbuffer);
		return result;
	}

	/*
	 * We're about to do the actual delete -- check for conflict first, to
	 * avoid possibly having to roll back work we've just done.
	 *
	 * This is safe without a recheck as long as there is no possibility of
	 * another process scanning the page between this check and the delete
	 * being visible to the scan (i.e., an exclusive buffer content lock is
	 * continuously held from this point until the tuple delete is visible).
	 */
	CheckForSerializableConflictIn(relation, &tp, buffer);

	/* replace cid with a combo cid if necessary */
	HeapTupleHeaderAdjustCmax(tp.t_data, &cid, &iscombo);

	/*
	 * Compute replica identity tuple before entering the critical section so
	 * we don't PANIC upon a memory allocation failure.
	 */
	old_key_tuple = ExtractReplicaIdentity(relation, &tp, true, &old_key_copied);

	/*
	 * If this is the first possibly-multixact-able operation in the current
	 * transaction, set my per-backend OldestMemberMXactId setting. We can be
	 * certain that the transaction will never become a member of any older
	 * MultiXactIds than that.  (We have to do this even if we end up just
	 * using our own TransactionId below, since some other backend could
	 * incorporate our XID into a MultiXact immediately afterwards.)
	 */
	MultiXactIdSetOldestMember();

	compute_new_xmax_infomask(HeapTupleHeaderGetRawXmax(tp.t_data),
							  tp.t_data->t_infomask, tp.t_data->t_infomask2,
							  xid, LockTupleExclusive, true,
							  &new_xmax, &new_infomask, &new_infomask2);

	START_CRIT_SECTION();

	/*
	 * If this transaction commits, the tuple will become DEAD sooner or
	 * later.  Set flag that this page is a candidate for pruning once our xid
	 * falls below the OldestXmin horizon.  If the transaction finally aborts,
	 * the subsequent page pruning will be a no-op and the hint will be
	 * cleared.
	 */
	PageSetPrunable(page, xid);

	if (PageIsAllVisible(page))
	{
		all_visible_cleared = true;
		PageClearAllVisible(page);
		visibilitymap_clear(relation, BufferGetBlockNumber(buffer),
							vmbuffer);
	}

	/* store transaction information of xact deleting the tuple */
	tp.t_data->t_infomask &= ~(HEAP_XMAX_BITS | HEAP_MOVED);
	tp.t_data->t_infomask2 &= ~HEAP_KEYS_UPDATED;
	tp.t_data->t_infomask |= new_infomask;
	tp.t_data->t_infomask2 |= new_infomask2;
	HeapTupleHeaderClearHotUpdated(tp.t_data);
	HeapTupleHeaderSetXmax(tp.t_data, new_xmax);
	HeapTupleHeaderSetCmax(tp.t_data, cid, iscombo);
	/* Make sure there is no forward chain link in t_ctid */
	tp.t_data->t_ctid = tp.t_self;

	MarkBufferDirty(buffer);

	/*
	 * XLOG stuff
	 *
	 * NB: heap_abort_speculative() uses the same xlog record and replay
	 * routines.
	 */
	if (RelationNeedsWAL(relation))
	{
		xl_heap_delete xlrec;
		XLogRecPtr	recptr;

		/* For logical decode we need combocids to properly decode the catalog */
		if (RelationIsAccessibleInLogicalDecoding(relation))
			log_heap_new_cid(relation, &tp);

		xlrec.flags = all_visible_cleared ? XLH_DELETE_ALL_VISIBLE_CLEARED : 0;
		xlrec.infobits_set = compute_infobits(tp.t_data->t_infomask,
											  tp.t_data->t_infomask2);
		xlrec.offnum = ItemPointerGetOffsetNumber(&tp.t_self);
		xlrec.xmax = new_xmax;

		if (old_key_tuple != NULL)
		{
			if (relation->rd_rel->relreplident == REPLICA_IDENTITY_FULL)
				xlrec.flags |= XLH_DELETE_CONTAINS_OLD_TUPLE;
			else
				xlrec.flags |= XLH_DELETE_CONTAINS_OLD_KEY;
		}

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfHeapDelete);

		XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

		/*
		 * Log replica identity of the deleted tuple if there is one
		 */
		if (old_key_tuple != NULL)
		{
			xl_heap_header xlhdr;

			xlhdr.t_infomask2 = old_key_tuple->t_data->t_infomask2;
			xlhdr.t_infomask = old_key_tuple->t_data->t_infomask;
			xlhdr.t_hoff = old_key_tuple->t_data->t_hoff;

			XLogRegisterData((char *) &xlhdr, SizeOfHeapHeader);
			XLogRegisterData((char *) old_key_tuple->t_data
							 + SizeofHeapTupleHeader,
							 old_key_tuple->t_len
							 - SizeofHeapTupleHeader);
		}

		/* filtering by origin on a row level is much more efficient */
		XLogIncludeOrigin();

		recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_DELETE);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

	if (vmbuffer != InvalidBuffer)
		ReleaseBuffer(vmbuffer);

	/*
	 * If the tuple has toasted out-of-line attributes, we need to delete
	 * those items too.  We have to do this before releasing the buffer
	 * because we need to look at the contents of the tuple, but it's OK to
	 * release the content lock on the buffer first.
	 */
	if (relation->rd_rel->relkind != RELKIND_RELATION &&
		relation->rd_rel->relkind != RELKIND_MATVIEW)
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
	CacheInvalidateHeapTuple(relation, &tp, NULL);

	/* Now we can release the buffer */
	ReleaseBuffer(buffer);

	/*
	 * Release the lmgr tuple lock, if we had it.
	 */
	if (have_tuple_lock)
		UnlockTupleTuplock(relation, &(tp.t_self), LockTupleExclusive);

	pgstat_count_heap_delete(relation);

	if (old_key_tuple != NULL && old_key_copied)
		heap_freetuple(old_key_tuple);

	return HeapTupleMayBeUpdated;
}

/*
 *	simple_heap_delete - delete a tuple
 *
 * This routine may be used to delete a tuple when concurrent updates of
 * the target tuple are not expected (for example, because we have a lock
 * on the relation associated with the tuple).  Any failure is reported
 * via ereport().
 */
void
simple_heap_delete(Relation relation, ItemPointer tid)
{
	HTSU_Result result;
	HeapUpdateFailureData hufd;

	result = heap_delete(relation, tid,
						 GetCurrentCommandId(true), InvalidSnapshot,
						 true /* wait for commit */ ,
						 &hufd);
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
 *	cid - update command ID (used for visibility test, and stored into
 *		cmax/cmin if successful)
 *	crosscheck - if not InvalidSnapshot, also check old tuple against this
 *	wait - true if should wait for any conflicting update to commit/abort
 *	hufd - output parameter, filled in failure cases (see below)
 *	lockmode - output parameter, filled with lock mode acquired on tuple
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
 * In the failure cases, the routine fills *hufd with the tuple's t_ctid,
 * t_xmax (resolving a possible MultiXact, if necessary), and t_cmax
 * (the last only for HeapTupleSelfUpdated, since we
 * cannot obtain cmax from a combocid generated by another transaction).
 * See comments for struct HeapUpdateFailureData for additional info.
 */
HTSU_Result
heap_update(Relation relation, ItemPointer otid, HeapTuple newtup,
			CommandId cid, Snapshot crosscheck, bool wait,
			HeapUpdateFailureData *hufd, LockTupleMode *lockmode)
{
	HTSU_Result result;
	TransactionId xid = GetCurrentTransactionId();
	Bitmapset  *hot_attrs;
	Bitmapset  *key_attrs;
	Bitmapset  *id_attrs;
	ItemId		lp;
	HeapTupleData oldtup;
	HeapTuple	heaptup;
	HeapTuple	old_key_tuple = NULL;
	bool		old_key_copied = false;
	Page		page;
	BlockNumber block;
	MultiXactStatus mxact_status;
	Buffer		buffer,
				newbuf,
				vmbuffer = InvalidBuffer,
				vmbuffer_new = InvalidBuffer;
	bool		need_toast,
				already_marked;
	Size		newtupsize,
				pagefree;
	bool		have_tuple_lock = false;
	bool		iscombo;
	bool		satisfies_hot;
	bool		satisfies_key;
	bool		satisfies_id;
	bool		use_hot_update = false;
	bool		key_intact;
	bool		all_visible_cleared = false;
	bool		all_visible_cleared_new = false;
	bool		checked_lockers;
	bool		locker_remains;
	TransactionId xmax_new_tuple,
				xmax_old_tuple;
	uint16		infomask_old_tuple,
				infomask2_old_tuple,
				infomask_new_tuple,
				infomask2_new_tuple;

	Assert(ItemPointerIsValid(otid));

	/*
	 * Forbid this during a parallel operation, lets it allocate a combocid.
	 * Other workers might need that combocid for visibility checks, and we
	 * have no provision for broadcasting it to them.
	 */
	if (IsInParallelMode())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("cannot update tuples during a parallel operation")));

	/*
	 * Fetch the list of attributes to be checked for HOT update.  This is
	 * wasted effort if we fail to update or have to put the new tuple on a
	 * different page.  But we must compute the list before obtaining buffer
	 * lock --- in the worst case, if we are doing an update on one of the
	 * relevant system catalogs, we could deadlock if we try to fetch the list
	 * later.  In any case, the relcache caches the data so this is usually
	 * pretty cheap.
	 *
	 * Note that we get a copy here, so we need not worry about relcache flush
	 * happening midway through.
	 */
	hot_attrs = RelationGetIndexAttrBitmap(relation, INDEX_ATTR_BITMAP_ALL);
	key_attrs = RelationGetIndexAttrBitmap(relation, INDEX_ATTR_BITMAP_KEY);
	id_attrs = RelationGetIndexAttrBitmap(relation,
										  INDEX_ATTR_BITMAP_IDENTITY_KEY);

	block = ItemPointerGetBlockNumber(otid);
	buffer = ReadBuffer(relation, block);
	page = BufferGetPage(buffer);

	/*
	 * Before locking the buffer, pin the visibility map page if it appears to
	 * be necessary.  Since we haven't got the lock yet, someone else might be
	 * in the middle of changing this, so we'll need to recheck after we have
	 * the lock.
	 */
	if (PageIsAllVisible(page))
		visibilitymap_pin(relation, block, &vmbuffer);

	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	lp = PageGetItemId(page, ItemPointerGetOffsetNumber(otid));
	Assert(ItemIdIsNormal(lp));

	/*
	 * Fill in enough data in oldtup for HeapSatisfiesHOTandKeyUpdate to work
	 * properly.
	 */
	oldtup.t_tableOid = RelationGetRelid(relation);
	oldtup.t_data = (HeapTupleHeader) PageGetItem(page, lp);
	oldtup.t_len = ItemIdGetLength(lp);
	oldtup.t_self = *otid;

	/* the new tuple is ready, except for this: */
	newtup->t_tableOid = RelationGetRelid(relation);

	/* Fill in OID for newtup */
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

	/*
	 * If we're not updating any "key" column, we can grab a weaker lock type.
	 * This allows for more concurrency when we are running simultaneously
	 * with foreign key checks.
	 *
	 * Note that if a column gets detoasted while executing the update, but
	 * the value ends up being the same, this test will fail and we will use
	 * the stronger lock.  This is acceptable; the important case to optimize
	 * is updates that don't manipulate key columns, not those that
	 * serendipitiously arrive at the same key values.
	 */
	HeapSatisfiesHOTandKeyUpdate(relation, hot_attrs, key_attrs, id_attrs,
								 &satisfies_hot, &satisfies_key,
								 &satisfies_id, &oldtup, newtup);
	if (satisfies_key)
	{
		*lockmode = LockTupleNoKeyExclusive;
		mxact_status = MultiXactStatusNoKeyUpdate;
		key_intact = true;

		/*
		 * If this is the first possibly-multixact-able operation in the
		 * current transaction, set my per-backend OldestMemberMXactId
		 * setting. We can be certain that the transaction will never become a
		 * member of any older MultiXactIds than that.  (We have to do this
		 * even if we end up just using our own TransactionId below, since
		 * some other backend could incorporate our XID into a MultiXact
		 * immediately afterwards.)
		 */
		MultiXactIdSetOldestMember();
	}
	else
	{
		*lockmode = LockTupleExclusive;
		mxact_status = MultiXactStatusUpdate;
		key_intact = false;
	}

	/*
	 * Note: beyond this point, use oldtup not otid to refer to old tuple.
	 * otid may very well point at newtup->t_self, which we will overwrite
	 * with the new tuple's location, so there's great risk of confusion if we
	 * use otid anymore.
	 */

l2:
	checked_lockers = false;
	locker_remains = false;
	result = HeapTupleSatisfiesUpdate(&oldtup, cid, buffer);

	/* see below about the "no wait" case */
	Assert(result != HeapTupleBeingUpdated || wait);

	if (result == HeapTupleInvisible)
	{
		UnlockReleaseBuffer(buffer);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("attempted to update invisible tuple")));
	}
	else if (result == HeapTupleBeingUpdated && wait)
	{
		TransactionId xwait;
		uint16		infomask;
		bool		can_continue = false;

		/*
		 * XXX note that we don't consider the "no wait" case here.  This
		 * isn't a problem currently because no caller uses that case, but it
		 * should be fixed if such a caller is introduced.  It wasn't a
		 * problem previously because this code would always wait, but now
		 * that some tuple locks do not conflict with one of the lock modes we
		 * use, it is possible that this case is interesting to handle
		 * specially.
		 *
		 * This may cause failures with third-party code that calls
		 * heap_update directly.
		 */

		/* must copy state data before unlocking buffer */
		xwait = HeapTupleHeaderGetRawXmax(oldtup.t_data);
		infomask = oldtup.t_data->t_infomask;

		/*
		 * Now we have to do something about the existing locker.  If it's a
		 * multi, sleep on it; we might be awakened before it is completely
		 * gone (or even not sleep at all in some cases); we need to preserve
		 * it as locker, unless it is gone completely.
		 *
		 * If it's not a multi, we need to check for sleeping conditions
		 * before actually going to sleep.  If the update doesn't conflict
		 * with the locks, we just continue without sleeping (but making sure
		 * it is preserved).
		 *
		 * Before sleeping, we need to acquire tuple lock to establish our
		 * priority for the tuple (see heap_lock_tuple).  LockTuple will
		 * release us when we are next-in-line for the tuple.  Note we must
		 * not acquire the tuple lock until we're sure we're going to sleep;
		 * otherwise we're open for race conditions with other transactions
		 * holding the tuple lock which sleep on us.
		 *
		 * If we are forced to "start over" below, we keep the tuple lock;
		 * this arranges that we stay at the head of the line while rechecking
		 * tuple state.
		 */
		if (infomask & HEAP_XMAX_IS_MULTI)
		{
			TransactionId update_xact;
			int			remain;

			if (DoesMultiXactIdConflict((MultiXactId) xwait, infomask,
										*lockmode))
			{
				LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

				/* acquire tuple lock, if necessary */
				heap_acquire_tuplock(relation, &(oldtup.t_self), *lockmode,
									 LockWaitBlock, &have_tuple_lock);

				/* wait for multixact */
				MultiXactIdWait((MultiXactId) xwait, mxact_status, infomask,
								relation, &oldtup.t_self, XLTW_Update,
								&remain);
				checked_lockers = true;
				locker_remains = remain != 0;
				LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

				/*
				 * If xwait had just locked the tuple then some other xact
				 * could update this tuple before we get to this point.  Check
				 * for xmax change, and start over if so.
				 */
				if (xmax_infomask_changed(oldtup.t_data->t_infomask,
										  infomask) ||
				!TransactionIdEquals(HeapTupleHeaderGetRawXmax(oldtup.t_data),
									 xwait))
					goto l2;
			}

			/*
			 * Note that the multixact may not be done by now.  It could have
			 * surviving members; our own xact or other subxacts of this
			 * backend, and also any other concurrent transaction that locked
			 * the tuple with KeyShare if we only got TupleLockUpdate.  If
			 * this is the case, we have to be careful to mark the updated
			 * tuple with the surviving members in Xmax.
			 *
			 * Note that there could have been another update in the
			 * MultiXact. In that case, we need to check whether it committed
			 * or aborted. If it aborted we are safe to update it again;
			 * otherwise there is an update conflict, and we have to return
			 * HeapTupleUpdated below.
			 *
			 * In the LockTupleExclusive case, we still need to preserve the
			 * surviving members: those would include the tuple locks we had
			 * before this one, which are important to keep in case this
			 * subxact aborts.
			 */
			if (!HEAP_XMAX_IS_LOCKED_ONLY(oldtup.t_data->t_infomask))
				update_xact = HeapTupleGetUpdateXid(oldtup.t_data);
			else
				update_xact = InvalidTransactionId;

			/*
			 * There was no UPDATE in the MultiXact; or it aborted. No
			 * TransactionIdIsInProgress() call needed here, since we called
			 * MultiXactIdWait() above.
			 */
			if (!TransactionIdIsValid(update_xact) ||
				TransactionIdDidAbort(update_xact))
				can_continue = true;
		}
		else if (TransactionIdIsCurrentTransactionId(xwait))
		{
			/*
			 * The only locker is ourselves; we can avoid grabbing the tuple
			 * lock here, but must preserve our locking information.
			 */
			checked_lockers = true;
			locker_remains = true;
			can_continue = true;
		}
		else if (HEAP_XMAX_IS_KEYSHR_LOCKED(infomask) && key_intact)
		{
			/*
			 * If it's just a key-share locker, and we're not changing the key
			 * columns, we don't need to wait for it to end; but we need to
			 * preserve it as locker.
			 */
			checked_lockers = true;
			locker_remains = true;
			can_continue = true;
		}
		else
		{
			/*
			 * Wait for regular transaction to end; but first, acquire tuple
			 * lock.
			 */
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			heap_acquire_tuplock(relation, &(oldtup.t_self), *lockmode,
								 LockWaitBlock, &have_tuple_lock);
			XactLockTableWait(xwait, relation, &oldtup.t_self,
							  XLTW_Update);
			checked_lockers = true;
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

			/*
			 * xwait is done, but if xwait had just locked the tuple then some
			 * other xact could update this tuple before we get to this point.
			 * Check for xmax change, and start over if so.
			 */
			if (xmax_infomask_changed(oldtup.t_data->t_infomask, infomask) ||
				!TransactionIdEquals(xwait,
								   HeapTupleHeaderGetRawXmax(oldtup.t_data)))
				goto l2;

			/* Otherwise check if it committed or aborted */
			UpdateXmaxHintBits(oldtup.t_data, buffer, xwait);
			if (oldtup.t_data->t_infomask & HEAP_XMAX_INVALID)
				can_continue = true;
		}

		result = can_continue ? HeapTupleMayBeUpdated : HeapTupleUpdated;
	}

	if (crosscheck != InvalidSnapshot && result == HeapTupleMayBeUpdated)
	{
		/* Perform additional check for transaction-snapshot mode RI updates */
		if (!HeapTupleSatisfiesVisibility(&oldtup, crosscheck, buffer))
			result = HeapTupleUpdated;
	}

	if (result != HeapTupleMayBeUpdated)
	{
		Assert(result == HeapTupleSelfUpdated ||
			   result == HeapTupleUpdated ||
			   result == HeapTupleBeingUpdated);
		Assert(!(oldtup.t_data->t_infomask & HEAP_XMAX_INVALID));
		hufd->ctid = oldtup.t_data->t_ctid;
		hufd->xmax = HeapTupleHeaderGetUpdateXid(oldtup.t_data);
		if (result == HeapTupleSelfUpdated)
			hufd->cmax = HeapTupleHeaderGetCmax(oldtup.t_data);
		else
			hufd->cmax = InvalidCommandId;
		UnlockReleaseBuffer(buffer);
		if (have_tuple_lock)
			UnlockTupleTuplock(relation, &(oldtup.t_self), *lockmode);
		if (vmbuffer != InvalidBuffer)
			ReleaseBuffer(vmbuffer);
		bms_free(hot_attrs);
		bms_free(key_attrs);
		return result;
	}

	/*
	 * If we didn't pin the visibility map page and the page has become all
	 * visible while we were busy locking the buffer, or during some
	 * subsequent window during which we had it unlocked, we'll have to unlock
	 * and re-lock, to avoid holding the buffer lock across an I/O.  That's a
	 * bit unfortunate, especially since we'll now have to recheck whether the
	 * tuple has been locked or updated under us, but hopefully it won't
	 * happen very often.
	 */
	if (vmbuffer == InvalidBuffer && PageIsAllVisible(page))
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		visibilitymap_pin(relation, block, &vmbuffer);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		goto l2;
	}

	/* Fill in transaction status data */

	/*
	 * If the tuple we're updating is locked, we need to preserve the locking
	 * info in the old tuple's Xmax.  Prepare a new Xmax value for this.
	 */
	compute_new_xmax_infomask(HeapTupleHeaderGetRawXmax(oldtup.t_data),
							  oldtup.t_data->t_infomask,
							  oldtup.t_data->t_infomask2,
							  xid, *lockmode, true,
							  &xmax_old_tuple, &infomask_old_tuple,
							  &infomask2_old_tuple);

	/*
	 * And also prepare an Xmax value for the new copy of the tuple.  If there
	 * was no xmax previously, or there was one but all lockers are now gone,
	 * then use InvalidXid; otherwise, get the xmax from the old tuple.  (In
	 * rare cases that might also be InvalidXid and yet not have the
	 * HEAP_XMAX_INVALID bit set; that's fine.)
	 */
	if ((oldtup.t_data->t_infomask & HEAP_XMAX_INVALID) ||
		(checked_lockers && !locker_remains))
		xmax_new_tuple = InvalidTransactionId;
	else
		xmax_new_tuple = HeapTupleHeaderGetRawXmax(oldtup.t_data);

	if (!TransactionIdIsValid(xmax_new_tuple))
	{
		infomask_new_tuple = HEAP_XMAX_INVALID;
		infomask2_new_tuple = 0;
	}
	else
	{
		/*
		 * If we found a valid Xmax for the new tuple, then the infomask bits
		 * to use on the new tuple depend on what was there on the old one.
		 * Note that since we're doing an update, the only possibility is that
		 * the lockers had FOR KEY SHARE lock.
		 */
		if (oldtup.t_data->t_infomask & HEAP_XMAX_IS_MULTI)
		{
			GetMultiXactIdHintBits(xmax_new_tuple, &infomask_new_tuple,
								   &infomask2_new_tuple);
		}
		else
		{
			infomask_new_tuple = HEAP_XMAX_KEYSHR_LOCK | HEAP_XMAX_LOCK_ONLY;
			infomask2_new_tuple = 0;
		}
	}

	/*
	 * Prepare the new tuple with the appropriate initial values of Xmin and
	 * Xmax, as well as initial infomask bits as computed above.
	 */
	newtup->t_data->t_infomask &= ~(HEAP_XACT_MASK);
	newtup->t_data->t_infomask2 &= ~(HEAP2_XACT_MASK);
	HeapTupleHeaderSetXmin(newtup->t_data, xid);
	HeapTupleHeaderSetCmin(newtup->t_data, cid);
	newtup->t_data->t_infomask |= HEAP_UPDATED | infomask_new_tuple;
	newtup->t_data->t_infomask2 |= infomask2_new_tuple;
	HeapTupleHeaderSetXmax(newtup->t_data, xmax_new_tuple);

	/*
	 * Replace cid with a combo cid if necessary.  Note that we already put
	 * the plain cid into the new tuple.
	 */
	HeapTupleHeaderAdjustCmax(oldtup.t_data, &cid, &iscombo);

	/*
	 * If the toaster needs to be activated, OR if the new tuple will not fit
	 * on the same page as the old, then we need to release the content lock
	 * (but not the pin!) on the old tuple's buffer while we are off doing
	 * TOAST and/or table-file-extension work.  We must mark the old tuple to
	 * show that it's already being updated, else other processes may try to
	 * update it themselves.
	 *
	 * We need to invoke the toaster if there are already any out-of-line
	 * toasted values present, or if the new tuple is over-threshold.
	 */
	if (relation->rd_rel->relkind != RELKIND_RELATION &&
		relation->rd_rel->relkind != RELKIND_MATVIEW)
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

	pagefree = PageGetHeapFreeSpace(page);

	newtupsize = MAXALIGN(newtup->t_len);

	if (need_toast || newtupsize > pagefree)
	{
		/* Clear obsolete visibility flags ... */
		oldtup.t_data->t_infomask &= ~(HEAP_XMAX_BITS | HEAP_MOVED);
		oldtup.t_data->t_infomask2 &= ~HEAP_KEYS_UPDATED;
		HeapTupleClearHotUpdated(&oldtup);
		/* ... and store info about transaction updating this tuple */
		Assert(TransactionIdIsValid(xmax_old_tuple));
		HeapTupleHeaderSetXmax(oldtup.t_data, xmax_old_tuple);
		oldtup.t_data->t_infomask |= infomask_old_tuple;
		oldtup.t_data->t_infomask2 |= infomask2_old_tuple;
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
			heaptup = toast_insert_or_update(relation, newtup, &oldtup, 0);
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
		 * buffer locks on both old and new pages.  To avoid deadlock against
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
											   buffer, 0, NULL,
											   &vmbuffer_new, &vmbuffer);
		}
		else
		{
			/* Re-acquire the lock on the old tuple's page. */
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
			/* Re-check using the up-to-date free space */
			pagefree = PageGetHeapFreeSpace(page);
			if (newtupsize > pagefree)
			{
				/*
				 * Rats, it doesn't fit anymore.  We must now unlock and
				 * relock to avoid deadlock.  Fortunately, this path should
				 * seldom be taken.
				 */
				LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
				newbuf = RelationGetBufferForTuple(relation, heaptup->t_len,
												   buffer, 0, NULL,
												   &vmbuffer_new, &vmbuffer);
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
	 * We're about to do the actual update -- check for conflict first, to
	 * avoid possibly having to roll back work we've just done.
	 *
	 * This is safe without a recheck as long as there is no possibility of
	 * another process scanning the pages between this check and the update
	 * being visible to the scan (i.e., exclusive buffer content lock(s) are
	 * continuously held from this point until the tuple update is visible).
	 *
	 * For the new tuple the only check needed is at the relation level, but
	 * since both tuples are in the same relation and the check for oldtup
	 * will include checking the relation level, there is no benefit to a
	 * separate check for the new tuple.
	 */
	CheckForSerializableConflictIn(relation, &oldtup, buffer);

	/*
	 * At this point newbuf and buffer are both pinned and locked, and newbuf
	 * has enough space for the new tuple.  If they are the same buffer, only
	 * one pin is held.
	 */

	if (newbuf == buffer)
	{
		/*
		 * Since the new tuple is going into the same page, we might be able
		 * to do a HOT update.  Check if any of the index columns have been
		 * changed.  If not, then HOT update is possible.
		 */
		if (satisfies_hot)
			use_hot_update = true;
	}
	else
	{
		/* Set a hint that the old page could use prune/defrag */
		PageSetFull(page);
	}

	/*
	 * Compute replica identity tuple before entering the critical section so
	 * we don't PANIC upon a memory allocation failure.
	 * ExtractReplicaIdentity() will return NULL if nothing needs to be
	 * logged.
	 */
	old_key_tuple = ExtractReplicaIdentity(relation, &oldtup, !satisfies_id, &old_key_copied);

	/* NO EREPORT(ERROR) from here till changes are logged */
	START_CRIT_SECTION();

	/*
	 * If this transaction commits, the old tuple will become DEAD sooner or
	 * later.  Set flag that this page is a candidate for pruning once our xid
	 * falls below the OldestXmin horizon.  If the transaction finally aborts,
	 * the subsequent page pruning will be a no-op and the hint will be
	 * cleared.
	 *
	 * XXX Should we set hint on newbuf as well?  If the transaction aborts,
	 * there would be a prunable tuple in the newbuf; but for now we choose
	 * not to optimize for aborts.  Note that heap_xlog_update must be kept in
	 * sync if this decision changes.
	 */
	PageSetPrunable(page, xid);

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

	RelationPutHeapTuple(relation, newbuf, heaptup, false);		/* insert new tuple */

	if (!already_marked)
	{
		/* Clear obsolete visibility flags ... */
		oldtup.t_data->t_infomask &= ~(HEAP_XMAX_BITS | HEAP_MOVED);
		oldtup.t_data->t_infomask2 &= ~HEAP_KEYS_UPDATED;
		/* ... and store info about transaction updating this tuple */
		Assert(TransactionIdIsValid(xmax_old_tuple));
		HeapTupleHeaderSetXmax(oldtup.t_data, xmax_old_tuple);
		oldtup.t_data->t_infomask |= infomask_old_tuple;
		oldtup.t_data->t_infomask2 |= infomask2_old_tuple;
		HeapTupleHeaderSetCmax(oldtup.t_data, cid, iscombo);
	}

	/* record address of new tuple in t_ctid of old one */
	oldtup.t_data->t_ctid = heaptup->t_self;

	/* clear PD_ALL_VISIBLE flags */
	if (PageIsAllVisible(BufferGetPage(buffer)))
	{
		all_visible_cleared = true;
		PageClearAllVisible(BufferGetPage(buffer));
		visibilitymap_clear(relation, BufferGetBlockNumber(buffer),
							vmbuffer);
	}
	if (newbuf != buffer && PageIsAllVisible(BufferGetPage(newbuf)))
	{
		all_visible_cleared_new = true;
		PageClearAllVisible(BufferGetPage(newbuf));
		visibilitymap_clear(relation, BufferGetBlockNumber(newbuf),
							vmbuffer_new);
	}

	if (newbuf != buffer)
		MarkBufferDirty(newbuf);
	MarkBufferDirty(buffer);

	/* XLOG stuff */
	if (RelationNeedsWAL(relation))
	{
		XLogRecPtr	recptr;

		/*
		 * For logical decoding we need combocids to properly decode the
		 * catalog.
		 */
		if (RelationIsAccessibleInLogicalDecoding(relation))
		{
			log_heap_new_cid(relation, &oldtup);
			log_heap_new_cid(relation, heaptup);
		}

		recptr = log_heap_update(relation, buffer,
								 newbuf, &oldtup, heaptup,
								 old_key_tuple,
								 all_visible_cleared,
								 all_visible_cleared_new);
		if (newbuf != buffer)
		{
			PageSetLSN(BufferGetPage(newbuf), recptr);
		}
		PageSetLSN(BufferGetPage(buffer), recptr);
	}

	END_CRIT_SECTION();

	if (newbuf != buffer)
		LockBuffer(newbuf, BUFFER_LOCK_UNLOCK);
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

	/*
	 * Mark old tuple for invalidation from system caches at next command
	 * boundary, and mark the new tuple for invalidation in case we abort. We
	 * have to do this before releasing the buffer because oldtup is in the
	 * buffer.  (heaptup is all in local memory, but it's necessary to process
	 * both tuple versions in one call to inval.c so we can avoid redundant
	 * sinval messages.)
	 */
	CacheInvalidateHeapTuple(relation, &oldtup, heaptup);

	/* Now we can release the buffer(s) */
	if (newbuf != buffer)
		ReleaseBuffer(newbuf);
	ReleaseBuffer(buffer);
	if (BufferIsValid(vmbuffer_new))
		ReleaseBuffer(vmbuffer_new);
	if (BufferIsValid(vmbuffer))
		ReleaseBuffer(vmbuffer);

	/*
	 * Release the lmgr tuple lock, if we had it.
	 */
	if (have_tuple_lock)
		UnlockTupleTuplock(relation, &(oldtup.t_self), *lockmode);

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

	if (old_key_tuple != NULL && old_key_copied)
		heap_freetuple(old_key_tuple);

	bms_free(hot_attrs);
	bms_free(key_attrs);

	return HeapTupleMayBeUpdated;
}

/*
 * Check if the specified attribute's value is same in both given tuples.
 * Subroutine for HeapSatisfiesHOTandKeyUpdate.
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
	 * there are many indexed columns.  Should HeapSatisfiesHOTandKeyUpdate do
	 * a single heap_deform_tuple call on each tuple, instead?	But that
	 * doesn't work for system columns ...
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
	 * same logical value.  But we should be OK as long as there are no false
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
 * Check which columns are being updated.
 *
 * This simultaneously checks conditions for HOT updates, for FOR KEY
 * SHARE updates, and REPLICA IDENTITY concerns.  Since much of the time they
 * will be checking very similar sets of columns, and doing the same tests on
 * them, it makes sense to optimize and do them together.
 *
 * We receive three bitmapsets comprising the three sets of columns we're
 * interested in.  Note these are destructively modified; that is OK since
 * this is invoked at most once in heap_update.
 *
 * hot_result is set to TRUE if it's okay to do a HOT update (i.e. it does not
 * modified indexed columns); key_result is set to TRUE if the update does not
 * modify columns used in the key; id_result is set to TRUE if the update does
 * not modify columns in any index marked as the REPLICA IDENTITY.
 */
static void
HeapSatisfiesHOTandKeyUpdate(Relation relation, Bitmapset *hot_attrs,
							 Bitmapset *key_attrs, Bitmapset *id_attrs,
							 bool *satisfies_hot, bool *satisfies_key,
							 bool *satisfies_id,
							 HeapTuple oldtup, HeapTuple newtup)
{
	int			next_hot_attnum;
	int			next_key_attnum;
	int			next_id_attnum;
	bool		hot_result = true;
	bool		key_result = true;
	bool		id_result = true;

	/* If REPLICA IDENTITY is set to FULL, id_attrs will be empty. */
	Assert(bms_is_subset(id_attrs, key_attrs));
	Assert(bms_is_subset(key_attrs, hot_attrs));

	/*
	 * If one of these sets contains no remaining bits, bms_first_member will
	 * return -1, and after adding FirstLowInvalidHeapAttributeNumber (which
	 * is negative!)  we'll get an attribute number that can't possibly be
	 * real, and thus won't match any actual attribute number.
	 */
	next_hot_attnum = bms_first_member(hot_attrs);
	next_hot_attnum += FirstLowInvalidHeapAttributeNumber;
	next_key_attnum = bms_first_member(key_attrs);
	next_key_attnum += FirstLowInvalidHeapAttributeNumber;
	next_id_attnum = bms_first_member(id_attrs);
	next_id_attnum += FirstLowInvalidHeapAttributeNumber;

	for (;;)
	{
		bool		changed;
		int			check_now;

		/*
		 * Since the HOT attributes are a superset of the key attributes and
		 * the key attributes are a superset of the id attributes, this logic
		 * is guaranteed to identify the next column that needs to be checked.
		 */
		if (hot_result && next_hot_attnum > FirstLowInvalidHeapAttributeNumber)
			check_now = next_hot_attnum;
		else if (key_result && next_key_attnum > FirstLowInvalidHeapAttributeNumber)
			check_now = next_key_attnum;
		else if (id_result && next_id_attnum > FirstLowInvalidHeapAttributeNumber)
			check_now = next_id_attnum;
		else
			break;

		/* See whether it changed. */
		changed = !heap_tuple_attr_equals(RelationGetDescr(relation),
										  check_now, oldtup, newtup);
		if (changed)
		{
			if (check_now == next_hot_attnum)
				hot_result = false;
			if (check_now == next_key_attnum)
				key_result = false;
			if (check_now == next_id_attnum)
				id_result = false;

			/* if all are false now, we can stop checking */
			if (!hot_result && !key_result && !id_result)
				break;
		}

		/*
		 * Advance the next attribute numbers for the sets that contain the
		 * attribute we just checked.  As we work our way through the columns,
		 * the next_attnum values will rise; but when each set becomes empty,
		 * bms_first_member() will return -1 and the attribute number will end
		 * up with a value less than FirstLowInvalidHeapAttributeNumber.
		 */
		if (hot_result && check_now == next_hot_attnum)
		{
			next_hot_attnum = bms_first_member(hot_attrs);
			next_hot_attnum += FirstLowInvalidHeapAttributeNumber;
		}
		if (key_result && check_now == next_key_attnum)
		{
			next_key_attnum = bms_first_member(key_attrs);
			next_key_attnum += FirstLowInvalidHeapAttributeNumber;
		}
		if (id_result && check_now == next_id_attnum)
		{
			next_id_attnum = bms_first_member(id_attrs);
			next_id_attnum += FirstLowInvalidHeapAttributeNumber;
		}
	}

	*satisfies_hot = hot_result;
	*satisfies_key = key_result;
	*satisfies_id = id_result;
}

/*
 *	simple_heap_update - replace a tuple
 *
 * This routine may be used to update a tuple when concurrent updates of
 * the target tuple are not expected (for example, because we have a lock
 * on the relation associated with the tuple).  Any failure is reported
 * via ereport().
 */
void
simple_heap_update(Relation relation, ItemPointer otid, HeapTuple tup)
{
	HTSU_Result result;
	HeapUpdateFailureData hufd;
	LockTupleMode lockmode;

	result = heap_update(relation, otid, tup,
						 GetCurrentCommandId(true), InvalidSnapshot,
						 true /* wait for commit */ ,
						 &hufd, &lockmode);
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
 * Return the MultiXactStatus corresponding to the given tuple lock mode.
 */
static MultiXactStatus
get_mxact_status_for_lock(LockTupleMode mode, bool is_update)
{
	int			retval;

	if (is_update)
		retval = tupleLockExtraInfo[mode].updstatus;
	else
		retval = tupleLockExtraInfo[mode].lockstatus;

	if (retval == -1)
		elog(ERROR, "invalid lock tuple mode %d/%s", mode,
			 is_update ? "true" : "false");

	return (MultiXactStatus) retval;
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
 *	wait_policy: what to do if tuple lock is not available
 *	follow_updates: if true, follow the update chain to also lock descendant
 *		tuples.
 *
 * Output parameters:
 *	*tuple: all fields filled in
 *	*buffer: set to buffer holding tuple (pinned but not locked at exit)
 *	*hufd: filled in failure cases (see below)
 *
 * Function result may be:
 *	HeapTupleMayBeUpdated: lock was successfully acquired
 *	HeapTupleInvisible: lock failed because tuple was never visible to us
 *	HeapTupleSelfUpdated: lock failed because tuple updated by self
 *	HeapTupleUpdated: lock failed because tuple updated by other xact
 *	HeapTupleWouldBlock: lock couldn't be acquired and wait_policy is skip
 *
 * In the failure cases other than HeapTupleInvisible, the routine fills
 * *hufd with the tuple's t_ctid, t_xmax (resolving a possible MultiXact,
 * if necessary), and t_cmax (the last only for HeapTupleSelfUpdated,
 * since we cannot obtain cmax from a combocid generated by another
 * transaction).
 * See comments for struct HeapUpdateFailureData for additional info.
 *
 * See README.tuplock for a thorough explanation of this mechanism.
 */
HTSU_Result
heap_lock_tuple(Relation relation, HeapTuple tuple,
				CommandId cid, LockTupleMode mode, LockWaitPolicy wait_policy,
				bool follow_updates,
				Buffer *buffer, HeapUpdateFailureData *hufd)
{
	HTSU_Result result;
	ItemPointer tid = &(tuple->t_self);
	ItemId		lp;
	Page		page;
	TransactionId xid,
				xmax;
	uint16		old_infomask,
				new_infomask,
				new_infomask2;
	bool		first_time = true;
	bool		have_tuple_lock = false;

	*buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(tid));
	LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);

	page = BufferGetPage(*buffer);
	lp = PageGetItemId(page, ItemPointerGetOffsetNumber(tid));
	Assert(ItemIdIsNormal(lp));

	tuple->t_data = (HeapTupleHeader) PageGetItem(page, lp);
	tuple->t_len = ItemIdGetLength(lp);
	tuple->t_tableOid = RelationGetRelid(relation);

l3:
	result = HeapTupleSatisfiesUpdate(tuple, cid, *buffer);

	if (result == HeapTupleInvisible)
	{
		LockBuffer(*buffer, BUFFER_LOCK_UNLOCK);

		/*
		 * This is possible, but only when locking a tuple for ON CONFLICT
		 * UPDATE.  We return this value here rather than throwing an error in
		 * order to give that case the opportunity to throw a more specific
		 * error.
		 */
		return HeapTupleInvisible;
	}
	else if (result == HeapTupleBeingUpdated)
	{
		TransactionId xwait;
		uint16		infomask;
		uint16		infomask2;
		bool		require_sleep;
		ItemPointerData t_ctid;

		/* must copy state data before unlocking buffer */
		xwait = HeapTupleHeaderGetRawXmax(tuple->t_data);
		infomask = tuple->t_data->t_infomask;
		infomask2 = tuple->t_data->t_infomask2;
		ItemPointerCopy(&tuple->t_data->t_ctid, &t_ctid);

		LockBuffer(*buffer, BUFFER_LOCK_UNLOCK);

		/*
		 * If any subtransaction of the current top transaction already holds
		 * a lock as strong as or stronger than what we're requesting, we
		 * effectively hold the desired lock already.  We *must* succeed
		 * without trying to take the tuple lock, else we will deadlock
		 * against anyone wanting to acquire a stronger lock.
		 *
		 * Note we only do this the first time we loop on the HTSU result;
		 * there is no point in testing in subsequent passes, because
		 * evidently our own transaction cannot have acquired a new lock after
		 * the first time we checked.
		 */
		if (first_time)
		{
			first_time = false;

			if (infomask & HEAP_XMAX_IS_MULTI)
			{
				int			i;
				int			nmembers;
				MultiXactMember *members;

				/*
				 * We don't need to allow old multixacts here; if that had
				 * been the case, HeapTupleSatisfiesUpdate would have returned
				 * MayBeUpdated and we wouldn't be here.
				 */
				nmembers =
					GetMultiXactIdMembers(xwait, &members, false,
										  HEAP_XMAX_IS_LOCKED_ONLY(infomask));

				for (i = 0; i < nmembers; i++)
				{
					/* only consider members of our own transaction */
					if (!TransactionIdIsCurrentTransactionId(members[i].xid))
						continue;

					if (TUPLOCK_from_mxstatus(members[i].status) >= mode)
					{
						pfree(members);
						return HeapTupleMayBeUpdated;
					}
				}

				if (members)
					pfree(members);
			}
			else if (TransactionIdIsCurrentTransactionId(xwait))
			{
				switch (mode)
				{
					case LockTupleKeyShare:
						Assert(HEAP_XMAX_IS_KEYSHR_LOCKED(infomask) ||
							   HEAP_XMAX_IS_SHR_LOCKED(infomask) ||
							   HEAP_XMAX_IS_EXCL_LOCKED(infomask));
						return HeapTupleMayBeUpdated;
						break;
					case LockTupleShare:
						if (HEAP_XMAX_IS_SHR_LOCKED(infomask) ||
							HEAP_XMAX_IS_EXCL_LOCKED(infomask))
							return HeapTupleMayBeUpdated;
						break;
					case LockTupleNoKeyExclusive:
						if (HEAP_XMAX_IS_EXCL_LOCKED(infomask))
							return HeapTupleMayBeUpdated;
						break;
					case LockTupleExclusive:
						if (HEAP_XMAX_IS_EXCL_LOCKED(infomask) &&
							infomask2 & HEAP_KEYS_UPDATED)
							return HeapTupleMayBeUpdated;
						break;
				}
			}
		}

		/*
		 * Initially assume that we will have to wait for the locking
		 * transaction(s) to finish.  We check various cases below in which
		 * this can be turned off.
		 */
		require_sleep = true;
		if (mode == LockTupleKeyShare)
		{
			/*
			 * If we're requesting KeyShare, and there's no update present, we
			 * don't need to wait.  Even if there is an update, we can still
			 * continue if the key hasn't been modified.
			 *
			 * However, if there are updates, we need to walk the update chain
			 * to mark future versions of the row as locked, too.  That way,
			 * if somebody deletes that future version, we're protected
			 * against the key going away.  This locking of future versions
			 * could block momentarily, if a concurrent transaction is
			 * deleting a key; or it could return a value to the effect that
			 * the transaction deleting the key has already committed.  So we
			 * do this before re-locking the buffer; otherwise this would be
			 * prone to deadlocks.
			 *
			 * Note that the TID we're locking was grabbed before we unlocked
			 * the buffer.  For it to change while we're not looking, the
			 * other properties we're testing for below after re-locking the
			 * buffer would also change, in which case we would restart this
			 * loop above.
			 */
			if (!(infomask2 & HEAP_KEYS_UPDATED))
			{
				bool		updated;

				updated = !HEAP_XMAX_IS_LOCKED_ONLY(infomask);

				/*
				 * If there are updates, follow the update chain; bail out if
				 * that cannot be done.
				 */
				if (follow_updates && updated)
				{
					HTSU_Result res;

					res = heap_lock_updated_tuple(relation, tuple, &t_ctid,
												  GetCurrentTransactionId(),
												  mode);
					if (res != HeapTupleMayBeUpdated)
					{
						result = res;
						/* recovery code expects to have buffer lock held */
						LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);
						goto failed;
					}
				}

				LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);

				/*
				 * Make sure it's still an appropriate lock, else start over.
				 * Also, if it wasn't updated before we released the lock, but
				 * is updated now, we start over too; the reason is that we
				 * now need to follow the update chain to lock the new
				 * versions.
				 */
				if (!HeapTupleHeaderIsOnlyLocked(tuple->t_data) &&
					((tuple->t_data->t_infomask2 & HEAP_KEYS_UPDATED) ||
					 !updated))
					goto l3;

				/* Things look okay, so we can skip sleeping */
				require_sleep = false;

				/*
				 * Note we allow Xmax to change here; other updaters/lockers
				 * could have modified it before we grabbed the buffer lock.
				 * However, this is not a problem, because with the recheck we
				 * just did we ensure that they still don't conflict with the
				 * lock we want.
				 */
			}
		}
		else if (mode == LockTupleShare)
		{
			/*
			 * If we're requesting Share, we can similarly avoid sleeping if
			 * there's no update and no exclusive lock present.
			 */
			if (HEAP_XMAX_IS_LOCKED_ONLY(infomask) &&
				!HEAP_XMAX_IS_EXCL_LOCKED(infomask))
			{
				LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);

				/*
				 * Make sure it's still an appropriate lock, else start over.
				 * See above about allowing xmax to change.
				 */
				if (!HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_data->t_infomask) ||
					HEAP_XMAX_IS_EXCL_LOCKED(tuple->t_data->t_infomask))
					goto l3;
				require_sleep = false;
			}
		}
		else if (mode == LockTupleNoKeyExclusive)
		{
			/*
			 * If we're requesting NoKeyExclusive, we might also be able to
			 * avoid sleeping; just ensure that there no conflicting lock
			 * already acquired.
			 */
			if (infomask & HEAP_XMAX_IS_MULTI)
			{
				if (!DoesMultiXactIdConflict((MultiXactId) xwait, infomask,
											 mode))
				{
					/*
					 * No conflict, but if the xmax changed under us in the
					 * meantime, start over.
					 */
					LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);
					if (xmax_infomask_changed(tuple->t_data->t_infomask, infomask) ||
						!TransactionIdEquals(HeapTupleHeaderGetRawXmax(tuple->t_data),
											 xwait))
						goto l3;

					/* otherwise, we're good */
					require_sleep = false;
				}
			}
			else if (HEAP_XMAX_IS_KEYSHR_LOCKED(infomask))
			{
				LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);

				/* if the xmax changed in the meantime, start over */
				if (xmax_infomask_changed(tuple->t_data->t_infomask, infomask) ||
					!TransactionIdEquals(
									HeapTupleHeaderGetRawXmax(tuple->t_data),
										 xwait))
					goto l3;
				/* otherwise, we're good */
				require_sleep = false;
			}
		}

		/*
		 * As a check independent from those above, we can also avoid sleeping
		 * if the current transaction is the sole locker of the tuple.  Note
		 * that the strength of the lock already held is irrelevant; this is
		 * not about recording the lock in Xmax (which will be done regardless
		 * of this optimization, below).  Also, note that the cases where we
		 * hold a lock stronger than we are requesting are already handled
		 * above by not doing anything.
		 *
		 * Note we only deal with the non-multixact case here; MultiXactIdWait
		 * is well equipped to deal with this situation on its own.
		 */
		if (require_sleep && !(infomask & HEAP_XMAX_IS_MULTI) &&
			TransactionIdIsCurrentTransactionId(xwait))
		{
			/* ... but if the xmax changed in the meantime, start over */
			LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);
			if (xmax_infomask_changed(tuple->t_data->t_infomask, infomask) ||
				!TransactionIdEquals(HeapTupleHeaderGetRawXmax(tuple->t_data),
									 xwait))
				goto l3;
			Assert(HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_data->t_infomask));
			require_sleep = false;
		}

		/*
		 * By here, we either have already acquired the buffer exclusive lock,
		 * or we must wait for the locking transaction or multixact; so below
		 * we ensure that we grab buffer lock after the sleep.
		 */

		if (require_sleep)
		{
			/*
			 * Acquire tuple lock to establish our priority for the tuple, or
			 * die trying.  LockTuple will release us when we are next-in-line
			 * for the tuple.  We must do this even if we are share-locking.
			 *
			 * If we are forced to "start over" below, we keep the tuple lock;
			 * this arranges that we stay at the head of the line while
			 * rechecking tuple state.
			 */
			if (!heap_acquire_tuplock(relation, tid, mode, wait_policy,
									  &have_tuple_lock))
			{
				/*
				 * This can only happen if wait_policy is Skip and the lock
				 * couldn't be obtained.
				 */
				result = HeapTupleWouldBlock;
				/* recovery code expects to have buffer lock held */
				LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);
				goto failed;
			}

			if (infomask & HEAP_XMAX_IS_MULTI)
			{
				MultiXactStatus status = get_mxact_status_for_lock(mode, false);

				/* We only ever lock tuples, never update them */
				if (status >= MultiXactStatusNoKeyUpdate)
					elog(ERROR, "invalid lock mode in heap_lock_tuple");

				/* wait for multixact to end, or die trying  */
				switch (wait_policy)
				{
					case LockWaitBlock:
						MultiXactIdWait((MultiXactId) xwait, status, infomask,
								  relation, &tuple->t_self, XLTW_Lock, NULL);
						break;
					case LockWaitSkip:
						if (!ConditionalMultiXactIdWait((MultiXactId) xwait,
												  status, infomask, relation,
														NULL))
						{
							result = HeapTupleWouldBlock;
							/* recovery code expects to have buffer lock held */
							LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);
							goto failed;
						}
						break;
					case LockWaitError:
						if (!ConditionalMultiXactIdWait((MultiXactId) xwait,
												  status, infomask, relation,
														NULL))
							ereport(ERROR,
									(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
									 errmsg("could not obtain lock on row in relation \"%s\"",
										RelationGetRelationName(relation))));

						break;
				}

				/*
				 * Of course, the multixact might not be done here: if we're
				 * requesting a light lock mode, other transactions with light
				 * locks could still be alive, as well as locks owned by our
				 * own xact or other subxacts of this backend.  We need to
				 * preserve the surviving MultiXact members.  Note that it
				 * isn't absolutely necessary in the latter case, but doing so
				 * is simpler.
				 */
			}
			else
			{
				/* wait for regular transaction to end, or die trying */
				switch (wait_policy)
				{
					case LockWaitBlock:
						XactLockTableWait(xwait, relation, &tuple->t_self,
										  XLTW_Lock);
						break;
					case LockWaitSkip:
						if (!ConditionalXactLockTableWait(xwait))
						{
							result = HeapTupleWouldBlock;
							/* recovery code expects to have buffer lock held */
							LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);
							goto failed;
						}
						break;
					case LockWaitError:
						if (!ConditionalXactLockTableWait(xwait))
							ereport(ERROR,
									(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
									 errmsg("could not obtain lock on row in relation \"%s\"",
										RelationGetRelationName(relation))));
						break;
				}
			}

			/* if there are updates, follow the update chain */
			if (follow_updates && !HEAP_XMAX_IS_LOCKED_ONLY(infomask))
			{
				HTSU_Result res;

				res = heap_lock_updated_tuple(relation, tuple, &t_ctid,
											  GetCurrentTransactionId(),
											  mode);
				if (res != HeapTupleMayBeUpdated)
				{
					result = res;
					/* recovery code expects to have buffer lock held */
					LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);
					goto failed;
				}
			}

			LockBuffer(*buffer, BUFFER_LOCK_EXCLUSIVE);

			/*
			 * xwait is done, but if xwait had just locked the tuple then some
			 * other xact could update this tuple before we get to this point.
			 * Check for xmax change, and start over if so.
			 */
			if (xmax_infomask_changed(tuple->t_data->t_infomask, infomask) ||
				!TransactionIdEquals(HeapTupleHeaderGetRawXmax(tuple->t_data),
									 xwait))
				goto l3;

			if (!(infomask & HEAP_XMAX_IS_MULTI))
			{
				/*
				 * Otherwise check if it committed or aborted.  Note we cannot
				 * be here if the tuple was only locked by somebody who didn't
				 * conflict with us; that would have been handled above.  So
				 * that transaction must necessarily be gone by now.  But
				 * don't check for this in the multixact case, because some
				 * locker transactions might still be running.
				 */
				UpdateXmaxHintBits(tuple->t_data, *buffer, xwait);
			}
		}

		/* By here, we're certain that we hold buffer exclusive lock again */

		/*
		 * We may lock if previous xmax aborted, or if it committed but only
		 * locked the tuple without updating it; or if we didn't have to wait
		 * at all for whatever reason.
		 */
		if (!require_sleep ||
			(tuple->t_data->t_infomask & HEAP_XMAX_INVALID) ||
			HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_data->t_infomask) ||
			HeapTupleHeaderIsOnlyLocked(tuple->t_data))
			result = HeapTupleMayBeUpdated;
		else
			result = HeapTupleUpdated;
	}

failed:
	if (result != HeapTupleMayBeUpdated)
	{
		Assert(result == HeapTupleSelfUpdated || result == HeapTupleUpdated ||
			   result == HeapTupleWouldBlock);
		Assert(!(tuple->t_data->t_infomask & HEAP_XMAX_INVALID));
		hufd->ctid = tuple->t_data->t_ctid;
		hufd->xmax = HeapTupleHeaderGetUpdateXid(tuple->t_data);
		if (result == HeapTupleSelfUpdated)
			hufd->cmax = HeapTupleHeaderGetCmax(tuple->t_data);
		else
			hufd->cmax = InvalidCommandId;
		LockBuffer(*buffer, BUFFER_LOCK_UNLOCK);
		if (have_tuple_lock)
			UnlockTupleTuplock(relation, tid, mode);
		return result;
	}

	xmax = HeapTupleHeaderGetRawXmax(tuple->t_data);
	old_infomask = tuple->t_data->t_infomask;

	/*
	 * If this is the first possibly-multixact-able operation in the current
	 * transaction, set my per-backend OldestMemberMXactId setting. We can be
	 * certain that the transaction will never become a member of any older
	 * MultiXactIds than that.  (We have to do this even if we end up just
	 * using our own TransactionId below, since some other backend could
	 * incorporate our XID into a MultiXact immediately afterwards.)
	 */
	MultiXactIdSetOldestMember();

	/*
	 * Compute the new xmax and infomask to store into the tuple.  Note we do
	 * not modify the tuple just yet, because that would leave it in the wrong
	 * state if multixact.c elogs.
	 */
	compute_new_xmax_infomask(xmax, old_infomask, tuple->t_data->t_infomask2,
							  GetCurrentTransactionId(), mode, false,
							  &xid, &new_infomask, &new_infomask2);

	START_CRIT_SECTION();

	/*
	 * Store transaction information of xact locking the tuple.
	 *
	 * Note: Cmax is meaningless in this context, so don't set it; this avoids
	 * possibly generating a useless combo CID.  Moreover, if we're locking a
	 * previously updated tuple, it's important to preserve the Cmax.
	 *
	 * Also reset the HOT UPDATE bit, but only if there's no update; otherwise
	 * we would break the HOT chain.
	 */
	tuple->t_data->t_infomask &= ~HEAP_XMAX_BITS;
	tuple->t_data->t_infomask2 &= ~HEAP_KEYS_UPDATED;
	tuple->t_data->t_infomask |= new_infomask;
	tuple->t_data->t_infomask2 |= new_infomask2;
	if (HEAP_XMAX_IS_LOCKED_ONLY(new_infomask))
		HeapTupleHeaderClearHotUpdated(tuple->t_data);
	HeapTupleHeaderSetXmax(tuple->t_data, xid);

	/*
	 * Make sure there is no forward chain link in t_ctid.  Note that in the
	 * cases where the tuple has been updated, we must not overwrite t_ctid,
	 * because it was set by the updater.  Moreover, if the tuple has been
	 * updated, we need to follow the update chain to lock the new versions of
	 * the tuple as well.
	 */
	if (HEAP_XMAX_IS_LOCKED_ONLY(new_infomask))
		tuple->t_data->t_ctid = *tid;

	MarkBufferDirty(*buffer);

	/*
	 * XLOG stuff.  You might think that we don't need an XLOG record because
	 * there is no state change worth restoring after a crash.  You would be
	 * wrong however: we have just written either a TransactionId or a
	 * MultiXactId that may never have been seen on disk before, and we need
	 * to make sure that there are XLOG entries covering those ID numbers.
	 * Else the same IDs might be re-used after a crash, which would be
	 * disastrous if this page made it to disk before the crash.  Essentially
	 * we have to enforce the WAL log-before-data rule even in this case.
	 * (Also, in a PITR log-shipping or 2PC environment, we have to have XLOG
	 * entries for everything anyway.)
	 */
	if (RelationNeedsWAL(relation))
	{
		xl_heap_lock xlrec;
		XLogRecPtr	recptr;

		XLogBeginInsert();
		XLogRegisterBuffer(0, *buffer, REGBUF_STANDARD);

		xlrec.offnum = ItemPointerGetOffsetNumber(&tuple->t_self);
		xlrec.locking_xid = xid;
		xlrec.infobits_set = compute_infobits(new_infomask,
											  tuple->t_data->t_infomask2);
		XLogRegisterData((char *) &xlrec, SizeOfHeapLock);

		/* we don't decode row locks atm, so no need to log the origin */

		recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_LOCK);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	LockBuffer(*buffer, BUFFER_LOCK_UNLOCK);

	/*
	 * Don't update the visibility map here. Locking a tuple doesn't change
	 * visibility info.
	 */

	/*
	 * Now that we have successfully marked the tuple as locked, we can
	 * release the lmgr tuple lock, if we had it.
	 */
	if (have_tuple_lock)
		UnlockTupleTuplock(relation, tid, mode);

	return HeapTupleMayBeUpdated;
}

/*
 * Acquire heavyweight lock on the given tuple, in preparation for acquiring
 * its normal, Xmax-based tuple lock.
 *
 * have_tuple_lock is an input and output parameter: on input, it indicates
 * whether the lock has previously been acquired (and this function does
 * nothing in that case).  If this function returns success, have_tuple_lock
 * has been flipped to true.
 *
 * Returns false if it was unable to obtain the lock; this can only happen if
 * wait_policy is Skip.
 */
static bool
heap_acquire_tuplock(Relation relation, ItemPointer tid, LockTupleMode mode,
					 LockWaitPolicy wait_policy, bool *have_tuple_lock)
{
	if (*have_tuple_lock)
		return true;

	switch (wait_policy)
	{
		case LockWaitBlock:
			LockTupleTuplock(relation, tid, mode);
			break;

		case LockWaitSkip:
			if (!ConditionalLockTupleTuplock(relation, tid, mode))
				return false;
			break;

		case LockWaitError:
			if (!ConditionalLockTupleTuplock(relation, tid, mode))
				ereport(ERROR,
						(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
					errmsg("could not obtain lock on row in relation \"%s\"",
						   RelationGetRelationName(relation))));
			break;
	}
	*have_tuple_lock = true;

	return true;
}

/*
 * Given an original set of Xmax and infomask, and a transaction (identified by
 * add_to_xmax) acquiring a new lock of some mode, compute the new Xmax and
 * corresponding infomasks to use on the tuple.
 *
 * Note that this might have side effects such as creating a new MultiXactId.
 *
 * Most callers will have called HeapTupleSatisfiesUpdate before this function;
 * that will have set the HEAP_XMAX_INVALID bit if the xmax was a MultiXactId
 * but it was not running anymore. There is a race condition, which is that the
 * MultiXactId may have finished since then, but that uncommon case is handled
 * either here, or within MultiXactIdExpand.
 *
 * There is a similar race condition possible when the old xmax was a regular
 * TransactionId.  We test TransactionIdIsInProgress again just to narrow the
 * window, but it's still possible to end up creating an unnecessary
 * MultiXactId.  Fortunately this is harmless.
 */
static void
compute_new_xmax_infomask(TransactionId xmax, uint16 old_infomask,
						  uint16 old_infomask2, TransactionId add_to_xmax,
						  LockTupleMode mode, bool is_update,
						  TransactionId *result_xmax, uint16 *result_infomask,
						  uint16 *result_infomask2)
{
	TransactionId new_xmax;
	uint16		new_infomask,
				new_infomask2;

	Assert(TransactionIdIsCurrentTransactionId(add_to_xmax));

l5:
	new_infomask = 0;
	new_infomask2 = 0;
	if (old_infomask & HEAP_XMAX_INVALID)
	{
		/*
		 * No previous locker; we just insert our own TransactionId.
		 *
		 * Note that it's critical that this case be the first one checked,
		 * because there are several blocks below that come back to this one
		 * to implement certain optimizations; old_infomask might contain
		 * other dirty bits in those cases, but we don't really care.
		 */
		if (is_update)
		{
			new_xmax = add_to_xmax;
			if (mode == LockTupleExclusive)
				new_infomask2 |= HEAP_KEYS_UPDATED;
		}
		else
		{
			new_infomask |= HEAP_XMAX_LOCK_ONLY;
			switch (mode)
			{
				case LockTupleKeyShare:
					new_xmax = add_to_xmax;
					new_infomask |= HEAP_XMAX_KEYSHR_LOCK;
					break;
				case LockTupleShare:
					new_xmax = add_to_xmax;
					new_infomask |= HEAP_XMAX_SHR_LOCK;
					break;
				case LockTupleNoKeyExclusive:
					new_xmax = add_to_xmax;
					new_infomask |= HEAP_XMAX_EXCL_LOCK;
					break;
				case LockTupleExclusive:
					new_xmax = add_to_xmax;
					new_infomask |= HEAP_XMAX_EXCL_LOCK;
					new_infomask2 |= HEAP_KEYS_UPDATED;
					break;
				default:
					new_xmax = InvalidTransactionId;	/* silence compiler */
					elog(ERROR, "invalid lock mode");
			}
		}
	}
	else if (old_infomask & HEAP_XMAX_IS_MULTI)
	{
		MultiXactStatus new_status;

		/*
		 * Currently we don't allow XMAX_COMMITTED to be set for multis, so
		 * cross-check.
		 */
		Assert(!(old_infomask & HEAP_XMAX_COMMITTED));

		/*
		 * A multixact together with LOCK_ONLY set but neither lock bit set
		 * (i.e. a pg_upgraded share locked tuple) cannot possibly be running
		 * anymore.  This check is critical for databases upgraded by
		 * pg_upgrade; both MultiXactIdIsRunning and MultiXactIdExpand assume
		 * that such multis are never passed.
		 */
		if (!(old_infomask & HEAP_LOCK_MASK) &&
			HEAP_XMAX_IS_LOCKED_ONLY(old_infomask))
		{
			old_infomask &= ~HEAP_XMAX_IS_MULTI;
			old_infomask |= HEAP_XMAX_INVALID;
			goto l5;
		}

		/*
		 * If the XMAX is already a MultiXactId, then we need to expand it to
		 * include add_to_xmax; but if all the members were lockers and are
		 * all gone, we can do away with the IS_MULTI bit and just set
		 * add_to_xmax as the only locker/updater.  If all lockers are gone
		 * and we have an updater that aborted, we can also do without a
		 * multi.
		 *
		 * The cost of doing GetMultiXactIdMembers would be paid by
		 * MultiXactIdExpand if we weren't to do this, so this check is not
		 * incurring extra work anyhow.
		 */
		if (!MultiXactIdIsRunning(xmax, HEAP_XMAX_IS_LOCKED_ONLY(old_infomask)))
		{
			if (HEAP_XMAX_IS_LOCKED_ONLY(old_infomask) ||
				!TransactionIdDidCommit(MultiXactIdGetUpdateXid(xmax,
															  old_infomask)))
			{
				/*
				 * Reset these bits and restart; otherwise fall through to
				 * create a new multi below.
				 */
				old_infomask &= ~HEAP_XMAX_IS_MULTI;
				old_infomask |= HEAP_XMAX_INVALID;
				goto l5;
			}
		}

		new_status = get_mxact_status_for_lock(mode, is_update);

		new_xmax = MultiXactIdExpand((MultiXactId) xmax, add_to_xmax,
									 new_status);
		GetMultiXactIdHintBits(new_xmax, &new_infomask, &new_infomask2);
	}
	else if (old_infomask & HEAP_XMAX_COMMITTED)
	{
		/*
		 * It's a committed update, so we need to preserve him as updater of
		 * the tuple.
		 */
		MultiXactStatus status;
		MultiXactStatus new_status;

		if (old_infomask2 & HEAP_KEYS_UPDATED)
			status = MultiXactStatusUpdate;
		else
			status = MultiXactStatusNoKeyUpdate;

		new_status = get_mxact_status_for_lock(mode, is_update);

		/*
		 * since it's not running, it's obviously impossible for the old
		 * updater to be identical to the current one, so we need not check
		 * for that case as we do in the block above.
		 */
		new_xmax = MultiXactIdCreate(xmax, status, add_to_xmax, new_status);
		GetMultiXactIdHintBits(new_xmax, &new_infomask, &new_infomask2);
	}
	else if (TransactionIdIsInProgress(xmax))
	{
		/*
		 * If the XMAX is a valid, in-progress TransactionId, then we need to
		 * create a new MultiXactId that includes both the old locker or
		 * updater and our own TransactionId.
		 */
		MultiXactStatus new_status;
		MultiXactStatus old_status;
		LockTupleMode old_mode;

		if (HEAP_XMAX_IS_LOCKED_ONLY(old_infomask))
		{
			if (HEAP_XMAX_IS_KEYSHR_LOCKED(old_infomask))
				old_status = MultiXactStatusForKeyShare;
			else if (HEAP_XMAX_IS_SHR_LOCKED(old_infomask))
				old_status = MultiXactStatusForShare;
			else if (HEAP_XMAX_IS_EXCL_LOCKED(old_infomask))
			{
				if (old_infomask2 & HEAP_KEYS_UPDATED)
					old_status = MultiXactStatusForUpdate;
				else
					old_status = MultiXactStatusForNoKeyUpdate;
			}
			else
			{
				/*
				 * LOCK_ONLY can be present alone only when a page has been
				 * upgraded by pg_upgrade.  But in that case,
				 * TransactionIdIsInProgress() should have returned false.  We
				 * assume it's no longer locked in this case.
				 */
				elog(WARNING, "LOCK_ONLY found for Xid in progress %u", xmax);
				old_infomask |= HEAP_XMAX_INVALID;
				old_infomask &= ~HEAP_XMAX_LOCK_ONLY;
				goto l5;
			}
		}
		else
		{
			/* it's an update, but which kind? */
			if (old_infomask2 & HEAP_KEYS_UPDATED)
				old_status = MultiXactStatusUpdate;
			else
				old_status = MultiXactStatusNoKeyUpdate;
		}

		old_mode = TUPLOCK_from_mxstatus(old_status);

		/*
		 * If the lock to be acquired is for the same TransactionId as the
		 * existing lock, there's an optimization possible: consider only the
		 * strongest of both locks as the only one present, and restart.
		 */
		if (xmax == add_to_xmax)
		{
			/*
			 * Note that it's not possible for the original tuple to be
			 * updated: we wouldn't be here because the tuple would have been
			 * invisible and we wouldn't try to update it.  As a subtlety,
			 * this code can also run when traversing an update chain to lock
			 * future versions of a tuple.  But we wouldn't be here either,
			 * because the add_to_xmax would be different from the original
			 * updater.
			 */
			Assert(HEAP_XMAX_IS_LOCKED_ONLY(old_infomask));

			/* acquire the strongest of both */
			if (mode < old_mode)
				mode = old_mode;
			/* mustn't touch is_update */

			old_infomask |= HEAP_XMAX_INVALID;
			goto l5;
		}

		/* otherwise, just fall back to creating a new multixact */
		new_status = get_mxact_status_for_lock(mode, is_update);
		new_xmax = MultiXactIdCreate(xmax, old_status,
									 add_to_xmax, new_status);
		GetMultiXactIdHintBits(new_xmax, &new_infomask, &new_infomask2);
	}
	else if (!HEAP_XMAX_IS_LOCKED_ONLY(old_infomask) &&
			 TransactionIdDidCommit(xmax))
	{
		/*
		 * It's a committed update, so we gotta preserve him as updater of the
		 * tuple.
		 */
		MultiXactStatus status;
		MultiXactStatus new_status;

		if (old_infomask2 & HEAP_KEYS_UPDATED)
			status = MultiXactStatusUpdate;
		else
			status = MultiXactStatusNoKeyUpdate;

		new_status = get_mxact_status_for_lock(mode, is_update);

		/*
		 * since it's not running, it's obviously impossible for the old
		 * updater to be identical to the current one, so we need not check
		 * for that case as we do in the block above.
		 */
		new_xmax = MultiXactIdCreate(xmax, status, add_to_xmax, new_status);
		GetMultiXactIdHintBits(new_xmax, &new_infomask, &new_infomask2);
	}
	else
	{
		/*
		 * Can get here iff the locking/updating transaction was running when
		 * the infomask was extracted from the tuple, but finished before
		 * TransactionIdIsInProgress got to run.  Deal with it as if there was
		 * no locker at all in the first place.
		 */
		old_infomask |= HEAP_XMAX_INVALID;
		goto l5;
	}

	*result_infomask = new_infomask;
	*result_infomask2 = new_infomask2;
	*result_xmax = new_xmax;
}

/*
 * Subroutine for heap_lock_updated_tuple_rec.
 *
 * Given a hypothetical multixact status held by the transaction identified
 * with the given xid, does the current transaction need to wait, fail, or can
 * it continue if it wanted to acquire a lock of the given mode?  "needwait"
 * is set to true if waiting is necessary; if it can continue, then
 * HeapTupleMayBeUpdated is returned.  In case of a conflict, a different
 * HeapTupleSatisfiesUpdate return code is returned.
 *
 * The held status is said to be hypothetical because it might correspond to a
 * lock held by a single Xid, i.e. not a real MultiXactId; we express it this
 * way for simplicity of API.
 */
static HTSU_Result
test_lockmode_for_conflict(MultiXactStatus status, TransactionId xid,
						   LockTupleMode mode, bool *needwait)
{
	MultiXactStatus wantedstatus;

	*needwait = false;
	wantedstatus = get_mxact_status_for_lock(mode, false);

	/*
	 * Note: we *must* check TransactionIdIsInProgress before
	 * TransactionIdDidAbort/Commit; see comment at top of tqual.c for an
	 * explanation.
	 */
	if (TransactionIdIsCurrentTransactionId(xid))
	{
		/*
		 * Updated by our own transaction? Just return failure.  This
		 * shouldn't normally happen.
		 */
		return HeapTupleSelfUpdated;
	}
	else if (TransactionIdIsInProgress(xid))
	{
		/*
		 * If the locking transaction is running, what we do depends on
		 * whether the lock modes conflict: if they do, then we must wait for
		 * it to finish; otherwise we can fall through to lock this tuple
		 * version without waiting.
		 */
		if (DoLockModesConflict(LOCKMODE_from_mxstatus(status),
								LOCKMODE_from_mxstatus(wantedstatus)))
		{
			*needwait = true;
		}

		/*
		 * If we set needwait above, then this value doesn't matter;
		 * otherwise, this value signals to caller that it's okay to proceed.
		 */
		return HeapTupleMayBeUpdated;
	}
	else if (TransactionIdDidAbort(xid))
		return HeapTupleMayBeUpdated;
	else if (TransactionIdDidCommit(xid))
	{
		/*
		 * The other transaction committed.  If it was only a locker, then the
		 * lock is completely gone now and we can return success; but if it
		 * was an update, then what we do depends on whether the two lock
		 * modes conflict.  If they conflict, then we must report error to
		 * caller. But if they don't, we can fall through to allow the current
		 * transaction to lock the tuple.
		 *
		 * Note: the reason we worry about ISUPDATE here is because as soon as
		 * a transaction ends, all its locks are gone and meaningless, and
		 * thus we can ignore them; whereas its updates persist.  In the
		 * TransactionIdIsInProgress case, above, we don't need to check
		 * because we know the lock is still "alive" and thus a conflict needs
		 * always be checked.
		 */
		if (!ISUPDATE_from_mxstatus(status))
			return HeapTupleMayBeUpdated;

		if (DoLockModesConflict(LOCKMODE_from_mxstatus(status),
								LOCKMODE_from_mxstatus(wantedstatus)))
			/* bummer */
			return HeapTupleUpdated;

		return HeapTupleMayBeUpdated;
	}

	/* Not in progress, not aborted, not committed -- must have crashed */
	return HeapTupleMayBeUpdated;
}


/*
 * Recursive part of heap_lock_updated_tuple
 *
 * Fetch the tuple pointed to by tid in rel, and mark it as locked by the given
 * xid with the given mode; if this tuple is updated, recurse to lock the new
 * version as well.
 */
static HTSU_Result
heap_lock_updated_tuple_rec(Relation rel, ItemPointer tid, TransactionId xid,
							LockTupleMode mode)
{
	ItemPointerData tupid;
	HeapTupleData mytup;
	Buffer		buf;
	uint16		new_infomask,
				new_infomask2,
				old_infomask,
				old_infomask2;
	TransactionId xmax,
				new_xmax;
	TransactionId priorXmax = InvalidTransactionId;

	ItemPointerCopy(tid, &tupid);

	for (;;)
	{
		new_infomask = 0;
		new_xmax = InvalidTransactionId;
		ItemPointerCopy(&tupid, &(mytup.t_self));

		if (!heap_fetch(rel, SnapshotAny, &mytup, &buf, false, NULL))
		{
			/*
			 * if we fail to find the updated version of the tuple, it's
			 * because it was vacuumed/pruned away after its creator
			 * transaction aborted.  So behave as if we got to the end of the
			 * chain, and there's no further tuple to lock: return success to
			 * caller.
			 */
			return HeapTupleMayBeUpdated;
		}

l4:
		CHECK_FOR_INTERRUPTS();
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

		/*
		 * Check the tuple XMIN against prior XMAX, if any.  If we reached the
		 * end of the chain, we're done, so return success.
		 */
		if (TransactionIdIsValid(priorXmax) &&
			!TransactionIdEquals(HeapTupleHeaderGetXmin(mytup.t_data),
								 priorXmax))
		{
			UnlockReleaseBuffer(buf);
			return HeapTupleMayBeUpdated;
		}

		old_infomask = mytup.t_data->t_infomask;
		old_infomask2 = mytup.t_data->t_infomask2;
		xmax = HeapTupleHeaderGetRawXmax(mytup.t_data);

		/*
		 * If this tuple version has been updated or locked by some concurrent
		 * transaction(s), what we do depends on whether our lock mode
		 * conflicts with what those other transactions hold, and also on the
		 * status of them.
		 */
		if (!(old_infomask & HEAP_XMAX_INVALID))
		{
			TransactionId rawxmax;
			bool		needwait;

			rawxmax = HeapTupleHeaderGetRawXmax(mytup.t_data);
			if (old_infomask & HEAP_XMAX_IS_MULTI)
			{
				int			nmembers;
				int			i;
				MultiXactMember *members;

				nmembers = GetMultiXactIdMembers(rawxmax, &members, false,
									 HEAP_XMAX_IS_LOCKED_ONLY(old_infomask));
				for (i = 0; i < nmembers; i++)
				{
					HTSU_Result res;

					res = test_lockmode_for_conflict(members[i].status,
													 members[i].xid,
													 mode, &needwait);

					if (needwait)
					{
						LockBuffer(buf, BUFFER_LOCK_UNLOCK);
						XactLockTableWait(members[i].xid, rel,
										  &mytup.t_self,
										  XLTW_LockUpdated);
						pfree(members);
						goto l4;
					}
					if (res != HeapTupleMayBeUpdated)
					{
						UnlockReleaseBuffer(buf);
						pfree(members);
						return res;
					}
				}
				if (members)
					pfree(members);
			}
			else
			{
				HTSU_Result res;
				MultiXactStatus status;

				/*
				 * For a non-multi Xmax, we first need to compute the
				 * corresponding MultiXactStatus by using the infomask bits.
				 */
				if (HEAP_XMAX_IS_LOCKED_ONLY(old_infomask))
				{
					if (HEAP_XMAX_IS_KEYSHR_LOCKED(old_infomask))
						status = MultiXactStatusForKeyShare;
					else if (HEAP_XMAX_IS_SHR_LOCKED(old_infomask))
						status = MultiXactStatusForShare;
					else if (HEAP_XMAX_IS_EXCL_LOCKED(old_infomask))
					{
						if (old_infomask2 & HEAP_KEYS_UPDATED)
							status = MultiXactStatusForUpdate;
						else
							status = MultiXactStatusForNoKeyUpdate;
					}
					else
					{
						/*
						 * LOCK_ONLY present alone (a pg_upgraded tuple marked
						 * as share-locked in the old cluster) shouldn't be
						 * seen in the middle of an update chain.
						 */
						elog(ERROR, "invalid lock status in tuple");
					}
				}
				else
				{
					/* it's an update, but which kind? */
					if (old_infomask2 & HEAP_KEYS_UPDATED)
						status = MultiXactStatusUpdate;
					else
						status = MultiXactStatusNoKeyUpdate;
				}

				res = test_lockmode_for_conflict(status, rawxmax, mode,
												 &needwait);
				if (needwait)
				{
					LockBuffer(buf, BUFFER_LOCK_UNLOCK);
					XactLockTableWait(rawxmax, rel, &mytup.t_self,
									  XLTW_LockUpdated);
					goto l4;
				}
				if (res != HeapTupleMayBeUpdated)
				{
					UnlockReleaseBuffer(buf);
					return res;
				}
			}
		}

		/* compute the new Xmax and infomask values for the tuple ... */
		compute_new_xmax_infomask(xmax, old_infomask, mytup.t_data->t_infomask2,
								  xid, mode, false,
								  &new_xmax, &new_infomask, &new_infomask2);

		START_CRIT_SECTION();

		/* ... and set them */
		HeapTupleHeaderSetXmax(mytup.t_data, new_xmax);
		mytup.t_data->t_infomask &= ~HEAP_XMAX_BITS;
		mytup.t_data->t_infomask2 &= ~HEAP_KEYS_UPDATED;
		mytup.t_data->t_infomask |= new_infomask;
		mytup.t_data->t_infomask2 |= new_infomask2;

		MarkBufferDirty(buf);

		/* XLOG stuff */
		if (RelationNeedsWAL(rel))
		{
			xl_heap_lock_updated xlrec;
			XLogRecPtr	recptr;
			Page		page = BufferGetPage(buf);

			XLogBeginInsert();
			XLogRegisterBuffer(0, buf, REGBUF_STANDARD);

			xlrec.offnum = ItemPointerGetOffsetNumber(&mytup.t_self);
			xlrec.xmax = new_xmax;
			xlrec.infobits_set = compute_infobits(new_infomask, new_infomask2);

			XLogRegisterData((char *) &xlrec, SizeOfHeapLockUpdated);

			recptr = XLogInsert(RM_HEAP2_ID, XLOG_HEAP2_LOCK_UPDATED);

			PageSetLSN(page, recptr);
		}

		END_CRIT_SECTION();

		/* if we find the end of update chain, we're done. */
		if (mytup.t_data->t_infomask & HEAP_XMAX_INVALID ||
			ItemPointerEquals(&mytup.t_self, &mytup.t_data->t_ctid) ||
			HeapTupleHeaderIsOnlyLocked(mytup.t_data))
		{
			UnlockReleaseBuffer(buf);
			return HeapTupleMayBeUpdated;
		}

		/* tail recursion */
		priorXmax = HeapTupleHeaderGetUpdateXid(mytup.t_data);
		ItemPointerCopy(&(mytup.t_data->t_ctid), &tupid);
		UnlockReleaseBuffer(buf);
	}
}

/*
 * heap_lock_updated_tuple
 *		Follow update chain when locking an updated tuple, acquiring locks (row
 *		marks) on the updated versions.
 *
 * The initial tuple is assumed to be already locked.
 *
 * This function doesn't check visibility, it just unconditionally marks the
 * tuple(s) as locked.  If any tuple in the updated chain is being deleted
 * concurrently (or updated with the key being modified), sleep until the
 * transaction doing it is finished.
 *
 * Note that we don't acquire heavyweight tuple locks on the tuples we walk
 * when we have to wait for other transactions to release them, as opposed to
 * what heap_lock_tuple does.  The reason is that having more than one
 * transaction walking the chain is probably uncommon enough that risk of
 * starvation is not likely: one of the preconditions for being here is that
 * the snapshot in use predates the update that created this tuple (because we
 * started at an earlier version of the tuple), but at the same time such a
 * transaction cannot be using repeatable read or serializable isolation
 * levels, because that would lead to a serializability failure.
 */
static HTSU_Result
heap_lock_updated_tuple(Relation rel, HeapTuple tuple, ItemPointer ctid,
						TransactionId xid, LockTupleMode mode)
{
	if (!ItemPointerEquals(&tuple->t_self, ctid))
	{
		/*
		 * If this is the first possibly-multixact-able operation in the
		 * current transaction, set my per-backend OldestMemberMXactId
		 * setting. We can be certain that the transaction will never become a
		 * member of any older MultiXactIds than that.  (We have to do this
		 * even if we end up just using our own TransactionId below, since
		 * some other backend could incorporate our XID into a MultiXact
		 * immediately afterwards.)
		 */
		MultiXactIdSetOldestMember();

		return heap_lock_updated_tuple_rec(rel, ctid, xid, mode);
	}

	/* nothing to lock */
	return HeapTupleMayBeUpdated;
}

/*
 *	heap_finish_speculative - mark speculative insertion as successful
 *
 * To successfully finish a speculative insertion we have to clear speculative
 * token from tuple.  To do so the t_ctid field, which will contain a
 * speculative token value, is modified in place to point to the tuple itself,
 * which is characteristic of a newly inserted ordinary tuple.
 *
 * NB: It is not ok to commit without either finishing or aborting a
 * speculative insertion.  We could treat speculative tuples of committed
 * transactions implicitly as completed, but then we would have to be prepared
 * to deal with speculative tokens on committed tuples.  That wouldn't be
 * difficult - no-one looks at the ctid field of a tuple with invalid xmax -
 * but clearing the token at completion isn't very expensive either.
 * An explicit confirmation WAL record also makes logical decoding simpler.
 */
void
heap_finish_speculative(Relation relation, HeapTuple tuple)
{
	Buffer		buffer;
	Page		page;
	OffsetNumber offnum;
	ItemId		lp = NULL;
	HeapTupleHeader htup;

	buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(&(tuple->t_self)));
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	page = (Page) BufferGetPage(buffer);

	offnum = ItemPointerGetOffsetNumber(&(tuple->t_self));
	if (PageGetMaxOffsetNumber(page) >= offnum)
		lp = PageGetItemId(page, offnum);

	if (PageGetMaxOffsetNumber(page) < offnum || !ItemIdIsNormal(lp))
		elog(ERROR, "invalid lp");

	htup = (HeapTupleHeader) PageGetItem(page, lp);

	/* SpecTokenOffsetNumber should be distinguishable from any real offset */
	StaticAssertStmt(MaxOffsetNumber < SpecTokenOffsetNumber,
					 "invalid speculative token constant");

	/* NO EREPORT(ERROR) from here till changes are logged */
	START_CRIT_SECTION();

	Assert(HeapTupleHeaderIsSpeculative(tuple->t_data));

	MarkBufferDirty(buffer);

	/*
	 * Replace the speculative insertion token with a real t_ctid, pointing to
	 * itself like it does on regular tuples.
	 */
	htup->t_ctid = tuple->t_self;

	/* XLOG stuff */
	if (RelationNeedsWAL(relation))
	{
		xl_heap_confirm xlrec;
		XLogRecPtr	recptr;

		xlrec.offnum = ItemPointerGetOffsetNumber(&tuple->t_self);

		XLogBeginInsert();

		/* We want the same filtering on this as on a plain insert */
		XLogIncludeOrigin();

		XLogRegisterData((char *) &xlrec, SizeOfHeapConfirm);
		XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

		recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_CONFIRM);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	UnlockReleaseBuffer(buffer);
}

/*
 *	heap_abort_speculative - kill a speculatively inserted tuple
 *
 * Marks a tuple that was speculatively inserted in the same command as dead,
 * by setting its xmin as invalid.  That makes it immediately appear as dead
 * to all transactions, including our own.  In particular, it makes
 * HeapTupleSatisfiesDirty() regard the tuple as dead, so that another backend
 * inserting a duplicate key value won't unnecessarily wait for our whole
 * transaction to finish (it'll just wait for our speculative insertion to
 * finish).
 *
 * Killing the tuple prevents "unprincipled deadlocks", which are deadlocks
 * that arise due to a mutual dependency that is not user visible.  By
 * definition, unprincipled deadlocks cannot be prevented by the user
 * reordering lock acquisition in client code, because the implementation level
 * lock acquisitions are not under the user's direct control.  If speculative
 * inserters did not take this precaution, then under high concurrency they
 * could deadlock with each other, which would not be acceptable.
 *
 * This is somewhat redundant with heap_delete, but we prefer to have a
 * dedicated routine with stripped down requirements.
 *
 * This routine does not affect logical decoding as it only looks at
 * confirmation records.
 */
void
heap_abort_speculative(Relation relation, HeapTuple tuple)
{
	TransactionId xid = GetCurrentTransactionId();
	ItemPointer tid = &(tuple->t_self);
	ItemId		lp;
	HeapTupleData tp;
	Page		page;
	BlockNumber block;
	Buffer		buffer;

	Assert(ItemPointerIsValid(tid));

	block = ItemPointerGetBlockNumber(tid);
	buffer = ReadBuffer(relation, block);
	page = BufferGetPage(buffer);

	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	/*
	 * Page can't be all visible, we just inserted into it, and are still
	 * running.
	 */
	Assert(!PageIsAllVisible(page));

	lp = PageGetItemId(page, ItemPointerGetOffsetNumber(tid));
	Assert(ItemIdIsNormal(lp));

	tp.t_tableOid = RelationGetRelid(relation);
	tp.t_data = (HeapTupleHeader) PageGetItem(page, lp);
	tp.t_len = ItemIdGetLength(lp);
	tp.t_self = *tid;

	/*
	 * Sanity check that the tuple really is a speculatively inserted tuple,
	 * inserted by us.
	 */
	if (tp.t_data->t_choice.t_heap.t_xmin != xid)
		elog(ERROR, "attempted to kill a tuple inserted by another transaction");
	if (!HeapTupleHeaderIsSpeculative(tp.t_data))
		elog(ERROR, "attempted to kill a non-speculative tuple");
	Assert(!HeapTupleHeaderIsHeapOnly(tp.t_data));

	/*
	 * No need to check for serializable conflicts here.  There is never a
	 * need for a combocid, either.  No need to extract replica identity, or
	 * do anything special with infomask bits.
	 */

	START_CRIT_SECTION();

	/*
	 * The tuple will become DEAD immediately.  Flag that this page
	 * immediately is a candidate for pruning by setting xmin to
	 * RecentGlobalXmin.  That's not pretty, but it doesn't seem worth
	 * inventing a nicer API for this.
	 */
	Assert(TransactionIdIsValid(RecentGlobalXmin));
	PageSetPrunable(page, RecentGlobalXmin);

	/* store transaction information of xact deleting the tuple */
	tp.t_data->t_infomask &= ~(HEAP_XMAX_BITS | HEAP_MOVED);
	tp.t_data->t_infomask2 &= ~HEAP_KEYS_UPDATED;

	/*
	 * Set the tuple header xmin to InvalidTransactionId.  This makes the
	 * tuple immediately invisible everyone.  (In particular, to any
	 * transactions waiting on the speculative token, woken up later.)
	 */
	HeapTupleHeaderSetXmin(tp.t_data, InvalidTransactionId);

	/* Clear the speculative insertion token too */
	tp.t_data->t_ctid = tp.t_self;

	MarkBufferDirty(buffer);

	/*
	 * XLOG stuff
	 *
	 * The WAL records generated here match heap_delete().  The same recovery
	 * routines are used.
	 */
	if (RelationNeedsWAL(relation))
	{
		xl_heap_delete xlrec;
		XLogRecPtr	recptr;

		xlrec.flags = XLH_DELETE_IS_SUPER;
		xlrec.infobits_set = compute_infobits(tp.t_data->t_infomask,
											  tp.t_data->t_infomask2);
		xlrec.offnum = ItemPointerGetOffsetNumber(&tp.t_self);
		xlrec.xmax = xid;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfHeapDelete);
		XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

		/* No replica identity & replication origin logged */

		recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_DELETE);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

	if (HeapTupleHasExternal(&tp))
		toast_delete(relation, &tp);

	/*
	 * Never need to mark tuple for invalidation, since catalogs don't support
	 * speculative insertion
	 */

	/* Now we can release the buffer */
	ReleaseBuffer(buffer);

	/* count deletion, as we counted the insertion too */
	pgstat_count_heap_delete(relation);
}

/*
 * heap_inplace_update - update a tuple "in place" (ie, overwrite it)
 *
 * Overwriting violates both MVCC and transactional safety, so the uses
 * of this function in Postgres are extremely limited.  Nonetheless we
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

	/*
	 * For now, parallel operations are required to be strictly read-only.
	 * Unlike a regular update, this should never create a combo CID, so it
	 * might be possible to relax this restriction, but not without more
	 * thought and testing.  It's not clear that it would be useful, anyway.
	 */
	if (IsInParallelMode())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("cannot update tuples during a parallel operation")));

	buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(&(tuple->t_self)));
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	page = (Page) BufferGetPage(buffer);

	offnum = ItemPointerGetOffsetNumber(&(tuple->t_self));
	if (PageGetMaxOffsetNumber(page) >= offnum)
		lp = PageGetItemId(page, offnum);

	if (PageGetMaxOffsetNumber(page) < offnum || !ItemIdIsNormal(lp))
		elog(ERROR, "invalid lp");

	htup = (HeapTupleHeader) PageGetItem(page, lp);

	oldlen = ItemIdGetLength(lp) - htup->t_hoff;
	newlen = tuple->t_len - tuple->t_data->t_hoff;
	if (oldlen != newlen || htup->t_hoff != tuple->t_data->t_hoff)
		elog(ERROR, "wrong tuple length");

	/* NO EREPORT(ERROR) from here till changes are logged */
	START_CRIT_SECTION();

	memcpy((char *) htup + htup->t_hoff,
		   (char *) tuple->t_data + tuple->t_data->t_hoff,
		   newlen);

	MarkBufferDirty(buffer);

	/* XLOG stuff */
	if (RelationNeedsWAL(relation))
	{
		xl_heap_inplace xlrec;
		XLogRecPtr	recptr;

		xlrec.offnum = ItemPointerGetOffsetNumber(&tuple->t_self);

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfHeapInplace);

		XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);
		XLogRegisterBufData(0, (char *) htup + htup->t_hoff, newlen);

		/* inplace updates aren't decoded atm, don't log the origin */

		recptr = XLogInsert(RM_HEAP_ID, XLOG_HEAP_INPLACE);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	UnlockReleaseBuffer(buffer);

	/*
	 * Send out shared cache inval if necessary.  Note that because we only
	 * pass the new version of the tuple, this mustn't be used for any
	 * operations that could change catcache lookup keys.  But we aren't
	 * bothering with index updates either, so that's true a fortiori.
	 */
	if (!IsBootstrapProcessingMode())
		CacheInvalidateHeapTuple(relation, tuple, NULL);
}

#define		FRM_NOOP				0x0001
#define		FRM_INVALIDATE_XMAX		0x0002
#define		FRM_RETURN_IS_XID		0x0004
#define		FRM_RETURN_IS_MULTI		0x0008
#define		FRM_MARK_COMMITTED		0x0010

/*
 * FreezeMultiXactId
 *		Determine what to do during freezing when a tuple is marked by a
 *		MultiXactId.
 *
 * NB -- this might have the side-effect of creating a new MultiXactId!
 *
 * "flags" is an output value; it's used to tell caller what to do on return.
 * Possible flags are:
 * FRM_NOOP
 *		don't do anything -- keep existing Xmax
 * FRM_INVALIDATE_XMAX
 *		mark Xmax as InvalidTransactionId and set XMAX_INVALID flag.
 * FRM_RETURN_IS_XID
 *		The Xid return value is a single update Xid to set as xmax.
 * FRM_MARK_COMMITTED
 *		Xmax can be marked as HEAP_XMAX_COMMITTED
 * FRM_RETURN_IS_MULTI
 *		The return value is a new MultiXactId to set as new Xmax.
 *		(caller must obtain proper infomask bits using GetMultiXactIdHintBits)
 */
static TransactionId
FreezeMultiXactId(MultiXactId multi, uint16 t_infomask,
				  TransactionId cutoff_xid, MultiXactId cutoff_multi,
				  uint16 *flags)
{
	TransactionId xid = InvalidTransactionId;
	int			i;
	MultiXactMember *members;
	int			nmembers;
	bool		need_replace;
	int			nnewmembers;
	MultiXactMember *newmembers;
	bool		has_lockers;
	TransactionId update_xid;
	bool		update_committed;
	bool		allow_old;

	*flags = 0;

	/* We should only be called in Multis */
	Assert(t_infomask & HEAP_XMAX_IS_MULTI);

	if (!MultiXactIdIsValid(multi))
	{
		/* Ensure infomask bits are appropriately set/reset */
		*flags |= FRM_INVALIDATE_XMAX;
		return InvalidTransactionId;
	}
	else if (MultiXactIdPrecedes(multi, cutoff_multi))
	{
		/*
		 * This old multi cannot possibly have members still running.  If it
		 * was a locker only, it can be removed without any further
		 * consideration; but if it contained an update, we might need to
		 * preserve it.
		 *
		 * Don't assert MultiXactIdIsRunning if the multi came from a
		 * pg_upgrade'd share-locked tuple, though, as doing that causes an
		 * error to be raised unnecessarily.
		 */
		Assert((!(t_infomask & HEAP_LOCK_MASK) &&
				HEAP_XMAX_IS_LOCKED_ONLY(t_infomask)) ||
			   !MultiXactIdIsRunning(multi,
									 HEAP_XMAX_IS_LOCKED_ONLY(t_infomask)));
		if (HEAP_XMAX_IS_LOCKED_ONLY(t_infomask))
		{
			*flags |= FRM_INVALIDATE_XMAX;
			xid = InvalidTransactionId; /* not strictly necessary */
		}
		else
		{
			/* replace multi by update xid */
			xid = MultiXactIdGetUpdateXid(multi, t_infomask);

			/* wasn't only a lock, xid needs to be valid */
			Assert(TransactionIdIsValid(xid));

			/*
			 * If the xid is older than the cutoff, it has to have aborted,
			 * otherwise the tuple would have gotten pruned away.
			 */
			if (TransactionIdPrecedes(xid, cutoff_xid))
			{
				Assert(!TransactionIdDidCommit(xid));
				*flags |= FRM_INVALIDATE_XMAX;
				xid = InvalidTransactionId;		/* not strictly necessary */
			}
			else
			{
				*flags |= FRM_RETURN_IS_XID;
			}
		}

		return xid;
	}

	/*
	 * This multixact might have or might not have members still running, but
	 * we know it's valid and is newer than the cutoff point for multis.
	 * However, some member(s) of it may be below the cutoff for Xids, so we
	 * need to walk the whole members array to figure out what to do, if
	 * anything.
	 */

	allow_old = !(t_infomask & HEAP_LOCK_MASK) &&
		HEAP_XMAX_IS_LOCKED_ONLY(t_infomask);
	nmembers =
		GetMultiXactIdMembers(multi, &members, allow_old,
							  HEAP_XMAX_IS_LOCKED_ONLY(t_infomask));
	if (nmembers <= 0)
	{
		/* Nothing worth keeping */
		*flags |= FRM_INVALIDATE_XMAX;
		return InvalidTransactionId;
	}

	/* is there anything older than the cutoff? */
	need_replace = false;
	for (i = 0; i < nmembers; i++)
	{
		if (TransactionIdPrecedes(members[i].xid, cutoff_xid))
		{
			need_replace = true;
			break;
		}
	}

	/*
	 * In the simplest case, there is no member older than the cutoff; we can
	 * keep the existing MultiXactId as is.
	 */
	if (!need_replace)
	{
		*flags |= FRM_NOOP;
		pfree(members);
		return InvalidTransactionId;
	}

	/*
	 * If the multi needs to be updated, figure out which members do we need
	 * to keep.
	 */
	nnewmembers = 0;
	newmembers = palloc(sizeof(MultiXactMember) * nmembers);
	has_lockers = false;
	update_xid = InvalidTransactionId;
	update_committed = false;

	for (i = 0; i < nmembers; i++)
	{
		/*
		 * Determine whether to keep this member or ignore it.
		 */
		if (ISUPDATE_from_mxstatus(members[i].status))
		{
			TransactionId xid = members[i].xid;

			/*
			 * It's an update; should we keep it?  If the transaction is known
			 * aborted or crashed then it's okay to ignore it, otherwise not.
			 * Note that an updater older than cutoff_xid cannot possibly be
			 * committed, because HeapTupleSatisfiesVacuum would have returned
			 * HEAPTUPLE_DEAD and we would not be trying to freeze the tuple.
			 *
			 * As with all tuple visibility routines, it's critical to test
			 * TransactionIdIsInProgress before TransactionIdDidCommit,
			 * because of race conditions explained in detail in tqual.c.
			 */
			if (TransactionIdIsCurrentTransactionId(xid) ||
				TransactionIdIsInProgress(xid))
			{
				Assert(!TransactionIdIsValid(update_xid));
				update_xid = xid;
			}
			else if (TransactionIdDidCommit(xid))
			{
				/*
				 * The transaction committed, so we can tell caller to set
				 * HEAP_XMAX_COMMITTED.  (We can only do this because we know
				 * the transaction is not running.)
				 */
				Assert(!TransactionIdIsValid(update_xid));
				update_committed = true;
				update_xid = xid;
			}

			/*
			 * Not in progress, not committed -- must be aborted or crashed;
			 * we can ignore it.
			 */

			/*
			 * Since the tuple wasn't marked HEAPTUPLE_DEAD by vacuum, the
			 * update Xid cannot possibly be older than the xid cutoff.
			 */
			Assert(!TransactionIdIsValid(update_xid) ||
				   !TransactionIdPrecedes(update_xid, cutoff_xid));

			/*
			 * If we determined that it's an Xid corresponding to an update
			 * that must be retained, additionally add it to the list of
			 * members of the new Multi, in case we end up using that.  (We
			 * might still decide to use only an update Xid and not a multi,
			 * but it's easier to maintain the list as we walk the old members
			 * list.)
			 */
			if (TransactionIdIsValid(update_xid))
				newmembers[nnewmembers++] = members[i];
		}
		else
		{
			/* We only keep lockers if they are still running */
			if (TransactionIdIsCurrentTransactionId(members[i].xid) ||
				TransactionIdIsInProgress(members[i].xid))
			{
				/* running locker cannot possibly be older than the cutoff */
				Assert(!TransactionIdPrecedes(members[i].xid, cutoff_xid));
				newmembers[nnewmembers++] = members[i];
				has_lockers = true;
			}
		}
	}

	pfree(members);

	if (nnewmembers == 0)
	{
		/* nothing worth keeping!? Tell caller to remove the whole thing */
		*flags |= FRM_INVALIDATE_XMAX;
		xid = InvalidTransactionId;
	}
	else if (TransactionIdIsValid(update_xid) && !has_lockers)
	{
		/*
		 * If there's a single member and it's an update, pass it back alone
		 * without creating a new Multi.  (XXX we could do this when there's a
		 * single remaining locker, too, but that would complicate the API too
		 * much; moreover, the case with the single updater is more
		 * interesting, because those are longer-lived.)
		 */
		Assert(nnewmembers == 1);
		*flags |= FRM_RETURN_IS_XID;
		if (update_committed)
			*flags |= FRM_MARK_COMMITTED;
		xid = update_xid;
	}
	else
	{
		/*
		 * Create a new multixact with the surviving members of the previous
		 * one, to set as new Xmax in the tuple.
		 */
		xid = MultiXactIdCreateFromMembers(nnewmembers, newmembers);
		*flags |= FRM_RETURN_IS_MULTI;
	}

	pfree(newmembers);

	return xid;
}

/*
 * heap_prepare_freeze_tuple
 *
 * Check to see whether any of the XID fields of a tuple (xmin, xmax, xvac)
 * are older than the specified cutoff XID and cutoff MultiXactId.  If so,
 * setup enough state (in the *frz output argument) to later execute and
 * WAL-log what we would need to do, and return TRUE.  Return FALSE if nothing
 * is to be changed.
 *
 * Caller is responsible for setting the offset field, if appropriate.
 *
 * It is assumed that the caller has checked the tuple with
 * HeapTupleSatisfiesVacuum() and determined that it is not HEAPTUPLE_DEAD
 * (else we should be removing the tuple, not freezing it).
 *
 * NB: cutoff_xid *must* be <= the current global xmin, to ensure that any
 * XID older than it could neither be running nor seen as running by any
 * open transaction.  This ensures that the replacement will not change
 * anyone's idea of the tuple state.
 * Similarly, cutoff_multi must be less than or equal to the smallest
 * MultiXactId used by any transaction currently open.
 *
 * If the tuple is in a shared buffer, caller must hold an exclusive lock on
 * that buffer.
 *
 * NB: It is not enough to set hint bits to indicate something is
 * committed/invalid -- they might not be set on a standby, or after crash
 * recovery.  We really need to remove old xids.
 */
bool
heap_prepare_freeze_tuple(HeapTupleHeader tuple, TransactionId cutoff_xid,
						  TransactionId cutoff_multi,
						  xl_heap_freeze_tuple *frz)

{
	bool		changed = false;
	bool		freeze_xmax = false;
	TransactionId xid;

	frz->frzflags = 0;
	frz->t_infomask2 = tuple->t_infomask2;
	frz->t_infomask = tuple->t_infomask;
	frz->xmax = HeapTupleHeaderGetRawXmax(tuple);

	/* Process xmin */
	xid = HeapTupleHeaderGetXmin(tuple);
	if (TransactionIdIsNormal(xid) &&
		TransactionIdPrecedes(xid, cutoff_xid))
	{
		frz->t_infomask |= HEAP_XMIN_FROZEN;
		changed = true;
	}

	/*
	 * Process xmax.  To thoroughly examine the current Xmax value we need to
	 * resolve a MultiXactId to its member Xids, in case some of them are
	 * below the given cutoff for Xids.  In that case, those values might need
	 * freezing, too.  Also, if a multi needs freezing, we cannot simply take
	 * it out --- if there's a live updater Xid, it needs to be kept.
	 *
	 * Make sure to keep heap_tuple_needs_freeze in sync with this.
	 */
	xid = HeapTupleHeaderGetRawXmax(tuple);

	if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
	{
		TransactionId newxmax;
		uint16		flags;

		newxmax = FreezeMultiXactId(xid, tuple->t_infomask,
									cutoff_xid, cutoff_multi, &flags);

		if (flags & FRM_INVALIDATE_XMAX)
			freeze_xmax = true;
		else if (flags & FRM_RETURN_IS_XID)
		{
			/*
			 * NB -- some of these transformations are only valid because we
			 * know the return Xid is a tuple updater (i.e. not merely a
			 * locker.) Also note that the only reason we don't explicitly
			 * worry about HEAP_KEYS_UPDATED is because it lives in
			 * t_infomask2 rather than t_infomask.
			 */
			frz->t_infomask &= ~HEAP_XMAX_BITS;
			frz->xmax = newxmax;
			if (flags & FRM_MARK_COMMITTED)
				frz->t_infomask &= HEAP_XMAX_COMMITTED;
			changed = true;
		}
		else if (flags & FRM_RETURN_IS_MULTI)
		{
			uint16		newbits;
			uint16		newbits2;

			/*
			 * We can't use GetMultiXactIdHintBits directly on the new multi
			 * here; that routine initializes the masks to all zeroes, which
			 * would lose other bits we need.  Doing it this way ensures all
			 * unrelated bits remain untouched.
			 */
			frz->t_infomask &= ~HEAP_XMAX_BITS;
			frz->t_infomask2 &= ~HEAP_KEYS_UPDATED;
			GetMultiXactIdHintBits(newxmax, &newbits, &newbits2);
			frz->t_infomask |= newbits;
			frz->t_infomask2 |= newbits2;

			frz->xmax = newxmax;

			changed = true;
		}
		else
		{
			Assert(flags & FRM_NOOP);
		}
	}
	else if (TransactionIdIsNormal(xid) &&
			 TransactionIdPrecedes(xid, cutoff_xid))
	{
		freeze_xmax = true;
	}

	if (freeze_xmax)
	{
		frz->xmax = InvalidTransactionId;

		/*
		 * The tuple might be marked either XMAX_INVALID or XMAX_COMMITTED +
		 * LOCKED.  Normalize to INVALID just to be sure no one gets confused.
		 * Also get rid of the HEAP_KEYS_UPDATED bit.
		 */
		frz->t_infomask &= ~HEAP_XMAX_BITS;
		frz->t_infomask |= HEAP_XMAX_INVALID;
		frz->t_infomask2 &= ~HEAP_HOT_UPDATED;
		frz->t_infomask2 &= ~HEAP_KEYS_UPDATED;
		changed = true;
	}

	/*
	 * Old-style VACUUM FULL is gone, but we have to keep this code as long as
	 * we support having MOVED_OFF/MOVED_IN tuples in the database.
	 */
	if (tuple->t_infomask & HEAP_MOVED)
	{
		xid = HeapTupleHeaderGetXvac(tuple);
		if (TransactionIdIsNormal(xid) &&
			TransactionIdPrecedes(xid, cutoff_xid))
		{
			/*
			 * If a MOVED_OFF tuple is not dead, the xvac transaction must
			 * have failed; whereas a non-dead MOVED_IN tuple must mean the
			 * xvac transaction succeeded.
			 */
			if (tuple->t_infomask & HEAP_MOVED_OFF)
				frz->frzflags |= XLH_INVALID_XVAC;
			else
				frz->frzflags |= XLH_FREEZE_XVAC;

			/*
			 * Might as well fix the hint bits too; usually XMIN_COMMITTED
			 * will already be set here, but there's a small chance not.
			 */
			Assert(!(tuple->t_infomask & HEAP_XMIN_INVALID));
			frz->t_infomask |= HEAP_XMIN_COMMITTED;
			changed = true;
		}
	}

	return changed;
}

/*
 * heap_execute_freeze_tuple
 *		Execute the prepared freezing of a tuple.
 *
 * Caller is responsible for ensuring that no other backend can access the
 * storage underlying this tuple, either by holding an exclusive lock on the
 * buffer containing it (which is what lazy VACUUM does), or by having it be
 * in private storage (which is what CLUSTER and friends do).
 *
 * Note: it might seem we could make the changes without exclusive lock, since
 * TransactionId read/write is assumed atomic anyway.  However there is a race
 * condition: someone who just fetched an old XID that we overwrite here could
 * conceivably not finish checking the XID against pg_clog before we finish
 * the VACUUM and perhaps truncate off the part of pg_clog he needs.  Getting
 * exclusive lock ensures no other backend is in process of checking the
 * tuple status.  Also, getting exclusive lock makes it safe to adjust the
 * infomask bits.
 *
 * NB: All code in here must be safe to execute during crash recovery!
 */
void
heap_execute_freeze_tuple(HeapTupleHeader tuple, xl_heap_freeze_tuple *frz)
{
	HeapTupleHeaderSetXmax(tuple, frz->xmax);

	if (frz->frzflags & XLH_FREEZE_XVAC)
		HeapTupleHeaderSetXvac(tuple, FrozenTransactionId);

	if (frz->frzflags & XLH_INVALID_XVAC)
		HeapTupleHeaderSetXvac(tuple, InvalidTransactionId);

	tuple->t_infomask = frz->t_infomask;
	tuple->t_infomask2 = frz->t_infomask2;
}

/*
 * heap_freeze_tuple
 *		Freeze tuple in place, without WAL logging.
 *
 * Useful for callers like CLUSTER that perform their own WAL logging.
 */
bool
heap_freeze_tuple(HeapTupleHeader tuple, TransactionId cutoff_xid,
				  TransactionId cutoff_multi)
{
	xl_heap_freeze_tuple frz;
	bool		do_freeze;

	do_freeze = heap_prepare_freeze_tuple(tuple, cutoff_xid, cutoff_multi,
										  &frz);

	/*
	 * Note that because this is not a WAL-logged operation, we don't need to
	 * fill in the offset in the freeze record.
	 */

	if (do_freeze)
		heap_execute_freeze_tuple(tuple, &frz);
	return do_freeze;
}

/*
 * For a given MultiXactId, return the hint bits that should be set in the
 * tuple's infomask.
 *
 * Normally this should be called for a multixact that was just created, and
 * so is on our local cache, so the GetMembers call is fast.
 */
static void
GetMultiXactIdHintBits(MultiXactId multi, uint16 *new_infomask,
					   uint16 *new_infomask2)
{
	int			nmembers;
	MultiXactMember *members;
	int			i;
	uint16		bits = HEAP_XMAX_IS_MULTI;
	uint16		bits2 = 0;
	bool		has_update = false;
	LockTupleMode strongest = LockTupleKeyShare;

	/*
	 * We only use this in multis we just created, so they cannot be values
	 * pre-pg_upgrade.
	 */
	nmembers = GetMultiXactIdMembers(multi, &members, false, false);

	for (i = 0; i < nmembers; i++)
	{
		LockTupleMode mode;

		/*
		 * Remember the strongest lock mode held by any member of the
		 * multixact.
		 */
		mode = TUPLOCK_from_mxstatus(members[i].status);
		if (mode > strongest)
			strongest = mode;

		/* See what other bits we need */
		switch (members[i].status)
		{
			case MultiXactStatusForKeyShare:
			case MultiXactStatusForShare:
			case MultiXactStatusForNoKeyUpdate:
				break;

			case MultiXactStatusForUpdate:
				bits2 |= HEAP_KEYS_UPDATED;
				break;

			case MultiXactStatusNoKeyUpdate:
				has_update = true;
				break;

			case MultiXactStatusUpdate:
				bits2 |= HEAP_KEYS_UPDATED;
				has_update = true;
				break;
		}
	}

	if (strongest == LockTupleExclusive ||
		strongest == LockTupleNoKeyExclusive)
		bits |= HEAP_XMAX_EXCL_LOCK;
	else if (strongest == LockTupleShare)
		bits |= HEAP_XMAX_SHR_LOCK;
	else if (strongest == LockTupleKeyShare)
		bits |= HEAP_XMAX_KEYSHR_LOCK;

	if (!has_update)
		bits |= HEAP_XMAX_LOCK_ONLY;

	if (nmembers > 0)
		pfree(members);

	*new_infomask = bits;
	*new_infomask2 = bits2;
}

/*
 * MultiXactIdGetUpdateXid
 *
 * Given a multixact Xmax and corresponding infomask, which does not have the
 * HEAP_XMAX_LOCK_ONLY bit set, obtain and return the Xid of the updating
 * transaction.
 *
 * Caller is expected to check the status of the updating transaction, if
 * necessary.
 */
static TransactionId
MultiXactIdGetUpdateXid(TransactionId xmax, uint16 t_infomask)
{
	TransactionId update_xact = InvalidTransactionId;
	MultiXactMember *members;
	int			nmembers;

	Assert(!(t_infomask & HEAP_XMAX_LOCK_ONLY));
	Assert(t_infomask & HEAP_XMAX_IS_MULTI);

	/*
	 * Since we know the LOCK_ONLY bit is not set, this cannot be a multi from
	 * pre-pg_upgrade.
	 */
	nmembers = GetMultiXactIdMembers(xmax, &members, false, false);

	if (nmembers > 0)
	{
		int			i;

		for (i = 0; i < nmembers; i++)
		{
			/* Ignore lockers */
			if (!ISUPDATE_from_mxstatus(members[i].status))
				continue;

			/* there can be at most one updater */
			Assert(update_xact == InvalidTransactionId);
			update_xact = members[i].xid;
#ifndef USE_ASSERT_CHECKING

			/*
			 * in an assert-enabled build, walk the whole array to ensure
			 * there's no other updater.
			 */
			break;
#endif
		}

		pfree(members);
	}

	return update_xact;
}

/*
 * HeapTupleGetUpdateXid
 *		As above, but use a HeapTupleHeader
 *
 * See also HeapTupleHeaderGetUpdateXid, which can be used without previously
 * checking the hint bits.
 */
TransactionId
HeapTupleGetUpdateXid(HeapTupleHeader tuple)
{
	return MultiXactIdGetUpdateXid(HeapTupleHeaderGetRawXmax(tuple),
								   tuple->t_infomask);
}

/*
 * Does the given multixact conflict with the current transaction grabbing a
 * tuple lock of the given strength?
 *
 * The passed infomask pairs up with the given multixact in the tuple header.
 */
static bool
DoesMultiXactIdConflict(MultiXactId multi, uint16 infomask,
						LockTupleMode lockmode)
{
	bool		allow_old;
	int			nmembers;
	MultiXactMember *members;
	bool		result = false;
	LOCKMODE	wanted = tupleLockExtraInfo[lockmode].hwlock;

	allow_old = !(infomask & HEAP_LOCK_MASK) && HEAP_XMAX_IS_LOCKED_ONLY(infomask);
	nmembers = GetMultiXactIdMembers(multi, &members, allow_old,
									 HEAP_XMAX_IS_LOCKED_ONLY(infomask));
	if (nmembers >= 0)
	{
		int			i;

		for (i = 0; i < nmembers; i++)
		{
			TransactionId memxid;
			LOCKMODE	memlockmode;

			memlockmode = LOCKMODE_from_mxstatus(members[i].status);

			/* ignore members that don't conflict with the lock we want */
			if (!DoLockModesConflict(memlockmode, wanted))
				continue;

			/* ignore members from current xact */
			memxid = members[i].xid;
			if (TransactionIdIsCurrentTransactionId(memxid))
				continue;

			if (ISUPDATE_from_mxstatus(members[i].status))
			{
				/* ignore aborted updaters */
				if (TransactionIdDidAbort(memxid))
					continue;
			}
			else
			{
				/* ignore lockers-only that are no longer in progress */
				if (!TransactionIdIsInProgress(memxid))
					continue;
			}

			/*
			 * Whatever remains are either live lockers that conflict with our
			 * wanted lock, and updaters that are not aborted.  Those conflict
			 * with what we want, so return true.
			 */
			result = true;
			break;
		}
		pfree(members);
	}

	return result;
}

/*
 * Do_MultiXactIdWait
 *		Actual implementation for the two functions below.
 *
 * 'multi', 'status' and 'infomask' indicate what to sleep on (the status is
 * needed to ensure we only sleep on conflicting members, and the infomask is
 * used to optimize multixact access in case it's a lock-only multi); 'nowait'
 * indicates whether to use conditional lock acquisition, to allow callers to
 * fail if lock is unavailable.  'rel', 'ctid' and 'oper' are used to set up
 * context information for error messages.  'remaining', if not NULL, receives
 * the number of members that are still running, including any (non-aborted)
 * subtransactions of our own transaction.
 *
 * We do this by sleeping on each member using XactLockTableWait.  Any
 * members that belong to the current backend are *not* waited for, however;
 * this would not merely be useless but would lead to Assert failure inside
 * XactLockTableWait.  By the time this returns, it is certain that all
 * transactions *of other backends* that were members of the MultiXactId
 * that conflict with the requested status are dead (and no new ones can have
 * been added, since it is not legal to add members to an existing
 * MultiXactId).
 *
 * But by the time we finish sleeping, someone else may have changed the Xmax
 * of the containing tuple, so the caller needs to iterate on us somehow.
 *
 * Note that in case we return false, the number of remaining members is
 * not to be trusted.
 */
static bool
Do_MultiXactIdWait(MultiXactId multi, MultiXactStatus status,
				   uint16 infomask, bool nowait,
				   Relation rel, ItemPointer ctid, XLTW_Oper oper,
				   int *remaining)
{
	bool		allow_old;
	bool		result = true;
	MultiXactMember *members;
	int			nmembers;
	int			remain = 0;

	allow_old = !(infomask & HEAP_LOCK_MASK) && HEAP_XMAX_IS_LOCKED_ONLY(infomask);
	nmembers = GetMultiXactIdMembers(multi, &members, allow_old,
									 HEAP_XMAX_IS_LOCKED_ONLY(infomask));

	if (nmembers >= 0)
	{
		int			i;

		for (i = 0; i < nmembers; i++)
		{
			TransactionId memxid = members[i].xid;
			MultiXactStatus memstatus = members[i].status;

			if (TransactionIdIsCurrentTransactionId(memxid))
			{
				remain++;
				continue;
			}

			if (!DoLockModesConflict(LOCKMODE_from_mxstatus(memstatus),
									 LOCKMODE_from_mxstatus(status)))
			{
				if (remaining && TransactionIdIsInProgress(memxid))
					remain++;
				continue;
			}

			/*
			 * This member conflicts with our multi, so we have to sleep (or
			 * return failure, if asked to avoid waiting.)
			 *
			 * Note that we don't set up an error context callback ourselves,
			 * but instead we pass the info down to XactLockTableWait.  This
			 * might seem a bit wasteful because the context is set up and
			 * tore down for each member of the multixact, but in reality it
			 * should be barely noticeable, and it avoids duplicate code.
			 */
			if (nowait)
			{
				result = ConditionalXactLockTableWait(memxid);
				if (!result)
					break;
			}
			else
				XactLockTableWait(memxid, rel, ctid, oper);
		}

		pfree(members);
	}

	if (remaining)
		*remaining = remain;

	return result;
}

/*
 * MultiXactIdWait
 *		Sleep on a MultiXactId.
 *
 * By the time we finish sleeping, someone else may have changed the Xmax
 * of the containing tuple, so the caller needs to iterate on us somehow.
 *
 * We return (in *remaining, if not NULL) the number of members that are still
 * running, including any (non-aborted) subtransactions of our own transaction.
 */
static void
MultiXactIdWait(MultiXactId multi, MultiXactStatus status, uint16 infomask,
				Relation rel, ItemPointer ctid, XLTW_Oper oper,
				int *remaining)
{
	(void) Do_MultiXactIdWait(multi, status, infomask, false,
							  rel, ctid, oper, remaining);
}

/*
 * ConditionalMultiXactIdWait
 *		As above, but only lock if we can get the lock without blocking.
 *
 * By the time we finish sleeping, someone else may have changed the Xmax
 * of the containing tuple, so the caller needs to iterate on us somehow.
 *
 * If the multixact is now all gone, return true.  Returns false if some
 * transactions might still be running.
 *
 * We return (in *remaining, if not NULL) the number of members that are still
 * running, including any (non-aborted) subtransactions of our own transaction.
 */
static bool
ConditionalMultiXactIdWait(MultiXactId multi, MultiXactStatus status,
						   uint16 infomask, Relation rel, int *remaining)
{
	return Do_MultiXactIdWait(multi, status, infomask, true,
							  rel, NULL, XLTW_None, remaining);
}

/*
 * heap_tuple_needs_eventual_freeze
 *
 * Check to see whether any of the XID fields of a tuple (xmin, xmax, xvac)
 * will eventually require freezing.  Similar to heap_tuple_needs_freeze,
 * but there's no cutoff, since we're trying to figure out whether freezing
 * will ever be needed, not whether it's needed now.
 */
bool
heap_tuple_needs_eventual_freeze(HeapTupleHeader tuple)
{
	TransactionId xid;

	/*
	 * If xmin is a normal transaction ID, this tuple is definitely not
	 * frozen.
	 */
	xid = HeapTupleHeaderGetXmin(tuple);
	if (TransactionIdIsNormal(xid))
		return true;

	/*
	 * If xmax is a valid xact or multixact, this tuple is also not frozen.
	 */
	if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
	{
		MultiXactId multi;

		multi = HeapTupleHeaderGetRawXmax(tuple);
		if (MultiXactIdIsValid(multi))
			return true;
	}
	else
	{
		xid = HeapTupleHeaderGetRawXmax(tuple);
		if (TransactionIdIsNormal(xid))
			return true;
	}

	if (tuple->t_infomask & HEAP_MOVED)
	{
		xid = HeapTupleHeaderGetXvac(tuple);
		if (TransactionIdIsNormal(xid))
			return true;
	}

	return false;
}

/*
 * heap_tuple_needs_freeze
 *
 * Check to see whether any of the XID fields of a tuple (xmin, xmax, xvac)
 * are older than the specified cutoff XID or MultiXactId.  If so, return TRUE.
 *
 * It doesn't matter whether the tuple is alive or dead, we are checking
 * to see if a tuple needs to be removed or frozen to avoid wraparound.
 *
 * NB: Cannot rely on hint bits here, they might not be set after a crash or
 * on a standby.
 */
bool
heap_tuple_needs_freeze(HeapTupleHeader tuple, TransactionId cutoff_xid,
						MultiXactId cutoff_multi, Buffer buf)
{
	TransactionId xid;

	xid = HeapTupleHeaderGetXmin(tuple);
	if (TransactionIdIsNormal(xid) &&
		TransactionIdPrecedes(xid, cutoff_xid))
		return true;

	/*
	 * The considerations for multixacts are complicated; look at
	 * heap_freeze_tuple for justifications.  This routine had better be in
	 * sync with that one!
	 */
	if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
	{
		MultiXactId multi;

		multi = HeapTupleHeaderGetRawXmax(tuple);
		if (!MultiXactIdIsValid(multi))
		{
			/* no xmax set, ignore */
			;
		}
		else if (MultiXactIdPrecedes(multi, cutoff_multi))
			return true;
		else
		{
			MultiXactMember *members;
			int			nmembers;
			int			i;
			bool		allow_old;

			/* need to check whether any member of the mxact is too old */

			allow_old = !(tuple->t_infomask & HEAP_LOCK_MASK) &&
				HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask);
			nmembers = GetMultiXactIdMembers(multi, &members, allow_old,
								HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask));

			for (i = 0; i < nmembers; i++)
			{
				if (TransactionIdPrecedes(members[i].xid, cutoff_xid))
				{
					pfree(members);
					return true;
				}
			}
			if (nmembers > 0)
				pfree(members);
		}
	}
	else
	{
		xid = HeapTupleHeaderGetRawXmax(tuple);
		if (TransactionIdIsNormal(xid) &&
			TransactionIdPrecedes(xid, cutoff_xid))
			return true;
	}

	if (tuple->t_infomask & HEAP_MOVED)
	{
		xid = HeapTupleHeaderGetXvac(tuple);
		if (TransactionIdIsNormal(xid) &&
			TransactionIdPrecedes(xid, cutoff_xid))
			return true;
	}

	return false;
}

/*
 * If 'tuple' contains any visible XID greater than latestRemovedXid,
 * ratchet forwards latestRemovedXid to the greatest one found.
 * This is used as the basis for generating Hot Standby conflicts, so
 * if a tuple was never visible then removing it should not conflict
 * with queries.
 */
void
HeapTupleHeaderAdvanceLatestRemovedXid(HeapTupleHeader tuple,
									   TransactionId *latestRemovedXid)
{
	TransactionId xmin = HeapTupleHeaderGetXmin(tuple);
	TransactionId xmax = HeapTupleHeaderGetUpdateXid(tuple);
	TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

	if (tuple->t_infomask & HEAP_MOVED)
	{
		if (TransactionIdPrecedes(*latestRemovedXid, xvac))
			*latestRemovedXid = xvac;
	}

	/*
	 * Ignore tuples inserted by an aborted transaction or if the tuple was
	 * updated/deleted by the inserting transaction.
	 *
	 * Look for a committed hint bit, or if no xmin bit is set, check clog.
	 * This needs to work on both master and standby, where it is used to
	 * assess btree delete records.
	 */
	if (HeapTupleHeaderXminCommitted(tuple) ||
		(!HeapTupleHeaderXminInvalid(tuple) && TransactionIdDidCommit(xmin)))
	{
		if (xmax != xmin &&
			TransactionIdFollows(xmax, *latestRemovedXid))
			*latestRemovedXid = xmax;
	}

	/* *latestRemovedXid may still be invalid at end */
}

/*
 * Perform XLogInsert to register a heap cleanup info message. These
 * messages are sent once per VACUUM and are required because
 * of the phasing of removal operations during a lazy VACUUM.
 * see comments for vacuum_log_cleanup_info().
 */
XLogRecPtr
log_heap_cleanup_info(RelFileNode rnode, TransactionId latestRemovedXid)
{
	xl_heap_cleanup_info xlrec;
	XLogRecPtr	recptr;

	xlrec.node = rnode;
	xlrec.latestRemovedXid = latestRemovedXid;

	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, SizeOfHeapCleanupInfo);

	recptr = XLogInsert(RM_HEAP2_ID, XLOG_HEAP2_CLEANUP_INFO);

	return recptr;
}

/*
 * Perform XLogInsert for a heap-clean operation.  Caller must already
 * have modified the buffer and marked it dirty.
 *
 * Note: prior to Postgres 8.3, the entries in the nowunused[] array were
 * zero-based tuple indexes.  Now they are one-based like other uses
 * of OffsetNumber.
 *
 * We also include latestRemovedXid, which is the greatest XID present in
 * the removed tuples. That allows recovery processing to cancel or wait
 * for long standby queries that can still see these tuples.
 */
XLogRecPtr
log_heap_clean(Relation reln, Buffer buffer,
			   OffsetNumber *redirected, int nredirected,
			   OffsetNumber *nowdead, int ndead,
			   OffsetNumber *nowunused, int nunused,
			   TransactionId latestRemovedXid)
{
	xl_heap_clean xlrec;
	XLogRecPtr	recptr;

	/* Caller should not call me on a non-WAL-logged relation */
	Assert(RelationNeedsWAL(reln));

	xlrec.latestRemovedXid = latestRemovedXid;
	xlrec.nredirected = nredirected;
	xlrec.ndead = ndead;

	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, SizeOfHeapClean);

	XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

	/*
	 * The OffsetNumber arrays are not actually in the buffer, but we pretend
	 * that they are.  When XLogInsert stores the whole buffer, the offset
	 * arrays need not be stored too.  Note that even if all three arrays are
	 * empty, we want to expose the buffer as a candidate for whole-page
	 * storage, since this record type implies a defragmentation operation
	 * even if no item pointers changed state.
	 */
	if (nredirected > 0)
		XLogRegisterBufData(0, (char *) redirected,
							nredirected * sizeof(OffsetNumber) * 2);

	if (ndead > 0)
		XLogRegisterBufData(0, (char *) nowdead,
							ndead * sizeof(OffsetNumber));

	if (nunused > 0)
		XLogRegisterBufData(0, (char *) nowunused,
							nunused * sizeof(OffsetNumber));

	recptr = XLogInsert(RM_HEAP2_ID, XLOG_HEAP2_CLEAN);

	return recptr;
}

/*
 * Perform XLogInsert for a heap-freeze operation.  Caller must have already
 * modified the buffer and marked it dirty.
 */
XLogRecPtr
log_heap_freeze(Relation reln, Buffer buffer, TransactionId cutoff_xid,
				xl_heap_freeze_tuple *tuples, int ntuples)
{
	xl_heap_freeze_page xlrec;
	XLogRecPtr	recptr;

	/* Caller should not call me on a non-WAL-logged relation */
	Assert(RelationNeedsWAL(reln));
	/* nor when there are no tuples to freeze */
	Assert(ntuples > 0);

	xlrec.cutoff_xid = cutoff_xid;
	xlrec.ntuples = ntuples;

	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, SizeOfHeapFreezePage);

	/*
	 * The freeze plan array is not actually in the buffer, but pretend that
	 * it is.  When XLogInsert stores the whole buffer, the freeze plan need
	 * not be stored too.
	 */
	XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);
	XLogRegisterBufData(0, (char *) tuples,
						ntuples * sizeof(xl_heap_freeze_tuple));

	recptr = XLogInsert(RM_HEAP2_ID, XLOG_HEAP2_FREEZE_PAGE);

	return recptr;
}

/*
 * Perform XLogInsert for a heap-visible operation.  'block' is the block
 * being marked all-visible, and vm_buffer is the buffer containing the
 * corresponding visibility map block.  Both should have already been modified
 * and dirtied.
 *
 * If checksums are enabled, we also generate a full-page image of
 * heap_buffer, if necessary.
 */
XLogRecPtr
log_heap_visible(RelFileNode rnode, Buffer heap_buffer, Buffer vm_buffer,
				 TransactionId cutoff_xid, uint8 vmflags)
{
	xl_heap_visible xlrec;
	XLogRecPtr	recptr;
	uint8		flags;

	Assert(BufferIsValid(heap_buffer));
	Assert(BufferIsValid(vm_buffer));

	xlrec.cutoff_xid = cutoff_xid;
	xlrec.flags = vmflags;
	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, SizeOfHeapVisible);

	XLogRegisterBuffer(0, vm_buffer, 0);

	flags = REGBUF_STANDARD;
	if (!XLogHintBitIsNeeded())
		flags |= REGBUF_NO_IMAGE;
	XLogRegisterBuffer(1, heap_buffer, flags);

	recptr = XLogInsert(RM_HEAP2_ID, XLOG_HEAP2_VISIBLE);

	return recptr;
}

/*
 * Perform XLogInsert for a heap-update operation.  Caller must already
 * have modified the buffer(s) and marked them dirty.
 */
static XLogRecPtr
log_heap_update(Relation reln, Buffer oldbuf,
				Buffer newbuf, HeapTuple oldtup, HeapTuple newtup,
				HeapTuple old_key_tuple,
				bool all_visible_cleared, bool new_all_visible_cleared)
{
	xl_heap_update xlrec;
	xl_heap_header xlhdr;
	xl_heap_header xlhdr_idx;
	uint8		info;
	uint16		prefix_suffix[2];
	uint16		prefixlen = 0,
				suffixlen = 0;
	XLogRecPtr	recptr;
	Page		page = BufferGetPage(newbuf);
	bool		need_tuple_data = RelationIsLogicallyLogged(reln);
	bool		init;
	int			bufflags;

	/* Caller should not call me on a non-WAL-logged relation */
	Assert(RelationNeedsWAL(reln));

	XLogBeginInsert();

	if (HeapTupleIsHeapOnly(newtup))
		info = XLOG_HEAP_HOT_UPDATE;
	else
		info = XLOG_HEAP_UPDATE;

	/*
	 * If the old and new tuple are on the same page, we only need to log the
	 * parts of the new tuple that were changed.  That saves on the amount of
	 * WAL we need to write.  Currently, we just count any unchanged bytes in
	 * the beginning and end of the tuple.  That's quick to check, and
	 * perfectly covers the common case that only one field is updated.
	 *
	 * We could do this even if the old and new tuple are on different pages,
	 * but only if we don't make a full-page image of the old page, which is
	 * difficult to know in advance.  Also, if the old tuple is corrupt for
	 * some reason, it would allow the corruption to propagate the new page,
	 * so it seems best to avoid.  Under the general assumption that most
	 * updates tend to create the new tuple version on the same page, there
	 * isn't much to be gained by doing this across pages anyway.
	 *
	 * Skip this if we're taking a full-page image of the new page, as we
	 * don't include the new tuple in the WAL record in that case.  Also
	 * disable if wal_level='logical', as logical decoding needs to be able to
	 * read the new tuple in whole from the WAL record alone.
	 */
	if (oldbuf == newbuf && !need_tuple_data &&
		!XLogCheckBufferNeedsBackup(newbuf))
	{
		char	   *oldp = (char *) oldtup->t_data + oldtup->t_data->t_hoff;
		char	   *newp = (char *) newtup->t_data + newtup->t_data->t_hoff;
		int			oldlen = oldtup->t_len - oldtup->t_data->t_hoff;
		int			newlen = newtup->t_len - newtup->t_data->t_hoff;

		/* Check for common prefix between old and new tuple */
		for (prefixlen = 0; prefixlen < Min(oldlen, newlen); prefixlen++)
		{
			if (newp[prefixlen] != oldp[prefixlen])
				break;
		}

		/*
		 * Storing the length of the prefix takes 2 bytes, so we need to save
		 * at least 3 bytes or there's no point.
		 */
		if (prefixlen < 3)
			prefixlen = 0;

		/* Same for suffix */
		for (suffixlen = 0; suffixlen < Min(oldlen, newlen) - prefixlen; suffixlen++)
		{
			if (newp[newlen - suffixlen - 1] != oldp[oldlen - suffixlen - 1])
				break;
		}
		if (suffixlen < 3)
			suffixlen = 0;
	}

	/* Prepare main WAL data chain */
	xlrec.flags = 0;
	if (all_visible_cleared)
		xlrec.flags |= XLH_UPDATE_OLD_ALL_VISIBLE_CLEARED;
	if (new_all_visible_cleared)
		xlrec.flags |= XLH_UPDATE_NEW_ALL_VISIBLE_CLEARED;
	if (prefixlen > 0)
		xlrec.flags |= XLH_UPDATE_PREFIX_FROM_OLD;
	if (suffixlen > 0)
		xlrec.flags |= XLH_UPDATE_SUFFIX_FROM_OLD;
	if (need_tuple_data)
	{
		xlrec.flags |= XLH_UPDATE_CONTAINS_NEW_TUPLE;
		if (old_key_tuple)
		{
			if (reln->rd_rel->relreplident == REPLICA_IDENTITY_FULL)
				xlrec.flags |= XLH_UPDATE_CONTAINS_OLD_TUPLE;
			else
				xlrec.flags |= XLH_UPDATE_CONTAINS_OLD_KEY;
		}
	}

	/* If new tuple is the single and first tuple on page... */
	if (ItemPointerGetOffsetNumber(&(newtup->t_self)) == FirstOffsetNumber &&
		PageGetMaxOffsetNumber(page) == FirstOffsetNumber)
	{
		info |= XLOG_HEAP_INIT_PAGE;
		init = true;
	}
	else
		init = false;

	/* Prepare WAL data for the old page */
	xlrec.old_offnum = ItemPointerGetOffsetNumber(&oldtup->t_self);
	xlrec.old_xmax = HeapTupleHeaderGetRawXmax(oldtup->t_data);
	xlrec.old_infobits_set = compute_infobits(oldtup->t_data->t_infomask,
											  oldtup->t_data->t_infomask2);

	/* Prepare WAL data for the new page */
	xlrec.new_offnum = ItemPointerGetOffsetNumber(&newtup->t_self);
	xlrec.new_xmax = HeapTupleHeaderGetRawXmax(newtup->t_data);

	bufflags = REGBUF_STANDARD;
	if (init)
		bufflags |= REGBUF_WILL_INIT;
	if (need_tuple_data)
		bufflags |= REGBUF_KEEP_DATA;

	XLogRegisterBuffer(0, newbuf, bufflags);
	if (oldbuf != newbuf)
		XLogRegisterBuffer(1, oldbuf, REGBUF_STANDARD);

	XLogRegisterData((char *) &xlrec, SizeOfHeapUpdate);

	/*
	 * Prepare WAL data for the new tuple.
	 */
	if (prefixlen > 0 || suffixlen > 0)
	{
		if (prefixlen > 0 && suffixlen > 0)
		{
			prefix_suffix[0] = prefixlen;
			prefix_suffix[1] = suffixlen;
			XLogRegisterBufData(0, (char *) &prefix_suffix, sizeof(uint16) * 2);
		}
		else if (prefixlen > 0)
		{
			XLogRegisterBufData(0, (char *) &prefixlen, sizeof(uint16));
		}
		else
		{
			XLogRegisterBufData(0, (char *) &suffixlen, sizeof(uint16));
		}
	}

	xlhdr.t_infomask2 = newtup->t_data->t_infomask2;
	xlhdr.t_infomask = newtup->t_data->t_infomask;
	xlhdr.t_hoff = newtup->t_data->t_hoff;
	Assert(SizeofHeapTupleHeader + prefixlen + suffixlen <= newtup->t_len);

	/*
	 * PG73FORMAT: write bitmap [+ padding] [+ oid] + data
	 *
	 * The 'data' doesn't include the common prefix or suffix.
	 */
	XLogRegisterBufData(0, (char *) &xlhdr, SizeOfHeapHeader);
	if (prefixlen == 0)
	{
		XLogRegisterBufData(0,
							((char *) newtup->t_data) + SizeofHeapTupleHeader,
						  newtup->t_len - SizeofHeapTupleHeader - suffixlen);
	}
	else
	{
		/*
		 * Have to write the null bitmap and data after the common prefix as
		 * two separate rdata entries.
		 */
		/* bitmap [+ padding] [+ oid] */
		if (newtup->t_data->t_hoff - SizeofHeapTupleHeader > 0)
		{
			XLogRegisterBufData(0,
						   ((char *) newtup->t_data) + SizeofHeapTupleHeader,
							 newtup->t_data->t_hoff - SizeofHeapTupleHeader);
		}

		/* data after common prefix */
		XLogRegisterBufData(0,
			  ((char *) newtup->t_data) + newtup->t_data->t_hoff + prefixlen,
			 newtup->t_len - newtup->t_data->t_hoff - prefixlen - suffixlen);
	}

	/* We need to log a tuple identity */
	if (need_tuple_data && old_key_tuple)
	{
		/* don't really need this, but its more comfy to decode */
		xlhdr_idx.t_infomask2 = old_key_tuple->t_data->t_infomask2;
		xlhdr_idx.t_infomask = old_key_tuple->t_data->t_infomask;
		xlhdr_idx.t_hoff = old_key_tuple->t_data->t_hoff;

		XLogRegisterData((char *) &xlhdr_idx, SizeOfHeapHeader);

		/* PG73FORMAT: write bitmap [+ padding] [+ oid] + data */
		XLogRegisterData((char *) old_key_tuple->t_data + SizeofHeapTupleHeader,
						 old_key_tuple->t_len - SizeofHeapTupleHeader);
	}

	/* filtering by origin on a row level is much more efficient */
	XLogIncludeOrigin();

	recptr = XLogInsert(RM_HEAP_ID, info);

	return recptr;
}

/*
 * Perform XLogInsert of an XLOG_HEAP2_NEW_CID record
 *
 * This is only used in wal_level >= WAL_LEVEL_LOGICAL, and only for catalog
 * tuples.
 */
static XLogRecPtr
log_heap_new_cid(Relation relation, HeapTuple tup)
{
	xl_heap_new_cid xlrec;

	XLogRecPtr	recptr;
	HeapTupleHeader hdr = tup->t_data;

	Assert(ItemPointerIsValid(&tup->t_self));
	Assert(tup->t_tableOid != InvalidOid);

	xlrec.top_xid = GetTopTransactionId();
	xlrec.target_node = relation->rd_node;
	xlrec.target_tid = tup->t_self;

	/*
	 * If the tuple got inserted & deleted in the same TX we definitely have a
	 * combocid, set cmin and cmax.
	 */
	if (hdr->t_infomask & HEAP_COMBOCID)
	{
		Assert(!(hdr->t_infomask & HEAP_XMAX_INVALID));
		Assert(!HeapTupleHeaderXminInvalid(hdr));
		xlrec.cmin = HeapTupleHeaderGetCmin(hdr);
		xlrec.cmax = HeapTupleHeaderGetCmax(hdr);
		xlrec.combocid = HeapTupleHeaderGetRawCommandId(hdr);
	}
	/* No combocid, so only cmin or cmax can be set by this TX */
	else
	{
		/*
		 * Tuple inserted.
		 *
		 * We need to check for LOCK ONLY because multixacts might be
		 * transferred to the new tuple in case of FOR KEY SHARE updates in
		 * which case there will be an xmax, although the tuple just got
		 * inserted.
		 */
		if (hdr->t_infomask & HEAP_XMAX_INVALID ||
			HEAP_XMAX_IS_LOCKED_ONLY(hdr->t_infomask))
		{
			xlrec.cmin = HeapTupleHeaderGetRawCommandId(hdr);
			xlrec.cmax = InvalidCommandId;
		}
		/* Tuple from a different tx updated or deleted. */
		else
		{
			xlrec.cmin = InvalidCommandId;
			xlrec.cmax = HeapTupleHeaderGetRawCommandId(hdr);

		}
		xlrec.combocid = InvalidCommandId;
	}

	/*
	 * Note that we don't need to register the buffer here, because this
	 * operation does not modify the page. The insert/update/delete that
	 * called us certainly did, but that's WAL-logged separately.
	 */
	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, SizeOfHeapNewCid);

	/* will be looked at irrespective of origin */

	recptr = XLogInsert(RM_HEAP2_ID, XLOG_HEAP2_NEW_CID);

	return recptr;
}

/*
 * Build a heap tuple representing the configured REPLICA IDENTITY to represent
 * the old tuple in a UPDATE or DELETE.
 *
 * Returns NULL if there's no need to log an identity or if there's no suitable
 * key in the Relation relation.
 */
static HeapTuple
ExtractReplicaIdentity(Relation relation, HeapTuple tp, bool key_changed, bool *copy)
{
	TupleDesc	desc = RelationGetDescr(relation);
	Oid			replidindex;
	Relation	idx_rel;
	TupleDesc	idx_desc;
	char		replident = relation->rd_rel->relreplident;
	HeapTuple	key_tuple = NULL;
	bool		nulls[MaxHeapAttributeNumber];
	Datum		values[MaxHeapAttributeNumber];
	int			natt;

	*copy = false;

	if (!RelationIsLogicallyLogged(relation))
		return NULL;

	if (replident == REPLICA_IDENTITY_NOTHING)
		return NULL;

	if (replident == REPLICA_IDENTITY_FULL)
	{
		/*
		 * When logging the entire old tuple, it very well could contain
		 * toasted columns. If so, force them to be inlined.
		 */
		if (HeapTupleHasExternal(tp))
		{
			*copy = true;
			tp = toast_flatten_tuple(tp, RelationGetDescr(relation));
		}
		return tp;
	}

	/* if the key hasn't changed and we're only logging the key, we're done */
	if (!key_changed)
		return NULL;

	/* find the replica identity index */
	replidindex = RelationGetReplicaIndex(relation);
	if (!OidIsValid(replidindex))
	{
		elog(DEBUG4, "could not find configured replica identity for table \"%s\"",
			 RelationGetRelationName(relation));
		return NULL;
	}

	idx_rel = RelationIdGetRelation(replidindex);
	idx_desc = RelationGetDescr(idx_rel);

	/* deform tuple, so we have fast access to columns */
	heap_deform_tuple(tp, desc, values, nulls);

	/* set all columns to NULL, regardless of whether they actually are */
	memset(nulls, 1, sizeof(nulls));

	/*
	 * Now set all columns contained in the index to NOT NULL, they cannot
	 * currently be NULL.
	 */
	for (natt = 0; natt < idx_desc->natts; natt++)
	{
		int			attno = idx_rel->rd_index->indkey.values[natt];

		if (attno < 0)
		{
			/*
			 * The OID column can appear in an index definition, but that's
			 * OK, because we always copy the OID if present (see below).
			 * Other system columns may not.
			 */
			if (attno == ObjectIdAttributeNumber)
				continue;
			elog(ERROR, "system column in index");
		}
		nulls[attno - 1] = false;
	}

	key_tuple = heap_form_tuple(desc, values, nulls);
	*copy = true;
	RelationClose(idx_rel);

	/*
	 * Always copy oids if the table has them, even if not included in the
	 * index. The space in the logged tuple is used anyway, so there's little
	 * point in not including the information.
	 */
	if (relation->rd_rel->relhasoids)
		HeapTupleSetOid(key_tuple, HeapTupleGetOid(tp));

	/*
	 * If the tuple, which by here only contains indexed columns, still has
	 * toasted columns, force them to be inlined. This is somewhat unlikely
	 * since there's limits on the size of indexed columns, so we don't
	 * duplicate toast_flatten_tuple()s functionality in the above loop over
	 * the indexed columns, even if it would be more efficient.
	 */
	if (HeapTupleHasExternal(key_tuple))
	{
		HeapTuple	oldtup = key_tuple;

		key_tuple = toast_flatten_tuple(oldtup, RelationGetDescr(relation));
		heap_freetuple(oldtup);
	}

	return key_tuple;
}

/*
 * Handles CLEANUP_INFO
 */
static void
heap_xlog_cleanup_info(XLogReaderState *record)
{
	xl_heap_cleanup_info *xlrec = (xl_heap_cleanup_info *) XLogRecGetData(record);

	if (InHotStandby)
		ResolveRecoveryConflictWithSnapshot(xlrec->latestRemovedXid, xlrec->node);

	/*
	 * Actual operation is a no-op. Record type exists to provide a means for
	 * conflict processing to occur before we begin index vacuum actions. see
	 * vacuumlazy.c and also comments in btvacuumpage()
	 */

	/* Backup blocks are not used in cleanup_info records */
	Assert(!XLogRecHasAnyBlockRefs(record));
}

/*
 * Handles HEAP2_CLEAN record type
 */
static void
heap_xlog_clean(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_heap_clean *xlrec = (xl_heap_clean *) XLogRecGetData(record);
	Buffer		buffer;
	Size		freespace = 0;
	RelFileNode rnode;
	BlockNumber blkno;
	XLogRedoAction action;

	XLogRecGetBlockTag(record, 0, &rnode, NULL, &blkno);

	/*
	 * We're about to remove tuples. In Hot Standby mode, ensure that there's
	 * no queries running for which the removed tuples are still visible.
	 *
	 * Not all HEAP2_CLEAN records remove tuples with xids, so we only want to
	 * conflict on the records that cause MVCC failures for user queries. If
	 * latestRemovedXid is invalid, skip conflict processing.
	 */
	if (InHotStandby && TransactionIdIsValid(xlrec->latestRemovedXid))
		ResolveRecoveryConflictWithSnapshot(xlrec->latestRemovedXid, rnode);

	/*
	 * If we have a full-page image, restore it (using a cleanup lock) and
	 * we're done.
	 */
	action = XLogReadBufferForRedoExtended(record, 0, RBM_NORMAL, true,
										   &buffer);
	if (action == BLK_NEEDS_REDO)
	{
		Page		page = (Page) BufferGetPage(buffer);
		OffsetNumber *end;
		OffsetNumber *redirected;
		OffsetNumber *nowdead;
		OffsetNumber *nowunused;
		int			nredirected;
		int			ndead;
		int			nunused;
		Size		datalen;

		redirected = (OffsetNumber *) XLogRecGetBlockData(record, 0, &datalen);

		nredirected = xlrec->nredirected;
		ndead = xlrec->ndead;
		end = (OffsetNumber *) ((char *) redirected + datalen);
		nowdead = redirected + (nredirected * 2);
		nowunused = nowdead + ndead;
		nunused = (end - nowunused);
		Assert(nunused >= 0);

		/* Update all item pointers per the record, and repair fragmentation */
		heap_page_prune_execute(buffer,
								redirected, nredirected,
								nowdead, ndead,
								nowunused, nunused);

		freespace = PageGetHeapFreeSpace(page); /* needed to update FSM below */

		/*
		 * Note: we don't worry about updating the page's prunability hints.
		 * At worst this will cause an extra prune cycle to occur soon.
		 */

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);

	/*
	 * Update the FSM as well.
	 *
	 * XXX: Don't do this if the page was restored from full page image. We
	 * don't bother to update the FSM in that case, it doesn't need to be
	 * totally accurate anyway.
	 */
	if (action == BLK_NEEDS_REDO)
		XLogRecordPageWithFreeSpace(rnode, blkno, freespace);
}

/*
 * Replay XLOG_HEAP2_VISIBLE record.
 *
 * The critical integrity requirement here is that we must never end up with
 * a situation where the visibility map bit is set, and the page-level
 * PD_ALL_VISIBLE bit is clear.  If that were to occur, then a subsequent
 * page modification would fail to clear the visibility map bit.
 */
static void
heap_xlog_visible(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_heap_visible *xlrec = (xl_heap_visible *) XLogRecGetData(record);
	Buffer		vmbuffer = InvalidBuffer;
	Buffer		buffer;
	Page		page;
	RelFileNode rnode;
	BlockNumber blkno;
	XLogRedoAction action;

	XLogRecGetBlockTag(record, 1, &rnode, NULL, &blkno);

	/*
	 * If there are any Hot Standby transactions running that have an xmin
	 * horizon old enough that this page isn't all-visible for them, they
	 * might incorrectly decide that an index-only scan can skip a heap fetch.
	 *
	 * NB: It might be better to throw some kind of "soft" conflict here that
	 * forces any index-only scan that is in flight to perform heap fetches,
	 * rather than killing the transaction outright.
	 */
	if (InHotStandby)
		ResolveRecoveryConflictWithSnapshot(xlrec->cutoff_xid, rnode);

	/*
	 * Read the heap page, if it still exists. If the heap file has dropped or
	 * truncated later in recovery, we don't need to update the page, but we'd
	 * better still update the visibility map.
	 */
	action = XLogReadBufferForRedo(record, 1, &buffer);
	if (action == BLK_NEEDS_REDO)
	{
		/*
		 * We don't bump the LSN of the heap page when setting the visibility
		 * map bit (unless checksums or wal_hint_bits is enabled, in which
		 * case we must), because that would generate an unworkable volume of
		 * full-page writes.  This exposes us to torn page hazards, but since
		 * we're not inspecting the existing page contents in any way, we
		 * don't care.
		 *
		 * However, all operations that clear the visibility map bit *do* bump
		 * the LSN, and those operations will only be replayed if the XLOG LSN
		 * follows the page LSN.  Thus, if the page LSN has advanced past our
		 * XLOG record's LSN, we mustn't mark the page all-visible, because
		 * the subsequent update won't be replayed to clear the flag.
		 */
		page = BufferGetPage(buffer);

		if (xlrec->flags & VISIBILITYMAP_ALL_VISIBLE)
			PageSetAllVisible(page);
		if (xlrec->flags & VISIBILITYMAP_ALL_FROZEN)
			PageSetAllFrozen(page);

		MarkBufferDirty(buffer);
	}
	else if (action == BLK_RESTORED)
	{
		/*
		 * If heap block was backed up, we already restored it and there's
		 * nothing more to do. (This can only happen with checksums or
		 * wal_log_hints enabled.)
		 */
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);

	/*
	 * Even if we skipped the heap page update due to the LSN interlock, it's
	 * still safe to update the visibility map.  Any WAL record that clears
	 * the visibility map bit does so before checking the page LSN, so any
	 * bits that need to be cleared will still be cleared.
	 */
	if (XLogReadBufferForRedoExtended(record, 0, RBM_ZERO_ON_ERROR, false,
									  &vmbuffer) == BLK_NEEDS_REDO)
	{
		Page		vmpage = BufferGetPage(vmbuffer);
		Relation	reln;

		/* initialize the page if it was read as zeros */
		if (PageIsNew(vmpage))
			PageInit(vmpage, BLCKSZ, 0);

		/*
		 * XLogReplayBufferExtended locked the buffer. But visibilitymap_set
		 * will handle locking itself.
		 */
		LockBuffer(vmbuffer, BUFFER_LOCK_UNLOCK);

		reln = CreateFakeRelcacheEntry(rnode);
		visibilitymap_pin(reln, blkno, &vmbuffer);

		/*
		 * Don't set the bit if replay has already passed this point.
		 *
		 * It might be safe to do this unconditionally; if replay has passed
		 * this point, we'll replay at least as far this time as we did
		 * before, and if this bit needs to be cleared, the record responsible
		 * for doing so should be again replayed, and clear it.  For right
		 * now, out of an abundance of conservatism, we use the same test here
		 * we did for the heap page.  If this results in a dropped bit, no
		 * real harm is done; and the next VACUUM will fix it.
		 */
		if (lsn > PageGetLSN(vmpage))
			visibilitymap_set(reln, blkno, InvalidBuffer, lsn, vmbuffer,
							  xlrec->cutoff_xid, xlrec->flags);

		ReleaseBuffer(vmbuffer);
		FreeFakeRelcacheEntry(reln);
	}
	else if (BufferIsValid(vmbuffer))
		UnlockReleaseBuffer(vmbuffer);
}

/*
 * Replay XLOG_HEAP2_FREEZE_PAGE records
 */
static void
heap_xlog_freeze_page(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_heap_freeze_page *xlrec = (xl_heap_freeze_page *) XLogRecGetData(record);
	TransactionId cutoff_xid = xlrec->cutoff_xid;
	Buffer		buffer;
	int			ntup;

	/*
	 * In Hot Standby mode, ensure that there's no queries running which still
	 * consider the frozen xids as running.
	 */
	if (InHotStandby)
	{
		RelFileNode rnode;
		TransactionId latestRemovedXid = cutoff_xid;

		TransactionIdRetreat(latestRemovedXid);

		XLogRecGetBlockTag(record, 0, &rnode, NULL, NULL);
		ResolveRecoveryConflictWithSnapshot(latestRemovedXid, rnode);
	}

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		Page		page = BufferGetPage(buffer);
		xl_heap_freeze_tuple *tuples;

		tuples = (xl_heap_freeze_tuple *) XLogRecGetBlockData(record, 0, NULL);

		/* now execute freeze plan for each frozen tuple */
		for (ntup = 0; ntup < xlrec->ntuples; ntup++)
		{
			xl_heap_freeze_tuple *xlrec_tp;
			ItemId		lp;
			HeapTupleHeader tuple;

			xlrec_tp = &tuples[ntup];
			lp = PageGetItemId(page, xlrec_tp->offset); /* offsets are one-based */
			tuple = (HeapTupleHeader) PageGetItem(page, lp);

			heap_execute_freeze_tuple(tuple, xlrec_tp);
		}

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

/*
 * Given an "infobits" field from an XLog record, set the correct bits in the
 * given infomask and infomask2 for the tuple touched by the record.
 *
 * (This is the reverse of compute_infobits).
 */
static void
fix_infomask_from_infobits(uint8 infobits, uint16 *infomask, uint16 *infomask2)
{
	*infomask &= ~(HEAP_XMAX_IS_MULTI | HEAP_XMAX_LOCK_ONLY |
				   HEAP_XMAX_KEYSHR_LOCK | HEAP_XMAX_EXCL_LOCK);
	*infomask2 &= ~HEAP_KEYS_UPDATED;

	if (infobits & XLHL_XMAX_IS_MULTI)
		*infomask |= HEAP_XMAX_IS_MULTI;
	if (infobits & XLHL_XMAX_LOCK_ONLY)
		*infomask |= HEAP_XMAX_LOCK_ONLY;
	if (infobits & XLHL_XMAX_EXCL_LOCK)
		*infomask |= HEAP_XMAX_EXCL_LOCK;
	/* note HEAP_XMAX_SHR_LOCK isn't considered here */
	if (infobits & XLHL_XMAX_KEYSHR_LOCK)
		*infomask |= HEAP_XMAX_KEYSHR_LOCK;

	if (infobits & XLHL_KEYS_UPDATED)
		*infomask2 |= HEAP_KEYS_UPDATED;
}

static void
heap_xlog_delete(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_heap_delete *xlrec = (xl_heap_delete *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;
	ItemId		lp = NULL;
	HeapTupleHeader htup;
	BlockNumber blkno;
	RelFileNode target_node;
	ItemPointerData target_tid;

	XLogRecGetBlockTag(record, 0, &target_node, NULL, &blkno);
	ItemPointerSetBlockNumber(&target_tid, blkno);
	ItemPointerSetOffsetNumber(&target_tid, xlrec->offnum);

	/*
	 * The visibility map may need to be fixed even if the heap page is
	 * already up-to-date.
	 */
	if (xlrec->flags & XLH_DELETE_ALL_VISIBLE_CLEARED)
	{
		Relation	reln = CreateFakeRelcacheEntry(target_node);
		Buffer		vmbuffer = InvalidBuffer;

		visibilitymap_pin(reln, blkno, &vmbuffer);
		visibilitymap_clear(reln, blkno, vmbuffer);
		ReleaseBuffer(vmbuffer);
		FreeFakeRelcacheEntry(reln);
	}

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		page = BufferGetPage(buffer);

		if (PageGetMaxOffsetNumber(page) >= xlrec->offnum)
			lp = PageGetItemId(page, xlrec->offnum);

		if (PageGetMaxOffsetNumber(page) < xlrec->offnum || !ItemIdIsNormal(lp))
			elog(PANIC, "invalid lp");

		htup = (HeapTupleHeader) PageGetItem(page, lp);

		htup->t_infomask &= ~(HEAP_XMAX_BITS | HEAP_MOVED);
		htup->t_infomask2 &= ~HEAP_KEYS_UPDATED;
		HeapTupleHeaderClearHotUpdated(htup);
		fix_infomask_from_infobits(xlrec->infobits_set,
								   &htup->t_infomask, &htup->t_infomask2);
		if (!(xlrec->flags & XLH_DELETE_IS_SUPER))
			HeapTupleHeaderSetXmax(htup, xlrec->xmax);
		else
			HeapTupleHeaderSetXmin(htup, InvalidTransactionId);
		HeapTupleHeaderSetCmax(htup, FirstCommandId, false);

		/* Mark the page as a candidate for pruning */
		PageSetPrunable(page, XLogRecGetXid(record));

		if (xlrec->flags & XLH_DELETE_ALL_VISIBLE_CLEARED)
			PageClearAllVisible(page);

		/* Make sure there is no forward chain link in t_ctid */
		htup->t_ctid = target_tid;
		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

static void
heap_xlog_insert(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_heap_insert *xlrec = (xl_heap_insert *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;
	union
	{
		HeapTupleHeaderData hdr;
		char		data[MaxHeapTupleSize];
	}			tbuf;
	HeapTupleHeader htup;
	xl_heap_header xlhdr;
	uint32		newlen;
	Size		freespace = 0;
	RelFileNode target_node;
	BlockNumber blkno;
	ItemPointerData target_tid;
	XLogRedoAction action;

	XLogRecGetBlockTag(record, 0, &target_node, NULL, &blkno);
	ItemPointerSetBlockNumber(&target_tid, blkno);
	ItemPointerSetOffsetNumber(&target_tid, xlrec->offnum);

	/*
	 * The visibility map may need to be fixed even if the heap page is
	 * already up-to-date.
	 */
	if (xlrec->flags & XLH_INSERT_ALL_VISIBLE_CLEARED)
	{
		Relation	reln = CreateFakeRelcacheEntry(target_node);
		Buffer		vmbuffer = InvalidBuffer;

		visibilitymap_pin(reln, blkno, &vmbuffer);
		visibilitymap_clear(reln, blkno, vmbuffer);
		ReleaseBuffer(vmbuffer);
		FreeFakeRelcacheEntry(reln);
	}

	/*
	 * If we inserted the first and only tuple on the page, re-initialize the
	 * page from scratch.
	 */
	if (XLogRecGetInfo(record) & XLOG_HEAP_INIT_PAGE)
	{
		buffer = XLogInitBufferForRedo(record, 0);
		page = BufferGetPage(buffer);
		PageInit(page, BufferGetPageSize(buffer), 0);
		action = BLK_NEEDS_REDO;
	}
	else
		action = XLogReadBufferForRedo(record, 0, &buffer);
	if (action == BLK_NEEDS_REDO)
	{
		Size		datalen;
		char	   *data;

		page = BufferGetPage(buffer);

		if (PageGetMaxOffsetNumber(page) + 1 < xlrec->offnum)
			elog(PANIC, "invalid max offset number");

		data = XLogRecGetBlockData(record, 0, &datalen);

		newlen = datalen - SizeOfHeapHeader;
		Assert(datalen > SizeOfHeapHeader && newlen <= MaxHeapTupleSize);
		memcpy((char *) &xlhdr, data, SizeOfHeapHeader);
		data += SizeOfHeapHeader;

		htup = &tbuf.hdr;
		MemSet((char *) htup, 0, SizeofHeapTupleHeader);
		/* PG73FORMAT: get bitmap [+ padding] [+ oid] + data */
		memcpy((char *) htup + SizeofHeapTupleHeader,
			   data,
			   newlen);
		newlen += SizeofHeapTupleHeader;
		htup->t_infomask2 = xlhdr.t_infomask2;
		htup->t_infomask = xlhdr.t_infomask;
		htup->t_hoff = xlhdr.t_hoff;
		HeapTupleHeaderSetXmin(htup, XLogRecGetXid(record));
		HeapTupleHeaderSetCmin(htup, FirstCommandId);
		htup->t_ctid = target_tid;

		if (PageAddItem(page, (Item) htup, newlen, xlrec->offnum,
						true, true) == InvalidOffsetNumber)
			elog(PANIC, "failed to add tuple");

		freespace = PageGetHeapFreeSpace(page); /* needed to update FSM below */

		PageSetLSN(page, lsn);

		if (xlrec->flags & XLH_INSERT_ALL_VISIBLE_CLEARED)
			PageClearAllVisible(page);

		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);

	/*
	 * If the page is running low on free space, update the FSM as well.
	 * Arbitrarily, our definition of "low" is less than 20%. We can't do much
	 * better than that without knowing the fill-factor for the table.
	 *
	 * XXX: Don't do this if the page was restored from full page image. We
	 * don't bother to update the FSM in that case, it doesn't need to be
	 * totally accurate anyway.
	 */
	if (action == BLK_NEEDS_REDO && freespace < BLCKSZ / 5)
		XLogRecordPageWithFreeSpace(target_node, blkno, freespace);
}

/*
 * Handles MULTI_INSERT record type.
 */
static void
heap_xlog_multi_insert(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_heap_multi_insert *xlrec;
	RelFileNode rnode;
	BlockNumber blkno;
	Buffer		buffer;
	Page		page;
	union
	{
		HeapTupleHeaderData hdr;
		char		data[MaxHeapTupleSize];
	}			tbuf;
	HeapTupleHeader htup;
	uint32		newlen;
	Size		freespace = 0;
	int			i;
	bool		isinit = (XLogRecGetInfo(record) & XLOG_HEAP_INIT_PAGE) != 0;
	XLogRedoAction action;

	/*
	 * Insertion doesn't overwrite MVCC data, so no conflict processing is
	 * required.
	 */
	xlrec = (xl_heap_multi_insert *) XLogRecGetData(record);

	XLogRecGetBlockTag(record, 0, &rnode, NULL, &blkno);

	/*
	 * The visibility map may need to be fixed even if the heap page is
	 * already up-to-date.
	 */
	if (xlrec->flags & XLH_INSERT_ALL_VISIBLE_CLEARED)
	{
		Relation	reln = CreateFakeRelcacheEntry(rnode);
		Buffer		vmbuffer = InvalidBuffer;

		visibilitymap_pin(reln, blkno, &vmbuffer);
		visibilitymap_clear(reln, blkno, vmbuffer);
		ReleaseBuffer(vmbuffer);
		FreeFakeRelcacheEntry(reln);
	}

	if (isinit)
	{
		buffer = XLogInitBufferForRedo(record, 0);
		page = BufferGetPage(buffer);
		PageInit(page, BufferGetPageSize(buffer), 0);
		action = BLK_NEEDS_REDO;
	}
	else
		action = XLogReadBufferForRedo(record, 0, &buffer);
	if (action == BLK_NEEDS_REDO)
	{
		char	   *tupdata;
		char	   *endptr;
		Size		len;

		/* Tuples are stored as block data */
		tupdata = XLogRecGetBlockData(record, 0, &len);
		endptr = tupdata + len;

		page = (Page) BufferGetPage(buffer);

		for (i = 0; i < xlrec->ntuples; i++)
		{
			OffsetNumber offnum;
			xl_multi_insert_tuple *xlhdr;

			/*
			 * If we're reinitializing the page, the tuples are stored in
			 * order from FirstOffsetNumber. Otherwise there's an array of
			 * offsets in the WAL record, and the tuples come after that.
			 */
			if (isinit)
				offnum = FirstOffsetNumber + i;
			else
				offnum = xlrec->offsets[i];
			if (PageGetMaxOffsetNumber(page) + 1 < offnum)
				elog(PANIC, "invalid max offset number");

			xlhdr = (xl_multi_insert_tuple *) SHORTALIGN(tupdata);
			tupdata = ((char *) xlhdr) + SizeOfMultiInsertTuple;

			newlen = xlhdr->datalen;
			Assert(newlen <= MaxHeapTupleSize);
			htup = &tbuf.hdr;
			MemSet((char *) htup, 0, SizeofHeapTupleHeader);
			/* PG73FORMAT: get bitmap [+ padding] [+ oid] + data */
			memcpy((char *) htup + SizeofHeapTupleHeader,
				   (char *) tupdata,
				   newlen);
			tupdata += newlen;

			newlen += SizeofHeapTupleHeader;
			htup->t_infomask2 = xlhdr->t_infomask2;
			htup->t_infomask = xlhdr->t_infomask;
			htup->t_hoff = xlhdr->t_hoff;
			HeapTupleHeaderSetXmin(htup, XLogRecGetXid(record));
			HeapTupleHeaderSetCmin(htup, FirstCommandId);
			ItemPointerSetBlockNumber(&htup->t_ctid, blkno);
			ItemPointerSetOffsetNumber(&htup->t_ctid, offnum);

			offnum = PageAddItem(page, (Item) htup, newlen, offnum, true, true);
			if (offnum == InvalidOffsetNumber)
				elog(PANIC, "failed to add tuple");
		}
		if (tupdata != endptr)
			elog(PANIC, "total tuple length mismatch");

		freespace = PageGetHeapFreeSpace(page); /* needed to update FSM below */

		PageSetLSN(page, lsn);

		if (xlrec->flags & XLH_INSERT_ALL_VISIBLE_CLEARED)
			PageClearAllVisible(page);

		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);

	/*
	 * If the page is running low on free space, update the FSM as well.
	 * Arbitrarily, our definition of "low" is less than 20%. We can't do much
	 * better than that without knowing the fill-factor for the table.
	 *
	 * XXX: Don't do this if the page was restored from full page image. We
	 * don't bother to update the FSM in that case, it doesn't need to be
	 * totally accurate anyway.
	 */
	if (action == BLK_NEEDS_REDO && freespace < BLCKSZ / 5)
		XLogRecordPageWithFreeSpace(rnode, blkno, freespace);
}

/*
 * Handles UPDATE and HOT_UPDATE
 */
static void
heap_xlog_update(XLogReaderState *record, bool hot_update)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_heap_update *xlrec = (xl_heap_update *) XLogRecGetData(record);
	RelFileNode rnode;
	BlockNumber oldblk;
	BlockNumber newblk;
	ItemPointerData newtid;
	Buffer		obuffer,
				nbuffer;
	Page		page;
	OffsetNumber offnum;
	ItemId		lp = NULL;
	HeapTupleData oldtup;
	HeapTupleHeader htup;
	uint16		prefixlen = 0,
				suffixlen = 0;
	char	   *newp;
	union
	{
		HeapTupleHeaderData hdr;
		char		data[MaxHeapTupleSize];
	}			tbuf;
	xl_heap_header xlhdr;
	uint32		newlen;
	Size		freespace = 0;
	XLogRedoAction oldaction;
	XLogRedoAction newaction;

	/* initialize to keep the compiler quiet */
	oldtup.t_data = NULL;
	oldtup.t_len = 0;

	XLogRecGetBlockTag(record, 0, &rnode, NULL, &newblk);
	if (XLogRecGetBlockTag(record, 1, NULL, NULL, &oldblk))
	{
		/* HOT updates are never done across pages */
		Assert(!hot_update);
	}
	else
		oldblk = newblk;

	ItemPointerSet(&newtid, newblk, xlrec->new_offnum);

	/*
	 * The visibility map may need to be fixed even if the heap page is
	 * already up-to-date.
	 */
	if (xlrec->flags & XLH_UPDATE_OLD_ALL_VISIBLE_CLEARED)
	{
		Relation	reln = CreateFakeRelcacheEntry(rnode);
		Buffer		vmbuffer = InvalidBuffer;

		visibilitymap_pin(reln, oldblk, &vmbuffer);
		visibilitymap_clear(reln, oldblk, vmbuffer);
		ReleaseBuffer(vmbuffer);
		FreeFakeRelcacheEntry(reln);
	}

	/*
	 * In normal operation, it is important to lock the two pages in
	 * page-number order, to avoid possible deadlocks against other update
	 * operations going the other way.  However, during WAL replay there can
	 * be no other update happening, so we don't need to worry about that. But
	 * we *do* need to worry that we don't expose an inconsistent state to Hot
	 * Standby queries --- so the original page can't be unlocked before we've
	 * added the new tuple to the new page.
	 */

	/* Deal with old tuple version */
	oldaction = XLogReadBufferForRedo(record, (oldblk == newblk) ? 0 : 1,
									  &obuffer);
	if (oldaction == BLK_NEEDS_REDO)
	{
		page = BufferGetPage(obuffer);
		offnum = xlrec->old_offnum;
		if (PageGetMaxOffsetNumber(page) >= offnum)
			lp = PageGetItemId(page, offnum);

		if (PageGetMaxOffsetNumber(page) < offnum || !ItemIdIsNormal(lp))
			elog(PANIC, "invalid lp");

		htup = (HeapTupleHeader) PageGetItem(page, lp);

		oldtup.t_data = htup;
		oldtup.t_len = ItemIdGetLength(lp);

		htup->t_infomask &= ~(HEAP_XMAX_BITS | HEAP_MOVED);
		htup->t_infomask2 &= ~HEAP_KEYS_UPDATED;
		if (hot_update)
			HeapTupleHeaderSetHotUpdated(htup);
		else
			HeapTupleHeaderClearHotUpdated(htup);
		fix_infomask_from_infobits(xlrec->old_infobits_set, &htup->t_infomask,
								   &htup->t_infomask2);
		HeapTupleHeaderSetXmax(htup, xlrec->old_xmax);
		HeapTupleHeaderSetCmax(htup, FirstCommandId, false);
		/* Set forward chain link in t_ctid */
		htup->t_ctid = newtid;

		/* Mark the page as a candidate for pruning */
		PageSetPrunable(page, XLogRecGetXid(record));

		if (xlrec->flags & XLH_UPDATE_OLD_ALL_VISIBLE_CLEARED)
			PageClearAllVisible(page);

		PageSetLSN(page, lsn);
		MarkBufferDirty(obuffer);
	}

	/*
	 * Read the page the new tuple goes into, if different from old.
	 */
	if (oldblk == newblk)
	{
		nbuffer = obuffer;
		newaction = oldaction;
	}
	else if (XLogRecGetInfo(record) & XLOG_HEAP_INIT_PAGE)
	{
		nbuffer = XLogInitBufferForRedo(record, 0);
		page = (Page) BufferGetPage(nbuffer);
		PageInit(page, BufferGetPageSize(nbuffer), 0);
		newaction = BLK_NEEDS_REDO;
	}
	else
		newaction = XLogReadBufferForRedo(record, 0, &nbuffer);

	/*
	 * The visibility map may need to be fixed even if the heap page is
	 * already up-to-date.
	 */
	if (xlrec->flags & XLH_UPDATE_NEW_ALL_VISIBLE_CLEARED)
	{
		Relation	reln = CreateFakeRelcacheEntry(rnode);
		Buffer		vmbuffer = InvalidBuffer;

		visibilitymap_pin(reln, newblk, &vmbuffer);
		visibilitymap_clear(reln, newblk, vmbuffer);
		ReleaseBuffer(vmbuffer);
		FreeFakeRelcacheEntry(reln);
	}

	/* Deal with new tuple */
	if (newaction == BLK_NEEDS_REDO)
	{
		char	   *recdata;
		char	   *recdata_end;
		Size		datalen;
		Size		tuplen;

		recdata = XLogRecGetBlockData(record, 0, &datalen);
		recdata_end = recdata + datalen;

		page = BufferGetPage(nbuffer);

		offnum = xlrec->new_offnum;
		if (PageGetMaxOffsetNumber(page) + 1 < offnum)
			elog(PANIC, "invalid max offset number");

		if (xlrec->flags & XLH_UPDATE_PREFIX_FROM_OLD)
		{
			Assert(newblk == oldblk);
			memcpy(&prefixlen, recdata, sizeof(uint16));
			recdata += sizeof(uint16);
		}
		if (xlrec->flags & XLH_UPDATE_SUFFIX_FROM_OLD)
		{
			Assert(newblk == oldblk);
			memcpy(&suffixlen, recdata, sizeof(uint16));
			recdata += sizeof(uint16);
		}

		memcpy((char *) &xlhdr, recdata, SizeOfHeapHeader);
		recdata += SizeOfHeapHeader;

		tuplen = recdata_end - recdata;
		Assert(tuplen <= MaxHeapTupleSize);

		htup = &tbuf.hdr;
		MemSet((char *) htup, 0, SizeofHeapTupleHeader);

		/*
		 * Reconstruct the new tuple using the prefix and/or suffix from the
		 * old tuple, and the data stored in the WAL record.
		 */
		newp = (char *) htup + SizeofHeapTupleHeader;
		if (prefixlen > 0)
		{
			int			len;

			/* copy bitmap [+ padding] [+ oid] from WAL record */
			len = xlhdr.t_hoff - SizeofHeapTupleHeader;
			memcpy(newp, recdata, len);
			recdata += len;
			newp += len;

			/* copy prefix from old tuple */
			memcpy(newp, (char *) oldtup.t_data + oldtup.t_data->t_hoff, prefixlen);
			newp += prefixlen;

			/* copy new tuple data from WAL record */
			len = tuplen - (xlhdr.t_hoff - SizeofHeapTupleHeader);
			memcpy(newp, recdata, len);
			recdata += len;
			newp += len;
		}
		else
		{
			/*
			 * copy bitmap [+ padding] [+ oid] + data from record, all in one
			 * go
			 */
			memcpy(newp, recdata, tuplen);
			recdata += tuplen;
			newp += tuplen;
		}
		Assert(recdata == recdata_end);

		/* copy suffix from old tuple */
		if (suffixlen > 0)
			memcpy(newp, (char *) oldtup.t_data + oldtup.t_len - suffixlen, suffixlen);

		newlen = SizeofHeapTupleHeader + tuplen + prefixlen + suffixlen;
		htup->t_infomask2 = xlhdr.t_infomask2;
		htup->t_infomask = xlhdr.t_infomask;
		htup->t_hoff = xlhdr.t_hoff;

		HeapTupleHeaderSetXmin(htup, XLogRecGetXid(record));
		HeapTupleHeaderSetCmin(htup, FirstCommandId);
		HeapTupleHeaderSetXmax(htup, xlrec->new_xmax);
		/* Make sure there is no forward chain link in t_ctid */
		htup->t_ctid = newtid;

		offnum = PageAddItem(page, (Item) htup, newlen, offnum, true, true);
		if (offnum == InvalidOffsetNumber)
			elog(PANIC, "failed to add tuple");

		if (xlrec->flags & XLH_UPDATE_NEW_ALL_VISIBLE_CLEARED)
			PageClearAllVisible(page);

		freespace = PageGetHeapFreeSpace(page); /* needed to update FSM below */

		PageSetLSN(page, lsn);
		MarkBufferDirty(nbuffer);
	}

	if (BufferIsValid(nbuffer) && nbuffer != obuffer)
		UnlockReleaseBuffer(nbuffer);
	if (BufferIsValid(obuffer))
		UnlockReleaseBuffer(obuffer);

	/*
	 * If the new page is running low on free space, update the FSM as well.
	 * Arbitrarily, our definition of "low" is less than 20%. We can't do much
	 * better than that without knowing the fill-factor for the table.
	 *
	 * However, don't update the FSM on HOT updates, because after crash
	 * recovery, either the old or the new tuple will certainly be dead and
	 * prunable. After pruning, the page will have roughly as much free space
	 * as it did before the update, assuming the new tuple is about the same
	 * size as the old one.
	 *
	 * XXX: Don't do this if the page was restored from full page image. We
	 * don't bother to update the FSM in that case, it doesn't need to be
	 * totally accurate anyway.
	 */
	if (newaction == BLK_NEEDS_REDO && !hot_update && freespace < BLCKSZ / 5)
		XLogRecordPageWithFreeSpace(rnode, newblk, freespace);
}

static void
heap_xlog_confirm(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_heap_confirm *xlrec = (xl_heap_confirm *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;
	OffsetNumber offnum;
	ItemId		lp = NULL;
	HeapTupleHeader htup;

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		page = BufferGetPage(buffer);

		offnum = xlrec->offnum;
		if (PageGetMaxOffsetNumber(page) >= offnum)
			lp = PageGetItemId(page, offnum);

		if (PageGetMaxOffsetNumber(page) < offnum || !ItemIdIsNormal(lp))
			elog(PANIC, "invalid lp");

		htup = (HeapTupleHeader) PageGetItem(page, lp);

		/*
		 * Confirm tuple as actually inserted
		 */
		ItemPointerSet(&htup->t_ctid, BufferGetBlockNumber(buffer), offnum);

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

static void
heap_xlog_lock(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_heap_lock *xlrec = (xl_heap_lock *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;
	OffsetNumber offnum;
	ItemId		lp = NULL;
	HeapTupleHeader htup;

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		page = (Page) BufferGetPage(buffer);

		offnum = xlrec->offnum;
		if (PageGetMaxOffsetNumber(page) >= offnum)
			lp = PageGetItemId(page, offnum);

		if (PageGetMaxOffsetNumber(page) < offnum || !ItemIdIsNormal(lp))
			elog(PANIC, "invalid lp");

		htup = (HeapTupleHeader) PageGetItem(page, lp);

		fix_infomask_from_infobits(xlrec->infobits_set, &htup->t_infomask,
								   &htup->t_infomask2);

		/*
		 * Clear relevant update flags, but only if the modified infomask says
		 * there's no update.
		 */
		if (HEAP_XMAX_IS_LOCKED_ONLY(htup->t_infomask))
		{
			HeapTupleHeaderClearHotUpdated(htup);
			/* Make sure there is no forward chain link in t_ctid */
			ItemPointerSet(&htup->t_ctid,
						   BufferGetBlockNumber(buffer),
						   offnum);
		}
		HeapTupleHeaderSetXmax(htup, xlrec->locking_xid);
		HeapTupleHeaderSetCmax(htup, FirstCommandId, false);
		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

static void
heap_xlog_lock_updated(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_heap_lock_updated *xlrec;
	Buffer		buffer;
	Page		page;
	OffsetNumber offnum;
	ItemId		lp = NULL;
	HeapTupleHeader htup;

	xlrec = (xl_heap_lock_updated *) XLogRecGetData(record);

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		page = BufferGetPage(buffer);

		offnum = xlrec->offnum;
		if (PageGetMaxOffsetNumber(page) >= offnum)
			lp = PageGetItemId(page, offnum);

		if (PageGetMaxOffsetNumber(page) < offnum || !ItemIdIsNormal(lp))
			elog(PANIC, "invalid lp");

		htup = (HeapTupleHeader) PageGetItem(page, lp);

		fix_infomask_from_infobits(xlrec->infobits_set, &htup->t_infomask,
								   &htup->t_infomask2);
		HeapTupleHeaderSetXmax(htup, xlrec->xmax);

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

static void
heap_xlog_inplace(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_heap_inplace *xlrec = (xl_heap_inplace *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;
	OffsetNumber offnum;
	ItemId		lp = NULL;
	HeapTupleHeader htup;
	uint32		oldlen;
	Size		newlen;

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		char	   *newtup = XLogRecGetBlockData(record, 0, &newlen);

		page = BufferGetPage(buffer);

		offnum = xlrec->offnum;
		if (PageGetMaxOffsetNumber(page) >= offnum)
			lp = PageGetItemId(page, offnum);

		if (PageGetMaxOffsetNumber(page) < offnum || !ItemIdIsNormal(lp))
			elog(PANIC, "invalid lp");

		htup = (HeapTupleHeader) PageGetItem(page, lp);

		oldlen = ItemIdGetLength(lp) - htup->t_hoff;
		if (oldlen != newlen)
			elog(PANIC, "wrong tuple length");

		memcpy((char *) htup + htup->t_hoff, newtup, newlen);

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

void
heap_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	/*
	 * These operations don't overwrite MVCC data so no conflict processing is
	 * required. The ones in heap2 rmgr do.
	 */

	switch (info & XLOG_HEAP_OPMASK)
	{
		case XLOG_HEAP_INSERT:
			heap_xlog_insert(record);
			break;
		case XLOG_HEAP_DELETE:
			heap_xlog_delete(record);
			break;
		case XLOG_HEAP_UPDATE:
			heap_xlog_update(record, false);
			break;
		case XLOG_HEAP_HOT_UPDATE:
			heap_xlog_update(record, true);
			break;
		case XLOG_HEAP_CONFIRM:
			heap_xlog_confirm(record);
			break;
		case XLOG_HEAP_LOCK:
			heap_xlog_lock(record);
			break;
		case XLOG_HEAP_INPLACE:
			heap_xlog_inplace(record);
			break;
		default:
			elog(PANIC, "heap_redo: unknown op code %u", info);
	}
}

void
heap2_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info & XLOG_HEAP_OPMASK)
	{
		case XLOG_HEAP2_CLEAN:
			heap_xlog_clean(record);
			break;
		case XLOG_HEAP2_FREEZE_PAGE:
			heap_xlog_freeze_page(record);
			break;
		case XLOG_HEAP2_CLEANUP_INFO:
			heap_xlog_cleanup_info(record);
			break;
		case XLOG_HEAP2_VISIBLE:
			heap_xlog_visible(record);
			break;
		case XLOG_HEAP2_MULTI_INSERT:
			heap_xlog_multi_insert(record);
			break;
		case XLOG_HEAP2_LOCK_UPDATED:
			heap_xlog_lock_updated(record);
			break;
		case XLOG_HEAP2_NEW_CID:

			/*
			 * Nothing to do on a real replay, only used during logical
			 * decoding.
			 */
			break;
		case XLOG_HEAP2_REWRITE:
			heap_xlog_logical_rewrite(record);
			break;
		default:
			elog(PANIC, "heap2_redo: unknown op code %u", info);
	}
}

/*
 *	heap_sync		- sync a heap, for use when no WAL has been written
 *
 * This forces the heap contents (including TOAST heap if any) down to disk.
 * If we skipped using WAL, and WAL is otherwise needed, we must force the
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
	/* non-WAL-logged tables never need fsync */
	if (!RelationNeedsWAL(rel))
		return;

	/* main heap */
	FlushRelationBuffers(rel);
	/* FlushRelationBuffers will have opened rd_smgr */
	smgrimmedsync(rel->rd_smgr, MAIN_FORKNUM);

	/* FSM is not critical, don't bother syncing it */

	/* toast heap, if any */
	if (OidIsValid(rel->rd_rel->reltoastrelid))
	{
		Relation	toastrel;

		toastrel = heap_open(rel->rd_rel->reltoastrelid, AccessShareLock);
		FlushRelationBuffers(toastrel);
		smgrimmedsync(toastrel->rd_smgr, MAIN_FORKNUM);
		heap_close(toastrel, AccessShareLock);
	}
}
