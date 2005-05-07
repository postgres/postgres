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
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/nbtree/nbtree.c,v 1.106.2.1 2005/05/07 21:33:21 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/nbtree.h"
#include "catalog/index.h"
#include "miscadmin.h"
#include "storage/freespace.h"
#include "storage/smgr.h"


/* Working state for btbuild and its callback */
typedef struct
{
	bool		usefast;
	bool		isUnique;
	bool		haveDead;
	Relation	heapRel;
	BTSpool    *spool;

	/*
	 * spool2 is needed only when the index is an unique index. Dead
	 * tuples are put into spool2 instead of spool in order to avoid
	 * uniqueness check.
	 */
	BTSpool    *spool2;
	double		indtuples;
} BTBuildState;


bool		FastBuild = true;	/* use SORT instead of insertion build */

static void _bt_restscan(IndexScanDesc scan);
static void btbuildCallback(Relation index,
				HeapTuple htup,
				Datum *attdata,
				char *nulls,
				bool tupleIsAlive,
				void *state);


/*
 * AtEOXact_nbtree() --- clean up nbtree subsystem at xact abort or commit.
 */
void
AtEOXact_nbtree(void)
{
	/* nothing to do at the moment */
}


/*
 *	btbuild() -- build a new btree index.
 *
 *		We use a global variable to record the fact that we're creating
 *		a new index.  This is used to avoid high-concurrency locking,
 *		since the index won't be visible until this transaction commits
 *		and since building is guaranteed to be single-threaded.
 */
Datum
btbuild(PG_FUNCTION_ARGS)
{
	Relation	heap = (Relation) PG_GETARG_POINTER(0);
	Relation	index = (Relation) PG_GETARG_POINTER(1);
	IndexInfo  *indexInfo = (IndexInfo *) PG_GETARG_POINTER(2);
	double		reltuples;
	BTBuildState buildstate;

	/*
	 * bootstrap processing does something strange, so don't use
	 * sort/build for initial catalog indices.	at some point i need to
	 * look harder at this.  (there is some kind of incremental processing
	 * going on there.) -- pma 08/29/95
	 */
	buildstate.usefast = (FastBuild && IsNormalProcessingMode());
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
	 * We expect to be called exactly once for any index relation. If
	 * that's not the case, big trouble's what we have.
	 */
	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index \"%s\" already contains data",
			 RelationGetRelationName(index));

	/* initialize the btree index metadata page */
	/* mark it valid right away only if using slow build */
	_bt_metapinit(index, !buildstate.usefast);

	if (buildstate.usefast)
	{
		buildstate.spool = _bt_spoolinit(index, indexInfo->ii_Unique);

		/*
		 * Different from spool, the uniqueness isn't checked for spool2.
		 */
		if (indexInfo->ii_Unique)
			buildstate.spool2 = _bt_spoolinit(index, false);
	}

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
	 * if we are doing bottom-up btree build, finish the build by (1)
	 * completing the sort of the spool file, (2) inserting the sorted
	 * tuples into btree pages and (3) building the upper levels.
	 */
	if (buildstate.usefast)
	{
		_bt_leafbuild(buildstate.spool, buildstate.spool2);
		_bt_spooldestroy(buildstate.spool);
		if (buildstate.spool2)
			_bt_spooldestroy(buildstate.spool2);
	}

#ifdef BTREE_BUILD_STATS
	if (log_btree_build_stats)
	{
		ShowUsage("BTREE BUILD STATS");
		ResetUsage();
	}
#endif   /* BTREE_BUILD_STATS */

	/*
	 * Since we just counted the tuples in the heap, we update its stats
	 * in pg_class to guarantee that the planner takes advantage of the
	 * index we just created.  But, only update statistics during normal
	 * index definitions, not for indices on system catalogs created
	 * during bootstrap processing.  We must close the relations before
	 * updating statistics to guarantee that the relcache entries are
	 * flushed when we increment the command counter in UpdateStats(). But
	 * we do not release any locks on the relations; those will be held
	 * until end of transaction.
	 */
	if (IsNormalProcessingMode())
	{
		Oid			hrelid = RelationGetRelid(heap);
		Oid			irelid = RelationGetRelid(index);

		heap_close(heap, NoLock);
		index_close(index);
		UpdateStats(hrelid, reltuples);
		UpdateStats(irelid, buildstate.indtuples);
	}

	PG_RETURN_VOID();
}

