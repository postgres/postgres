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
 *	  $PostgreSQL: pgsql/src/backend/access/nbtree/nbtree.c,v 1.147 2006/05/07 01:21:30 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/nbtree.h"
#include "catalog/index.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "storage/freespace.h"
#include "storage/smgr.h"
#include "utils/inval.h"
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


static void btbuildCallback(Relation index,
				HeapTuple htup,
				Datum *values,
				bool *isnull,
				bool tupleIsAlive,
				void *state);


/*
 *	btbuild() -- build a new btree index.
 */
Datum
btbuild(PG_FUNCTION_ARGS)
{
	Relation	heap = (Relation) PG_GETARG_POINTER(0);
	Relation	index = (Relation) PG_GETARG_POINTER(1);
	IndexInfo  *indexInfo = (IndexInfo *) PG_GETARG_POINTER(2);
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
	 * If building a unique index, put dead tuples in a second spool to
	 * keep them out of the uniqueness check.
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
	 * If we are reindexing a pre-existing index, it is critical to send out
	 * a relcache invalidation SI message to ensure all backends re-read the
	 * index metapage.  In most circumstances the update-stats operation will
	 * cause that to happen, but at the moment there are corner cases where
	 * no pg_class update will occur, so force an inval here.  XXX FIXME:
	 * the upper levels of CREATE INDEX should handle the stats update as
	 * well as guaranteeing relcache inval.
	 */
	CacheInvalidateRelcache(index);

	/* since we just counted the # of tuples, may as well update stats */
	IndexCloseAndUpdateStats(heap, reltuples, index, buildstate.indtuples);

	PG_RETURN_VOID();
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
			 * Yes, remember it for later.  (We'll deal with all such tuples
			 * at once right before leaving the index page.)  The test for
			 * numKilled overrun is not just paranoia: if the caller reverses
			 * direction in the indexscan then the same item might get entered
			 * multiple times.  It's not worth trying to optimize that, so we
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
		 * Advance to next tuple within page.  This is the same as the
		 * easy case in _bt_next().
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
		so->killedItems = NULL;					/* until needed */
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

	/* bump pin on current buffer for assignment to mark buffer */
	if (BTScanPosIsValid(so->currPos))
	{
		IncrBufferRefCount(so->currPos.buf);
		memcpy(&so->markPos, &so->currPos,
			   offsetof(BTScanPosData, items[1]) +
			   so->currPos.lastItem * sizeof(BTScanPosItem));
	}

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

	/* bump pin on marked buffer */
	if (BTScanPosIsValid(so->markPos))
	{
		IncrBufferRefCount(so->markPos.buf);
		memcpy(&so->currPos, &so->markPos,
			   offsetof(BTScanPosData, items[1]) +
			   so->markPos.lastItem * sizeof(BTScanPosItem));
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
	IndexBulkDeleteResult *stats = (IndexBulkDeleteResult *) PG_GETARG_POINTER(1);
	IndexBulkDeleteCallback callback = (IndexBulkDeleteCallback) PG_GETARG_POINTER(2);
	void	   *callback_state = (void *) PG_GETARG_POINTER(3);
	Relation	rel = info->index;
	double		tuples_removed = 0;
	OffsetNumber deletable[MaxOffsetNumber];
	int			ndeletable;
	Buffer		buf;

	/*
	 * The outer loop iterates over index leaf pages, the inner over items on
	 * a leaf page.  We issue just one _bt_delitems() call per page, so as to
	 * minimize WAL traffic.
	 *
	 * Note that we exclusive-lock every leaf page containing data items, in
	 * sequence left to right.	It sounds attractive to only exclusive-lock
	 * those containing items we need to delete, but unfortunately that is not
	 * safe: we could then pass a stopped indexscan, which could in rare cases
	 * lead to deleting items that the indexscan will still return later.
	 * (See discussion in nbtree/README.)  We can skip obtaining exclusive
	 * lock on empty pages though, since no indexscan could be stopped on
	 * those.  (Note: this presumes that a split couldn't have left either
	 * page totally empty.)
	 */
	buf = _bt_get_endpoint(rel, 0, false);

	if (BufferIsValid(buf))		/* check for empty index */
	{
		for (;;)
		{
			Page		page;
			BTPageOpaque opaque;
			OffsetNumber offnum,
						minoff,
						maxoff;
			BlockNumber nextpage;

			ndeletable = 0;
			page = BufferGetPage(buf);
			opaque = (BTPageOpaque) PageGetSpecialPointer(page);
			minoff = P_FIRSTDATAKEY(opaque);
			maxoff = PageGetMaxOffsetNumber(page);
			/* We probably cannot see deleted pages, but skip 'em if so */
			if (minoff <= maxoff && !P_ISDELETED(opaque))
			{
				/*
				 * Trade in the initial read lock for a super-exclusive write
				 * lock on this page.
				 */
				LockBuffer(buf, BUFFER_LOCK_UNLOCK);
				LockBufferForCleanup(buf);

				/*
				 * Recompute minoff/maxoff, both of which could have changed
				 * while we weren't holding the lock.
				 */
				minoff = P_FIRSTDATAKEY(opaque);
				maxoff = PageGetMaxOffsetNumber(page);

				/*
				 * Scan over all items to see which ones need deleted
				 * according to the callback function.
				 */
				for (offnum = minoff;
					 offnum <= maxoff;
					 offnum = OffsetNumberNext(offnum))
				{
					IndexTuple	itup;
					ItemPointer htup;

					itup = (IndexTuple)
						PageGetItem(page, PageGetItemId(page, offnum));
					htup = &(itup->t_tid);
					if (callback(htup, callback_state))
					{
						deletable[ndeletable++] = offnum;
						tuples_removed += 1;
					}
				}
			}

			/* Apply any needed deletes */
			if (ndeletable > 0)
				_bt_delitems(rel, buf, deletable, ndeletable);

			/* Fetch nextpage link before releasing the buffer */
			nextpage = opaque->btpo_next;
			_bt_relbuf(rel, buf);

			/* call vacuum_delay_point while not holding any buffer lock */
			vacuum_delay_point();

			/* And advance to next page, if any */
			if (nextpage == P_NONE)
				break;
			buf = _bt_getbuf(rel, nextpage, BT_READ);
		}
	}

	/* return statistics */
	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
	stats->tuples_removed += tuples_removed;
	/* btvacuumcleanup will fill in num_pages and num_index_tuples */

	PG_RETURN_POINTER(stats);
}

/*
 * Post-VACUUM cleanup.
 *
 * Here, we scan looking for pages we can delete or return to the freelist.
 *
 * Result: a palloc'd struct containing statistical info for VACUUM displays.
 */
Datum
btvacuumcleanup(PG_FUNCTION_ARGS)
{
	IndexVacuumInfo *info = (IndexVacuumInfo *) PG_GETARG_POINTER(0);
	IndexBulkDeleteResult *stats = (IndexBulkDeleteResult *) PG_GETARG_POINTER(1);
	Relation	rel = info->index;
	BlockNumber num_pages;
	BlockNumber blkno;
	BlockNumber *freePages;
	int			nFreePages,
				maxFreePages;
	double		num_index_tuples = 0;
	BlockNumber pages_deleted = 0;
	MemoryContext mycontext;
	MemoryContext oldcontext;
	bool		needLock;

	/* Set up all-zero stats if btbulkdelete wasn't called */
	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

	/*
	 * First find out the number of pages in the index.  We must acquire the
	 * relation-extension lock while doing this to avoid a race condition: if
	 * someone else is extending the relation, there is a window where
	 * bufmgr/smgr have created a new all-zero page but it hasn't yet been
	 * write-locked by _bt_getbuf().  If we manage to scan such a page here,
	 * we'll improperly assume it can be recycled. Taking the lock
	 * synchronizes things enough to prevent a problem: either num_pages won't
	 * include the new page, or _bt_getbuf already has write lock on the
	 * buffer and it will be fully initialized before we can examine it.  (See
	 * also vacuumlazy.c, which has the same issue.)
	 *
	 * We can skip locking for new or temp relations, however, since no one
	 * else could be accessing them.
	 */
	needLock = !RELATION_IS_LOCAL(rel);

	if (needLock)
		LockRelationForExtension(rel, ExclusiveLock);

	num_pages = RelationGetNumberOfBlocks(rel);

	if (needLock)
		UnlockRelationForExtension(rel, ExclusiveLock);

	/* No point in remembering more than MaxFSMPages pages */
	maxFreePages = MaxFSMPages;
	if ((BlockNumber) maxFreePages > num_pages)
		maxFreePages = (int) num_pages;
	freePages = (BlockNumber *) palloc(maxFreePages * sizeof(BlockNumber));
	nFreePages = 0;

	/* Create a temporary memory context to run _bt_pagedel in */
	mycontext = AllocSetContextCreate(CurrentMemoryContext,
									  "_bt_pagedel",
									  ALLOCSET_DEFAULT_MINSIZE,
									  ALLOCSET_DEFAULT_INITSIZE,
									  ALLOCSET_DEFAULT_MAXSIZE);

	/*
	 * Scan through all pages of index, except metapage.  (Any pages added
	 * after we start the scan will not be examined; this should be fine,
	 * since they can't possibly be empty.)
	 */
	for (blkno = BTREE_METAPAGE + 1; blkno < num_pages; blkno++)
	{
		Buffer		buf;
		Page		page;
		BTPageOpaque opaque;

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
		if (_bt_page_recyclable(page))
		{
			/* Okay to recycle this page */
			if (nFreePages < maxFreePages)
				freePages[nFreePages++] = blkno;
			pages_deleted++;
		}
		else if (P_ISDELETED(opaque))
		{
			/* Already deleted, but can't recycle yet */
			pages_deleted++;
		}
		else if ((opaque->btpo_flags & BTP_HALF_DEAD) ||
				 P_FIRSTDATAKEY(opaque) > PageGetMaxOffsetNumber(page))
		{
			/* Empty, try to delete */
			int			ndel;

			/* Run pagedel in a temp context to avoid memory leakage */
			MemoryContextReset(mycontext);
			oldcontext = MemoryContextSwitchTo(mycontext);

			ndel = _bt_pagedel(rel, buf, info->vacuum_full);

			/* count only this page, else may double-count parent */
			if (ndel)
				pages_deleted++;

			/*
			 * During VACUUM FULL it's okay to recycle deleted pages
			 * immediately, since there can be no other transactions scanning
			 * the index.  Note that we will only recycle the current page and
			 * not any parent pages that _bt_pagedel might have recursed to;
			 * this seems reasonable in the name of simplicity.  (Trying to do
			 * otherwise would mean we'd have to sort the list of recyclable
			 * pages we're building.)
			 */
			if (ndel && info->vacuum_full)
			{
				if (nFreePages < maxFreePages)
					freePages[nFreePages++] = blkno;
			}

			MemoryContextSwitchTo(oldcontext);
			continue;			/* pagedel released buffer */
		}
		else if (P_ISLEAF(opaque))
		{
			/* Count the index entries of live leaf pages */
			num_index_tuples += PageGetMaxOffsetNumber(page) + 1 -
				P_FIRSTDATAKEY(opaque);
		}
		_bt_relbuf(rel, buf);
	}

	/*
	 * During VACUUM FULL, we truncate off any recyclable pages at the end of
	 * the index.  In a normal vacuum it'd be unsafe to do this except by
	 * acquiring exclusive lock on the index and then rechecking all the
	 * pages; doesn't seem worth it.
	 */
	if (info->vacuum_full && nFreePages > 0)
	{
		BlockNumber new_pages = num_pages;

		while (nFreePages > 0 && freePages[nFreePages - 1] == new_pages - 1)
		{
			new_pages--;
			pages_deleted--;
			nFreePages--;
		}
		if (new_pages != num_pages)
		{
			/*
			 * Okay to truncate.
			 */
			RelationTruncate(rel, new_pages);

			/* update statistics */
			stats->pages_removed = num_pages - new_pages;

			num_pages = new_pages;
		}
	}

	/*
	 * Update the shared Free Space Map with the info we now have about free
	 * pages in the index, discarding any old info the map may have. We do not
	 * need to sort the page numbers; they're in order already.
	 */
	RecordIndexFreeSpace(&rel->rd_node, nFreePages, freePages);

	pfree(freePages);

	MemoryContextDelete(mycontext);

	/* update statistics */
	stats->num_pages = num_pages;
	stats->num_index_tuples = num_index_tuples;
	stats->pages_deleted = pages_deleted;
	stats->pages_free = nFreePages;

	PG_RETURN_POINTER(stats);
}
