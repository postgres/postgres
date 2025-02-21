/*-------------------------------------------------------------------------
 *
 * hash.c
 *	  Implementation of Margo Seltzer's Hashing package for postgres.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
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
#include "access/hash_xlog.h"
#include "access/relscan.h"
#include "access/stratnum.h"
#include "access/tableam.h"
#include "access/xloginsert.h"
#include "commands/progress.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "optimizer/plancat.h"
#include "pgstat.h"
#include "utils/fmgrprotos.h"
#include "utils/index_selfuncs.h"
#include "utils/rel.h"

/* Working state for hashbuild and its callback */
typedef struct
{
	HSpool	   *spool;			/* NULL if not using spooling */
	double		indtuples;		/* # tuples accepted into index */
	Relation	heapRel;		/* heap relation descriptor */
} HashBuildState;

static void hashbuildCallback(Relation index,
							  ItemPointer tid,
							  Datum *values,
							  bool *isnull,
							  bool tupleIsAlive,
							  void *state);


/*
 * Hash handler function: return IndexAmRoutine with access method parameters
 * and callbacks.
 */
Datum
hashhandler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

	amroutine->amstrategies = HTMaxStrategyNumber;
	amroutine->amsupport = HASHNProcs;
	amroutine->amoptsprocnum = HASHOPTIONS_PROC;
	amroutine->amcanorder = false;
	amroutine->amcanorderbyop = false;
	amroutine->amcanbackward = true;
	amroutine->amcanunique = false;
	amroutine->amcanmulticol = false;
	amroutine->amoptionalkey = false;
	amroutine->amsearcharray = false;
	amroutine->amsearchnulls = false;
	amroutine->amstorage = false;
	amroutine->amclusterable = false;
	amroutine->ampredlocks = true;
	amroutine->amcanparallel = false;
	amroutine->amcanbuildparallel = false;
	amroutine->amcaninclude = false;
	amroutine->amusemaintenanceworkmem = false;
	amroutine->amsummarizing = false;
	amroutine->amparallelvacuumoptions =
		VACUUM_OPTION_PARALLEL_BULKDEL;
	amroutine->amkeytype = INT4OID;

	amroutine->ambuild = hashbuild;
	amroutine->ambuildempty = hashbuildempty;
	amroutine->aminsert = hashinsert;
	amroutine->aminsertcleanup = NULL;
	amroutine->ambulkdelete = hashbulkdelete;
	amroutine->amvacuumcleanup = hashvacuumcleanup;
	amroutine->amcanreturn = NULL;
	amroutine->amcostestimate = hashcostestimate;
	amroutine->amgettreeheight = NULL;
	amroutine->amoptions = hashoptions;
	amroutine->amproperty = NULL;
	amroutine->ambuildphasename = NULL;
	amroutine->amvalidate = hashvalidate;
	amroutine->amadjustmembers = hashadjustmembers;
	amroutine->ambeginscan = hashbeginscan;
	amroutine->amrescan = hashrescan;
	amroutine->amgettuple = hashgettuple;
	amroutine->amgetbitmap = hashgetbitmap;
	amroutine->amendscan = hashendscan;
	amroutine->ammarkpos = NULL;
	amroutine->amrestrpos = NULL;
	amroutine->amestimateparallelscan = NULL;
	amroutine->aminitparallelscan = NULL;
	amroutine->amparallelrescan = NULL;
	amroutine->amtranslatestrategy = hashtranslatestrategy;
	amroutine->amtranslatecmptype = hashtranslatecmptype;

	PG_RETURN_POINTER(amroutine);
}

/*
 *	hashbuild() -- build a new hash index.
 */
