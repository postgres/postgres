/*
 * brin.c
 *		Implementation of BRIN indexes for Postgres
 *
 * See src/backend/access/brin/README for details.
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/brin/brin.c
 *
 * TODO
 *		* ScalarArrayOpExpr (amsearcharray -> SK_SEARCHARRAY)
 */
#include "postgres.h"

#include "access/brin.h"
#include "access/brin_internal.h"
#include "access/brin_page.h"
#include "access/brin_pageops.h"
#include "access/brin_xlog.h"
#include "access/reloptions.h"
#include "access/relscan.h"
#include "access/xact.h"
#include "access/xloginsert.h"
#include "catalog/index.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/freespace.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"


/*
 * We use a BrinBuildState during initial construction of a BRIN index.
 * The running state is kept in a BrinMemTuple.
 */
typedef struct BrinBuildState
{
	Relation	bs_irel;
	int			bs_numtuples;
	Buffer		bs_currentInsertBuf;
	BlockNumber bs_pagesPerRange;
	BlockNumber bs_currRangeStart;
	BrinRevmap *bs_rmAccess;
	BrinDesc   *bs_bdesc;
	BrinMemTuple *bs_dtuple;
} BrinBuildState;

/*
 * Struct used as "opaque" during index scans
 */
typedef struct BrinOpaque
{
	BlockNumber bo_pagesPerRange;
	BrinRevmap *bo_rmAccess;
	BrinDesc   *bo_bdesc;
} BrinOpaque;

static BrinBuildState *initialize_brin_buildstate(Relation idxRel,
						   BrinRevmap *revmap, BlockNumber pagesPerRange);
static void terminate_brin_buildstate(BrinBuildState *state);
static void brinsummarize(Relation index, Relation heapRel,
			  double *numSummarized, double *numExisting);
static void form_and_insert_tuple(BrinBuildState *state);
static void union_tuples(BrinDesc *bdesc, BrinMemTuple *a,
			 BrinTuple *b);
static void brin_vacuum_scan(Relation idxrel, BufferAccessStrategy strategy);


/*
 * A tuple in the heap is being inserted.  To keep a brin index up to date,
 * we need to obtain the relevant index tuple and compare its stored values
 * with those of the new tuple.  If the tuple values are not consistent with
 * the summary tuple, we need to update the index tuple.
 *
 * If the range is not currently summarized (i.e. the revmap returns NULL for
 * it), there's nothing to do.
 */
Datum
brininsert(PG_FUNCTION_ARGS)
{
	Relation	idxRel = (Relation) PG_GETARG_POINTER(0);
	Datum	   *values = (Datum *) PG_GETARG_POINTER(1);
	bool	   *nulls = (bool *) PG_GETARG_POINTER(2);
	ItemPointer heaptid = (ItemPointer) PG_GETARG_POINTER(3);

	/* we ignore the rest of our arguments */
	BlockNumber pagesPerRange;
	BrinDesc   *bdesc = NULL;
	BrinRevmap *revmap;
	Buffer		buf = InvalidBuffer;
	MemoryContext tupcxt = NULL;
	MemoryContext oldcxt = NULL;

	revmap = brinRevmapInitialize(idxRel, &pagesPerRange);

	for (;;)
	{
		bool		need_insert = false;
		OffsetNumber off;
		BrinTuple  *brtup;
		BrinMemTuple *dtup;
		BlockNumber heapBlk;
		int			keyno;

		CHECK_FOR_INTERRUPTS();

		heapBlk = ItemPointerGetBlockNumber(heaptid);
		/* normalize the block number to be the first block in the range */
		heapBlk = (heapBlk / pagesPerRange) * pagesPerRange;
		brtup = brinGetTupleForHeapBlock(revmap, heapBlk, &buf, &off, NULL,
										 BUFFER_LOCK_SHARE);

		/* if range is unsummarized, there's nothing to do */
		if (!brtup)
			break;

		/* First time through? */
		if (bdesc == NULL)
		{
			bdesc = brin_build_desc(idxRel);
			tupcxt = AllocSetContextCreate(CurrentMemoryContext,
										   "brininsert cxt",
										   ALLOCSET_DEFAULT_MINSIZE,
										   ALLOCSET_DEFAULT_INITSIZE,
										   ALLOCSET_DEFAULT_MAXSIZE);
			oldcxt = MemoryContextSwitchTo(tupcxt);
		}

		dtup = brin_deform_tuple(bdesc, brtup);

		/*
		 * Compare the key values of the new tuple to the stored index values;
		 * our deformed tuple will get updated if the new tuple doesn't fit
		 * the original range (note this means we can't break out of the loop
		 * early). Make a note of whether this happens, so that we know to
		 * insert the modified tuple later.
		 */
		for (keyno = 0; keyno < bdesc->bd_tupdesc->natts; keyno++)
		{
			Datum		result;
			BrinValues *bval;
			FmgrInfo   *addValue;

			bval = &dtup->bt_columns[keyno];
			addValue = index_getprocinfo(idxRel, keyno + 1,
										 BRIN_PROCNUM_ADDVALUE);
			result = FunctionCall4Coll(addValue,
									   idxRel->rd_indcollation[keyno],
									   PointerGetDatum(bdesc),
									   PointerGetDatum(bval),
									   values[keyno],
									   nulls[keyno]);
			/* if that returned true, we need to insert the updated tuple */
			need_insert |= DatumGetBool(result);
		}

		if (!need_insert)
		{
			/*
			 * The tuple is consistent with the new values, so there's nothing
			 * to do.
			 */
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		}
		else
		{
			Page		page = BufferGetPage(buf);
			ItemId		lp = PageGetItemId(page, off);
			Size		origsz;
			BrinTuple  *origtup;
			Size		newsz;
			BrinTuple  *newtup;
			bool		samepage;

			/*
			 * Make a copy of the old tuple, so that we can compare it after
			 * re-acquiring the lock.
			 */
			origsz = ItemIdGetLength(lp);
			origtup = brin_copy_tuple(brtup, origsz);

			/*
			 * Before releasing the lock, check if we can attempt a same-page
			 * update.  Another process could insert a tuple concurrently in
			 * the same page though, so downstream we must be prepared to cope
			 * if this turns out to not be possible after all.
			 */
			newtup = brin_form_tuple(bdesc, heapBlk, dtup, &newsz);
			samepage = brin_can_do_samepage_update(buf, origsz, newsz);
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);

			/*
			 * Try to update the tuple.  If this doesn't work for whatever
			 * reason, we need to restart from the top; the revmap might be
			 * pointing at a different tuple for this block now, so we need to
			 * recompute to ensure both our new heap tuple and the other
			 * inserter's are covered by the combined tuple.  It might be that
			 * we don't need to update at all.
			 */
			if (!brin_doupdate(idxRel, pagesPerRange, revmap, heapBlk,
							   buf, off, origtup, origsz, newtup, newsz,
							   samepage))
			{
				/* no luck; start over */
				MemoryContextResetAndDeleteChildren(tupcxt);
				continue;
			}
		}

		/* success! */
		break;
	}

	brinRevmapTerminate(revmap);
	if (BufferIsValid(buf))
		ReleaseBuffer(buf);
	if (bdesc != NULL)
	{
		brin_free_desc(bdesc);
		MemoryContextSwitchTo(oldcxt);
		MemoryContextDelete(tupcxt);
	}

	return BoolGetDatum(false);
}