/*
 * Per-tuple callback from IndexBuildHeapScan
 */
static void
btbuildCallback(Relation index,
				HeapTuple htup,
				Datum *attdata,
				char *nulls,
				bool tupleIsAlive,
				void *state)
{
	BTBuildState *buildstate = (BTBuildState *) state;
	IndexTuple	itup;
	BTItem		btitem;
	InsertIndexResult res;

	/* form an index tuple and point it at the heap tuple */
	itup = index_formtuple(RelationGetDescr(index), attdata, nulls);
	itup->t_tid = htup->t_self;

	btitem = _bt_formitem(itup);

	/*
	 * if we are doing bottom-up btree build, we insert the index into a
	 * spool file for subsequent processing.  otherwise, we insert into
	 * the btree.
	 */
	if (buildstate->usefast)
	{
		if (tupleIsAlive || buildstate->spool2 == NULL)
			_bt_spool(btitem, buildstate->spool);
		else
		{
			/* dead tuples are put into spool2 */
			buildstate->haveDead = true;
			_bt_spool(btitem, buildstate->spool2);
		}
	}
	else
	{
		res = _bt_doinsert(index, btitem,
						   buildstate->isUnique, buildstate->heapRel);
		if (res)
			pfree(res);
	}

	buildstate->indtuples += 1;

	pfree(btitem);
	pfree(itup);
}

/*
 *	btinsert() -- insert an index tuple into a btree.
 *
 *		Descend the tree recursively, find the appropriate location for our
 *		new tuple, put it there, set its unique OID as appropriate, and
 *		return an InsertIndexResult to the caller.
 */
