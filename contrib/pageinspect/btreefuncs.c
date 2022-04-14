/*
 * contrib/pageinspect/btreefuncs.c
 *
 *
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

#include "access/nbtree.h"
#include "access/relation.h"
#include "catalog/namespace.h"
#include "catalog/pg_am.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pageinspect.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/varlena.h"

PG_FUNCTION_INFO_V1(bt_metap);
PG_FUNCTION_INFO_V1(bt_page_items_1_9);
PG_FUNCTION_INFO_V1(bt_page_items);
PG_FUNCTION_INFO_V1(bt_page_items_bytea);
PG_FUNCTION_INFO_V1(bt_page_stats_1_9);
PG_FUNCTION_INFO_V1(bt_page_stats);

#define IS_INDEX(r) ((r)->rd_rel->relkind == RELKIND_INDEX)
#define IS_BTREE(r) ((r)->rd_rel->relam == BTREE_AM_OID)
#define DatumGetItemPointer(X)	 ((ItemPointer) DatumGetPointer(X))
#define ItemPointerGetDatum(X)	 PointerGetDatum(X)

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
	uint32		btpo_level;
	uint16		btpo_flags;
	BTCycleId	btpo_cycleid;
} BTPageStat;


/* -------------------------------------------------
 * GetBTPageStatistics()
 *
 * Collect statistics of single b-tree page
 * -------------------------------------------------
 */
static void
GetBTPageStatistics(BlockNumber blkno, Buffer buffer, BTPageStat *stat)
{
	Page		page = BufferGetPage(buffer);
	PageHeader	phdr = (PageHeader) page;
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	BTPageOpaque opaque = BTPageGetOpaque(page);
	int			item_size = 0;
	int			off;

	stat->blkno = blkno;

	stat->max_avail = BLCKSZ - (BLCKSZ - phdr->pd_special + SizeOfPageHeaderData);

	stat->dead_items = stat->live_items = 0;

	stat->page_size = PageGetPageSize(page);

	/* page type (flags) */
	if (P_ISDELETED(opaque))
	{
		/* We divide deleted pages into leaf ('d') or internal ('D') */
		if (P_ISLEAF(opaque) || !P_HAS_FULLXID(opaque))
			stat->type = 'd';
		else
			stat->type = 'D';

		/*
		 * Report safexid in a deleted page.
		 *
		 * Handle pg_upgrade'd deleted pages that used the previous safexid
		 * representation in btpo_level field (this used to be a union type
		 * called "bpto").
		 */
		if (P_HAS_FULLXID(opaque))
		{
			FullTransactionId safexid = BTPageGetDeleteXid(page);

			elog(DEBUG2, "deleted page from block %u has safexid %u:%u",
				 blkno, EpochFromFullTransactionId(safexid),
				 XidFromFullTransactionId(safexid));
		}
		else
			elog(DEBUG2, "deleted page from block %u has safexid %u",
				 blkno, opaque->btpo_level);

		/* Don't interpret BTDeletedPageData as index tuples */
		maxoff = InvalidOffsetNumber;
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
	stat->btpo_level = opaque->btpo_level;
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
 * bt_page_stats()
 *
 * Usage: SELECT * FROM bt_page_stats('t1_pkey', 1);
 * -----------------------------------------------
 */
static Datum
bt_page_stats_internal(PG_FUNCTION_ARGS, enum pageinspect_version ext_version)
{
	text	   *relname = PG_GETARG_TEXT_PP(0);
	int64		blkno = (ext_version == PAGEINSPECT_V1_8 ? PG_GETARG_UINT32(1) : PG_GETARG_INT64(1));
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
				 errmsg("must be superuser to use pageinspect functions")));

	relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname));
	rel = relation_openrv(relrv, AccessShareLock);

	if (!IS_INDEX(rel) || !IS_BTREE(rel))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a %s index",
						RelationGetRelationName(rel), "btree")));

	/*
	 * Reject attempts to read non-local temporary relations; we would be
	 * likely to get wrong data since we have no visibility into the owning
	 * session's local buffers.
	 */
	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot access temporary tables of other sessions")));

	if (blkno < 0 || blkno > MaxBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid block number")));

	if (blkno == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("block 0 is a meta page")));

	CHECK_RELATION_BLOCK_RANGE(rel, blkno);

	buffer = ReadBuffer(rel, blkno);
	LockBuffer(buffer, BUFFER_LOCK_SHARE);

	/* keep compiler quiet */
	stat.btpo_prev = stat.btpo_next = InvalidBlockNumber;
	stat.btpo_flags = stat.free_size = stat.avg_item_size = 0;

	GetBTPageStatistics(blkno, buffer, &stat);

	UnlockReleaseBuffer(buffer);
	relation_close(rel, AccessShareLock);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupleDesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	j = 0;
	values[j++] = psprintf("%u", stat.blkno);
	values[j++] = psprintf("%c", stat.type);
	values[j++] = psprintf("%u", stat.live_items);
	values[j++] = psprintf("%u", stat.dead_items);
	values[j++] = psprintf("%u", stat.avg_item_size);
	values[j++] = psprintf("%u", stat.page_size);
	values[j++] = psprintf("%u", stat.free_size);
	values[j++] = psprintf("%u", stat.btpo_prev);
	values[j++] = psprintf("%u", stat.btpo_next);
	values[j++] = psprintf("%u", stat.btpo_level);
	values[j++] = psprintf("%d", stat.btpo_flags);

	tuple = BuildTupleFromCStrings(TupleDescGetAttInMetadata(tupleDesc),
								   values);

	result = HeapTupleGetDatum(tuple);

	PG_RETURN_DATUM(result);
}