IndexBuildResult *
hashbuild(Relation heap, Relation index, IndexInfo *indexInfo)
{
	IndexBuildResult *result;
	BlockNumber relpages;
	double		reltuples;
	double		allvisfrac;
	uint32		num_buckets;
	Size		sort_threshold;
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
	num_buckets = _hash_init(index, reltuples, MAIN_FORKNUM);

	/*
	 * If we just insert the tuples into the index in scan order, then
	 * (assuming their hash codes are pretty random) there will be no locality
	 * of access to the index, and if the index is bigger than available RAM
	 * then we'll thrash horribly.  To prevent that scenario, we can sort the
	 * tuples by (expected) bucket number.  However, such a sort is useless
	 * overhead when the index does fit in RAM.  We choose to sort if the
	 * initial index size exceeds maintenance_work_mem, or the number of
	 * buffers usable for the index, whichever is less.  (Limiting by the
	 * number of buffers should reduce thrashing between PG buffers and kernel
	 * buffers, which seems useful even if no physical I/O results.  Limiting
	 * by maintenance_work_mem is useful to allow easy testing of the sort
	 * code path, and may be useful to DBAs as an additional control knob.)
	 *
	 * NOTE: this test will need adjustment if a bucket is ever different from
	 * one page.  Also, "initial index size" accounting does not include the
	 * metapage, nor the first bitmap page.
	 */
	sort_threshold = (maintenance_work_mem * (Size) 1024) / BLCKSZ;
	if (index->rd_rel->relpersistence != RELPERSISTENCE_TEMP)
		sort_threshold = Min(sort_threshold, NBuffers);
	else
		sort_threshold = Min(sort_threshold, NLocBuffer);

	if (num_buckets >= sort_threshold)
		buildstate.spool = _h_spoolinit(heap, index, num_buckets);
	else
		buildstate.spool = NULL;

	/* prepare to build the index */
	buildstate.indtuples = 0;
	buildstate.heapRel = heap;

	/* do the heap scan */
	reltuples = table_index_build_scan(heap, index, indexInfo, true, true,
									   hashbuildCallback,
									   &buildstate, NULL);
	pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_TOTAL,
								 buildstate.indtuples);

	if (buildstate.spool)
	{
		/* sort the tuples and insert them into the index */
		_h_indexbuild(buildstate.spool, buildstate.heapRel);
		_h_spooldestroy(buildstate.spool);
	}

	/*
	 * Return statistics
	 */
	result = (IndexBuildResult *) palloc(sizeof(IndexBuildResult));

	result->heap_tuples = reltuples;
	result->index_tuples = buildstate.indtuples;

	return result;
}

/*
 *	hashbuildempty() -- build an empty hash index in the initialization fork
 */
void
hashbuildempty(Relation index)
{
	_hash_init(index, 0, INIT_FORKNUM);
}

/*
 * Per-tuple callback for table_index_build_scan
 */
