/*-------------------------------------------------------------------------
 *
 * nbtree.c
 *	  Implementation of Lehman and Yao's btree management algorithm for
 *	  Postgres.
 *
 * NOTES
 *	  This file contains only the public interface routines.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/nbtree/nbtree.c,v 1.153.2.1 2008/04/17 00:00:00 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/nbtree.h"
#include "catalog/index.h"
#include "commands/vacuum.h"
#include "storage/freespace.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "utils/memutils.h"


/* Working state for btbuild and its callback */
typedef struct
{
	bool		isUnique;
	bool		haveDead;
	Relation	heapRel;
	BTSpool    *spool;

	/*
	 * spool2 is needed only when the index is an unique index. Dead tuples
	 * are put into spool2 instead of spool in order to avoid uniqueness
	 * check.
	 */
	BTSpool    *spool2;
	double		indtuples;
} BTBuildState;

/* Working state needed by btvacuumpage */
typedef struct
{
	IndexVacuumInfo *info;
	IndexBulkDeleteResult *stats;
	IndexBulkDeleteCallback callback;
	void	   *callback_state;
	BTCycleId	cycleid;
	BlockNumber *freePages;
	int			nFreePages;		/* number of entries in freePages[] */
	int			maxFreePages;	/* allocated size of freePages[] */
	BlockNumber totFreePages;	/* true total # of free pages */
	MemoryContext pagedelcontext;
} BTVacState;


static void btbuildCallback(Relation index,
				HeapTuple htup,
				Datum *values,
				bool *isnull,
				bool tupleIsAlive,
				void *state);
static void btvacuumscan(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
			 IndexBulkDeleteCallback callback, void *callback_state,
			 BTCycleId cycleid);
static void btvacuumpage(BTVacState *vstate, BlockNumber blkno,
			 BlockNumber orig_blkno);


/*
 *	btbuild() -- build a new btree index.
 */
Datum
btbuild(PG_FUNCTION_ARGS)
{
	Relation	heap = (Relation) PG_GETARG_POINTER(0);
	Relation	index = (Relation) PG_GETARG_POINTER(1);
	IndexInfo  *indexInfo = (IndexInfo *) PG_GETARG_POINTER(2);
	IndexBuildResult *result;
	double		reltuples;
	BTBuildState buildstate;

	buildstate.isUnique = indexInfo->ii_Unique;
	buildstate.haveDead = false;
	buildstate.heapRel = heap;
	buildstate.spool = NULL;
	buildstate.spool2 = NULL;
	buildstate.indtuples = 0;

#ifdef BTREE_BUILD_STATS
	if (log_btree_build_stats)
		ResetUsage();
#endif   /* BTREE_BUILD_STATS */

	/*
	 * We expect to be called exactly once for any index relation. If that's
	 * not the case, big trouble's what we have.
	 */
	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index \"%s\" already contains data",
			 RelationGetRelationName(index));

	buildstate.spool = _bt_spoolinit(index, indexInfo->ii_Unique, false);

	/*
	 * If building a unique index, put dead tuples in a second spool to keep
	 * them out of the uniqueness check.
	 */
	if (indexInfo->ii_Unique)
		buildstate.spool2 = _bt_spoolinit(index, false, true);

	/* do the heap scan */
	reltuples = IndexBuildHeapScan(heap, index, indexInfo,
								   btbuildCallback, (void *) &buildstate);

	/* okay, all heap tuples are indexed */
	if (buildstate.spool2 && !buildstate.haveDead)
	{
		/* spool2 turns out to be unnecessary */
		_bt_spooldestroy(buildstate.spool2);
		buildstate.spool2 = NULL;
	}

	/*
	 * Finish the build by (1) completing the sort of the spool file, (2)
	 * inserting the sorted tuples into btree pages and (3) building the upper
	 * levels.
	 */
	_bt_leafbuild(buildstate.spool, buildstate.spool2);
	_bt_spooldestroy(buildstate.spool);
	if (buildstate.spool2)
		_bt_spooldestroy(buildstate.spool2);