/*
 * Initialize state for a BRIN index scan.
 *
 * We read the metapage here to determine the pages-per-range number that this
 * index was built with.  Note that since this cannot be changed while we're
 * holding lock on index, it's not necessary to recompute it during brinrescan.
 */
Datum
brinbeginscan(PG_FUNCTION_ARGS)
{
	Relation	r = (Relation) PG_GETARG_POINTER(0);
	int			nkeys = PG_GETARG_INT32(1);
	int			norderbys = PG_GETARG_INT32(2);
	IndexScanDesc scan;
	BrinOpaque *opaque;

	scan = RelationGetIndexScan(r, nkeys, norderbys);

	opaque = (BrinOpaque *) palloc(sizeof(BrinOpaque));
	opaque->bo_rmAccess = brinRevmapInitialize(r, &opaque->bo_pagesPerRange);
	opaque->bo_bdesc = brin_build_desc(r);
	scan->opaque = opaque;

	PG_RETURN_POINTER(scan);
}

/*
 * Execute the index scan.
 *
 * This works by reading index TIDs from the revmap, and obtaining the index
 * tuples pointed to by them; the summary values in the index tuples are
 * compared to the scan keys.  We return into the TID bitmap all the pages in
 * ranges corresponding to index tuples that match the scan keys.
 *
 * If a TID from the revmap is read as InvalidTID, we know that range is
 * unsummarized.  Pages in those ranges need to be returned regardless of scan
 * keys.
 */
