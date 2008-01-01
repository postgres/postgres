/*-------------------------------------------------------------------------
 *
 * hash.c
 *	  Implementation of Margo Seltzer's Hashing package for postgres.
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/hash/hash.c,v 1.98 2008/01/01 19:45:46 momjian Exp $
 *
 * NOTES
 *	  This file contains only the public interface routines.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/hash.h"
#include "catalog/index.h"
#include "commands/vacuum.h"


/* Working state for hashbuild and its callback */
typedef struct
{
	double		indtuples;
} HashBuildState;

static void hashbuildCallback(Relation index,
				  HeapTuple htup,
				  Datum *values,
				  bool *isnull,
				  bool tupleIsAlive,
				  void *state);


/*
 *	hashbuild() -- build a new hash index.
 */
Datum
hashbuild(PG_FUNCTION_ARGS)
{
	Relation	heap = (Relation) PG_GETARG_POINTER(0);
	Relation	index = (Relation) PG_GETARG_POINTER(1);
	IndexInfo  *indexInfo = (IndexInfo *) PG_GETARG_POINTER(2);
	IndexBuildResult *result;
	double		reltuples;
	HashBuildState buildstate;

	/*
	 * We expect to be called exactly once for any index relation. If that's
	 * not the case, big trouble's what we have.
	 */
	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index \"%s\" already contains data",
			 RelationGetRelationName(index));

	/* initialize the hash index metadata page */
	_hash_metapinit(index);

	/* build the index */
	buildstate.indtuples = 0;

	/* do the heap scan */
	reltuples = IndexBuildHeapScan(heap, index, indexInfo,
								   hashbuildCallback, (void *) &buildstate);

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
hashbuildCallback(Relation index,
				  HeapTuple htup,
				  Datum *values,
				  bool *isnull,
				  bool tupleIsAlive,
				  void *state)
{
	HashBuildState *buildstate = (HashBuildState *) state;
	IndexTuple	itup;

	/* form an index tuple and point it at the heap tuple */
	itup = index_form_tuple(RelationGetDescr(index), values, isnull);
	itup->t_tid = htup->t_self;

	/* Hash indexes don't index nulls, see notes in hashinsert */
	if (IndexTupleHasNulls(itup))
	{
		pfree(itup);
		return;
	}

	_hash_doinsert(index, itup);

	buildstate->indtuples += 1;

	pfree(itup);
}

/*
 *	hashinsert() -- insert an index tuple into a hash table.
 *
 *	Hash on the index tuple's key, find the appropriate location
 *	for the new tuple, and put it there.
 */
Datum
hashinsert(PG_FUNCTION_ARGS)
{
	Relation	rel = (Relation) PG_GETARG_POINTER(0);
	Datum	   *values = (Datum *) PG_GETARG_POINTER(1);
	bool	   *isnull = (bool *) PG_GETARG_POINTER(2);
	ItemPointer ht_ctid = (ItemPointer) PG_GETARG_POINTER(3);

#ifdef NOT_USED
	Relation	heapRel = (Relation) PG_GETARG_POINTER(4);
	bool		checkUnique = PG_GETARG_BOOL(5);
#endif
	IndexTuple	itup;

	/* generate an index tuple */
	itup = index_form_tuple(RelationGetDescr(rel), values, isnull);
	itup->t_tid = *ht_ctid;

	/*
	 * If the single index key is null, we don't insert it into the index.
	 * Hash tables support scans on '='. Relational algebra says that A = B
	 * returns null if either A or B is null.  This means that no
	 * qualification used in an index scan could ever return true on a null
	 * attribute.  It also means that indices can't be used by ISNULL or
	 * NOTNULL scans, but that's an artifact of the strategy map architecture
	 * chosen in 1986, not of the way nulls are handled here.
	 */
	if (IndexTupleHasNulls(itup))
	{
		pfree(itup);
		PG_RETURN_BOOL(false);
	}

	_hash_doinsert(rel, itup);

	pfree(itup);

	PG_RETURN_BOOL(true);
}


/*
 *	hashgettuple() -- Get the next tuple in the scan.
 */