#ifdef BTREE_BUILD_STATS
	if (log_btree_build_stats)
	{
		ShowUsage("BTREE BUILD STATS");
		ResetUsage();
	}
#endif   /* BTREE_BUILD_STATS */

	/*
	 * If we are reindexing a pre-existing index, it is critical to send out a
	 * relcache invalidation SI message to ensure all backends re-read the
	 * index metapage.	We expect that the caller will ensure that happens
	 * (typically as a side effect of updating index stats, but it must happen
	 * even if the stats don't change!)
	 */

	/*
	 * Return statistics
	 */
	result = (IndexBuildResult *) palloc(sizeof(IndexBuildResult));

	result->heap_tuples = reltuples;
	result->index_tuples = buildstate.indtuples;

	PG_RETURN_POINTER(result);
}

/*
 * Per-tuple callback from IndexBuildHeapScan
 */
static void
btbuildCallback(Relation index,
				HeapTuple htup,
				Datum *values,
				bool *isnull,
				bool tupleIsAlive,
				void *state)
{
	BTBuildState *buildstate = (BTBuildState *) state;
	IndexTuple	itup;

	/* form an index tuple and point it at the heap tuple */
	itup = index_form_tuple(RelationGetDescr(index), values, isnull);
	itup->t_tid = htup->t_self;

	/*
	 * insert the index tuple into the appropriate spool file for subsequent
	 * processing
	 */
	if (tupleIsAlive || buildstate->spool2 == NULL)
		_bt_spool(itup, buildstate->spool);
	else
	{
		/* dead tuples are put into spool2 */
		buildstate->haveDead = true;
		_bt_spool(itup, buildstate->spool2);
	}

	buildstate->indtuples += 1;

	pfree(itup);
}

/*
 *	btinsert() -- insert an index tuple into a btree.
 *
 *		Descend the tree recursively, find the appropriate location for our
 *		new tuple, and put it there.
 */
Datum
btinsert(PG_FUNCTION_ARGS)
{
	Relation	rel = (Relation) PG_GETARG_POINTER(0);
	Datum	   *values = (Datum *) PG_GETARG_POINTER(1);
	bool	   *isnull = (bool *) PG_GETARG_POINTER(2);
	ItemPointer ht_ctid = (ItemPointer) PG_GETARG_POINTER(3);
	Relation	heapRel = (Relation) PG_GETARG_POINTER(4);
	bool		checkUnique = PG_GETARG_BOOL(5);
	IndexTuple	itup;

	/* generate an index tuple */
	itup = index_form_tuple(RelationGetDescr(rel), values, isnull);
	itup->t_tid = *ht_ctid;

	_bt_doinsert(rel, itup, checkUnique, heapRel);

	pfree(itup);

	PG_RETURN_BOOL(true);
}

/*
 *	btgettuple() -- Get the next tuple in the scan.
 */
Datum
btgettuple(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ScanDirection dir = (ScanDirection) PG_GETARG_INT32(1);
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	bool		res;

	/*
	 * If we've already initialized this scan, we can just advance it in the
	 * appropriate direction.  If we haven't done so yet, we call a routine to
	 * get the first item in the scan.
	 */
	if (BTScanPosIsValid(so->currPos))
	{
		/*
		 * Check to see if we should kill the previously-fetched tuple.
		 */
		if (scan->kill_prior_tuple)
		{
			/*
			 * Yes, remember it for later.	(We'll deal with all such tuples
			 * at once right before leaving the index page.)  The test for
			 * numKilled overrun is not just paranoia: if the caller reverses
			 * direction in the indexscan then the same item might get entered
			 * multiple times.	It's not worth trying to optimize that, so we
			 * don't detect it, but instead just forget any excess entries.
			 */
			if (so->killedItems == NULL)
				so->killedItems = (int *)
					palloc(MaxIndexTuplesPerPage * sizeof(int));
			if (so->numKilled < MaxIndexTuplesPerPage)
				so->killedItems[so->numKilled++] = so->currPos.itemIndex;
		}

		/*
		 * Now continue the scan.
		 */
		res = _bt_next(scan, dir);
	}
	else
		res = _bt_first(scan, dir);

	PG_RETURN_BOOL(res);
}

