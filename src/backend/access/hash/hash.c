/*-------------------------------------------------------------------------
 *
 * hash.c
 *	  Implementation of Margo Seltzer's Hashing package for postgres.
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/hash/hash.c
 *
 * NOTES
 *	  This file contains only the public interface routines.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/hash.h"
#include "access/relscan.h"
#include "catalog/index.h"
#include "commands/vacuum.h"
#include "optimizer/cost.h"
#include "optimizer/plancat.h"
#include "storage/bufmgr.h"
#include "utils/rel.h"


/* Working state for hashbuild and its callback */
typedef struct
{
	HSpool	   *spool;			/* NULL if not using spooling */
	double		indtuples;		/* # tuples accepted into index */
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
	BlockNumber relpages;
	double		reltuples;
	double		allvisfrac;
	uint32		num_buckets;
	HashBuildState buildstate;

	/*
	 * We expect to be called exactly once for any index relation. If that's
	 * not the case, big trouble's what we have.
	 */
	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index \"%s\" already contains data",
			 RelationGetRelationName(index));

	/* Estimate the number of rows currently present in the table */
	estimate_rel_size(heap, NULL, &relpages, &reltuples, &allvisfrac);

	/* Initialize the hash index metadata page and initial buckets */
	num_buckets = _hash_metapinit(index, reltuples, MAIN_FORKNUM);

	/*
	 * If we just insert the tuples into the index in scan order, then
	 * (assuming their hash codes are pretty random) there will be no locality
	 * of access to the index, and if the index is bigger than available RAM
	 * then we'll thrash horribly.  To prevent that scenario, we can sort the
	 * tuples by (expected) bucket number.  However, such a sort is useless
	 * overhead when the index does fit in RAM.  We choose to sort if the
	 * initial index size exceeds NBuffers.
	 *
	 * NOTE: this test will need adjustment if a bucket is ever different from
	 * one page.
	 */
	if (num_buckets >= (uint32) NBuffers)
		buildstate.spool = _h_spoolinit(heap, index, num_buckets);
	else
		buildstate.spool = NULL;

	/* prepare to build the index */
	buildstate.indtuples = 0;

	/* do the heap scan */
	reltuples = IndexBuildHeapScan(heap, index, indexInfo, true,
								   hashbuildCallback, (void *) &buildstate);

	if (buildstate.spool)
	{
		/* sort the tuples and insert them into the index */
		_h_indexbuild(buildstate.spool);
		_h_spooldestroy(buildstate.spool);
	}

	/*
	 * Return statistics
	 */
	result = (IndexBuildResult *) palloc(sizeof(IndexBuildResult));

	result->heap_tuples = reltuples;
	result->index_tuples = buildstate.indtuples;

	PG_RETURN_POINTER(result);
}

/*
 *	hashbuildempty() -- build an empty hash index in the initialization fork
 */
Datum
hashbuildempty(PG_FUNCTION_ARGS)
{
	Relation	index = (Relation) PG_GETARG_POINTER(0);

	_hash_metapinit(index, 0, INIT_FORKNUM);

	PG_RETURN_VOID();
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
	Datum		index_values[1];
	bool		index_isnull[1];
	IndexTuple	itup;

	/* convert data to a hash key; on failure, do not insert anything */
	if (!_hash_convert_tuple(index,
							 values, isnull,
							 index_values, index_isnull))
		return;

	/* Either spool the tuple for sorting, or just put it into the index */
	if (buildstate->spool)
		_h_spool(buildstate->spool, &htup->t_self,
				 index_values, index_isnull);
	else
	{
		/* form an index tuple and point it at the heap tuple */
		itup = index_form_tuple(RelationGetDescr(index),
								index_values, index_isnull);
		itup->t_tid = htup->t_self;
		_hash_doinsert(index, itup);
		pfree(itup);
	}

	buildstate->indtuples += 1;
}