Datum
hashgettuple(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ScanDirection dir = (ScanDirection) PG_GETARG_INT32(1);
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	Relation	rel = scan->indexRelation;
	Page		page;
	OffsetNumber offnum;
	bool		res;

	/*
	 * We hold pin but not lock on current buffer while outside the hash AM.
	 * Reacquire the read lock here.
	 */
	if (BufferIsValid(so->hashso_curbuf))
		_hash_chgbufaccess(rel, so->hashso_curbuf, HASH_NOLOCK, HASH_READ);

	/*
	 * If we've already initialized this scan, we can just advance it in the
	 * appropriate direction.  If we haven't done so yet, we call a routine to
	 * get the first item in the scan.
	 */
	if (ItemPointerIsValid(&(so->hashso_curpos)))
	{
		/*
		 * Check to see if we should kill the previously-fetched tuple.
		 */
		if (scan->kill_prior_tuple)
		{
			/*
			 * Yes, so mark it by setting the LP_DEAD state in the item flags.
			 */
			offnum = ItemPointerGetOffsetNumber(&(so->hashso_curpos));
			page = BufferGetPage(so->hashso_curbuf);
			ItemIdMarkDead(PageGetItemId(page, offnum));

			/*
			 * Since this can be redone later if needed, it's treated the same
			 * as a commit-hint-bit status update for heap tuples: we mark the
			 * buffer dirty but don't make a WAL log entry.
			 */
			SetBufferCommitInfoNeedsSave(so->hashso_curbuf);
		}

		/*
		 * Now continue the scan.
		 */
		res = _hash_next(scan, dir);
	}
	else
		res = _hash_first(scan, dir);

	/*
	 * Skip killed tuples if asked to.
	 */
	if (scan->ignore_killed_tuples)
	{
		while (res)
		{
			offnum = ItemPointerGetOffsetNumber(&(so->hashso_curpos));
			page = BufferGetPage(so->hashso_curbuf);
			if (!ItemIdIsDead(PageGetItemId(page, offnum)))
				break;
			res = _hash_next(scan, dir);
		}
	}

	/* Release read lock on current buffer, but keep it pinned */
	if (BufferIsValid(so->hashso_curbuf))
		_hash_chgbufaccess(rel, so->hashso_curbuf, HASH_READ, HASH_NOLOCK);

	PG_RETURN_BOOL(res);
}


/*
 *	hashgetmulti() -- get multiple tuples at once
 *
 * This is a somewhat generic implementation: it avoids lock reacquisition
 * overhead, but there's no smarts about picking especially good stopping
 * points such as index page boundaries.
 */
Datum
hashgetmulti(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ItemPointer tids = (ItemPointer) PG_GETARG_POINTER(1);
	int32		max_tids = PG_GETARG_INT32(2);
	int32	   *returned_tids = (int32 *) PG_GETARG_POINTER(3);
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	Relation	rel = scan->indexRelation;
	bool		res = true;
	int32		ntids = 0;

	/*
	 * We hold pin but not lock on current buffer while outside the hash AM.
	 * Reacquire the read lock here.
	 */
	if (BufferIsValid(so->hashso_curbuf))
		_hash_chgbufaccess(rel, so->hashso_curbuf, HASH_NOLOCK, HASH_READ);

	while (ntids < max_tids)
	{
		/*
		 * Start scan, or advance to next tuple.
		 */
		if (ItemPointerIsValid(&(so->hashso_curpos)))
			res = _hash_next(scan, ForwardScanDirection);
		else
			res = _hash_first(scan, ForwardScanDirection);

		/*
		 * Skip killed tuples if asked to.
		 */
		if (scan->ignore_killed_tuples)
		{
			while (res)
			{
				Page		page;
				OffsetNumber offnum;

				offnum = ItemPointerGetOffsetNumber(&(so->hashso_curpos));
				page = BufferGetPage(so->hashso_curbuf);
				if (!ItemIdIsDead(PageGetItemId(page, offnum)))
					break;
				res = _hash_next(scan, ForwardScanDirection);
			}
		}

		if (!res)
			break;
		/* Save tuple ID, and continue scanning */
		tids[ntids] = scan->xs_ctup.t_self;
		ntids++;
	}

	/* Release read lock on current buffer, but keep it pinned */
	if (BufferIsValid(so->hashso_curbuf))
		_hash_chgbufaccess(rel, so->hashso_curbuf, HASH_READ, HASH_NOLOCK);

	*returned_tids = ntids;
	PG_RETURN_BOOL(res);
}