/*
 * btgetmulti() -- get multiple tuples at once
 *
 * In the current implementation there seems no strong reason to stop at
 * index page boundaries; we just press on until we fill the caller's buffer
 * or run out of matches.
 */
Datum
btgetmulti(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ItemPointer tids = (ItemPointer) PG_GETARG_POINTER(1);
	int32		max_tids = PG_GETARG_INT32(2);
	int32	   *returned_tids = (int32 *) PG_GETARG_POINTER(3);
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	bool		res = true;
	int32		ntids = 0;

	if (max_tids <= 0)			/* behave correctly in boundary case */
		PG_RETURN_BOOL(true);

	/* If we haven't started the scan yet, fetch the first page & tuple. */
	if (!BTScanPosIsValid(so->currPos))
	{
		res = _bt_first(scan, ForwardScanDirection);
		if (!res)
		{
			/* empty scan */
			*returned_tids = ntids;
			PG_RETURN_BOOL(res);
		}
		/* Save tuple ID, and continue scanning */
		tids[ntids] = scan->xs_ctup.t_self;
		ntids++;
	}

	while (ntids < max_tids)
	{
		/*
		 * Advance to next tuple within page.  This is the same as the easy
		 * case in _bt_next().
		 */
		if (++so->currPos.itemIndex > so->currPos.lastItem)
		{
			/* let _bt_next do the heavy lifting */
			res = _bt_next(scan, ForwardScanDirection);
			if (!res)
				break;
		}

		/* Save tuple ID, and continue scanning */
		tids[ntids] = so->currPos.items[so->currPos.itemIndex].heapTid;
		ntids++;
	}

	*returned_tids = ntids;
	PG_RETURN_BOOL(res);
}

/*
 *	btbeginscan() -- start a scan on a btree index
 */
Datum
btbeginscan(PG_FUNCTION_ARGS)
{
	Relation	rel = (Relation) PG_GETARG_POINTER(0);
	int			keysz = PG_GETARG_INT32(1);
	ScanKey		scankey = (ScanKey) PG_GETARG_POINTER(2);
	IndexScanDesc scan;

	/* get the scan */
	scan = RelationGetIndexScan(rel, keysz, scankey);

	PG_RETURN_POINTER(scan);
}

/*
 *	btrescan() -- rescan an index relation
 */
Datum
btrescan(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ScanKey		scankey = (ScanKey) PG_GETARG_POINTER(1);
	BTScanOpaque so;

	so = (BTScanOpaque) scan->opaque;

	if (so == NULL)				/* if called from btbeginscan */
	{
		so = (BTScanOpaque) palloc(sizeof(BTScanOpaqueData));
		so->currPos.buf = so->markPos.buf = InvalidBuffer;
		if (scan->numberOfKeys > 0)
			so->keyData = (ScanKey) palloc(scan->numberOfKeys * sizeof(ScanKeyData));
		else
			so->keyData = NULL;
		so->killedItems = NULL; /* until needed */
		so->numKilled = 0;
		scan->opaque = so;
	}

	/* we aren't holding any read locks, but gotta drop the pins */
	if (BTScanPosIsValid(so->currPos))
	{
		/* Before leaving current page, deal with any killed items */
		if (so->numKilled > 0)
			_bt_killitems(scan, false);
		ReleaseBuffer(so->currPos.buf);
		so->currPos.buf = InvalidBuffer;
	}

	if (BTScanPosIsValid(so->markPos))
	{
		ReleaseBuffer(so->markPos.buf);
		so->markPos.buf = InvalidBuffer;
	}
	so->markItemIndex = -1;

	/*
	 * Reset the scan keys. Note that keys ordering stuff moved to _bt_first.
	 * - vadim 05/05/97
	 */
	if (scankey && scan->numberOfKeys > 0)
		memmove(scan->keyData,
				scankey,
				scan->numberOfKeys * sizeof(ScanKeyData));
	so->numberOfKeys = 0;		/* until _bt_preprocess_keys sets it */

	PG_RETURN_VOID();
}