Datum
bt_page_stats_1_9(PG_FUNCTION_ARGS)
{
	return bt_page_stats_internal(fcinfo, PAGEINSPECT_V1_9);
}

/* entry point for old extension version */
Datum
bt_page_stats(PG_FUNCTION_ARGS)
{
	return bt_page_stats_internal(fcinfo, PAGEINSPECT_V1_8);
}


/*
 * cross-call data structure for SRF
 */
struct user_args
{
	Page		page;
	OffsetNumber offset;
	bool		leafpage;
	bool		rightmost;
	TupleDesc	tupd;
};

/*-------------------------------------------------------
 * bt_page_print_tuples()
 *
 * Form a tuple describing index tuple at a given offset
 * ------------------------------------------------------
 */
static Datum
bt_page_print_tuples(struct user_args *uargs)
{
	Page		page = uargs->page;
	OffsetNumber offset = uargs->offset;
	bool		leafpage = uargs->leafpage;
	bool		rightmost = uargs->rightmost;
	bool		ispivottuple;
	Datum		values[9];
	bool		nulls[9];
	HeapTuple	tuple;
	ItemId		id;
	IndexTuple	itup;
	int			j;
	int			off;
	int			dlen;
	char	   *dump,
			   *datacstring;
	char	   *ptr;
	ItemPointer htid;

	id = PageGetItemId(page, offset);

	if (!ItemIdIsValid(id))
		elog(ERROR, "invalid ItemId");

	itup = (IndexTuple) PageGetItem(page, id);

	j = 0;
	memset(nulls, 0, sizeof(nulls));
	values[j++] = DatumGetInt16(offset);
	values[j++] = ItemPointerGetDatum(&itup->t_tid);
	values[j++] = Int32GetDatum((int) IndexTupleSize(itup));
	values[j++] = BoolGetDatum(IndexTupleHasNulls(itup));
	values[j++] = BoolGetDatum(IndexTupleHasVarwidths(itup));

	ptr = (char *) itup + IndexInfoFindDataOffset(itup->t_info);
	dlen = IndexTupleSize(itup) - IndexInfoFindDataOffset(itup->t_info);

	/*
	 * Make sure that "data" column does not include posting list or pivot
	 * tuple representation of heap TID(s).
	 *
	 * Note: BTreeTupleIsPivot() won't work reliably on !heapkeyspace indexes
	 * (those built before BTREE_VERSION 4), but we have no way of determining
	 * if this page came from a !heapkeyspace index.  We may only have a bytea
	 * nbtree page image to go on, so in general there is no metapage that we
	 * can check.
	 *
	 * That's okay here because BTreeTupleIsPivot() can only return false for
	 * a !heapkeyspace pivot, never true for a !heapkeyspace non-pivot.  Since
	 * heap TID isn't part of the keyspace in a !heapkeyspace index anyway,
	 * there cannot possibly be a pivot tuple heap TID representation that we
	 * fail to make an adjustment for.  A !heapkeyspace index can have
	 * BTreeTupleIsPivot() return true (due to things like suffix truncation
	 * for INCLUDE indexes in Postgres v11), but when that happens
	 * BTreeTupleGetHeapTID() can be trusted to work reliably (i.e. return
	 * NULL).
	 *
	 * Note: BTreeTupleIsPosting() always works reliably, even with
	 * !heapkeyspace indexes.
	 */
	if (BTreeTupleIsPosting(itup))
		dlen -= IndexTupleSize(itup) - BTreeTupleGetPostingOffset(itup);
	else if (BTreeTupleIsPivot(itup) && BTreeTupleGetHeapTID(itup) != NULL)
		dlen -= MAXALIGN(sizeof(ItemPointerData));

	if (dlen < 0 || dlen > INDEX_SIZE_MASK)
		elog(ERROR, "invalid tuple length %d for tuple at offset number %u",
			 dlen, offset);
	dump = palloc0(dlen * 3 + 1);
	datacstring = dump;
	for (off = 0; off < dlen; off++)
	{
		if (off > 0)
			*dump++ = ' ';
		sprintf(dump, "%02x", *(ptr + off) & 0xff);
		dump += 2;
	}
	values[j++] = CStringGetTextDatum(datacstring);
	pfree(datacstring);

	/*
	 * We need to work around the BTreeTupleIsPivot() !heapkeyspace limitation
	 * again.  Deduce whether or not tuple must be a pivot tuple based on
	 * whether or not the page is a leaf page, as well as the page offset
	 * number of the tuple.
	 */
	ispivottuple = (!leafpage || (!rightmost && offset == P_HIKEY));

	/* LP_DEAD bit can never be set for pivot tuples, so show a NULL there */
	if (!ispivottuple)
		values[j++] = BoolGetDatum(ItemIdIsDead(id));
	else
	{
		Assert(!ItemIdIsDead(id));
		nulls[j++] = true;
	}

	htid = BTreeTupleGetHeapTID(itup);
	if (ispivottuple && !BTreeTupleIsPivot(itup))
	{
		/* Don't show bogus heap TID in !heapkeyspace pivot tuple */
		htid = NULL;
	}

	if (htid)
		values[j++] = ItemPointerGetDatum(htid);
	else
		nulls[j++] = true;

	if (BTreeTupleIsPosting(itup))
	{
		/* Build an array of item pointers */
		ItemPointer tids;
		Datum	   *tids_datum;
		int			nposting;

		tids = BTreeTupleGetPosting(itup);
		nposting = BTreeTupleGetNPosting(itup);
		tids_datum = (Datum *) palloc(nposting * sizeof(Datum));
		for (int i = 0; i < nposting; i++)
			tids_datum[i] = ItemPointerGetDatum(&tids[i]);
		values[j++] = PointerGetDatum(construct_array(tids_datum,
													  nposting,
													  TIDOID,
													  sizeof(ItemPointerData),
													  false, TYPALIGN_SHORT));
		pfree(tids_datum);
	}
	else
		nulls[j++] = true;

	/* Build and return the result tuple */
	tuple = heap_form_tuple(uargs->tupd, values, nulls);

	return HeapTupleGetDatum(tuple);
}