/*
 *	hashbeginscan() -- start a scan on a hash index
 */
Datum
hashbeginscan(PG_FUNCTION_ARGS)
{
	Relation	rel = (Relation) PG_GETARG_POINTER(0);
	int			keysz = PG_GETARG_INT32(1);
	ScanKey		scankey = (ScanKey) PG_GETARG_POINTER(2);
	IndexScanDesc scan;
	HashScanOpaque so;

	scan = RelationGetIndexScan(rel, keysz, scankey);
	so = (HashScanOpaque) palloc(sizeof(HashScanOpaqueData));
	so->hashso_bucket_valid = false;
	so->hashso_bucket_blkno = 0;
	so->hashso_curbuf = so->hashso_mrkbuf = InvalidBuffer;
	/* set positions invalid (this will cause _hash_first call) */
	ItemPointerSetInvalid(&(so->hashso_curpos));
	ItemPointerSetInvalid(&(so->hashso_mrkpos));

	scan->opaque = so;

	/* register scan in case we change pages it's using */
	_hash_regscan(scan);

	PG_RETURN_POINTER(scan);
}

/*
 *	hashrescan() -- rescan an index relation
 */
Datum
hashrescan(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ScanKey		scankey = (ScanKey) PG_GETARG_POINTER(1);
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	Relation	rel = scan->indexRelation;

	/* if we are called from beginscan, so is still NULL */
	if (so)
	{
		/* release any pins we still hold */
		if (BufferIsValid(so->hashso_curbuf))
			_hash_dropbuf(rel, so->hashso_curbuf);
		so->hashso_curbuf = InvalidBuffer;

		if (BufferIsValid(so->hashso_mrkbuf))
			_hash_dropbuf(rel, so->hashso_mrkbuf);
		so->hashso_mrkbuf = InvalidBuffer;

		/* release lock on bucket, too */
		if (so->hashso_bucket_blkno)
			_hash_droplock(rel, so->hashso_bucket_blkno, HASH_SHARE);
		so->hashso_bucket_blkno = 0;

		/* set positions invalid (this will cause _hash_first call) */
		ItemPointerSetInvalid(&(so->hashso_curpos));
		ItemPointerSetInvalid(&(so->hashso_mrkpos));
	}

	/* Update scan key, if a new one is given */
	if (scankey && scan->numberOfKeys > 0)
	{
		memmove(scan->keyData,
				scankey,
				scan->numberOfKeys * sizeof(ScanKeyData));
		if (so)
			so->hashso_bucket_valid = false;
	}

	PG_RETURN_VOID();
}

/*
 *	hashendscan() -- close down a scan
 */
Datum
hashendscan(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	Relation	rel = scan->indexRelation;

	/* don't need scan registered anymore */
	_hash_dropscan(scan);

	/* release any pins we still hold */
	if (BufferIsValid(so->hashso_curbuf))
		_hash_dropbuf(rel, so->hashso_curbuf);
	so->hashso_curbuf = InvalidBuffer;

	if (BufferIsValid(so->hashso_mrkbuf))
		_hash_dropbuf(rel, so->hashso_mrkbuf);
	so->hashso_mrkbuf = InvalidBuffer;

	/* release lock on bucket, too */
	if (so->hashso_bucket_blkno)
		_hash_droplock(rel, so->hashso_bucket_blkno, HASH_SHARE);
	so->hashso_bucket_blkno = 0;

	pfree(so);
	scan->opaque = NULL;

	PG_RETURN_VOID();
}

/*
 *	hashmarkpos() -- save current scan position
 */
Datum
hashmarkpos(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	Relation	rel = scan->indexRelation;

	/* release pin on old marked data, if any */
	if (BufferIsValid(so->hashso_mrkbuf))
		_hash_dropbuf(rel, so->hashso_mrkbuf);
	so->hashso_mrkbuf = InvalidBuffer;
	ItemPointerSetInvalid(&(so->hashso_mrkpos));

	/* bump pin count on current buffer and copy to marked buffer */
	if (ItemPointerIsValid(&(so->hashso_curpos)))
	{
		IncrBufferRefCount(so->hashso_curbuf);
		so->hashso_mrkbuf = so->hashso_curbuf;
		so->hashso_mrkpos = so->hashso_curpos;
	}

	PG_RETURN_VOID();
}

