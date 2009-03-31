/*
 * btreefuncs.c
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
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/builtins.h"


extern Datum bt_metap(PG_FUNCTION_ARGS);
extern Datum bt_page_items(PG_FUNCTION_ARGS);
extern Datum bt_page_stats(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(bt_metap);
PG_FUNCTION_INFO_V1(bt_page_items);
PG_FUNCTION_INFO_V1(bt_page_stats);

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
 * structure for single btree page statistics
 * ------------------------------------------------
 */
typedef struct BTPageStat
{
	uint32		blkno;
	uint32		live_items;
	uint32		dead_items;
	uint32		page_size;
	uint32		max_avail;
	uint32		free_size;
	uint32		avg_item_size;
	char		type;

	/* opaque data */
	BlockNumber btpo_prev;
	BlockNumber btpo_next;
	union
	{
		uint32		level;
		TransactionId xact;
	}			btpo;
	uint16		btpo_flags;
	BTCycleId	btpo_cycleid;
}	BTPageStat;


/* -------------------------------------------------
 * GetBTPageStatistics()
 *
 * Collect statistics of single b-tree page
 * -------------------------------------------------
 */
static void
GetBTPageStatistics(BlockNumber blkno, Buffer buffer, BTPageStat * stat)
{
	Page		page = BufferGetPage(buffer);
	PageHeader	phdr = (PageHeader) page;
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	BTPageOpaque opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	int			item_size = 0;
	int			off;

	stat->blkno = blkno;

	stat->max_avail = BLCKSZ - (BLCKSZ - phdr->pd_special + SizeOfPageHeaderData);

	stat->dead_items = stat->live_items = 0;

	stat->page_size = PageGetPageSize(page);

	/* page type (flags) */
	if (P_ISDELETED(opaque))
	{
		stat->type = 'd';
		stat->btpo.xact = opaque->btpo.xact;
		return;
	}
	else if (P_IGNORE(opaque))
		stat->type = 'e';
	else if (P_ISLEAF(opaque))
		stat->type = 'l';
	else if (P_ISROOT(opaque))
		stat->type = 'r';
	else
		stat->type = 'i';

	/* btpage opaque data */
	stat->btpo_prev = opaque->btpo_prev;
	stat->btpo_next = opaque->btpo_next;
	stat->btpo.level = opaque->btpo.level;
	stat->btpo_flags = opaque->btpo_flags;
	stat->btpo_cycleid = opaque->btpo_cycleid;

	/* count live and dead tuples, and free space */
	for (off = FirstOffsetNumber; off <= maxoff; off++)
	{
		IndexTuple	itup;

		ItemId		id = PageGetItemId(page, off);

		itup = (IndexTuple) PageGetItem(page, id);

		item_size += IndexTupleSize(itup);

		if (!ItemIdIsDead(id))
			stat->live_items++;
		else
			stat->dead_items++;
	}
	stat->free_size = PageGetFreeSpace(page);

	if ((stat->live_items + stat->dead_items) > 0)
		stat->avg_item_size = item_size / (stat->live_items + stat->dead_items);
	else
		stat->avg_item_size = 0;
}

/* -----------------------------------------------
 * bt_page()
 *
 * Usage: SELECT * FROM bt_page('t1_pkey', 1);
 * -----------------------------------------------
 */