/*-------------------------------------------------------
 * bt_page_items()
 *
 * Get IndexTupleData set in a btree page
 *
 * Usage: SELECT * FROM bt_page_items('t1_pkey', 1);
 *-------------------------------------------------------
 */
static Datum
bt_page_items_internal(PG_FUNCTION_ARGS, enum pageinspect_version ext_version)
{
	text	   *relname = PG_GETARG_TEXT_PP(0);
	int64		blkno = (ext_version == PAGEINSPECT_V1_8 ? PG_GETARG_UINT32(1) : PG_GETARG_INT64(1));
	Datum		result;
	FuncCallContext *fctx;
	MemoryContext mctx;
	struct user_args *uargs;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use pageinspect functions")));

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
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is not a %s index",
							RelationGetRelationName(rel), "btree")));

		/*
		 * Reject attempts to read non-local temporary relations; we would be
		 * likely to get wrong data since we have no visibility into the
		 * owning session's local buffers.
		 */
		if (RELATION_IS_OTHER_TEMP(rel))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot access temporary tables of other sessions")));

		if (blkno < 0 || blkno > MaxBlockNumber)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid block number")));

		if (blkno == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("block 0 is a meta page")));

		CHECK_RELATION_BLOCK_RANGE(rel, blkno);

		buffer = ReadBuffer(rel, blkno);
		LockBuffer(buffer, BUFFER_LOCK_SHARE);

		/*
		 * We copy the page into local storage to avoid holding pin on the
		 * buffer longer than we must, and possibly failing to release it at
		 * all if the calling query doesn't fetch all rows.
		 */
		mctx = MemoryContextSwitchTo(fctx->multi_call_memory_ctx);

		uargs = palloc(sizeof(struct user_args));

		uargs->page = palloc(BLCKSZ);
		memcpy(uargs->page, BufferGetPage(buffer), BLCKSZ);

		UnlockReleaseBuffer(buffer);
		relation_close(rel, AccessShareLock);

		uargs->offset = FirstOffsetNumber;

		opaque = BTPageGetOpaque(uargs->page);

		if (!P_ISDELETED(opaque))
			fctx->max_calls = PageGetMaxOffsetNumber(uargs->page);
		else
		{
			/* Don't interpret BTDeletedPageData as index tuples */
			elog(NOTICE, "page from block " INT64_FORMAT " is deleted", blkno);
			fctx->max_calls = 0;
		}
		uargs->leafpage = P_ISLEAF(opaque);
		uargs->rightmost = P_RIGHTMOST(opaque);

		/* Build a tuple descriptor for our result type */
		if (get_call_result_type(fcinfo, NULL, &tupleDesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");
		tupleDesc = BlessTupleDesc(tupleDesc);

		uargs->tupd = tupleDesc;

		fctx->user_fctx = uargs;

		MemoryContextSwitchTo(mctx);
	}

	fctx = SRF_PERCALL_SETUP();
	uargs = fctx->user_fctx;

	if (fctx->call_cntr < fctx->max_calls)
	{
		result = bt_page_print_tuples(uargs);
		uargs->offset++;
		SRF_RETURN_NEXT(fctx, result);
	}

	SRF_RETURN_DONE(fctx);
}