Datum
bringetbitmap(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	TIDBitmap  *tbm = (TIDBitmap *) PG_GETARG_POINTER(1);
	Relation	idxRel = scan->indexRelation;
	Buffer		buf = InvalidBuffer;
	BrinDesc   *bdesc;
	Oid			heapOid;
	Relation	heapRel;
	BrinOpaque *opaque;
	BlockNumber nblocks;
	BlockNumber heapBlk;
	int			totalpages = 0;
	FmgrInfo   *consistentFn;
	MemoryContext oldcxt;
	MemoryContext perRangeCxt;

	opaque = (BrinOpaque *) scan->opaque;
	bdesc = opaque->bo_bdesc;
	pgstat_count_index_scan(idxRel);

	/*
	 * We need to know the size of the table so that we know how long to
	 * iterate on the revmap.
	 */
	heapOid = IndexGetRelation(RelationGetRelid(idxRel), false);
	heapRel = heap_open(heapOid, AccessShareLock);
	nblocks = RelationGetNumberOfBlocks(heapRel);
	heap_close(heapRel, AccessShareLock);

	/*
	 * Make room for the consistent support procedures of indexed columns.  We
	 * don't look them up here; we do that lazily the first time we see a scan
	 * key reference each of them.  We rely on zeroing fn_oid to InvalidOid.
	 */
	consistentFn = palloc0(sizeof(FmgrInfo) * bdesc->bd_tupdesc->natts);

	/*
	 * Setup and use a per-range memory context, which is reset every time we
	 * loop below.  This avoids having to free the tuples within the loop.
	 */
	perRangeCxt = AllocSetContextCreate(CurrentMemoryContext,
										"bringetbitmap cxt",
										ALLOCSET_DEFAULT_MINSIZE,
										ALLOCSET_DEFAULT_INITSIZE,
										ALLOCSET_DEFAULT_MAXSIZE);
	oldcxt = MemoryContextSwitchTo(perRangeCxt);

	/*
	 * Now scan the revmap.  We start by querying for heap page 0,
	 * incrementing by the number of pages per range; this gives us a full
	 * view of the table.
	 */
	for (heapBlk = 0; heapBlk < nblocks; heapBlk += opaque->bo_pagesPerRange)
	{
		bool		addrange;
		BrinTuple  *tup;
		OffsetNumber off;
		Size		size;

		CHECK_FOR_INTERRUPTS();

		MemoryContextResetAndDeleteChildren(perRangeCxt);

		tup = brinGetTupleForHeapBlock(opaque->bo_rmAccess, heapBlk, &buf,
									   &off, &size, BUFFER_LOCK_SHARE);
		if (tup)
		{
			tup = brin_copy_tuple(tup, size);
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		}

		/*
		 * For page ranges with no indexed tuple, we must return the whole
		 * range; otherwise, compare it to the scan keys.
		 */
		if (tup == NULL)
		{
			addrange = true;
		}
		else
		{
			BrinMemTuple *dtup;

			dtup = brin_deform_tuple(bdesc, tup);
			if (dtup->bt_placeholder)
			{
				/*
				 * Placeholder tuples are always returned, regardless of the
				 * values stored in them.
				 */
				addrange = true;
			}
			else
			{
				int			keyno;

				/*
				 * Compare scan keys with summary values stored for the range.
				 * If scan keys are matched, the page range must be added to
				 * the bitmap.  We initially assume the range needs to be
				 * added; in particular this serves the case where there are
				 * no keys.
				 */
				addrange = true;
				for (keyno = 0; keyno < scan->numberOfKeys; keyno++)
				{
					ScanKey		key = &scan->keyData[keyno];
					AttrNumber	keyattno = key->sk_attno;
					BrinValues *bval = &dtup->bt_columns[keyattno - 1];
					Datum		add;

					/*
					 * The collation of the scan key must match the collation
					 * used in the index column (but only if the search is not
					 * IS NULL/ IS NOT NULL).  Otherwise we shouldn't be using
					 * this index ...
					 */
					Assert((key->sk_flags & SK_ISNULL) ||
						   (key->sk_collation ==
					  bdesc->bd_tupdesc->attrs[keyattno - 1]->attcollation));

					/* First time this column? look up consistent function */
					if (consistentFn[keyattno - 1].fn_oid == InvalidOid)
					{
						FmgrInfo   *tmp;

						tmp = index_getprocinfo(idxRel, keyattno,
												BRIN_PROCNUM_CONSISTENT);
						fmgr_info_copy(&consistentFn[keyattno - 1], tmp,
									   CurrentMemoryContext);
					}

					/*
					 * Check whether the scan key is consistent with the page
					 * range values; if so, have the pages in the range added
					 * to the output bitmap.
					 *
					 * When there are multiple scan keys, failure to meet the
					 * criteria for a single one of them is enough to discard
					 * the range as a whole, so break out of the loop as soon
					 * as a false return value is obtained.
					 */
					add = FunctionCall3Coll(&consistentFn[keyattno - 1],
											key->sk_collation,
											PointerGetDatum(bdesc),
											PointerGetDatum(bval),
											PointerGetDatum(key));
					addrange = DatumGetBool(add);
					if (!addrange)
						break;
				}
			}
		}

		/* add the pages in the range to the output bitmap, if needed */
		if (addrange)
		{
			BlockNumber pageno;

			for (pageno = heapBlk;
				 pageno <= heapBlk + opaque->bo_pagesPerRange - 1;
				 pageno++)
			{
				MemoryContextSwitchTo(oldcxt);
				tbm_add_page(tbm, pageno);
				totalpages++;
				MemoryContextSwitchTo(perRangeCxt);
			}
		}
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(perRangeCxt);

	if (buf != InvalidBuffer)
		ReleaseBuffer(buf);

	/*
	 * XXX We have an approximation of the number of *pages* that our scan
	 * returns, but we don't have a precise idea of the number of heap tuples
	 * involved.
	 */
	PG_RETURN_INT64(totalpages * 10);
}

/*
 * Re-initialize state for a BRIN index scan
 */
Datum
brinrescan(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ScanKey		scankey = (ScanKey) PG_GETARG_POINTER(1);

	/* other arguments ignored */

	/*
	 * Other index AMs preprocess the scan keys at this point, or sometime
	 * early during the scan; this lets them optimize by removing redundant
	 * keys, or doing early returns when they are impossible to satisfy; see
	 * _bt_preprocess_keys for an example.  Something like that could be added
	 * here someday, too.
	 */

	if (scankey && scan->numberOfKeys > 0)
		memmove(scan->keyData, scankey,
				scan->numberOfKeys * sizeof(ScanKeyData));

	PG_RETURN_VOID();
}

/*
 * Close down a BRIN index scan
 */
Datum
brinendscan(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	BrinOpaque *opaque = (BrinOpaque *) scan->opaque;

	brinRevmapTerminate(opaque->bo_rmAccess);
	brin_free_desc(opaque->bo_bdesc);
	pfree(opaque);

	PG_RETURN_VOID();
}

Datum
brinmarkpos(PG_FUNCTION_ARGS)
{
	elog(ERROR, "BRIN does not support mark/restore");
	PG_RETURN_VOID();
}

Datum
brinrestrpos(PG_FUNCTION_ARGS)
{
	elog(ERROR, "BRIN does not support mark/restore");
	PG_RETURN_VOID();
}

/*
 * Per-heap-tuple callback for IndexBuildHeapScan.
 *
 * Note we don't worry about the page range at the end of the table here; it is
 * present in the build state struct after we're called the last time, but not
 * inserted into the index.  Caller must ensure to do so, if appropriate.
 */
static void
brinbuildCallback(Relation index,
				  HeapTuple htup,
				  Datum *values,
				  bool *isnull,
				  bool tupleIsAlive,
				  void *brstate)
{
	BrinBuildState *state = (BrinBuildState *) brstate;
	BlockNumber thisblock;
	int			i;

	thisblock = ItemPointerGetBlockNumber(&htup->t_self);

	/*
	 * If we're in a block that belongs to a future range, summarize what
	 * we've got and start afresh.  Note the scan might have skipped many
	 * pages, if they were devoid of live tuples; make sure to insert index
	 * tuples for those too.
	 */
	while (thisblock > state->bs_currRangeStart + state->bs_pagesPerRange - 1)
	{

		BRIN_elog((DEBUG2,
				   "brinbuildCallback: completed a range: %u--%u",
				   state->bs_currRangeStart,
				   state->bs_currRangeStart + state->bs_pagesPerRange));

		/* create the index tuple and insert it */
		form_and_insert_tuple(state);

		/* set state to correspond to the next range */
		state->bs_currRangeStart += state->bs_pagesPerRange;

		/* re-initialize state for it */
		brin_memtuple_initialize(state->bs_dtuple, state->bs_bdesc);
	}

	/* Accumulate the current tuple into the running state */
	for (i = 0; i < state->bs_bdesc->bd_tupdesc->natts; i++)
	{
		FmgrInfo   *addValue;
		BrinValues *col;

		col = &state->bs_dtuple->bt_columns[i];
		addValue = index_getprocinfo(index, i + 1,
									 BRIN_PROCNUM_ADDVALUE);

		/*
		 * Update dtuple state, if and as necessary.
		 */
		FunctionCall4Coll(addValue,
						  state->bs_bdesc->bd_tupdesc->attrs[i]->attcollation,
						  PointerGetDatum(state->bs_bdesc),
						  PointerGetDatum(col),
						  values[i], isnull[i]);
	}
}

/*
 * brinbuild() -- build a new BRIN index.
 */
Datum
brinbuild(PG_FUNCTION_ARGS)
{
	Relation	heap = (Relation) PG_GETARG_POINTER(0);
	Relation	index = (Relation) PG_GETARG_POINTER(1);
	IndexInfo  *indexInfo = (IndexInfo *) PG_GETARG_POINTER(2);
	IndexBuildResult *result;
	double		reltuples;
	double		idxtuples;
	BrinRevmap *revmap;
	BrinBuildState *state;
	Buffer		meta;
	BlockNumber pagesPerRange;

	/*
	 * We expect to be called exactly once for any index relation.
	 */
	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index \"%s\" already contains data",
			 RelationGetRelationName(index));

	/*
	 * Critical section not required, because on error the creation of the
	 * whole relation will be rolled back.
	 */

	meta = ReadBuffer(index, P_NEW);
	Assert(BufferGetBlockNumber(meta) == BRIN_METAPAGE_BLKNO);
	LockBuffer(meta, BUFFER_LOCK_EXCLUSIVE);

	brin_metapage_init(BufferGetPage(meta), BrinGetPagesPerRange(index),
					   BRIN_CURRENT_VERSION);
	MarkBufferDirty(meta);

	if (RelationNeedsWAL(index))
	{
		xl_brin_createidx xlrec;
		XLogRecPtr	recptr;
		Page		page;

		xlrec.version = BRIN_CURRENT_VERSION;
		xlrec.pagesPerRange = BrinGetPagesPerRange(index);

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfBrinCreateIdx);
		XLogRegisterBuffer(0, meta, REGBUF_WILL_INIT);

		recptr = XLogInsert(RM_BRIN_ID, XLOG_BRIN_CREATE_INDEX);

		page = BufferGetPage(meta);
		PageSetLSN(page, recptr);
	}

	UnlockReleaseBuffer(meta);

	/*
	 * Initialize our state, including the deformed tuple state.
	 */
	revmap = brinRevmapInitialize(index, &pagesPerRange);
	state = initialize_brin_buildstate(index, revmap, pagesPerRange);

	/*
	 * Now scan the relation.  No syncscan allowed here because we want the
	 * heap blocks in physical order.
	 */
	reltuples = IndexBuildHeapScan(heap, index, indexInfo, false,
								   brinbuildCallback, (void *) state);

	/* process the final batch */
	form_and_insert_tuple(state);

	/* release resources */
	idxtuples = state->bs_numtuples;
	brinRevmapTerminate(state->bs_rmAccess);
	terminate_brin_buildstate(state);

	/*
	 * Return statistics
	 */
	result = (IndexBuildResult *) palloc(sizeof(IndexBuildResult));

	result->heap_tuples = reltuples;
	result->index_tuples = idxtuples;

	PG_RETURN_POINTER(result);
}