/*
 *	hashinsert() -- insert an index tuple into a hash table.
 *
 *	Hash on the heap tuple's key, form an index tuple with hash code.
 *	Find the appropriate location for the new tuple, and put it there.
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
	IndexUniqueCheck checkUnique = (IndexUniqueCheck) PG_GETARG_INT32(5);
#endif
	Datum		index_values[1];
	bool		index_isnull[1];
	IndexTuple	itup;

	/* convert data to a hash key; on failure, do not insert anything */
	if (!_hash_convert_tuple(rel,
							 values, isnull,
							 index_values, index_isnull))
		PG_RETURN_BOOL(false);

	/* form an index tuple and point it at the heap tuple */
	itup = index_form_tuple(RelationGetDescr(rel), index_values, index_isnull);
	itup->t_tid = *ht_ctid;

	_hash_doinsert(rel, itup);

	pfree(itup);

	PG_RETURN_BOOL(false);
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
	Buffer		buf;
	Page		page;
	OffsetNumber offnum;
	ItemPointer current;
	bool		res;

	/* Hash indexes are always lossy since we store only the hash code */
	scan->xs_recheck = true;

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
	current = &(so->hashso_curpos);
	if (ItemPointerIsValid(current))
	{
		/*
		 * An insertion into the current index page could have happened while
		 * we didn't have read lock on it.  Re-find our position by looking
		 * for the TID we previously returned.  (Because we hold share lock on
		 * the bucket, no deletions or splits could have occurred; therefore
		 * we can expect that the TID still exists in the current index page,
		 * at an offset >= where we were.)
		 */
		OffsetNumber maxoffnum;

		buf = so->hashso_curbuf;
		Assert(BufferIsValid(buf));
		page = BufferGetPage(buf);
		maxoffnum = PageGetMaxOffsetNumber(page);
		for (offnum = ItemPointerGetOffsetNumber(current);
			 offnum <= maxoffnum;
			 offnum = OffsetNumberNext(offnum))
		{
			IndexTuple	itup;

			itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, offnum));
			if (ItemPointerEquals(&(so->hashso_heappos), &(itup->t_tid)))
				break;
		}
		if (offnum > maxoffnum)
			elog(ERROR, "failed to re-find scan position within index \"%s\"",
				 RelationGetRelationName(rel));
		ItemPointerSetOffsetNumber(current, offnum);

		/*
		 * Check to see if we should kill the previously-fetched tuple.
		 */
		if (scan->kill_prior_tuple)
		{
			/*
			 * Yes, so mark it by setting the LP_DEAD state in the item flags.
			 */
			ItemIdMarkDead(PageGetItemId(page, offnum));

			/*
			 * Since this can be redone later if needed, mark as a hint.
			 */
			MarkBufferDirtyHint(buf, true);
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
			offnum = ItemPointerGetOffsetNumber(current);
			page = BufferGetPage(so->hashso_curbuf);
			if (!ItemIdIsDead(PageGetItemId(page, offnum)))
				break;
			res = _hash_next(scan, dir);
		}
	}

	/* Release read lock on current buffer, but keep it pinned */
	if (BufferIsValid(so->hashso_curbuf))
		_hash_chgbufaccess(rel, so->hashso_curbuf, HASH_READ, HASH_NOLOCK);

	/* Return current heap TID on success */
	scan->xs_ctup.t_self = so->hashso_heappos;

	PG_RETURN_BOOL(res);
}


/*
 *	hashgetbitmap() -- get all tuples at once
 */
Datum
hashgetbitmap(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	TIDBitmap  *tbm = (TIDBitmap *) PG_GETARG_POINTER(1);
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	bool		res;
	int64		ntids = 0;

	res = _hash_first(scan, ForwardScanDirection);

	while (res)
	{
		bool		add_tuple;

		/*
		 * Skip killed tuples if asked to.
		 */
		if (scan->ignore_killed_tuples)
		{
			Page		page;
			OffsetNumber offnum;

			offnum = ItemPointerGetOffsetNumber(&(so->hashso_curpos));
			page = BufferGetPage(so->hashso_curbuf);
			add_tuple = !ItemIdIsDead(PageGetItemId(page, offnum));
		}
		else
			add_tuple = true;

		/* Save tuple ID, and continue scanning */
		if (add_tuple)
		{
			/* Note we mark the tuple ID as requiring recheck */
			tbm_add_tuples(tbm, &(so->hashso_heappos), 1, true);
			ntids++;
		}

		res = _hash_next(scan, ForwardScanDirection);
	}

	PG_RETURN_INT64(ntids);
}


/*
 *	hashbeginscan() -- start a scan on a hash index
 */