Datum
bt_page_items_1_9(PG_FUNCTION_ARGS)
{
	return bt_page_items_internal(fcinfo, PAGEINSPECT_V1_9);
}

/* entry point for old extension version */
Datum
bt_page_items(PG_FUNCTION_ARGS)
{
	return bt_page_items_internal(fcinfo, PAGEINSPECT_V1_8);
}

/*-------------------------------------------------------
 * bt_page_items_bytea()
 *
 * Get IndexTupleData set in a btree page
 *
 * Usage: SELECT * FROM bt_page_items(get_raw_page('t1_pkey', 1));
 *-------------------------------------------------------
 */

Datum
bt_page_items_bytea(PG_FUNCTION_ARGS)
{
	bytea	   *raw_page = PG_GETARG_BYTEA_P(0);
	Datum		result;
	FuncCallContext *fctx;
	struct user_args *uargs;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use raw page functions")));

	if (SRF_IS_FIRSTCALL())
	{
		BTPageOpaque opaque;
		MemoryContext mctx;
		TupleDesc	tupleDesc;

		fctx = SRF_FIRSTCALL_INIT();
		mctx = MemoryContextSwitchTo(fctx->multi_call_memory_ctx);

		uargs = palloc(sizeof(struct user_args));

		uargs->page = get_page_from_raw(raw_page);

		if (PageIsNew(uargs->page))
		{
			MemoryContextSwitchTo(mctx);
			PG_RETURN_NULL();
		}

		uargs->offset = FirstOffsetNumber;

		/* verify the special space has the expected size */
		if (PageGetSpecialSize(uargs->page) != MAXALIGN(sizeof(BTPageOpaqueData)))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("input page is not a valid %s page", "btree"),
					 errdetail("Expected special size %d, got %d.",
							   (int) MAXALIGN(sizeof(BTPageOpaqueData)),
							   (int) PageGetSpecialSize(uargs->page))));

		opaque = BTPageGetOpaque(uargs->page);

		if (P_ISMETA(opaque))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("block is a meta page")));

		if (P_ISLEAF(opaque) && opaque->btpo_level != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("block is not a valid btree leaf page")));

		if (P_ISDELETED(opaque))
			elog(NOTICE, "page is deleted");

		if (!P_ISDELETED(opaque))
			fctx->max_calls = PageGetMaxOffsetNumber(uargs->page);
		else
		{
			/* Don't interpret BTDeletedPageData as index tuples */
			elog(NOTICE, "page from block is deleted");
			fctx->max_calls = 0;
		}
		uargs->leafpage = P_ISLEAF(opaque);
		uargs->rightmost = P_RIGHTMOST(opaque);

		/* Build a tuple descriptor for our result type */
		if (get_call_result_type(fcinfo, NULL, &tupleDesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");
		tupleDesc = BlessTupleDesc(tupleDesc);

		uargs->tupd = tupleDesc;

		fctx->user_fctx = uargs;

		MemoryContextSwitchTo(mctx);
	}

	fctx = SRF_PERCALL_SETUP();
	uargs = fctx->user_fctx;

	if (fctx->call_cntr < fctx->max_calls)
	{
		result = bt_page_print_tuples(uargs);
		uargs->offset++;
		SRF_RETURN_NEXT(fctx, result);
	}

	SRF_RETURN_DONE(fctx);
}