Datum
brinbuildempty(PG_FUNCTION_ARGS)
{
	Relation	index = (Relation) PG_GETARG_POINTER(0);
	Buffer		metabuf;

	/* An empty BRIN index has a metapage only. */
	metabuf =
		ReadBufferExtended(index, INIT_FORKNUM, P_NEW, RBM_NORMAL, NULL);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

	/* Initialize and xlog metabuffer. */
	START_CRIT_SECTION();
	brin_metapage_init(BufferGetPage(metabuf), BrinGetPagesPerRange(index),
					   BRIN_CURRENT_VERSION);
	MarkBufferDirty(metabuf);
	log_newpage_buffer(metabuf, false);
	END_CRIT_SECTION();

	UnlockReleaseBuffer(metabuf);

	PG_RETURN_VOID();
}

/*
 * brinbulkdelete
 *		Since there are no per-heap-tuple index tuples in BRIN indexes,
 *		there's not a lot we can do here.
 *
 * XXX we could mark item tuples as "dirty" (when a minimum or maximum heap
 * tuple is deleted), meaning the need to re-run summarization on the affected
 * range.  Would need to add an extra flag in brintuples for that.
 */
Datum
brinbulkdelete(PG_FUNCTION_ARGS)
{
	/* other arguments are not currently used */
	IndexBulkDeleteResult *stats =
	(IndexBulkDeleteResult *) PG_GETARG_POINTER(1);

	/* allocate stats if first time through, else re-use existing struct */
	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

	PG_RETURN_POINTER(stats);
}