Datum
btinsert(PG_FUNCTION_ARGS)
{
	Relation	rel = (Relation) PG_GETARG_POINTER(0);
	Datum	   *datum = (Datum *) PG_GETARG_POINTER(1);
	char	   *nulls = (char *) PG_GETARG_POINTER(2);
	ItemPointer ht_ctid = (ItemPointer) PG_GETARG_POINTER(3);
	Relation	heapRel = (Relation) PG_GETARG_POINTER(4);
	bool		checkUnique = PG_GETARG_BOOL(5);
	InsertIndexResult res;
	BTItem		btitem;
	IndexTuple	itup;

	/* generate an index tuple */
	itup = index_formtuple(RelationGetDescr(rel), datum, nulls);
	itup->t_tid = *ht_ctid;
	btitem = _bt_formitem(itup);

	res = _bt_doinsert(rel, btitem, checkUnique, heapRel);

	pfree(btitem);
	pfree(itup);

	PG_RETURN_POINTER(res);
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
	Page		page;
	OffsetNumber offnum;
	bool		res;

	/*
	 * If we've already initialized this scan, we can just advance it in
	 * the appropriate direction.  If we haven't done so yet, we call a
	 * routine to get the first item in the scan.
	 */
	if (ItemPointerIsValid(&(scan->currentItemData)))
	{
		/*
		 * Restore scan position using heap TID returned by previous call
		 * to btgettuple(). _bt_restscan() re-grabs the read lock on the
		 * buffer, too.
		 */
		_bt_restscan(scan);

		/*
		 * Check to see if we should kill the previously-fetched tuple.
		 */
		if (scan->kill_prior_tuple)
		{
			/*
			 * Yes, so mark it by setting the LP_DELETE bit in the item
			 * flags.
			 */
			offnum = ItemPointerGetOffsetNumber(&(scan->currentItemData));
			page = BufferGetPage(so->btso_curbuf);
			PageGetItemId(page, offnum)->lp_flags |= LP_DELETE;

			/*
			 * Since this can be redone later if needed, it's treated the
			 * same as a commit-hint-bit status update for heap tuples: we
			 * mark the buffer dirty but don't make a WAL log entry.
			 */
			SetBufferCommitInfoNeedsSave(so->btso_curbuf);
		}

		/*
		 * Now continue the scan.
		 */
		res = _bt_next(scan, dir);
	}
	else
		res = _bt_first(scan, dir);

	/*
	 * Skip killed tuples if asked to.
	 */
	if (scan->ignore_killed_tuples)
	{
		while (res)
		{
			offnum = ItemPointerGetOffsetNumber(&(scan->currentItemData));
			page = BufferGetPage(so->btso_curbuf);
			if (!ItemIdDeleted(PageGetItemId(page, offnum)))
				break;
			res = _bt_next(scan, dir);
		}
	}

	/*
	 * Save heap TID to use it in _bt_restscan.  Then release the read
	 * lock on the buffer so that we aren't blocking other backends.
	 *
	 * NOTE: we do keep the pin on the buffer!	This is essential to ensure
	 * that someone else doesn't delete the index entry we are stopped on.
	 */
	if (res)
	{
		((BTScanOpaque) scan->opaque)->curHeapIptr = scan->xs_ctup.t_self;
		LockBuffer(((BTScanOpaque) scan->opaque)->btso_curbuf,
				   BUFFER_LOCK_UNLOCK);
	}

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
	ItemPointer iptr;
	BTScanOpaque so;

	so = (BTScanOpaque) scan->opaque;

	if (so == NULL)				/* if called from btbeginscan */
	{
		so = (BTScanOpaque) palloc(sizeof(BTScanOpaqueData));
		so->btso_curbuf = so->btso_mrkbuf = InvalidBuffer;
		ItemPointerSetInvalid(&(so->curHeapIptr));
		ItemPointerSetInvalid(&(so->mrkHeapIptr));
		if (scan->numberOfKeys > 0)
			so->keyData = (ScanKey) palloc(scan->numberOfKeys * sizeof(ScanKeyData));
		else
			so->keyData = (ScanKey) NULL;
		so->numberOfKeys = scan->numberOfKeys;
		scan->opaque = so;
	}

	/* we aren't holding any read locks, but gotta drop the pins */
	if (ItemPointerIsValid(iptr = &(scan->currentItemData)))
	{
		ReleaseBuffer(so->btso_curbuf);
		so->btso_curbuf = InvalidBuffer;
		ItemPointerSetInvalid(&(so->curHeapIptr));
		ItemPointerSetInvalid(iptr);
	}

	if (ItemPointerIsValid(iptr = &(scan->currentMarkData)))
	{
		ReleaseBuffer(so->btso_mrkbuf);
		so->btso_mrkbuf = InvalidBuffer;
		ItemPointerSetInvalid(&(so->mrkHeapIptr));
		ItemPointerSetInvalid(iptr);
	}

	/*
	 * Reset the scan keys. Note that keys ordering stuff moved to
	 * _bt_first.	   - vadim 05/05/97
	 */
	if (scankey && scan->numberOfKeys > 0)
	{
		memmove(scan->keyData,
				scankey,
				scan->numberOfKeys * sizeof(ScanKeyData));
		so->numberOfKeys = scan->numberOfKeys;
		memmove(so->keyData,
				scankey,
				so->numberOfKeys * sizeof(ScanKeyData));
	}

	PG_RETURN_VOID();
}

void
btmovescan(IndexScanDesc scan, Datum v)
{
	ItemPointer iptr;
	BTScanOpaque so;

	so = (BTScanOpaque) scan->opaque;

	/* we aren't holding any read locks, but gotta drop the pin */
	if (ItemPointerIsValid(iptr = &(scan->currentItemData)))
	{
		ReleaseBuffer(so->btso_curbuf);
		so->btso_curbuf = InvalidBuffer;
		ItemPointerSetInvalid(iptr);
	}

	so->keyData[0].sk_argument = v;
}

/*
 *	btendscan() -- close down a scan
 */
