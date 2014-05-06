/*
 * contrib/pgstattuple/pgstatindex.c
 *
 *
 * pgstatindex
 *
 * Copyright (c) 2006 Satoshi Nagayasu <nagayasus@nttdata.co.jp>
 *
 * Permission to use, copy, modify, and distribute this software and
 * its documentation for any purpose, without fee, and without a
 * written agreement is hereby granted, provided that the above
 * copyright notice and this paragraph and the following two
 * paragraphs appear in all copies.
 *
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE TO ANY PARTY FOR DIRECT,
 * INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
 * LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
 * DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA HAS BEEN ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * THE AUTHOR SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS
 * IS" BASIS, AND THE AUTHOR HAS NO OBLIGATIONS TO PROVIDE MAINTENANCE,
 * SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 */

#include "postgres.h"

#include "access/gin_private.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "catalog/namespace.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"
#include "utils/rel.h"


/*
 * Because of backward-compatibility issue, we have decided to have
 * two types of interfaces, with regclass-type input arg and text-type
 * input arg, for each function.
 *
 * Those functions which have text-type input arg will be deprecated
 * in the future release.
 */
PG_FUNCTION_INFO_V1(pgstatindex);
PG_FUNCTION_INFO_V1(pgstatindexbyid);
PG_FUNCTION_INFO_V1(pg_relpages);
PG_FUNCTION_INFO_V1(pg_relpagesbyid);
PG_FUNCTION_INFO_V1(pgstatginindex);

#define IS_INDEX(r) ((r)->rd_rel->relkind == RELKIND_INDEX)
#define IS_BTREE(r) ((r)->rd_rel->relam == BTREE_AM_OID)
#define IS_GIN(r) ((r)->rd_rel->relam == GIN_AM_OID)

#define CHECK_PAGE_OFFSET_RANGE(pg, offnum) { \
		if ( !(FirstOffsetNumber <= (offnum) && \
						(offnum) <= PageGetMaxOffsetNumber(pg)) ) \
			 elog(ERROR, "page offset number out of range"); }

/* note: BlockNumber is unsigned, hence can't be negative */
#define CHECK_RELATION_BLOCK_RANGE(rel, blkno) { \
		if ( RelationGetNumberOfBlocks(rel) <= (BlockNumber) (blkno) ) \
			 elog(ERROR, "block number out of range"); }

/* ------------------------------------------------
 * A structure for a whole btree index statistics
 * used by pgstatindex().
 * ------------------------------------------------
 */
typedef struct BTIndexStat
{
	uint32		version;
	uint32		level;
	BlockNumber root_blkno;

	uint64		root_pages;
	uint64		internal_pages;
	uint64		leaf_pages;
	uint64		empty_pages;
	uint64		deleted_pages;

	uint64		max_avail;
	uint64		free_space;

	uint64		fragments;
} BTIndexStat;

/* ------------------------------------------------
 * A structure for a whole GIN index statistics
 * used by pgstatginindex().
 * ------------------------------------------------
 */
typedef struct GinIndexStat
{
	int32		version;

	BlockNumber pending_pages;
	int64		pending_tuples;
} GinIndexStat;

static Datum pgstatindex_impl(Relation rel, FunctionCallInfo fcinfo);

/* ------------------------------------------------------
 * pgstatindex()
 *
 * Usage: SELECT * FROM pgstatindex('t1_pkey');
 * ------------------------------------------------------
 */
Datum
pgstatindex(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_P(0);
	Relation	rel;
	RangeVar   *relrv;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to use pgstattuple functions"))));

	relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname));
	rel = relation_openrv(relrv, AccessShareLock);

	PG_RETURN_DATUM(pgstatindex_impl(rel, fcinfo));
}

Datum
pgstatindexbyid(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to use pgstattuple functions"))));

	rel = relation_open(relid, AccessShareLock);

	PG_RETURN_DATUM(pgstatindex_impl(rel, fcinfo));
}