/*
 * This routine is in charge of "vacuuming" a BRIN index: we just summarize
 * ranges that are currently unsummarized.
 */
Datum
brinvacuumcleanup(PG_FUNCTION_ARGS)
{
	IndexVacuumInfo *info = (IndexVacuumInfo *) PG_GETARG_POINTER(0);
	IndexBulkDeleteResult *stats =
	(IndexBulkDeleteResult *) PG_GETARG_POINTER(1);
	Relation	heapRel;

	/* No-op in ANALYZE ONLY mode */
	if (info->analyze_only)
		PG_RETURN_POINTER(stats);

	if (!stats)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
	stats->num_pages = RelationGetNumberOfBlocks(info->index);
	/* rest of stats is initialized by zeroing */

	heapRel = heap_open(IndexGetRelation(RelationGetRelid(info->index), false),
						AccessShareLock);

	brin_vacuum_scan(info->index, info->strategy);

	brinsummarize(info->index, heapRel,
				  &stats->num_index_tuples, &stats->num_index_tuples);

	heap_close(heapRel, AccessShareLock);

	PG_RETURN_POINTER(stats);
}

/*
 * reloptions processor for BRIN indexes
 */
Datum
brinoptions(PG_FUNCTION_ARGS)
{
	Datum		reloptions = PG_GETARG_DATUM(0);
	bool		validate = PG_GETARG_BOOL(1);
	relopt_value *options;
	BrinOptions *rdopts;
	int			numoptions;
	static const relopt_parse_elt tab[] = {
		{"pages_per_range", RELOPT_TYPE_INT, offsetof(BrinOptions, pagesPerRange)}
	};

	options = parseRelOptions(reloptions, validate, RELOPT_KIND_BRIN,
							  &numoptions);

	/* if none set, we're done */
	if (numoptions == 0)
		PG_RETURN_NULL();

	rdopts = allocateReloptStruct(sizeof(BrinOptions), options, numoptions);

	fillRelOptions((void *) rdopts, sizeof(BrinOptions), options, numoptions,
				   validate, tab, lengthof(tab));

	pfree(options);

	PG_RETURN_BYTEA_P(rdopts);
}

/*
 * SQL-callable function to scan through an index and summarize all ranges
 * that are not currently summarized.
 */