Datum
bt_page_stats(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_P(0);
	uint32		blkno = PG_GETARG_UINT32(1);
	Buffer		buffer;
	Relation	rel;
	RangeVar   *relrv;
	Datum		result;
	HeapTuple	tuple;
	TupleDesc	tupleDesc;
	int			j;
	char	   *values[11];
	BTPageStat	stat;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to use pageinspect functions"))));

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

	if (blkno == 0)
		elog(ERROR, "block 0 is a meta page");

	CHECK_RELATION_BLOCK_RANGE(rel, blkno);

	buffer = ReadBuffer(rel, blkno);

	/* keep compiler quiet */
	stat.btpo_prev = stat.btpo_next = InvalidBlockNumber;
	stat.btpo_flags = stat.free_size = stat.avg_item_size = 0;

	GetBTPageStatistics(blkno, buffer, &stat);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupleDesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	j = 0;
	values[j] = palloc(32);
	snprintf(values[j++], 32, "%d", stat.blkno);
	values[j] = palloc(32);
	snprintf(values[j++], 32, "%c", stat.type);
	values[j] = palloc(32);
	snprintf(values[j++], 32, "%d", stat.live_items);
	values[j] = palloc(32);
	snprintf(values[j++], 32, "%d", stat.dead_items);
	values[j] = palloc(32);
	snprintf(values[j++], 32, "%d", stat.avg_item_size);
	values[j] = palloc(32);
	snprintf(values[j++], 32, "%d", stat.page_size);
	values[j] = palloc(32);
	snprintf(values[j++], 32, "%d", stat.free_size);
	values[j] = palloc(32);
	snprintf(values[j++], 32, "%d", stat.btpo_prev);
	values[j] = palloc(32);
	snprintf(values[j++], 32, "%d", stat.btpo_next);
	values[j] = palloc(32);
	if (stat.type == 'd')
		snprintf(values[j++], 32, "%d", stat.btpo.xact);
	else
		snprintf(values[j++], 32, "%d", stat.btpo.level);
	values[j] = palloc(32);
	snprintf(values[j++], 32, "%d", stat.btpo_flags);

	tuple = BuildTupleFromCStrings(TupleDescGetAttInMetadata(tupleDesc),
								   values);

	result = HeapTupleGetDatum(tuple);

	ReleaseBuffer(buffer);

	relation_close(rel, AccessShareLock);

	PG_RETURN_DATUM(result);
}

/*-------------------------------------------------------
 * bt_page_items()
 *
 * Get IndexTupleData set in a btree page
 *
 * Usage: SELECT * FROM bt_page_items('t1_pkey', 1);
 *-------------------------------------------------------
 */

/*
 * cross-call data structure for SRF
 */
struct user_args
{
	Page		page;
	OffsetNumber offset;
};