Datum
btendscan(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ItemPointer iptr;
	BTScanOpaque so;

	so = (BTScanOpaque) scan->opaque;

	/* we aren't holding any read locks, but gotta drop the pins */
	if (ItemPointerIsValid(iptr = &(scan->currentItemData)))
	{
		if (BufferIsValid(so->btso_curbuf))
			ReleaseBuffer(so->btso_curbuf);
		so->btso_curbuf = InvalidBuffer;
		ItemPointerSetInvalid(iptr);
	}

	if (ItemPointerIsValid(iptr = &(scan->currentMarkData)))
	{
		if (BufferIsValid(so->btso_mrkbuf))
			ReleaseBuffer(so->btso_mrkbuf);
		so->btso_mrkbuf = InvalidBuffer;
		ItemPointerSetInvalid(iptr);
	}

	if (so->keyData != (ScanKey) NULL)
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
	ItemPointer iptr;
	BTScanOpaque so;

	so = (BTScanOpaque) scan->opaque;

	/* we aren't holding any read locks, but gotta drop the pin */
	if (ItemPointerIsValid(iptr = &(scan->currentMarkData)))
	{
		ReleaseBuffer(so->btso_mrkbuf);
		so->btso_mrkbuf = InvalidBuffer;
		ItemPointerSetInvalid(iptr);
	}

	/* bump pin on current buffer for assignment to mark buffer */
	if (ItemPointerIsValid(&(scan->currentItemData)))
	{
		so->btso_mrkbuf = ReadBuffer(scan->indexRelation,
								  BufferGetBlockNumber(so->btso_curbuf));
		scan->currentMarkData = scan->currentItemData;
		so->mrkHeapIptr = so->curHeapIptr;
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
	ItemPointer iptr;
	BTScanOpaque so;

	so = (BTScanOpaque) scan->opaque;

	/* we aren't holding any read locks, but gotta drop the pin */
	if (ItemPointerIsValid(iptr = &(scan->currentItemData)))
	{
		ReleaseBuffer(so->btso_curbuf);
		so->btso_curbuf = InvalidBuffer;
		ItemPointerSetInvalid(iptr);
	}

	/* bump pin on marked buffer */
	if (ItemPointerIsValid(&(scan->currentMarkData)))
	{
		so->btso_curbuf = ReadBuffer(scan->indexRelation,
								  BufferGetBlockNumber(so->btso_mrkbuf));
		scan->currentItemData = scan->currentMarkData;
		so->curHeapIptr = so->mrkHeapIptr;
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
	Relation	rel = (Relation) PG_GETARG_POINTER(0);
	IndexBulkDeleteCallback callback = (IndexBulkDeleteCallback) PG_GETARG_POINTER(1);
	void	   *callback_state = (void *) PG_GETARG_POINTER(2);
	IndexBulkDeleteResult *result;
	double		tuples_removed;
	double		num_index_tuples;
	OffsetNumber deletable[BLCKSZ / sizeof(OffsetNumber)];
	int			ndeletable;
	Buffer		buf;
	BlockNumber num_pages;

	tuples_removed = 0;
	num_index_tuples = 0;

	/*
	 * The outer loop iterates over index leaf pages, the inner over items
	 * on a leaf page.	We issue just one _bt_delitems() call per page, so
	 * as to minimize WAL traffic.
	 *
	 * Note that we exclusive-lock every leaf page containing data items, in
	 * sequence left to right.	It sounds attractive to only
	 * exclusive-lock those containing items we need to delete, but
	 * unfortunately that is not safe: we could then pass a stopped
	 * indexscan, which could in rare cases lead to deleting the item it
	 * needs to find when it resumes.  (See _bt_restscan --- this could
	 * only happen if an indexscan stops on a deletable item and then a
	 * page split moves that item into a page further to its right, which
	 * the indexscan will have no pin on.)	We can skip obtaining
	 * exclusive lock on empty pages though, since no indexscan could be
	 * stopped on those.
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

			CHECK_FOR_INTERRUPTS();

			ndeletable = 0;
			page = BufferGetPage(buf);
			opaque = (BTPageOpaque) PageGetSpecialPointer(page);
			minoff = P_FIRSTDATAKEY(opaque);
			maxoff = PageGetMaxOffsetNumber(page);
			/* We probably cannot see deleted pages, but skip 'em if so */
			if (minoff <= maxoff && !P_ISDELETED(opaque))
			{
				/*
				 * Trade in the initial read lock for a super-exclusive
				 * write lock on this page.
				 */
				LockBuffer(buf, BUFFER_LOCK_UNLOCK);
				LockBufferForCleanup(buf);

				/*
				 * Recompute minoff/maxoff, both of which could have
				 * changed while we weren't holding the lock.
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
					BTItem		btitem;
					ItemPointer htup;

					btitem = (BTItem) PageGetItem(page,
											PageGetItemId(page, offnum));
					htup = &(btitem->bti_itup.t_tid);
					if (callback(htup, callback_state))
					{
						deletable[ndeletable++] = offnum;
						tuples_removed += 1;
					}
					else
						num_index_tuples += 1;
				}
			}

			/*
			 * If we need to delete anything, do it and write the buffer;
			 * else just release the buffer.
			 */
			nextpage = opaque->btpo_next;
			if (ndeletable > 0)
			{
				_bt_delitems(rel, buf, deletable, ndeletable);
				_bt_wrtbuf(rel, buf);
			}
			else
				_bt_relbuf(rel, buf);
			/* And advance to next page, if any */
			if (nextpage == P_NONE)
				break;
			buf = _bt_getbuf(rel, nextpage, BT_READ);
		}
	}

	/* return statistics */
	num_pages = RelationGetNumberOfBlocks(rel);

	result = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
	result->num_pages = num_pages;
	result->num_index_tuples = num_index_tuples;
	result->tuples_removed = tuples_removed;

	PG_RETURN_POINTER(result);
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
	Relation	rel = (Relation) PG_GETARG_POINTER(0);
	IndexVacuumCleanupInfo *info = (IndexVacuumCleanupInfo *) PG_GETARG_POINTER(1);
	IndexBulkDeleteResult *stats = (IndexBulkDeleteResult *) PG_GETARG_POINTER(2);
	BlockNumber num_pages;
	BlockNumber blkno;
	BlockNumber *freePages;
	int			nFreePages,
				maxFreePages;
	BlockNumber pages_deleted = 0;
	MemoryContext mycontext;
	MemoryContext oldcontext;
	bool		needLock;

	Assert(stats != NULL);

	/*
	 * First find out the number of pages in the index.  We must acquire
	 * the relation-extension lock while doing this to avoid a race
	 * condition: if someone else is extending the relation, there is
	 * a window where bufmgr/smgr have created a new all-zero page but
	 * it hasn't yet been write-locked by _bt_getbuf().  If we manage to
	 * scan such a page here, we'll improperly assume it can be recycled.
	 * Taking the lock synchronizes things enough to prevent a problem:
	 * either num_pages won't include the new page, or _bt_getbuf already
	 * has write lock on the buffer and it will be fully initialized before
	 * we can examine it.  (See also vacuumlazy.c, which has the same issue.)
	 *
	 * We can skip locking for new or temp relations,
	 * however, since no one else could be accessing them.
	 */
	needLock = !(rel->rd_isnew || rel->rd_istemp);

	if (needLock)
		LockPage(rel, 0, ExclusiveLock);

	num_pages = RelationGetNumberOfBlocks(rel);

	if (needLock)
		UnlockPage(rel, 0, ExclusiveLock);

	/* No point in remembering more than MaxFSMPages pages */
	maxFreePages = MaxFSMPages;
	if ((BlockNumber) maxFreePages > num_pages)
		maxFreePages = (int) num_pages + 1;		/* +1 to avoid palloc(0) */
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

		buf = _bt_getbuf(rel, blkno, BT_READ);
		page = BufferGetPage(buf);
		opaque = (BTPageOpaque) PageGetSpecialPointer(page);
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
			 * immediately, since there can be no other transactions
			 * scanning the index.	Note that we will only recycle the
			 * current page and not any parent pages that _bt_pagedel
			 * might have recursed to; this seems reasonable in the name
			 * of simplicity.  (Trying to do otherwise would mean we'd
			 * have to sort the list of recyclable pages we're building.)
			 */
			if (ndel && info->vacuum_full)
			{
				if (nFreePages < maxFreePages)
					freePages[nFreePages++] = blkno;
			}

			MemoryContextSwitchTo(oldcontext);
			continue;			/* pagedel released buffer */
		}
		_bt_relbuf(rel, buf);
	}

	/*
	 * During VACUUM FULL, we truncate off any recyclable pages at the end
	 * of the index.  In a normal vacuum it'd be unsafe to do this except
	 * by acquiring exclusive lock on the index and then rechecking all
	 * the pages; doesn't seem worth it.
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
			int			i;

			/*
			 * Okay to truncate.
			 *
			 * First, flush any shared buffers for the blocks we intend to
			 * delete.	FlushRelationBuffers is a bit more than we need
			 * for this, since it will also write out dirty buffers for
			 * blocks we aren't deleting, but it's the closest thing in
			 * bufmgr's API.
			 */
			i = FlushRelationBuffers(rel, new_pages);
			if (i < 0)
				elog(ERROR, "FlushRelationBuffers returned %d", i);

			/*
			 * Do the physical truncation.
			 */
			new_pages = smgrtruncate(DEFAULT_SMGR, rel, new_pages);
			rel->rd_nblocks = new_pages;		/* update relcache
												 * immediately */
			rel->rd_targblock = InvalidBlockNumber;
			num_pages = new_pages;
		}
	}

	/*
	 * Update the shared Free Space Map with the info we now have about
	 * free pages in the index, discarding any old info the map may have.
	 * We do not need to sort the page numbers; they're in order already.
	 */
	RecordIndexFreeSpace(&rel->rd_node, nFreePages, freePages);

	pfree(freePages);

	MemoryContextDelete(mycontext);

	/* update statistics */
	stats->num_pages = num_pages;
	stats->pages_deleted = pages_deleted;
	stats->pages_free = nFreePages;

	PG_RETURN_POINTER(stats);
}