static void
hashbuildCallback(Relation index,
				  ItemPointer tid,
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
		_h_spool(buildstate->spool, tid, index_values, index_isnull);
	else
	{
		/* form an index tuple and point it at the heap tuple */
		itup = index_form_tuple(RelationGetDescr(index),
								index_values, index_isnull);
		itup->t_tid = *tid;
		_hash_doinsert(index, itup, buildstate->heapRel, false);
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
bool
hashinsert(Relation rel, Datum *values, bool *isnull,
		   ItemPointer ht_ctid, Relation heapRel,
		   IndexUniqueCheck checkUnique,
		   bool indexUnchanged,
		   IndexInfo *indexInfo)
{
	Datum		index_values[1];
	bool		index_isnull[1];
	IndexTuple	itup;

	/* convert data to a hash key; on failure, do not insert anything */
	if (!_hash_convert_tuple(rel,
							 values, isnull,
							 index_values, index_isnull))
		return false;

	/* form an index tuple and point it at the heap tuple */
	itup = index_form_tuple(RelationGetDescr(rel), index_values, index_isnull);
	itup->t_tid = *ht_ctid;

	_hash_doinsert(rel, itup, heapRel, false);

	pfree(itup);

	return false;
}


/*
 *	hashgettuple() -- Get the next tuple in the scan.
 */
bool
hashgettuple(IndexScanDesc scan, ScanDirection dir)
{
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	bool		res;

	/* Hash indexes are always lossy since we store only the hash code */
	scan->xs_recheck = true;

	/*
	 * If we've already initialized this scan, we can just advance it in the
	 * appropriate direction.  If we haven't done so yet, we call a routine to
	 * get the first item in the scan.
	 */
	if (!HashScanPosIsValid(so->currPos))
		res = _hash_first(scan, dir);
	else
	{
		/*
		 * Check to see if we should kill the previously-fetched tuple.
		 */
		if (scan->kill_prior_tuple)
		{
			/*
			 * Yes, so remember it for later. (We'll deal with all such tuples
			 * at once right after leaving the index page or at end of scan.)
			 * In case if caller reverses the indexscan direction it is quite
			 * possible that the same item might get entered multiple times.
			 * But, we don't detect that; instead, we just forget any excess
			 * entries.
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
		res = _hash_next(scan, dir);
	}

	return res;
}


/*
 *	hashgetbitmap() -- get all tuples at once
 */
int64
hashgetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	bool		res;
	int64		ntids = 0;
	HashScanPosItem *currItem;

	res = _hash_first(scan, ForwardScanDirection);

	while (res)
	{
		currItem = &so->currPos.items[so->currPos.itemIndex];

		/*
		 * _hash_first and _hash_next handle eliminate dead index entries
		 * whenever scan->ignore_killed_tuples is true.  Therefore, there's
		 * nothing to do here except add the results to the TIDBitmap.
		 */
		tbm_add_tuples(tbm, &(currItem->heapTid), 1, true);
		ntids++;

		res = _hash_next(scan, ForwardScanDirection);
	}

	return ntids;
}


/*
 *	hashbeginscan() -- start a scan on a hash index
 */
IndexScanDesc
hashbeginscan(Relation rel, int nkeys, int norderbys)
{
	IndexScanDesc scan;
	HashScanOpaque so;

	/* no order by operators allowed */
	Assert(norderbys == 0);

	scan = RelationGetIndexScan(rel, nkeys, norderbys);

	so = (HashScanOpaque) palloc(sizeof(HashScanOpaqueData));
	HashScanPosInvalidate(so->currPos);
	so->hashso_bucket_buf = InvalidBuffer;
	so->hashso_split_bucket_buf = InvalidBuffer;

	so->hashso_buc_populated = false;
	so->hashso_buc_split = false;

	so->killedItems = NULL;
	so->numKilled = 0;

	scan->opaque = so;

	return scan;
}

/*
 *	hashrescan() -- rescan an index relation
 */
void
hashrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
		   ScanKey orderbys, int norderbys)
{
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	Relation	rel = scan->indexRelation;

	if (HashScanPosIsValid(so->currPos))
	{
		/* Before leaving current page, deal with any killed items */
		if (so->numKilled > 0)
			_hash_kill_items(scan);
	}

	_hash_dropscanbuf(rel, so);

	/* set position invalid (this will cause _hash_first call) */
	HashScanPosInvalidate(so->currPos);

	/* Update scan key, if a new one is given */
	if (scankey && scan->numberOfKeys > 0)
		memcpy(scan->keyData, scankey, scan->numberOfKeys * sizeof(ScanKeyData));

	so->hashso_buc_populated = false;
	so->hashso_buc_split = false;
}

/*
 *	hashendscan() -- close down a scan
 */
void
hashendscan(IndexScanDesc scan)
{
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	Relation	rel = scan->indexRelation;

	if (HashScanPosIsValid(so->currPos))
	{
		/* Before leaving current page, deal with any killed items */
		if (so->numKilled > 0)
			_hash_kill_items(scan);
	}

	_hash_dropscanbuf(rel, so);

	if (so->killedItems != NULL)
		pfree(so->killedItems);
	pfree(so);
	scan->opaque = NULL;
}

/*
 * Bulk deletion of all index entries pointing to a set of heap tuples.
 * The set of target tuples is specified via a callback routine that tells
 * whether any given heap tuple (identified by ItemPointer) is being deleted.
 *
 * This function also deletes the tuples that are moved by split to other
 * bucket.
 *
 * Result: a palloc'd struct containing statistical info for VACUUM displays.
 */
IndexBulkDeleteResult *
hashbulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
			   IndexBulkDeleteCallback callback, void *callback_state)
{
	Relation	rel = info->index;
	double		tuples_removed;
	double		num_index_tuples;
	double		orig_ntuples;
	Bucket		orig_maxbucket;
	Bucket		cur_maxbucket;
	Bucket		cur_bucket;
	Buffer		metabuf = InvalidBuffer;
	HashMetaPage metap;
	HashMetaPage cachedmetap;

	tuples_removed = 0;
	num_index_tuples = 0;

	/*
	 * We need a copy of the metapage so that we can use its hashm_spares[]
	 * values to compute bucket page addresses, but a cached copy should be
	 * good enough.  (If not, we'll detect that further down and refresh the
	 * cache as necessary.)
	 */
	cachedmetap = _hash_getcachedmetap(rel, &metabuf, false);
	Assert(cachedmetap != NULL);

	orig_maxbucket = cachedmetap->hashm_maxbucket;
	orig_ntuples = cachedmetap->hashm_ntuples;

	/* Scan the buckets that we know exist */
	cur_bucket = 0;
	cur_maxbucket = orig_maxbucket;

loop_top:
	while (cur_bucket <= cur_maxbucket)
	{
		BlockNumber bucket_blkno;
		BlockNumber blkno;
		Buffer		bucket_buf;
		Buffer		buf;
		HashPageOpaque bucket_opaque;
		Page		page;
		bool		split_cleanup = false;

		/* Get address of bucket's start page */
		bucket_blkno = BUCKET_TO_BLKNO(cachedmetap, cur_bucket);

		blkno = bucket_blkno;

		/*
		 * We need to acquire a cleanup lock on the primary bucket page to out
		 * wait concurrent scans before deleting the dead tuples.
		 */
		buf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL, info->strategy);
		LockBufferForCleanup(buf);
		_hash_checkpage(rel, buf, LH_BUCKET_PAGE);

		page = BufferGetPage(buf);
		bucket_opaque = HashPageGetOpaque(page);

		/*
		 * If the bucket contains tuples that are moved by split, then we need
		 * to delete such tuples.  We can't delete such tuples if the split
		 * operation on bucket is not finished as those are needed by scans.
		 */
		if (!H_BUCKET_BEING_SPLIT(bucket_opaque) &&
			H_NEEDS_SPLIT_CLEANUP(bucket_opaque))
		{
			split_cleanup = true;

			/*
			 * This bucket might have been split since we last held a lock on
			 * the metapage.  If so, hashm_maxbucket, hashm_highmask and
			 * hashm_lowmask might be old enough to cause us to fail to remove
			 * tuples left behind by the most recent split.  To prevent that,
			 * now that the primary page of the target bucket has been locked
			 * (and thus can't be further split), check whether we need to
			 * update our cached metapage data.
			 */
			Assert(bucket_opaque->hasho_prevblkno != InvalidBlockNumber);
			if (bucket_opaque->hasho_prevblkno > cachedmetap->hashm_maxbucket)
			{
				cachedmetap = _hash_getcachedmetap(rel, &metabuf, true);
				Assert(cachedmetap != NULL);
			}
		}

		bucket_buf = buf;

		hashbucketcleanup(rel, cur_bucket, bucket_buf, blkno, info->strategy,
						  cachedmetap->hashm_maxbucket,
						  cachedmetap->hashm_highmask,
						  cachedmetap->hashm_lowmask, &tuples_removed,
						  &num_index_tuples, split_cleanup,
						  callback, callback_state);

		_hash_dropbuf(rel, bucket_buf);

		/* Advance to next bucket */
		cur_bucket++;
	}

	if (BufferIsInvalid(metabuf))
		metabuf = _hash_getbuf(rel, HASH_METAPAGE, HASH_NOLOCK, LH_META_PAGE);

	/* Write-lock metapage and check for split since we started */
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	metap = HashPageGetMeta(BufferGetPage(metabuf));

	if (cur_maxbucket != metap->hashm_maxbucket)
	{
		/* There's been a split, so process the additional bucket(s) */
		LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
		cachedmetap = _hash_getcachedmetap(rel, &metabuf, true);
		Assert(cachedmetap != NULL);
		cur_maxbucket = cachedmetap->hashm_maxbucket;
		goto loop_top;
	}

	/* Okay, we're really done.  Update tuple count in metapage. */
	START_CRIT_SECTION();

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

	MarkBufferDirty(metabuf);

	/* XLOG stuff */
	if (RelationNeedsWAL(rel))
	{
		xl_hash_update_meta_page xlrec;
		XLogRecPtr	recptr;

		xlrec.ntuples = metap->hashm_ntuples;

		XLogBeginInsert();
		XLogRegisterData(&xlrec, SizeOfHashUpdateMetaPage);

		XLogRegisterBuffer(0, metabuf, REGBUF_STANDARD);

		recptr = XLogInsert(RM_HASH_ID, XLOG_HASH_UPDATE_META_PAGE);
		PageSetLSN(BufferGetPage(metabuf), recptr);
	}

	END_CRIT_SECTION();

	_hash_relbuf(rel, metabuf);

	/* return statistics */
	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
	stats->estimated_count = false;
	stats->num_index_tuples = num_index_tuples;
	stats->tuples_removed += tuples_removed;
	/* hashvacuumcleanup will fill in num_pages */

	return stats;
}

/*
 * Post-VACUUM cleanup.
 *
 * Result: a palloc'd struct containing statistical info for VACUUM displays.
 */
IndexBulkDeleteResult *
hashvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	Relation	rel = info->index;
	BlockNumber num_pages;

	/* If hashbulkdelete wasn't called, return NULL signifying no change */
	/* Note: this covers the analyze_only case too */
	if (stats == NULL)
		return NULL;

	/* update statistics */
	num_pages = RelationGetNumberOfBlocks(rel);
	stats->num_pages = num_pages;

	return stats;
}