/*
 *	btendscan() -- close down a scan
 */
Datum
btendscan(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	BTScanOpaque so = (BTScanOpaque) scan->opaque;

	/* we aren't holding any read locks, but gotta drop the pins */
	if (BTScanPosIsValid(so->currPos))
	{
		/* Before leaving current page, deal with any killed items */
		if (so->numKilled > 0)
			_bt_killitems(scan, false);
		ReleaseBuffer(so->currPos.buf);
		so->currPos.buf = InvalidBuffer;
	}

	if (BTScanPosIsValid(so->markPos))
	{
		ReleaseBuffer(so->markPos.buf);
		so->markPos.buf = InvalidBuffer;
	}
	so->markItemIndex = -1;

	if (so->killedItems != NULL)
		pfree(so->killedItems);
	if (so->keyData != NULL)
		pfree(so->keyData);
	pfree(so);

	PG_RETURN_VOID();
}

/*
 *	btmarkpos() -- save current scan position
 */
Datum
btmarkpos(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	BTScanOpaque so = (BTScanOpaque) scan->opaque;

	/* we aren't holding any read locks, but gotta drop the pin */
	if (BTScanPosIsValid(so->markPos))
	{
		ReleaseBuffer(so->markPos.buf);
		so->markPos.buf = InvalidBuffer;
	}

	/*
	 * Just record the current itemIndex.  If we later step to next page
	 * before releasing the marked position, _bt_steppage makes a full copy of
	 * the currPos struct in markPos.  If (as often happens) the mark is moved
	 * before we leave the page, we don't have to do that work.
	 */
	if (BTScanPosIsValid(so->currPos))
		so->markItemIndex = so->currPos.itemIndex;
	else
		so->markItemIndex = -1;

	PG_RETURN_VOID();
}

/*
 *	btrestrpos() -- restore scan to last saved position
 */
Datum
btrestrpos(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	BTScanOpaque so = (BTScanOpaque) scan->opaque;

	if (so->markItemIndex >= 0)
	{
		/*
		 * The mark position is on the same page we are currently on. Just
		 * restore the itemIndex.
		 */
		so->currPos.itemIndex = so->markItemIndex;
	}
	else
	{
		/* we aren't holding any read locks, but gotta drop the pin */
		if (BTScanPosIsValid(so->currPos))
		{
			/* Before leaving current page, deal with any killed items */
			if (so->numKilled > 0 &&
				so->currPos.buf != so->markPos.buf)
				_bt_killitems(scan, false);
			ReleaseBuffer(so->currPos.buf);
			so->currPos.buf = InvalidBuffer;
		}

		if (BTScanPosIsValid(so->markPos))
		{
			/* bump pin on mark buffer for assignment to current buffer */
			IncrBufferRefCount(so->markPos.buf);
			memcpy(&so->currPos, &so->markPos,
				   offsetof(BTScanPosData, items[1]) +
				   so->markPos.lastItem * sizeof(BTScanPosItem));
		}
	}

	PG_RETURN_VOID();
}

/*
 * Bulk deletion of all index entries pointing to a set of heap tuples.
 * The set of target tuples is specified via a callback routine that tells
 * whether any given heap tuple (identified by ItemPointer) is being deleted.
 *
 * Result: a palloc'd struct containing statistical info for VACUUM displays.
 */