Datum
hashbeginscan(PG_FUNCTION_ARGS)
{
	Relation	rel = (Relation) PG_GETARG_POINTER(0);
	int			nkeys = PG_GETARG_INT32(1);
	int			norderbys = PG_GETARG_INT32(2);
	IndexScanDesc scan;
	HashScanOpaque so;

	/* no order by operators allowed */
	Assert(norderbys == 0);

	scan = RelationGetIndexScan(rel, nkeys, norderbys);

	so = (HashScanOpaque) palloc(sizeof(HashScanOpaqueData));
	so->hashso_bucket_valid = false;
	so->hashso_bucket_blkno = 0;
	so->hashso_curbuf = InvalidBuffer;
	/* set position invalid (this will cause _hash_first call) */
	ItemPointerSetInvalid(&(so->hashso_curpos));
	ItemPointerSetInvalid(&(so->hashso_heappos));

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

	/* remaining arguments are ignored */
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	Relation	rel = scan->indexRelation;

	/* release any pin we still hold */
	if (BufferIsValid(so->hashso_curbuf))
		_hash_dropbuf(rel, so->hashso_curbuf);
	so->hashso_curbuf = InvalidBuffer;

	/* release lock on bucket, too */
	if (so->hashso_bucket_blkno)
		_hash_droplock(rel, so->hashso_bucket_blkno, HASH_SHARE);
	so->hashso_bucket_blkno = 0;

	/* set position invalid (this will cause _hash_first call) */
	ItemPointerSetInvalid(&(so->hashso_curpos));
	ItemPointerSetInvalid(&(so->hashso_heappos));

	/* Update scan key, if a new one is given */
	if (scankey && scan->numberOfKeys > 0)
	{
		memmove(scan->keyData,
				scankey,
				scan->numberOfKeys * sizeof(ScanKeyData));
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

	/* release any pin we still hold */
	if (BufferIsValid(so->hashso_curbuf))
		_hash_dropbuf(rel, so->hashso_curbuf);
	so->hashso_curbuf = InvalidBuffer;

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
	elog(ERROR, "hash does not support mark/restore");
	PG_RETURN_VOID();
}

/*
 *	hashrestrpos() -- restore scan to last saved position
 */
Datum
hashrestrpos(PG_FUNCTION_ARGS)
{
	elog(ERROR, "hash does not support mark/restore");
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
	 * hashm_spares[] values to compute bucket page addresses.  This is a bit
	 * hokey but perfectly safe, since the interesting entries in the spares
	 * array cannot change under us; and it beats rereading the metapage for
	 * each bucket.
	 */
	metabuf = _hash_getbuf(rel, HASH_METAPAGE, HASH_READ, LH_META_PAGE);
	metap = HashPageGetMeta(BufferGetPage(metabuf));
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
			OffsetNumber deletable[MaxOffsetNumber];
			int			ndeletable = 0;

			vacuum_delay_point();

			buf = _hash_getbuf_with_strategy(rel, blkno, HASH_WRITE,
										   LH_BUCKET_PAGE | LH_OVERFLOW_PAGE,
											 info->strategy);
			page = BufferGetPage(buf);
			opaque = (HashPageOpaque) PageGetSpecialPointer(page);
			Assert(opaque->hasho_bucket == cur_bucket);

			/* Scan each tuple in page */
			maxoffno = PageGetMaxOffsetNumber(page);
			for (offno = FirstOffsetNumber;
				 offno <= maxoffno;
				 offno = OffsetNumberNext(offno))
			{
				IndexTuple	itup;
				ItemPointer htup;

				itup = (IndexTuple) PageGetItem(page,
												PageGetItemId(page, offno));
				htup = &(itup->t_tid);
				if (callback(htup, callback_state))
				{
					/* mark the item for deletion */
					deletable[ndeletable++] = offno;
					tuples_removed += 1;
				}
				else
					num_index_tuples += 1;
			}

			/*
			 * Apply deletions and write page if needed, advance to next page.
			 */
			blkno = opaque->hasho_nextblkno;

			if (ndeletable > 0)
			{
				PageIndexMultiDelete(page, deletable, ndeletable);
				_hash_wrtbuf(rel, buf);
				bucket_dirty = true;
			}
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
	metap = HashPageGetMeta(BufferGetPage(metabuf));

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
		 * double-scanned tuples in split buckets.  Proceed by dead-reckoning.
		 * (Note: we still return estimated_count = false, because using this
		 * count is better than not updating reltuples at all.)
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
	stats->estimated_count = false;
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
	/* Note: this covers the analyze_only case too */
	if (stats == NULL)
		PG_RETURN_POINTER(NULL);

	/* update statistics */
	num_pages = RelationGetNumberOfBlocks(rel);
	stats->num_pages = num_pages;

	PG_RETURN_POINTER(stats);
}


void
hash_redo(XLogReaderState *record)
{
	elog(PANIC, "hash_redo: unimplemented");
}