Datum
brin_summarize_new_values(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	Oid			heapoid;
	Relation	indexRel;
	Relation	heapRel;
	double		numSummarized = 0;

	/*
	 * We must lock table before index to avoid deadlocks.  However, if the
	 * passed indexoid isn't an index then IndexGetRelation() will fail.
	 * Rather than emitting a not-very-helpful error message, postpone
	 * complaining, expecting that the is-it-an-index test below will fail.
	 */
	heapoid = IndexGetRelation(indexoid, true);
	if (OidIsValid(heapoid))
		heapRel = heap_open(heapoid, ShareUpdateExclusiveLock);
	else
		heapRel = NULL;

	indexRel = index_open(indexoid, ShareUpdateExclusiveLock);

	/* Must be a BRIN index */
	if (indexRel->rd_rel->relkind != RELKIND_INDEX ||
		indexRel->rd_rel->relam != BRIN_AM_OID)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a BRIN index",
						RelationGetRelationName(indexRel))));

	/* User must own the index (comparable to privileges needed for VACUUM) */
	if (!pg_class_ownercheck(indexoid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
					   RelationGetRelationName(indexRel));

	/*
	 * Since we did the IndexGetRelation call above without any lock, it's
	 * barely possible that a race against an index drop/recreation could have
	 * netted us the wrong table.  Recheck.
	 */
	if (heapRel == NULL || heapoid != IndexGetRelation(indexoid, false))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("could not open parent table of index %s",
						RelationGetRelationName(indexRel))));

	/* OK, do it */
	brinsummarize(indexRel, heapRel, &numSummarized, NULL);

	relation_close(indexRel, ShareUpdateExclusiveLock);
	relation_close(heapRel, ShareUpdateExclusiveLock);

	PG_RETURN_INT32((int32) numSummarized);
}

/*
 * Build a BrinDesc used to create or scan a BRIN index
 */
BrinDesc *
brin_build_desc(Relation rel)
{
	BrinOpcInfo **opcinfo;
	BrinDesc   *bdesc;
	TupleDesc	tupdesc;
	int			totalstored = 0;
	int			keyno;
	long		totalsize;
	MemoryContext cxt;
	MemoryContext oldcxt;

	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"brin desc cxt",
								ALLOCSET_SMALL_INITSIZE,
								ALLOCSET_SMALL_MINSIZE,
								ALLOCSET_SMALL_MAXSIZE);
	oldcxt = MemoryContextSwitchTo(cxt);
	tupdesc = RelationGetDescr(rel);

	/*
	 * Obtain BrinOpcInfo for each indexed column.  While at it, accumulate
	 * the number of columns stored, since the number is opclass-defined.
	 */
	opcinfo = (BrinOpcInfo **) palloc(sizeof(BrinOpcInfo *) * tupdesc->natts);
	for (keyno = 0; keyno < tupdesc->natts; keyno++)
	{
		FmgrInfo   *opcInfoFn;

		opcInfoFn = index_getprocinfo(rel, keyno + 1, BRIN_PROCNUM_OPCINFO);

		opcinfo[keyno] = (BrinOpcInfo *)
			DatumGetPointer(FunctionCall1(opcInfoFn,
										  tupdesc->attrs[keyno]->atttypid));
		totalstored += opcinfo[keyno]->oi_nstored;
	}

	/* Allocate our result struct and fill it in */
	totalsize = offsetof(BrinDesc, bd_info) +
		sizeof(BrinOpcInfo *) * tupdesc->natts;

	bdesc = palloc(totalsize);
	bdesc->bd_context = cxt;
	bdesc->bd_index = rel;
	bdesc->bd_tupdesc = tupdesc;
	bdesc->bd_disktdesc = NULL; /* generated lazily */
	bdesc->bd_totalstored = totalstored;

	for (keyno = 0; keyno < tupdesc->natts; keyno++)
		bdesc->bd_info[keyno] = opcinfo[keyno];
	pfree(opcinfo);

	MemoryContextSwitchTo(oldcxt);

	return bdesc;
}

void
brin_free_desc(BrinDesc *bdesc)
{
	/* make sure the tupdesc is still valid */
	Assert(bdesc->bd_tupdesc->tdrefcount >= 1);
	/* no need for retail pfree */
	MemoryContextDelete(bdesc->bd_context);
}

/*
 * Initialize a BrinBuildState appropriate to create tuples on the given index.
 */
static BrinBuildState *
initialize_brin_buildstate(Relation idxRel, BrinRevmap *revmap,
						   BlockNumber pagesPerRange)
{
	BrinBuildState *state;

	state = palloc(sizeof(BrinBuildState));

	state->bs_irel = idxRel;
	state->bs_numtuples = 0;
	state->bs_currentInsertBuf = InvalidBuffer;
	state->bs_pagesPerRange = pagesPerRange;
	state->bs_currRangeStart = 0;
	state->bs_rmAccess = revmap;
	state->bs_bdesc = brin_build_desc(idxRel);
	state->bs_dtuple = brin_new_memtuple(state->bs_bdesc);

	brin_memtuple_initialize(state->bs_dtuple, state->bs_bdesc);

	return state;
}

/*
 * Release resources associated with a BrinBuildState.
 */