Datum
btbulkdelete(PG_FUNCTION_ARGS)
{
	IndexVacuumInfo *info = (IndexVacuumInfo *) PG_GETARG_POINTER(0);
	IndexBulkDeleteResult *volatile stats = (IndexBulkDeleteResult *) PG_GETARG_POINTER(1);
	IndexBulkDeleteCallback callback = (IndexBulkDeleteCallback) PG_GETARG_POINTER(2);
	void	   *callback_state = (void *) PG_GETARG_POINTER(3);
	Relation	rel = info->index;
	BTCycleId	cycleid;

	/* allocate stats if first time through, else re-use existing struct */
	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

	/* Establish the vacuum cycle ID to use for this scan */
	/* The ENSURE stuff ensures we clean up shared memory on failure */
	PG_ENSURE_ERROR_CLEANUP(_bt_end_vacuum_callback, PointerGetDatum(rel));
	{
		cycleid = _bt_start_vacuum(rel);

		btvacuumscan(info, stats, callback, callback_state, cycleid);
	}
	PG_END_ENSURE_ERROR_CLEANUP(_bt_end_vacuum_callback, PointerGetDatum(rel));
	_bt_end_vacuum(rel);

	PG_RETURN_POINTER(stats);
}

/*
 * Post-VACUUM cleanup.
 *
 * Result: a palloc'd struct containing statistical info for VACUUM displays.
 */
Datum
btvacuumcleanup(PG_FUNCTION_ARGS)
{
	IndexVacuumInfo *info = (IndexVacuumInfo *) PG_GETARG_POINTER(0);
	IndexBulkDeleteResult *stats = (IndexBulkDeleteResult *) PG_GETARG_POINTER(1);

	/*
	 * If btbulkdelete was called, we need not do anything, just return the
	 * stats from the latest btbulkdelete call.  If it wasn't called, we must
	 * still do a pass over the index, to recycle any newly-recyclable pages
	 * and to obtain index statistics.
	 *
	 * Since we aren't going to actually delete any leaf items, there's no
	 * need to go through all the vacuum-cycle-ID pushups.
	 */
	if (stats == NULL)
	{
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
		btvacuumscan(info, stats, NULL, NULL, 0);
	}

	/*
	 * During a non-FULL vacuum it's quite possible for us to be fooled by
	 * concurrent page splits into double-counting some index tuples, so
	 * disbelieve any total that exceeds the underlying heap's count. (We
	 * can't check this during btbulkdelete.)
	 */
	if (!info->vacuum_full)
	{
		if (stats->num_index_tuples > info->num_heap_tuples)
			stats->num_index_tuples = info->num_heap_tuples;
	}

	PG_RETURN_POINTER(stats);
}

/*
 * btvacuumscan --- scan the index for VACUUMing purposes
 *
 * This combines the functions of looking for leaf tuples that are deletable
 * according to the vacuum callback, looking for empty pages that can be
 * deleted, and looking for old deleted pages that can be recycled.  Both
 * btbulkdelete and btvacuumcleanup invoke this (the latter only if no
 * btbulkdelete call occurred).
 *
 * The caller is responsible for initially allocating/zeroing a stats struct
 * and for obtaining a vacuum cycle ID if necessary.
 */
