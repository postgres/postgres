/*
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

#include "access/heapam.h"
#include "access/nbtree.h"
#include "catalog/namespace.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/builtins.h"


extern Datum pgstatindex(PG_FUNCTION_ARGS);
extern Datum pg_relpages(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pgstatindex);
PG_FUNCTION_INFO_V1(pg_relpages);

#define IS_INDEX(r) ((r)->rd_rel->relkind == RELKIND_INDEX)
#define IS_BTREE(r) ((r)->rd_rel->relam == BTREE_AM_OID)

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
	BlockNumber root_blkno;
	uint32		level;

	uint32		root_pages;
	uint32		internal_pages;
	uint32		leaf_pages;
	uint32		empty_pages;
	uint32		deleted_pages;

	uint32		max_avail;
	uint32		free_space;

	uint32		fragments;
}	BTIndexStat;

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
	Datum		result;
	uint32		nblocks;
	uint32		blkno;
	BTIndexStat indexStat;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to use pgstattuple functions"))));

	relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname));
	rel = relation_openrv(relrv, AccessShareLock);

	if (!IS_INDEX(rel) || !IS_BTREE(rel))
		elog(ERROR, "relation \"%s\" is not a btree index",
			 RelationGetRelationName(rel));

	/*
	 * Reject attempts to read non-local temporary relations; we would
	 * be likely to get wrong data since we have no visibility into the
	 * owning session's local buffers.
	 */
	if (isOtherTempNamespace(RelationGetNamespace(rel)))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot access temporary tables of other sessions")));

	/*
	 * Read metapage
	 */
	{
		Buffer		buffer = ReadBuffer(rel, 0);
		Page		page = BufferGetPage(buffer);
		BTMetaPageData *metad = BTPageGetMeta(page);

		indexStat.version = metad->btm_version;
		indexStat.root_blkno = metad->btm_root;
		indexStat.level = metad->btm_level;

		ReleaseBuffer(buffer);
	}

	nblocks = RelationGetNumberOfBlocks(rel);

	/* -- init stat -- */
	indexStat.fragments = 0;

	indexStat.root_pages = 0;
	indexStat.leaf_pages = 0;
	indexStat.internal_pages = 0;
	indexStat.empty_pages = 0;
	indexStat.deleted_pages = 0;

	indexStat.max_avail = 0;
	indexStat.free_space = 0;

	/*-----------------------
	 * Scan all blocks
	 *-----------------------
	 */
	for (blkno = 1; blkno < nblocks; blkno++)
	{
		Buffer		buffer;
		Page		page;
		BTPageOpaque opaque;

		/* Read and lock buffer */
		buffer = ReadBuffer(rel, blkno);
		LockBuffer(buffer, BUFFER_LOCK_SHARE);

		page = BufferGetPage(buffer);
		opaque = (BTPageOpaque) PageGetSpecialPointer(page);

		/* Determine page type, and update totals */

		if (P_ISDELETED(opaque))
			indexStat.deleted_pages++;

		else if (P_IGNORE(opaque))
			indexStat.empty_pages++;

		else if (P_ISLEAF(opaque))
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
		values[j] = palloc(32);
		snprintf(values[j++], 32, "%d", indexStat.version);
		values[j] = palloc(32);
		snprintf(values[j++], 32, "%d", indexStat.level);
		values[j] = palloc(32);
		snprintf(values[j++], 32, "%d", (indexStat.root_pages +
										 indexStat.leaf_pages +
										 indexStat.internal_pages +
										 indexStat.deleted_pages +
										 indexStat.empty_pages) * BLCKSZ);
		values[j] = palloc(32);
		snprintf(values[j++], 32, "%d", indexStat.root_blkno);
		values[j] = palloc(32);
		snprintf(values[j++], 32, "%d", indexStat.internal_pages);
		values[j] = palloc(32);
		snprintf(values[j++], 32, "%d", indexStat.leaf_pages);
		values[j] = palloc(32);
		snprintf(values[j++], 32, "%d", indexStat.empty_pages);
		values[j] = palloc(32);
		snprintf(values[j++], 32, "%d", indexStat.deleted_pages);
		values[j] = palloc(32);
		snprintf(values[j++], 32, "%.2f", 100.0 - (float) indexStat.free_space / (float) indexStat.max_avail * 100.0);
		values[j] = palloc(32);
		snprintf(values[j++], 32, "%.2f", (float) indexStat.fragments / (float) indexStat.leaf_pages * 100.0);

		tuple = BuildTupleFromCStrings(TupleDescGetAttInMetadata(tupleDesc),
									   values);

		result = HeapTupleGetDatum(tuple);
	}

	PG_RETURN_DATUM(result);
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
	Relation	rel;
	RangeVar   *relrv;
	int4		relpages;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to use pgstattuple functions"))));

	relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname));
	rel = relation_openrv(relrv, AccessShareLock);

	/* note: this will work OK on non-local temp tables */

	relpages = RelationGetNumberOfBlocks(rel);

	relation_close(rel, AccessShareLock);

	PG_RETURN_INT32(relpages);
}
