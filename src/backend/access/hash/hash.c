/*-------------------------------------------------------------------------
 *
 * hash.c
 *	  Implementation of Margo Seltzer's Hashing package for postgres.
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
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
#include "catalog/index.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "optimizer/plancat.h"
#include "utils/builtins.h"
#include "utils/index_selfuncs.h"
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
 * Hash handler function: return IndexAmRoutine with access method parameters
 * and callbacks.
 */
Datum
hashhandler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

	amroutine->amstrategies = HTMaxStrategyNumber;
	amroutine->amsupport = HASHNProcs;
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
	amroutine->ampredlocks = false;
	amroutine->amkeytype = INT4OID;

	amroutine->ambuild = hashbuild;
	amroutine->ambuildempty = hashbuildempty;
	amroutine->aminsert = hashinsert;
	amroutine->ambulkdelete = hashbulkdelete;
	amroutine->amvacuumcleanup = hashvacuumcleanup;
	amroutine->amcanreturn = NULL;
	amroutine->amcostestimate = hashcostestimate;
	amroutine->amoptions = hashoptions;
	amroutine->amproperty = NULL;
	amroutine->amvalidate = hashvalidate;
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
	long		sort_threshold;
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
	sort_threshold = (maintenance_work_mem * 1024L) / BLCKSZ;
	if (index->rd_rel->relpersistence != RELPERSISTENCE_TEMP)
		sort_threshold = Min(sort_threshold, NBuffers);
	else
		sort_threshold = Min(sort_threshold, NLocBuffer);

	if (num_buckets >= (uint32) sort_threshold)
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

	return result;
}

/*
 *	hashbuildempty() -- build an empty hash index in the initialization fork
 */
void
hashbuildempty(Relation index)
{
	_hash_metapinit(index, 0, INIT_FORKNUM);
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
bool
hashinsert(Relation rel, Datum *values, bool *isnull,
		   ItemPointer ht_ctid, Relation heapRel,
		   IndexUniqueCheck checkUnique)
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

	_hash_doinsert(rel, itup);

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
		LockBuffer(so->hashso_curbuf, BUFFER_LOCK_SHARE);

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
		 * for the TID we previously returned.  (Because we hold a pin on the
		 * primary bucket page, no deletions or splits could have occurred;
		 * therefore we can expect that the TID still exists in the current
		 * index page, at an offset >= where we were.)
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
		LockBuffer(so->hashso_curbuf, BUFFER_LOCK_UNLOCK);

	/* Return current heap TID on success */
	scan->xs_ctup.t_self = so->hashso_heappos;

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
	so->hashso_curbuf = InvalidBuffer;
	so->hashso_bucket_buf = InvalidBuffer;
	so->hashso_split_bucket_buf = InvalidBuffer;
	/* set position invalid (this will cause _hash_first call) */
	ItemPointerSetInvalid(&(so->hashso_curpos));
	ItemPointerSetInvalid(&(so->hashso_heappos));

	so->hashso_buc_populated = false;
	so->hashso_buc_split = false;

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

	_hash_dropscanbuf(rel, so);

	/* set position invalid (this will cause _hash_first call) */
	ItemPointerSetInvalid(&(so->hashso_curpos));
	ItemPointerSetInvalid(&(so->hashso_heappos));

	/* Update scan key, if a new one is given */
	if (scankey && scan->numberOfKeys > 0)
	{
		memmove(scan->keyData,
				scankey,
				scan->numberOfKeys * sizeof(ScanKeyData));
	}

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

	_hash_dropscanbuf(rel, so);

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
	/* release the lock, but keep pin */
	LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);

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
		bucket_blkno = BUCKET_TO_BLKNO(&local_metapage, cur_bucket);

		blkno = bucket_blkno;

		/*
		 * We need to acquire a cleanup lock on the primary bucket page to out
		 * wait concurrent scans before deleting the dead tuples.
		 */
		buf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL, info->strategy);
		LockBufferForCleanup(buf);
		_hash_checkpage(rel, buf, LH_BUCKET_PAGE);

		page = BufferGetPage(buf);
		bucket_opaque = (HashPageOpaque) PageGetSpecialPointer(page);

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
			 * (and thus can't be further split), update our cached metapage
			 * data.
			 */
			LockBuffer(metabuf, BUFFER_LOCK_SHARE);
			memcpy(&local_metapage, metap, sizeof(local_metapage));
			LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
		}

		bucket_buf = buf;

		hashbucketcleanup(rel, cur_bucket, bucket_buf, blkno, info->strategy,
						  local_metapage.hashm_maxbucket,
						  local_metapage.hashm_highmask,
						  local_metapage.hashm_lowmask, &tuples_removed,
						  &num_index_tuples, split_cleanup,
						  callback, callback_state);

		_hash_dropbuf(rel, bucket_buf);

		/* Advance to next bucket */
		cur_bucket++;
	}

	/* Write-lock metapage and check for split since we started */
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	metap = HashPageGetMeta(BufferGetPage(metabuf));

	if (cur_maxbucket != metap->hashm_maxbucket)
	{
		/* There's been a split, so process the additional bucket(s) */
		cur_maxbucket = metap->hashm_maxbucket;
		memcpy(&local_metapage, metap, sizeof(local_metapage));
		LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
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

	MarkBufferDirty(metabuf);
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
 * It would be very bad if this function cleaned a page while some other
 * backend was in the midst of scanning it, because hashgettuple assumes
 * that the next valid TID will be greater than or equal to the current
 * valid TID.  There can't be any concurrent scans in progress when we first
 * enter this function because of the cleanup lock we hold on the primary
 * bucket page, but as soon as we release that lock, there might be.  We
 * handle that by conspiring to prevent those scans from passing our cleanup
 * scan.  To do that, we lock the next page in the bucket chain before
 * releasing the lock on the previous page.  (This type of lock chaining is
 * not ideal, so we might want to look for a better solution at some point.)
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
	Bucket new_bucket PG_USED_FOR_ASSERTS_ONLY = InvalidBucket;
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

		vacuum_delay_point();

		page = BufferGetPage(buf);
		opaque = (HashPageOpaque) PageGetSpecialPointer(page);

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
					 * We expect tuples to either belong to curent bucket or
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
			PageIndexMultiDelete(page, deletable, ndeletable);
			bucket_dirty = true;
			MarkBufferDirty(buf);
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
		bucket_opaque = (HashPageOpaque) PageGetSpecialPointer(page);

		bucket_opaque->hasho_flag &= ~LH_BUCKET_NEEDS_SPLIT_CLEANUP;
		MarkBufferDirty(bucket_buf);
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

void
hash_redo(XLogReaderState *record)
{
	elog(PANIC, "hash_redo: unimplemented");
}