static Datum
pgstatindex_impl(Relation rel, FunctionCallInfo fcinfo)
{
	Datum		result;
	BlockNumber nblocks;
	BlockNumber blkno;
	BTIndexStat indexStat;
	BufferAccessStrategy bstrategy = GetAccessStrategy(BAS_BULKREAD);

	if (!IS_INDEX(rel) || !IS_BTREE(rel))
		elog(ERROR, "relation \"%s\" is not a btree index",
			 RelationGetRelationName(rel));

	/*
	 * Reject attempts to read non-local temporary relations; we would be
	 * likely to get wrong data since we have no visibility into the owning
	 * session's local buffers.
	 */
	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot access temporary tables of other sessions")));

	/*
	 * Read metapage
	 */
	{
		Buffer		buffer = ReadBufferExtended(rel, MAIN_FORKNUM, 0, RBM_NORMAL, bstrategy);
		Page		page = BufferGetPage(buffer);
		BTMetaPageData *metad = BTPageGetMeta(page);

		indexStat.version = metad->btm_version;
		indexStat.level = metad->btm_level;
		indexStat.root_blkno = metad->btm_root;

		ReleaseBuffer(buffer);
	}

	/* -- init counters -- */
	indexStat.root_pages = 0;
	indexStat.internal_pages = 0;
	indexStat.leaf_pages = 0;
	indexStat.empty_pages = 0;
	indexStat.deleted_pages = 0;

	indexStat.max_avail = 0;
	indexStat.free_space = 0;

	indexStat.fragments = 0;

	/*
	 * Scan all blocks except the metapage
	 */
	nblocks = RelationGetNumberOfBlocks(rel);

	for (blkno = 1; blkno < nblocks; blkno++)
	{
		Buffer		buffer;
		Page		page;
		BTPageOpaque opaque;

		CHECK_FOR_INTERRUPTS();

		/* Read and lock buffer */
		buffer = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL, bstrategy);
		LockBuffer(buffer, BUFFER_LOCK_SHARE);

		page = BufferGetPage(buffer);
		opaque = (BTPageOpaque) PageGetSpecialPointer(page);

		/* Determine page type, and update totals */

		if (P_ISLEAF(opaque))
		{
			int			max_avail;

			max_avail = BLCKSZ - (BLCKSZ - ((PageHeader) page)->pd_special + SizeOfPageHeaderData);
			indexStat.max_avail += max_avail;
			indexStat.free_space += PageGetFreeSpace(page);

			indexStat.leaf_pages++;

			/*
			 * If the next leaf is on an earlier block, it means a
			 * fragmentation.
			 */
			if (opaque->btpo_next != P_NONE && opaque->btpo_next < blkno)
				indexStat.fragments++;
		}
		else if (P_ISDELETED(opaque))
			indexStat.deleted_pages++;
		else if (P_IGNORE(opaque))
			indexStat.empty_pages++;
		else if (P_ISROOT(opaque))
			indexStat.root_pages++;
		else
			indexStat.internal_pages++;

		/* Unlock and release buffer */
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
	}

	relation_close(rel, AccessShareLock);

	/*----------------------------
	 * Build a result tuple
	 *----------------------------
	 */
	{
		TupleDesc	tupleDesc;
		int			j;
		char	   *values[10];
		HeapTuple	tuple;

		/* Build a tuple descriptor for our result type */
		if (get_call_result_type(fcinfo, NULL, &tupleDesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");

		j = 0;
		values[j++] = psprintf("%d", indexStat.version);
		values[j++] = psprintf("%d", indexStat.level);
		values[j++] = psprintf(INT64_FORMAT,
							   (indexStat.root_pages +
								indexStat.leaf_pages +
								indexStat.internal_pages +
								indexStat.deleted_pages +
								indexStat.empty_pages) * BLCKSZ);
		values[j++] = psprintf("%u", indexStat.root_blkno);
		values[j++] = psprintf(INT64_FORMAT, indexStat.internal_pages);
		values[j++] = psprintf(INT64_FORMAT, indexStat.leaf_pages);
		values[j++] = psprintf(INT64_FORMAT, indexStat.empty_pages);
		values[j++] = psprintf(INT64_FORMAT, indexStat.deleted_pages);
		if (indexStat.max_avail > 0)
			values[j++] = psprintf("%.2f",
								   100.0 - (double) indexStat.free_space / (double) indexStat.max_avail * 100.0);
		else
			values[j++] = pstrdup("NaN");
		if (indexStat.leaf_pages > 0)
			values[j++] = psprintf("%.2f",
								   (double) indexStat.fragments / (double) indexStat.leaf_pages * 100.0);
		else
			values[j++] = pstrdup("NaN");

		tuple = BuildTupleFromCStrings(TupleDescGetAttInMetadata(tupleDesc),
									   values);

		result = HeapTupleGetDatum(tuple);
	}

	return result;
}

/* --------------------------------------------------------
 * pg_relpages()
 *
 * Get the number of pages of the table/index.
 *
 * Usage: SELECT pg_relpages('t1');
 *		  SELECT pg_relpages('t1_pkey');
 * --------------------------------------------------------
 */
Datum
pg_relpages(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_P(0);
	int64		relpages;
	Relation	rel;
	RangeVar   *relrv;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to use pgstattuple functions"))));

	relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname));
	rel = relation_openrv(relrv, AccessShareLock);

	/* note: this will work OK on non-local temp tables */

	relpages = RelationGetNumberOfBlocks(rel);

	relation_close(rel, AccessShareLock);

	PG_RETURN_INT64(relpages);
}