static void
terminate_brin_buildstate(BrinBuildState *state)
{
	/* release the last index buffer used */
	if (!BufferIsInvalid(state->bs_currentInsertBuf))
	{
		Page		page;

		page = BufferGetPage(state->bs_currentInsertBuf);
		RecordPageWithFreeSpace(state->bs_irel,
							BufferGetBlockNumber(state->bs_currentInsertBuf),
								PageGetFreeSpace(page));
		ReleaseBuffer(state->bs_currentInsertBuf);
	}

	brin_free_desc(state->bs_bdesc);
	pfree(state->bs_dtuple);
	pfree(state);
}

/*
 * Summarize the given page range of the given index.
 *
 * This routine can run in parallel with insertions into the heap.  To avoid
 * missing those values from the summary tuple, we first insert a placeholder
 * index tuple into the index, then execute the heap scan; transactions
 * concurrent with the scan update the placeholder tuple.  After the scan, we
 * union the placeholder tuple with the one computed by this routine.  The
 * update of the index value happens in a loop, so that if somebody updates
 * the placeholder tuple after we read it, we detect the case and try again.
 * This ensures that the concurrently inserted tuples are not lost.
 */
static void
summarize_range(IndexInfo *indexInfo, BrinBuildState *state, Relation heapRel,
				BlockNumber heapBlk, BlockNumber heapNumBlks)
{
	Buffer		phbuf;
	BrinTuple  *phtup;
	Size		phsz;
	OffsetNumber offset;
	BlockNumber scanNumBlks;

	/*
	 * Insert the placeholder tuple
	 */
	phbuf = InvalidBuffer;
	phtup = brin_form_placeholder_tuple(state->bs_bdesc, heapBlk, &phsz);
	offset = brin_doinsert(state->bs_irel, state->bs_pagesPerRange,
						   state->bs_rmAccess, &phbuf,
						   heapBlk, phtup, phsz);

	/*
	 * Execute the partial heap scan covering the heap blocks in the specified
	 * page range, summarizing the heap tuples in it.  This scan stops just
	 * short of brinbuildCallback creating the new index entry.
	 *
	 * Note that it is critical we use the "any visible" mode of
	 * IndexBuildHeapRangeScan here: otherwise, we would miss tuples inserted
	 * by transactions that are still in progress, among other corner cases.
	 */
	state->bs_currRangeStart = heapBlk;
	scanNumBlks = heapBlk + state->bs_pagesPerRange <= heapNumBlks ?
		state->bs_pagesPerRange : heapNumBlks - heapBlk;
	IndexBuildHeapRangeScan(heapRel, state->bs_irel, indexInfo, false, true,
							heapBlk, scanNumBlks,
							brinbuildCallback, (void *) state);

	/*
	 * Now we update the values obtained by the scan with the placeholder
	 * tuple.  We do this in a loop which only terminates if we're able to
	 * update the placeholder tuple successfully; if we are not, this means
	 * somebody else modified the placeholder tuple after we read it.
	 */
	for (;;)
	{
		BrinTuple  *newtup;
		Size		newsize;
		bool		didupdate;
		bool		samepage;

		CHECK_FOR_INTERRUPTS();

		/*
		 * Update the summary tuple and try to update.
		 */
		newtup = brin_form_tuple(state->bs_bdesc,
								 heapBlk, state->bs_dtuple, &newsize);
		samepage = brin_can_do_samepage_update(phbuf, phsz, newsize);
		didupdate =
			brin_doupdate(state->bs_irel, state->bs_pagesPerRange,
						  state->bs_rmAccess, heapBlk, phbuf, offset,
						  phtup, phsz, newtup, newsize, samepage);
		brin_free_tuple(phtup);
		brin_free_tuple(newtup);

		/* If the update succeeded, we're done. */
		if (didupdate)
			break;

		/*
		 * If the update didn't work, it might be because somebody updated the
		 * placeholder tuple concurrently.  Extract the new version, union it
		 * with the values we have from the scan, and start over.  (There are
		 * other reasons for the update to fail, but it's simple to treat them
		 * the same.)
		 */
		phtup = brinGetTupleForHeapBlock(state->bs_rmAccess, heapBlk, &phbuf,
										 &offset, &phsz, BUFFER_LOCK_SHARE);
		/* the placeholder tuple must exist */
		if (phtup == NULL)
			elog(ERROR, "missing placeholder tuple");
		phtup = brin_copy_tuple(phtup, phsz);
		LockBuffer(phbuf, BUFFER_LOCK_UNLOCK);

		/* merge it into the tuple from the heap scan */
		union_tuples(state->bs_bdesc, state->bs_dtuple, phtup);
	}

	ReleaseBuffer(phbuf);
}

/*
 * Scan a complete BRIN index, and summarize each page range that's not already
 * summarized.  The index and heap must have been locked by caller in at
 * least ShareUpdateExclusiveLock mode.
 *
 * For each new index tuple inserted, *numSummarized (if not NULL) is
 * incremented; for each existing tuple, *numExisting (if not NULL) is
 * incremented.
 */