/*
 * Restore scan position when btgettuple is called to continue a scan.
 *
 * This is nontrivial because concurrent insertions might have moved the
 * index tuple we stopped on.  We assume the tuple can only have moved to
 * the right from our stop point, because we kept a pin on the buffer,
 * and so no deletion can have occurred on that page.
 *
 * On entry, we have a pin but no read lock on the buffer that contained
 * the index tuple we stopped the scan on.	On exit, we have pin and read
 * lock on the buffer that now contains that index tuple, and the scandesc's
 * current position is updated to point at it.
 */
static void
_bt_restscan(IndexScanDesc scan)
{
	Relation	rel = scan->indexRelation;
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	Buffer		buf = so->btso_curbuf;
	Page		page;
	ItemPointer current = &(scan->currentItemData);
	OffsetNumber offnum = ItemPointerGetOffsetNumber(current),
				maxoff;
	BTPageOpaque opaque;
	Buffer		nextbuf;
	ItemPointer target = &(so->curHeapIptr);
	BTItem		item;
	BlockNumber blkno;

	/*
	 * Reacquire read lock on the buffer.  (We should still have a
	 * reference-count pin on it, so need not get that.)
	 */
	LockBuffer(buf, BT_READ);

	page = BufferGetPage(buf);
	maxoff = PageGetMaxOffsetNumber(page);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);

	/*
	 * We use this as flag when first index tuple on page is deleted but
	 * we do not move left (this would slowdown vacuum) - so we set
	 * current->ip_posid before first index tuple on the current page
	 * (_bt_step will move it right)...  XXX still needed?
	 */
	if (!ItemPointerIsValid(target))
	{
		ItemPointerSetOffsetNumber(current,
							   OffsetNumberPrev(P_FIRSTDATAKEY(opaque)));
		return;
	}

	/*
	 * The item we were on may have moved right due to insertions. Find it
	 * again.  We use the heap TID to identify the item uniquely.
	 */
	for (;;)
	{
		/* Check for item on this page */
		for (;
			 offnum <= maxoff;
			 offnum = OffsetNumberNext(offnum))
		{
			item = (BTItem) PageGetItem(page, PageGetItemId(page, offnum));
			if (BTTidSame(item->bti_itup.t_tid, *target))
			{
				/* Found it */
				current->ip_posid = offnum;
				return;
			}
		}

		/*
		 * The item we're looking for moved right at least one page, so
		 * move right.	We are careful here to pin and read-lock the next
		 * non-dead page before releasing the current one.	This ensures
		 * that a concurrent btbulkdelete scan cannot pass our position
		 * --- if it did, it might be able to reach and delete our target
		 * item before we can find it again.
		 */
		if (P_RIGHTMOST(opaque))
			elog(ERROR, "failed to re-find previous key in \"%s\"",
				 RelationGetRelationName(rel));
		/* Advance to next non-dead page --- there must be one */
		nextbuf = InvalidBuffer;
		for (;;)
		{
			blkno = opaque->btpo_next;
			if (nextbuf != InvalidBuffer)
				_bt_relbuf(rel, nextbuf);
			nextbuf = _bt_getbuf(rel, blkno, BT_READ);
			page = BufferGetPage(nextbuf);
			opaque = (BTPageOpaque) PageGetSpecialPointer(page);
			if (!P_IGNORE(opaque))
				break;
			if (P_RIGHTMOST(opaque))
				elog(ERROR, "fell off the end of \"%s\"",
					 RelationGetRelationName(rel));
		}
		_bt_relbuf(rel, buf);
		so->btso_curbuf = buf = nextbuf;
		maxoff = PageGetMaxOffsetNumber(page);
		offnum = P_FIRSTDATAKEY(opaque);
		ItemPointerSet(current, blkno, offnum);
	}
}