/*
 * Helper function to perform deletion of index entries from a bucket.
 *
 * This function expects that the caller has acquired a cleanup lock on the
 * primary bucket page, and will return with a write lock again held on the
 * primary bucket page.  The lock won't necessarily be held continuously,
 * though, because we'll release it when visiting overflow pages.
 *
 * There can't be any concurrent scans in progress when we first enter this
 * function because of the cleanup lock we hold on the primary bucket page,
 * but as soon as we release that lock, there might be.  If those scans got
 * ahead of our cleanup scan, they might see a tuple before we kill it and
 * wake up only after VACUUM has completed and the TID has been recycled for
 * an unrelated tuple.  To avoid that calamity, we prevent scans from passing
 * our cleanup scan by locking the next page in the bucket chain before
 * releasing the lock on the previous page.  (This type of lock chaining is not
 * ideal, so we might want to look for a better solution at some point.)
 *
 * We need to retain a pin on the primary bucket to ensure that no concurrent
 * split can start.
 */
void
hashbucketcleanup(Relation rel, Bucket cur_bucket, Buffer bucket_buf,
				  BlockNumber bucket_blkno, BufferAccessStrategy bstrategy,
				  uint32 maxbucket, uint32 highmask, uint32 lowmask,
				  double *tuples_removed, double *num_index_tuples,
				  bool split_cleanup,
				  IndexBulkDeleteCallback callback, void *callback_state)
{
	BlockNumber blkno;
	Buffer		buf;
	Bucket		new_bucket PG_USED_FOR_ASSERTS_ONLY = InvalidBucket;
	bool		bucket_dirty = false;

	blkno = bucket_blkno;
	buf = bucket_buf;

	if (split_cleanup)
		new_bucket = _hash_get_newbucket_from_oldbucket(rel, cur_bucket,
														lowmask, maxbucket);

	/* Scan each page in bucket */
	for (;;)
	{
		HashPageOpaque opaque;
		OffsetNumber offno;
		OffsetNumber maxoffno;
		Buffer		next_buf;
		Page		page;
		OffsetNumber deletable[MaxOffsetNumber];
		int			ndeletable = 0;
		bool		retain_pin = false;
		bool		clear_dead_marking = false;

		vacuum_delay_point(false);

		page = BufferGetPage(buf);
		opaque = HashPageGetOpaque(page);

		/* Scan each tuple in page */
		maxoffno = PageGetMaxOffsetNumber(page);
		for (offno = FirstOffsetNumber;
			 offno <= maxoffno;
			 offno = OffsetNumberNext(offno))
		{
			ItemPointer htup;
			IndexTuple	itup;
			Bucket		bucket;
			bool		kill_tuple = false;

			itup = (IndexTuple) PageGetItem(page,
											PageGetItemId(page, offno));
			htup = &(itup->t_tid);

			/*
			 * To remove the dead tuples, we strictly want to rely on results
			 * of callback function.  refer btvacuumpage for detailed reason.
			 */
			if (callback && callback(htup, callback_state))
			{
				kill_tuple = true;
				if (tuples_removed)
					*tuples_removed += 1;
			}
			else if (split_cleanup)
			{
				/* delete the tuples that are moved by split. */
				bucket = _hash_hashkey2bucket(_hash_get_indextuple_hashkey(itup),
											  maxbucket,
											  highmask,
											  lowmask);
				/* mark the item for deletion */
				if (bucket != cur_bucket)
				{
					/*
					 * We expect tuples to either belong to current bucket or
					 * new_bucket.  This is ensured because we don't allow
					 * further splits from bucket that contains garbage. See
					 * comments in _hash_expandtable.
					 */
					Assert(bucket == new_bucket);
					kill_tuple = true;
				}
			}

			if (kill_tuple)
			{
				/* mark the item for deletion */
				deletable[ndeletable++] = offno;
			}
			else
			{
				/* we're keeping it, so count it */
				if (num_index_tuples)
					*num_index_tuples += 1;
			}
		}

		/* retain the pin on primary bucket page till end of bucket scan */
		if (blkno == bucket_blkno)
			retain_pin = true;
		else
			retain_pin = false;

		blkno = opaque->hasho_nextblkno;

		/*
		 * Apply deletions, advance to next page and write page if needed.
		 */
		if (ndeletable > 0)
		{
			/* No ereport(ERROR) until changes are logged */
			START_CRIT_SECTION();

			PageIndexMultiDelete(page, deletable, ndeletable);
			bucket_dirty = true;

			/*
			 * Let us mark the page as clean if vacuum removes the DEAD tuples
			 * from an index page. We do this by clearing
			 * LH_PAGE_HAS_DEAD_TUPLES flag.
			 */
			if (tuples_removed && *tuples_removed > 0 &&
				H_HAS_DEAD_TUPLES(opaque))
			{
				opaque->hasho_flag &= ~LH_PAGE_HAS_DEAD_TUPLES;
				clear_dead_marking = true;
			}

			MarkBufferDirty(buf);

			/* XLOG stuff */
			if (RelationNeedsWAL(rel))
			{
				xl_hash_delete xlrec;
				XLogRecPtr	recptr;

				xlrec.clear_dead_marking = clear_dead_marking;
				xlrec.is_primary_bucket_page = (buf == bucket_buf);

				XLogBeginInsert();
				XLogRegisterData(&xlrec, SizeOfHashDelete);

				/*
				 * bucket buffer was not changed, but still needs to be
				 * registered to ensure that we can acquire a cleanup lock on
				 * it during replay.
				 */
				if (!xlrec.is_primary_bucket_page)
				{
					uint8		flags = REGBUF_STANDARD | REGBUF_NO_IMAGE | REGBUF_NO_CHANGE;

					XLogRegisterBuffer(0, bucket_buf, flags);
				}

				XLogRegisterBuffer(1, buf, REGBUF_STANDARD);
				XLogRegisterBufData(1, deletable,
									ndeletable * sizeof(OffsetNumber));

				recptr = XLogInsert(RM_HASH_ID, XLOG_HASH_DELETE);
				PageSetLSN(BufferGetPage(buf), recptr);
			}

			END_CRIT_SECTION();
		}

		/* bail out if there are no more pages to scan. */
		if (!BlockNumberIsValid(blkno))
			break;

		next_buf = _hash_getbuf_with_strategy(rel, blkno, HASH_WRITE,
											  LH_OVERFLOW_PAGE,
											  bstrategy);

		/*
		 * release the lock on previous page after acquiring the lock on next
		 * page
		 */
		if (retain_pin)
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		else
			_hash_relbuf(rel, buf);

		buf = next_buf;
	}

	/*
	 * lock the bucket page to clear the garbage flag and squeeze the bucket.
	 * if the current buffer is same as bucket buffer, then we already have
	 * lock on bucket page.
	 */
	if (buf != bucket_buf)
	{
		_hash_relbuf(rel, buf);
		LockBuffer(bucket_buf, BUFFER_LOCK_EXCLUSIVE);
	}

	/*
	 * Clear the garbage flag from bucket after deleting the tuples that are
	 * moved by split.  We purposefully clear the flag before squeeze bucket,
	 * so that after restart, vacuum shouldn't again try to delete the moved
	 * by split tuples.
	 */
	if (split_cleanup)
	{
		HashPageOpaque bucket_opaque;
		Page		page;

		page = BufferGetPage(bucket_buf);
		bucket_opaque = HashPageGetOpaque(page);

		/* No ereport(ERROR) until changes are logged */
		START_CRIT_SECTION();

		bucket_opaque->hasho_flag &= ~LH_BUCKET_NEEDS_SPLIT_CLEANUP;
		MarkBufferDirty(bucket_buf);

		/* XLOG stuff */
		if (RelationNeedsWAL(rel))
		{
			XLogRecPtr	recptr;

			XLogBeginInsert();
			XLogRegisterBuffer(0, bucket_buf, REGBUF_STANDARD);

			recptr = XLogInsert(RM_HASH_ID, XLOG_HASH_SPLIT_CLEANUP);
			PageSetLSN(page, recptr);
		}

		END_CRIT_SECTION();
	}

	/*
	 * If we have deleted anything, try to compact free space.  For squeezing
	 * the bucket, we must have a cleanup lock, else it can impact the
	 * ordering of tuples for a scan that has started before it.
	 */
	if (bucket_dirty && IsBufferCleanupOK(bucket_buf))
		_hash_squeezebucket(rel, cur_bucket, bucket_blkno, bucket_buf,
							bstrategy);
	else
		LockBuffer(bucket_buf, BUFFER_LOCK_UNLOCK);
}

CompareType
hashtranslatestrategy(StrategyNumber strategy, Oid opfamily)
{
	if (strategy == HTEqualStrategyNumber)
		return COMPARE_EQ;
	return COMPARE_INVALID;
}

StrategyNumber
hashtranslatecmptype(CompareType cmptype, Oid opfamily)
{
	if (cmptype == COMPARE_EQ)
		return HTEqualStrategyNumber;
	return InvalidStrategy;
}