/*
 *	hashrestrpos() -- restore scan to last saved position
 */
Datum
hashrestrpos(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	Relation	rel = scan->indexRelation;

	/* release pin on current data, if any */
	if (BufferIsValid(so->hashso_curbuf))
		_hash_dropbuf(rel, so->hashso_curbuf);
	so->hashso_curbuf = InvalidBuffer;
	ItemPointerSetInvalid(&(so->hashso_curpos));

	/* bump pin count on marked buffer and copy to current buffer */
	if (ItemPointerIsValid(&(so->hashso_mrkpos)))
	{
		IncrBufferRefCount(so->hashso_mrkbuf);
		so->hashso_curbuf = so->hashso_mrkbuf;
		so->hashso_curpos = so->hashso_mrkpos;
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
hashbulkdelete(PG_FUNCTION_ARGS)
{
	IndexVacuumInfo *info = (IndexVacuumInfo *) PG_GETARG_POINTER(0);
	IndexBulkDeleteResult *stats = (IndexBulkDeleteResult *) PG_GETARG_POINTER(1);
	IndexBulkDeleteCallback callback = (IndexBulkDeleteCallback) PG_GETARG_POINTER(2);
	void	   *callback_state = (void *) PG_GETARG_POINTER(3);
	Relation	rel = info->index;
	double		tuples_removed;
	double		num_index_tuples;
	double		orig_ntuples;
	Bucket		orig_maxbucket;
	Bucket		cur_maxbucket;
	Bucket		cur_bucket;
	Buffer		metabuf;
	HashMetaPage metap;
	HashMetaPageData local_metapage;

	tuples_removed = 0;
	num_index_tuples = 0;

	/*
	 * Read the metapage to fetch original bucket and tuple counts.  Also, we
	 * keep a copy of the last-seen metapage so that we can use its
	 * hashm_spares[] values to compute bucket page addresses.	This is a bit
	 * hokey but perfectly safe, since the interesting entries in the spares
	 * array cannot change under us; and it beats rereading the metapage for
	 * each bucket.
	 */
	metabuf = _hash_getbuf(rel, HASH_METAPAGE, HASH_READ, LH_META_PAGE);
	metap = (HashMetaPage) BufferGetPage(metabuf);
	orig_maxbucket = metap->hashm_maxbucket;
	orig_ntuples = metap->hashm_ntuples;
	memcpy(&local_metapage, metap, sizeof(local_metapage));
	_hash_relbuf(rel, metabuf);

	/* Scan the buckets that we know exist */
	cur_bucket = 0;
	cur_maxbucket = orig_maxbucket;

loop_top:
	while (cur_bucket <= cur_maxbucket)
	{
		BlockNumber bucket_blkno;
		BlockNumber blkno;
		bool		bucket_dirty = false;

		/* Get address of bucket's start page */
		bucket_blkno = BUCKET_TO_BLKNO(&local_metapage, cur_bucket);

		/* Exclusive-lock the bucket so we can shrink it */
		_hash_getlock(rel, bucket_blkno, HASH_EXCLUSIVE);

		/* Shouldn't have any active scans locally, either */
		if (_hash_has_active_scan(rel, cur_bucket))
			elog(ERROR, "hash index has active scan during VACUUM");

		/* Scan each page in bucket */
		blkno = bucket_blkno;
		while (BlockNumberIsValid(blkno))
		{
			Buffer		buf;
			Page		page;
			HashPageOpaque opaque;
			OffsetNumber offno;
			OffsetNumber maxoffno;
			bool		page_dirty = false;

			vacuum_delay_point();

			buf = _hash_getbuf_with_strategy(rel, blkno, HASH_WRITE,
										   LH_BUCKET_PAGE | LH_OVERFLOW_PAGE,
											 info->strategy);
			page = BufferGetPage(buf);
			opaque = (HashPageOpaque) PageGetSpecialPointer(page);
			Assert(opaque->hasho_bucket == cur_bucket);

			/* Scan each tuple in page */
			offno = FirstOffsetNumber;
			maxoffno = PageGetMaxOffsetNumber(page);
			while (offno <= maxoffno)
			{
				IndexTuple	itup;
				ItemPointer htup;

				itup = (IndexTuple) PageGetItem(page,
												PageGetItemId(page, offno));
				htup = &(itup->t_tid);
				if (callback(htup, callback_state))
				{
					/* delete the item from the page */
					PageIndexTupleDelete(page, offno);
					bucket_dirty = page_dirty = true;

					/* don't increment offno, instead decrement maxoffno */
					maxoffno = OffsetNumberPrev(maxoffno);

					tuples_removed += 1;
				}
				else
				{
					offno = OffsetNumberNext(offno);

					num_index_tuples += 1;
				}
			}

			/*
			 * Write page if needed, advance to next page.
			 */
			blkno = opaque->hasho_nextblkno;

			if (page_dirty)
				_hash_wrtbuf(rel, buf);
			else
				_hash_relbuf(rel, buf);
		}

		/* If we deleted anything, try to compact free space */
		if (bucket_dirty)
			_hash_squeezebucket(rel, cur_bucket, bucket_blkno,
								info->strategy);

		/* Release bucket lock */
		_hash_droplock(rel, bucket_blkno, HASH_EXCLUSIVE);

		/* Advance to next bucket */
		cur_bucket++;
	}

	/* Write-lock metapage and check for split since we started */
	metabuf = _hash_getbuf(rel, HASH_METAPAGE, HASH_WRITE, LH_META_PAGE);
	metap = (HashMetaPage) BufferGetPage(metabuf);

	if (cur_maxbucket != metap->hashm_maxbucket)
	{
		/* There's been a split, so process the additional bucket(s) */
		cur_maxbucket = metap->hashm_maxbucket;
		memcpy(&local_metapage, metap, sizeof(local_metapage));
		_hash_relbuf(rel, metabuf);
		goto loop_top;
	}

	/* Okay, we're really done.  Update tuple count in metapage. */

	if (orig_maxbucket == metap->hashm_maxbucket &&
		orig_ntuples == metap->hashm_ntuples)
	{
		/*
		 * No one has split or inserted anything since start of scan, so
		 * believe our count as gospel.
		 */
		metap->hashm_ntuples = num_index_tuples;
	}
	else
	{
		/*
		 * Otherwise, our count is untrustworthy since we may have
		 * double-scanned tuples in split buckets.	Proceed by dead-reckoning.
		 */
		if (metap->hashm_ntuples > tuples_removed)
			metap->hashm_ntuples -= tuples_removed;
		else
			metap->hashm_ntuples = 0;
		num_index_tuples = metap->hashm_ntuples;
	}

	_hash_wrtbuf(rel, metabuf);

	/* return statistics */
	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
	stats->num_index_tuples = num_index_tuples;
	stats->tuples_removed += tuples_removed;
	/* hashvacuumcleanup will fill in num_pages */

	PG_RETURN_POINTER(stats);
}

/*
 * Post-VACUUM cleanup.
 *
 * Result: a palloc'd struct containing statistical info for VACUUM displays.
 */
Datum
hashvacuumcleanup(PG_FUNCTION_ARGS)
{
	IndexVacuumInfo *info = (IndexVacuumInfo *) PG_GETARG_POINTER(0);
	IndexBulkDeleteResult *stats = (IndexBulkDeleteResult *) PG_GETARG_POINTER(1);
	Relation	rel = info->index;
	BlockNumber num_pages;

	/* If hashbulkdelete wasn't called, return NULL signifying no change */
	if (stats == NULL)
		PG_RETURN_POINTER(NULL);

	/* update statistics */
	num_pages = RelationGetNumberOfBlocks(rel);
	stats->num_pages = num_pages;

	PG_RETURN_POINTER(stats);
}


void
hash_redo(XLogRecPtr lsn, XLogRecord *record)
{
	elog(PANIC, "hash_redo: unimplemented");
}

void
hash_desc(StringInfo buf, uint8 xl_info, char *rec)
{
}