Datum
bt_page_items(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_P(0);
	uint32		blkno = PG_GETARG_UINT32(1);
	Datum		result;
	char	   *values[6];
	HeapTuple	tuple;
	FuncCallContext *fctx;
	MemoryContext mctx;
	struct user_args *uargs;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to use pageinspect functions"))));

	if (SRF_IS_FIRSTCALL())
	{
		RangeVar   *relrv;
		Relation	rel;
		Buffer		buffer;
		BTPageOpaque opaque;
		TupleDesc	tupleDesc;

		fctx = SRF_FIRSTCALL_INIT();

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

		if (blkno == 0)
			elog(ERROR, "block 0 is a meta page");

		CHECK_RELATION_BLOCK_RANGE(rel, blkno);

		buffer = ReadBuffer(rel, blkno);

		/*
		 * We copy the page into local storage to avoid holding pin on the
		 * buffer longer than we must, and possibly failing to release it at
		 * all if the calling query doesn't fetch all rows.
		 */
		mctx = MemoryContextSwitchTo(fctx->multi_call_memory_ctx);

		uargs = palloc(sizeof(struct user_args));

		uargs->page = palloc(BLCKSZ);
		memcpy(uargs->page, BufferGetPage(buffer), BLCKSZ);

		ReleaseBuffer(buffer);
		relation_close(rel, AccessShareLock);

		uargs->offset = FirstOffsetNumber;

		opaque = (BTPageOpaque) PageGetSpecialPointer(uargs->page);

		if (P_ISDELETED(opaque))
			elog(NOTICE, "page is deleted");

		fctx->max_calls = PageGetMaxOffsetNumber(uargs->page);

		/* Build a tuple descriptor for our result type */
		if (get_call_result_type(fcinfo, NULL, &tupleDesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");

		fctx->attinmeta = TupleDescGetAttInMetadata(tupleDesc);

		fctx->user_fctx = uargs;

		MemoryContextSwitchTo(mctx);
	}

	fctx = SRF_PERCALL_SETUP();
	uargs = fctx->user_fctx;

	if (fctx->call_cntr < fctx->max_calls)
	{
		ItemId		id;
		IndexTuple	itup;
		int			j;
		int			off;
		int			dlen;
		char	   *dump;
		char	   *ptr;

		id = PageGetItemId(uargs->page, uargs->offset);

		if (!ItemIdIsValid(id))
			elog(ERROR, "invalid ItemId");

		itup = (IndexTuple) PageGetItem(uargs->page, id);

		j = 0;
		values[j] = palloc(32);
		snprintf(values[j++], 32, "%d", uargs->offset);
		values[j] = palloc(32);
		snprintf(values[j++], 32, "(%u,%u)",
				 BlockIdGetBlockNumber(&(itup->t_tid.ip_blkid)),
				 itup->t_tid.ip_posid);
		values[j] = palloc(32);
		snprintf(values[j++], 32, "%d", (int) IndexTupleSize(itup));
		values[j] = palloc(32);
		snprintf(values[j++], 32, "%c", IndexTupleHasNulls(itup) ? 't' : 'f');
		values[j] = palloc(32);
		snprintf(values[j++], 32, "%c", IndexTupleHasVarwidths(itup) ? 't' : 'f');

		ptr = (char *) itup + IndexInfoFindDataOffset(itup->t_info);
		dlen = IndexTupleSize(itup) - IndexInfoFindDataOffset(itup->t_info);
		dump = palloc0(dlen * 3 + 1);
		values[j] = dump;
		for (off = 0; off < dlen; off++)
		{
			if (off > 0)
				*dump++ = ' ';
			sprintf(dump, "%02x", *(ptr + off) & 0xff);
			dump += 2;
		}

		tuple = BuildTupleFromCStrings(fctx->attinmeta, values);
		result = HeapTupleGetDatum(tuple);

		uargs->offset = uargs->offset + 1;

		SRF_RETURN_NEXT(fctx, result);
	}
	else
	{
		pfree(uargs->page);
		pfree(uargs);
		SRF_RETURN_DONE(fctx);
	}
}


/* ------------------------------------------------
 * bt_metap()
 *
 * Get a btree's meta-page information
 *
 * Usage: SELECT * FROM bt_metap('t1_pkey')
 * ------------------------------------------------
 */
Datum
bt_metap(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_P(0);
	Datum		result;
	Relation	rel;
	RangeVar   *relrv;
	BTMetaPageData *metad;
	TupleDesc	tupleDesc;
	int			j;
	char	   *values[6];
	Buffer		buffer;
	Page		page;
	HeapTuple	tuple;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to use pageinspect functions"))));

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

	buffer = ReadBuffer(rel, 0);
	page = BufferGetPage(buffer);
	metad = BTPageGetMeta(page);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupleDesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	j = 0;
	values[j] = palloc(32);
	snprintf(values[j++], 32, "%d", metad->btm_magic);
	values[j] = palloc(32);
	snprintf(values[j++], 32, "%d", metad->btm_version);
	values[j] = palloc(32);
	snprintf(values[j++], 32, "%d", metad->btm_root);
	values[j] = palloc(32);
	snprintf(values[j++], 32, "%d", metad->btm_level);
	values[j] = palloc(32);
	snprintf(values[j++], 32, "%d", metad->btm_fastroot);
	values[j] = palloc(32);
	snprintf(values[j++], 32, "%d", metad->btm_fastlevel);

	tuple = BuildTupleFromCStrings(TupleDescGetAttInMetadata(tupleDesc),
								   values);

	result = HeapTupleGetDatum(tuple);

	ReleaseBuffer(buffer);

	relation_close(rel, AccessShareLock);

	PG_RETURN_DATUM(result);
}