static void
brinsummarize(Relation index, Relation heapRel, double *numSummarized,
			  double *numExisting)
{
	BrinRevmap *revmap;
	BrinBuildState *state = NULL;
	IndexInfo  *indexInfo = NULL;
	BlockNumber heapNumBlocks;
	BlockNumber heapBlk;
	BlockNumber pagesPerRange;
	Buffer		buf;

	revmap = brinRevmapInitialize(index, &pagesPerRange);

	/*
	 * Scan the revmap to find unsummarized items.
	 */
	buf = InvalidBuffer;
	heapNumBlocks = RelationGetNumberOfBlocks(heapRel);
	for (heapBlk = 0; heapBlk < heapNumBlocks; heapBlk += pagesPerRange)
	{
		BrinTuple  *tup;
		OffsetNumber off;

		CHECK_FOR_INTERRUPTS();

		tup = brinGetTupleForHeapBlock(revmap, heapBlk, &buf, &off, NULL,
									   BUFFER_LOCK_SHARE);
		if (tup == NULL)
		{
			/* no revmap entry for this heap range. Summarize it. */
			if (state == NULL)
			{
				/* first time through */
				Assert(!indexInfo);
				state = initialize_brin_buildstate(index, revmap,
												   pagesPerRange);
				indexInfo = BuildIndexInfo(index);
			}
			summarize_range(indexInfo, state, heapRel, heapBlk, heapNumBlocks);

			/* and re-initialize state for the next range */
			brin_memtuple_initialize(state->bs_dtuple, state->bs_bdesc);

			if (numSummarized)
				*numSummarized += 1.0;
		}
		else
		{
			if (numExisting)
				*numExisting += 1.0;
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		}
	}

	if (BufferIsValid(buf))
		ReleaseBuffer(buf);

	/* free resources */
	brinRevmapTerminate(revmap);
	if (state)
	{
		terminate_brin_buildstate(state);
		pfree(indexInfo);
	}
}

/*
 * Given a deformed tuple in the build state, convert it into the on-disk
 * format and insert it into the index, making the revmap point to it.
 */
static void
form_and_insert_tuple(BrinBuildState *state)
{
	BrinTuple  *tup;
	Size		size;

	tup = brin_form_tuple(state->bs_bdesc, state->bs_currRangeStart,
						  state->bs_dtuple, &size);
	brin_doinsert(state->bs_irel, state->bs_pagesPerRange, state->bs_rmAccess,
				  &state->bs_currentInsertBuf, state->bs_currRangeStart,
				  tup, size);
	state->bs_numtuples++;

	pfree(tup);
}

/*
 * Given two deformed tuples, adjust the first one so that it's consistent
 * with the summary values in both.
 */
static void
union_tuples(BrinDesc *bdesc, BrinMemTuple *a, BrinTuple *b)
{
	int			keyno;
	BrinMemTuple *db;
	MemoryContext cxt;
	MemoryContext oldcxt;

	/* Use our own memory context to avoid retail pfree */
	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"brin union",
								ALLOCSET_DEFAULT_MINSIZE,
								ALLOCSET_DEFAULT_INITSIZE,
								ALLOCSET_DEFAULT_MAXSIZE);
	oldcxt = MemoryContextSwitchTo(cxt);
	db = brin_deform_tuple(bdesc, b);
	MemoryContextSwitchTo(oldcxt);

	for (keyno = 0; keyno < bdesc->bd_tupdesc->natts; keyno++)
	{
		FmgrInfo   *unionFn;
		BrinValues *col_a = &a->bt_columns[keyno];
		BrinValues *col_b = &db->bt_columns[keyno];

		unionFn = index_getprocinfo(bdesc->bd_index, keyno + 1,
									BRIN_PROCNUM_UNION);
		FunctionCall3Coll(unionFn,
						  bdesc->bd_index->rd_indcollation[keyno],
						  PointerGetDatum(bdesc),
						  PointerGetDatum(col_a),
						  PointerGetDatum(col_b));
	}

	MemoryContextDelete(cxt);
}

/*
 * brin_vacuum_scan
 *		Do a complete scan of the index during VACUUM.
 *
 * This routine scans the complete index looking for uncatalogued index pages,
 * i.e. those that might have been lost due to a crash after index extension
 * and such.
 */
static void
brin_vacuum_scan(Relation idxrel, BufferAccessStrategy strategy)
{
	bool		vacuum_fsm = false;
	BlockNumber blkno;

	/*
	 * Scan the index in physical order, and clean up any possible mess in
	 * each page.
	 */
	for (blkno = 0; blkno < RelationGetNumberOfBlocks(idxrel); blkno++)
	{
		Buffer		buf;

		CHECK_FOR_INTERRUPTS();

		buf = ReadBufferExtended(idxrel, MAIN_FORKNUM, blkno,
								 RBM_NORMAL, strategy);

		vacuum_fsm |= brin_page_cleanup(idxrel, buf);

		ReleaseBuffer(buf);
	}

	/*
	 * If we made any change to the FSM, make sure the new info is visible all
	 * the way to the top.
	 */
	if (vacuum_fsm)
		FreeSpaceMapVacuum(idxrel);
}