Datum
pg_relpagesbyid(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		relpages;
	Relation	rel;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to use pgstattuple functions"))));

	rel = relation_open(relid, AccessShareLock);

	/* note: this will work OK on non-local temp tables */

	relpages = RelationGetNumberOfBlocks(rel);

	relation_close(rel, AccessShareLock);

	PG_RETURN_INT64(relpages);
}

/* ------------------------------------------------------
 * pgstatginindex()
 *
 * Usage: SELECT * FROM pgstatginindex('ginindex');
 * ------------------------------------------------------
 */
Datum
pgstatginindex(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel;
	Buffer		buffer;
	Page		page;
	GinMetaPageData *metadata;
	GinIndexStat stats;
	HeapTuple	tuple;
	TupleDesc	tupleDesc;
	Datum		values[3];
	bool		nulls[3] = {false, false, false};
	Datum		result;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to use pgstattuple functions"))));

	rel = relation_open(relid, AccessShareLock);

	if (!IS_INDEX(rel) || !IS_GIN(rel))
		elog(ERROR, "relation \"%s\" is not a GIN index",
			 RelationGetRelationName(rel));

	/*
	 * Reject attempts to read non-local temporary relations; we would be
	 * likely to get wrong data since we have no visibility into the owning
	 * session's local buffers.
	 */
	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			   errmsg("cannot access temporary indexes of other sessions")));

	/*
	 * Read metapage
	 */
	buffer = ReadBuffer(rel, GIN_METAPAGE_BLKNO);
	LockBuffer(buffer, GIN_SHARE);
	page = BufferGetPage(buffer);
	metadata = GinPageGetMeta(page);

	stats.version = metadata->ginVersion;
	stats.pending_pages = metadata->nPendingPages;
	stats.pending_tuples = metadata->nPendingHeapTuples;

	UnlockReleaseBuffer(buffer);
	relation_close(rel, AccessShareLock);

	/*
	 * Build a tuple descriptor for our result type
	 */
	if (get_call_result_type(fcinfo, NULL, &tupleDesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	values[0] = Int32GetDatum(stats.version);
	values[1] = UInt32GetDatum(stats.pending_pages);
	values[2] = Int64GetDatum(stats.pending_tuples);

	/*
	 * Build and return the tuple
	 */
	tuple = heap_form_tuple(tupleDesc, values, nulls);
	result = HeapTupleGetDatum(tuple);

	PG_RETURN_DATUM(result);
}