/* Number of output arguments (columns) for bt_metap() */
#define BT_METAP_COLS_V1_8		9

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
	text	   *relname = PG_GETARG_TEXT_PP(0);
	Datum		result;
	Relation	rel;
	RangeVar   *relrv;
	BTMetaPageData *metad;
	TupleDesc	tupleDesc;
	int			j;
	char	   *values[9];
	Buffer		buffer;
	Page		page;
	HeapTuple	tuple;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use pageinspect functions")));

	relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname));
	rel = relation_openrv(relrv, AccessShareLock);

	if (!IS_INDEX(rel) || !IS_BTREE(rel))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a %s index",
						RelationGetRelationName(rel), "btree")));

	/*
	 * Reject attempts to read non-local temporary relations; we would be
	 * likely to get wrong data since we have no visibility into the owning
	 * session's local buffers.
	 */
	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot access temporary tables of other sessions")));

	buffer = ReadBuffer(rel, 0);
	LockBuffer(buffer, BUFFER_LOCK_SHARE);

	page = BufferGetPage(buffer);
	metad = BTPageGetMeta(page);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupleDesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	/*
	 * We need a kluge here to detect API versions prior to 1.8.  Earlier
	 * versions incorrectly used int4 for certain columns.
	 *
	 * There is no way to reliably avoid the problems created by the old
	 * function definition at this point, so insist that the user update the
	 * extension.
	 */
	if (tupleDesc->natts < BT_METAP_COLS_V1_8)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("function has wrong number of declared columns"),
				 errhint("To resolve the problem, update the \"pageinspect\" extension to the latest version.")));

	j = 0;
	values[j++] = psprintf("%d", metad->btm_magic);
	values[j++] = psprintf("%d", metad->btm_version);
	values[j++] = psprintf(INT64_FORMAT, (int64) metad->btm_root);
	values[j++] = psprintf(INT64_FORMAT, (int64) metad->btm_level);
	values[j++] = psprintf(INT64_FORMAT, (int64) metad->btm_fastroot);
	values[j++] = psprintf(INT64_FORMAT, (int64) metad->btm_fastlevel);

	/*
	 * Get values of extended metadata if available, use default values
	 * otherwise.  Note that we rely on the assumption that btm_allequalimage
	 * is initialized to zero with indexes that were built on versions prior
	 * to Postgres 13 (just like _bt_metaversion()).
	 */
	if (metad->btm_version >= BTREE_NOVAC_VERSION)
	{
		values[j++] = psprintf(INT64_FORMAT,
							   (int64) metad->btm_last_cleanup_num_delpages);
		values[j++] = psprintf("%f", metad->btm_last_cleanup_num_heap_tuples);
		values[j++] = metad->btm_allequalimage ? "t" : "f";
	}
	else
	{
		values[j++] = "0";
		values[j++] = "-1";
		values[j++] = "f";
	}

	tuple = BuildTupleFromCStrings(TupleDescGetAttInMetadata(tupleDesc),
								   values);

	result = HeapTupleGetDatum(tuple);

	UnlockReleaseBuffer(buffer);
	relation_close(rel, AccessShareLock);

	PG_RETURN_DATUM(result);
}