static void
btvacuumscan(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
			 IndexBulkDeleteCallback callback, void *callback_state,
			 BTCycleId cycleid)
{
	Relation	rel = info->index;
	BTVacState	vstate;
	BlockNumber num_pages;
	BlockNumber blkno;
	bool		needLock;

	/*
	 * Reset counts that will be incremented during the scan; needed in case
	 * of multiple scans during a single VACUUM command
	 */
	stats->num_index_tuples = 0;
	stats->pages_deleted = 0;

	/* Set up info to pass down to btvacuumpage */
	vstate.info = info;
	vstate.stats = stats;
	vstate.callback = callback;
	vstate.callback_state = callback_state;
	vstate.cycleid = cycleid;
	vstate.freePages = NULL;	/* temporarily */
	vstate.nFreePages = 0;
	vstate.maxFreePages = 0;
	vstate.totFreePages = 0;

	/* Create a temporary memory context to run _bt_pagedel in */
	vstate.pagedelcontext = AllocSetContextCreate(CurrentMemoryContext,
												  "_bt_pagedel",
												  ALLOCSET_DEFAULT_MINSIZE,
												  ALLOCSET_DEFAULT_INITSIZE,
												  ALLOCSET_DEFAULT_MAXSIZE);

	/*
	 * The outer loop iterates over all index pages except the metapage, in
	 * physical order (we hope the kernel will cooperate in providing
	 * read-ahead for speed).  It is critical that we visit all leaf pages,
	 * including ones added after we start the scan, else we might fail to
	 * delete some deletable tuples.  Hence, we must repeatedly check the
	 * relation length.  We must acquire the relation-extension lock while
	 * doing so to avoid a race condition: if someone else is extending the
	 * relation, there is a window where bufmgr/smgr have created a new
	 * all-zero page but it hasn't yet been write-locked by _bt_getbuf(). If
	 * we manage to scan such a page here, we'll improperly assume it can be
	 * recycled.  Taking the lock synchronizes things enough to prevent a
	 * problem: either num_pages won't include the new page, or _bt_getbuf
	 * already has write lock on the buffer and it will be fully initialized
	 * before we can examine it.  (See also vacuumlazy.c, which has the same
	 * issue.)	Also, we need not worry if a page is added immediately after
	 * we look; the page splitting code already has write-lock on the left
	 * page before it adds a right page, so we must already have processed any
	 * tuples due to be moved into such a page.
	 *
	 * We can skip locking for new or temp relations, however, since no one
	 * else could be accessing them.
	 */
	needLock = !RELATION_IS_LOCAL(rel);

	blkno = BTREE_METAPAGE + 1;
	for (;;)
	{
		/* Get the current relation length */
		if (needLock)
			LockRelationForExtension(rel, ExclusiveLock);
		num_pages = RelationGetNumberOfBlocks(rel);
		if (needLock)
			UnlockRelationForExtension(rel, ExclusiveLock);

		/* Allocate freePages after we read num_pages the first time */
		if (vstate.freePages == NULL)
		{
			/* No point in remembering more than MaxFSMPages pages */
			vstate.maxFreePages = MaxFSMPages;
			if ((BlockNumber) vstate.maxFreePages > num_pages)
				vstate.maxFreePages = (int) num_pages;
			vstate.freePages = (BlockNumber *)
				palloc(vstate.maxFreePages * sizeof(BlockNumber));
		}

		/* Quit if we've scanned the whole relation */
		if (blkno >= num_pages)
			break;
		/* Iterate over pages, then loop back to recheck length */
		for (; blkno < num_pages; blkno++)
		{
			btvacuumpage(&vstate, blkno, blkno);
		}
	}

	/*
	 * During VACUUM FULL, we truncate off any recyclable pages at the end of
	 * the index.  In a normal vacuum it'd be unsafe to do this except by
	 * acquiring exclusive lock on the index and then rechecking all the
	 * pages; doesn't seem worth it.
	 */
	if (info->vacuum_full && vstate.nFreePages > 0)
	{
		BlockNumber new_pages = num_pages;

		while (vstate.nFreePages > 0 &&
			   vstate.freePages[vstate.nFreePages - 1] == new_pages - 1)
		{
			new_pages--;
			stats->pages_deleted--;
			vstate.nFreePages--;
			vstate.totFreePages = vstate.nFreePages;	/* can't be more */
		}
		if (new_pages != num_pages)
		{
			/*
			 * Okay to truncate.
			 */
			RelationTruncate(rel, new_pages);

			/* update statistics */
			stats->pages_removed += num_pages - new_pages;

			num_pages = new_pages;
		}
	}

	/*
	 * Update the shared Free Space Map with the info we now have about free
	 * pages in the index, discarding any old info the map may have. We do not
	 * need to sort the page numbers; they're in order already.
	 */
	RecordIndexFreeSpace(&rel->rd_node, vstate.totFreePages,
						 vstate.nFreePages, vstate.freePages);

	pfree(vstate.freePages);

	MemoryContextDelete(vstate.pagedelcontext);

	/* update statistics */
	stats->num_pages = num_pages;
	stats->pages_free = vstate.totFreePages;
}

/*
 * btvacuumpage --- VACUUM one page
 *
 * This processes a single page for btvacuumscan().  In some cases we
 * must go back and re-examine previously-scanned pages; this routine
 * recurses when necessary to handle that case.
 *
 * blkno is the page to process.  orig_blkno is the highest block number
 * reached by the outer btvacuumscan loop (the same as blkno, unless we
 * are recursing to re-examine a previous page).
 */
static void
btvacuumpage(BTVacState *vstate, BlockNumber blkno, BlockNumber orig_blkno)
{
	IndexVacuumInfo *info = vstate->info;
	IndexBulkDeleteResult *stats = vstate->stats;
	IndexBulkDeleteCallback callback = vstate->callback;
	void	   *callback_state = vstate->callback_state;
	Relation	rel = info->index;
	bool		delete_now;
	BlockNumber recurse_to;
	Buffer		buf;
	Page		page;
	BTPageOpaque opaque;

restart:
	delete_now = false;
	recurse_to = P_NONE;

	/* call vacuum_delay_point while not holding any buffer lock */
	vacuum_delay_point();

	/*
	 * We can't use _bt_getbuf() here because it always applies
	 * _bt_checkpage(), which will barf on an all-zero page. We want to
	 * recycle all-zero pages, not fail.
	 */
	buf = ReadBuffer(rel, blkno);
	LockBuffer(buf, BT_READ);
	page = BufferGetPage(buf);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	if (!PageIsNew(page))
		_bt_checkpage(rel, buf);

	/*
	 * If we are recursing, the only case we want to do anything with is a
	 * live leaf page having the current vacuum cycle ID.  Any other state
	 * implies we already saw the page (eg, deleted it as being empty). In
	 * particular, we don't want to risk adding it to freePages twice.
	 */
	if (blkno != orig_blkno)
	{
		if (_bt_page_recyclable(page) ||
			P_IGNORE(opaque) ||
			!P_ISLEAF(opaque) ||
			opaque->btpo_cycleid != vstate->cycleid)
		{
			_bt_relbuf(rel, buf);
			return;
		}
	}

	/* Page is valid, see what to do with it */
	if (_bt_page_recyclable(page))
	{
		/* Okay to recycle this page */
		if (vstate->nFreePages < vstate->maxFreePages)
			vstate->freePages[vstate->nFreePages++] = blkno;
		vstate->totFreePages++;
		stats->pages_deleted++;
	}
	else if (P_ISDELETED(opaque))
	{
		/* Already deleted, but can't recycle yet */
		stats->pages_deleted++;
	}
	else if (P_ISHALFDEAD(opaque))
	{
		/* Half-dead, try to delete */
		delete_now = true;
	}
	else if (P_ISLEAF(opaque))
	{
		OffsetNumber deletable[MaxOffsetNumber];
		int			ndeletable;
		OffsetNumber offnum,
					minoff,
					maxoff;

		/*
		 * Trade in the initial read lock for a super-exclusive write lock on
		 * this page.  We must get such a lock on every leaf page over the
		 * course of the vacuum scan, whether or not it actually contains any
		 * deletable tuples --- see nbtree/README.
		 */
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		LockBufferForCleanup(buf);

		/*
		 * Check whether we need to recurse back to earlier pages.	What we
		 * are concerned about is a page split that happened since we started
		 * the vacuum scan.  If the split moved some tuples to a lower page
		 * then we might have missed 'em.  If so, set up for tail recursion.
		 * (Must do this before possibly clearing btpo_cycleid below!)
		 */
		if (vstate->cycleid != 0 &&
			opaque->btpo_cycleid == vstate->cycleid &&
			!(opaque->btpo_flags & BTP_SPLIT_END) &&
			!P_RIGHTMOST(opaque) &&
			opaque->btpo_next < orig_blkno)
			recurse_to = opaque->btpo_next;

		/*
		 * Scan over all items to see which ones need deleted according to the
		 * callback function.
		 */
		ndeletable = 0;
		minoff = P_FIRSTDATAKEY(opaque);
		maxoff = PageGetMaxOffsetNumber(page);
		if (callback)
		{
			for (offnum = minoff;
				 offnum <= maxoff;
				 offnum = OffsetNumberNext(offnum))
			{
				IndexTuple	itup;
				ItemPointer htup;

				itup = (IndexTuple) PageGetItem(page,
												PageGetItemId(page, offnum));
				htup = &(itup->t_tid);
				if (callback(htup, callback_state))
					deletable[ndeletable++] = offnum;
			}
		}

		/*
		 * Apply any needed deletes.  We issue just one _bt_delitems() call
		 * per page, so as to minimize WAL traffic.
		 */
		if (ndeletable > 0)
		{
			_bt_delitems(rel, buf, deletable, ndeletable);
			stats->tuples_removed += ndeletable;
			/* must recompute maxoff */
			maxoff = PageGetMaxOffsetNumber(page);
		}
		else
		{
			/*
			 * If the page has been split during this vacuum cycle, it seems
			 * worth expending a write to clear btpo_cycleid even if we don't
			 * have any deletions to do.  (If we do, _bt_delitems takes care
			 * of this.)  This ensures we won't process the page again.
			 *
			 * We treat this like a hint-bit update because there's no need to
			 * WAL-log it.
			 */
			if (vstate->cycleid != 0 &&
				opaque->btpo_cycleid == vstate->cycleid)
			{
				opaque->btpo_cycleid = 0;
				SetBufferCommitInfoNeedsSave(buf);
			}
		}

		/*
		 * If it's now empty, try to delete; else count the live tuples. We
		 * don't delete when recursing, though, to avoid putting entries into
		 * freePages out-of-order (doesn't seem worth any extra code to handle
		 * the case).
		 */
		if (minoff > maxoff)
			delete_now = (blkno == orig_blkno);
		else
			stats->num_index_tuples += maxoff - minoff + 1;
	}

	if (delete_now)
	{
		MemoryContext oldcontext;
		int			ndel;

		/* Run pagedel in a temp context to avoid memory leakage */
		MemoryContextReset(vstate->pagedelcontext);
		oldcontext = MemoryContextSwitchTo(vstate->pagedelcontext);

		ndel = _bt_pagedel(rel, buf, NULL, info->vacuum_full);

		/* count only this page, else may double-count parent */
		if (ndel)
			stats->pages_deleted++;

		/*
		 * During VACUUM FULL it's okay to recycle deleted pages immediately,
		 * since there can be no other transactions scanning the index.  Note
		 * that we will only recycle the current page and not any parent pages
		 * that _bt_pagedel might have recursed to; this seems reasonable in
		 * the name of simplicity.	(Trying to do otherwise would mean we'd
		 * have to sort the list of recyclable pages we're building.)
		 */
		if (ndel && info->vacuum_full)
		{
			if (vstate->nFreePages < vstate->maxFreePages)
				vstate->freePages[vstate->nFreePages++] = blkno;
			vstate->totFreePages++;
		}

		MemoryContextSwitchTo(oldcontext);
		/* pagedel released buffer, so we shouldn't */
	}
	else
		_bt_relbuf(rel, buf);

	/*
	 * This is really tail recursion, but if the compiler is too stupid to
	 * optimize it as such, we'd eat an uncomfortably large amount of stack
	 * space per recursion level (due to the deletable[] array). A failure is
	 * improbable since the number of levels isn't likely to be large ... but
	 * just in case, let's hand-optimize into a loop.
	 */
	if (recurse_to != P_NONE)
	{
		blkno = recurse_to;
		goto restart;
	}
}
