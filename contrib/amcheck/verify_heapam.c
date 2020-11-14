/*-------------------------------------------------------------------------
 *
 * verify_heapam.c
 *	  Functions to check postgresql heap relations for corruption
 *
 * Copyright (c) 2016-2020, PostgreSQL Global Development Group
 *
 *	  contrib/amcheck/verify_heapam.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/heaptoast.h"
#include "access/multixact.h"
#include "access/toast_internals.h"
#include "access/visibilitymap.h"
#include "catalog/pg_am.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"

PG_FUNCTION_INFO_V1(verify_heapam);

/* The number of columns in tuples returned by verify_heapam */
#define HEAPCHECK_RELATION_COLS 4

/*
 * Despite the name, we use this for reporting problems with both XIDs and
 * MXIDs.
 */
typedef enum XidBoundsViolation
{
	XID_INVALID,
	XID_IN_FUTURE,
	XID_PRECEDES_CLUSTERMIN,
	XID_PRECEDES_RELMIN,
	XID_BOUNDS_OK
} XidBoundsViolation;

typedef enum XidCommitStatus
{
	XID_COMMITTED,
	XID_IN_PROGRESS,
	XID_ABORTED
} XidCommitStatus;

typedef enum SkipPages
{
	SKIP_PAGES_ALL_FROZEN,
	SKIP_PAGES_ALL_VISIBLE,
	SKIP_PAGES_NONE
} SkipPages;

/*
 * Struct holding the running context information during
 * a lifetime of a verify_heapam execution.
 */
typedef struct HeapCheckContext
{
	/*
	 * Cached copies of values from ShmemVariableCache and computed values
	 * from them.
	 */
	FullTransactionId next_fxid;	/* ShmemVariableCache->nextXid */
	TransactionId next_xid;		/* 32-bit version of next_fxid */
	TransactionId oldest_xid;	/* ShmemVariableCache->oldestXid */
	FullTransactionId oldest_fxid;	/* 64-bit version of oldest_xid, computed
									 * relative to next_fxid */

	/*
	 * Cached copy of value from MultiXactState
	 */
	MultiXactId next_mxact;		/* MultiXactState->nextMXact */
	MultiXactId oldest_mxact;	/* MultiXactState->oldestMultiXactId */

	/*
	 * Cached copies of the most recently checked xid and its status.
	 */
	TransactionId cached_xid;
	XidCommitStatus cached_status;

	/* Values concerning the heap relation being checked */
	Relation	rel;
	TransactionId relfrozenxid;
	FullTransactionId relfrozenfxid;
	TransactionId relminmxid;
	Relation	toast_rel;
	Relation   *toast_indexes;
	Relation	valid_toast_index;
	int			num_toast_indexes;

	/* Values for iterating over pages in the relation */
	BlockNumber blkno;
	BufferAccessStrategy bstrategy;
	Buffer		buffer;
	Page		page;

	/* Values for iterating over tuples within a page */
	OffsetNumber offnum;
	ItemId		itemid;
	uint16		lp_len;
	uint16		lp_off;
	HeapTupleHeader tuphdr;
	int			natts;

	/* Values for iterating over attributes within the tuple */
	uint32		offset;			/* offset in tuple data */
	AttrNumber	attnum;

	/* Values for iterating over toast for the attribute */
	int32		chunkno;
	int32		attrsize;
	int32		endchunk;
	int32		totalchunks;

	/* Whether verify_heapam has yet encountered any corrupt tuples */
	bool		is_corrupt;

	/* The descriptor and tuplestore for verify_heapam's result tuples */
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
} HeapCheckContext;

/* Internal implementation */
static void sanity_check_relation(Relation rel);
static void check_tuple(HeapCheckContext *ctx);
static void check_toast_tuple(HeapTuple toasttup, HeapCheckContext *ctx);

static bool check_tuple_attribute(HeapCheckContext *ctx);
static bool check_tuple_header_and_visibilty(HeapTupleHeader tuphdr,
											 HeapCheckContext *ctx);

static void report_corruption(HeapCheckContext *ctx, char *msg);
static TupleDesc verify_heapam_tupdesc(void);
static FullTransactionId FullTransactionIdFromXidAndCtx(TransactionId xid,
														const HeapCheckContext *ctx);
static void update_cached_xid_range(HeapCheckContext *ctx);
static void update_cached_mxid_range(HeapCheckContext *ctx);
static XidBoundsViolation check_mxid_in_range(MultiXactId mxid,
											  HeapCheckContext *ctx);
static XidBoundsViolation check_mxid_valid_in_rel(MultiXactId mxid,
												  HeapCheckContext *ctx);
static XidBoundsViolation get_xid_status(TransactionId xid,
										 HeapCheckContext *ctx,
										 XidCommitStatus *status);

/*
 * Scan and report corruption in heap pages, optionally reconciling toasted
 * attributes with entries in the associated toast table.  Intended to be
 * called from SQL with the following parameters:
 *
 *   relation:
 *     The Oid of the heap relation to be checked.
 *
 *   on_error_stop:
 *     Whether to stop at the end of the first page for which errors are
 *     detected.  Note that multiple rows may be returned.
 *
 *   check_toast:
 *     Whether to check each toasted attribute against the toast table to
 *     verify that it can be found there.
 *
 *   skip:
 *     What kinds of pages in the heap relation should be skipped.  Valid
 *     options are "all-visible", "all-frozen", and "none".
 *
 * Returns to the SQL caller a set of tuples, each containing the location
 * and a description of a corruption found in the heap.
 *
 * This code goes to some trouble to avoid crashing the server even if the
 * table pages are badly corrupted, but it's probably not perfect. If
 * check_toast is true, we'll use regular index lookups to try to fetch TOAST
 * tuples, which can certainly cause crashes if the right kind of corruption
 * exists in the toast table or index. No matter what parameters you pass,
 * we can't protect against crashes that might occur trying to look up the
 * commit status of transaction IDs (though we avoid trying to do such lookups
 * for transaction IDs that can't legally appear in the table).
 */
Datum
verify_heapam(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	MemoryContext old_context;
	bool		random_access;
	HeapCheckContext ctx;
	Buffer		vmbuffer = InvalidBuffer;
	Oid			relid;
	bool		on_error_stop;
	bool		check_toast;
	SkipPages	skip_option = SKIP_PAGES_NONE;
	BlockNumber first_block;
	BlockNumber last_block;
	BlockNumber nblocks;
	const char *skip;

	/* Check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	/* Check supplied arguments */
	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("relation cannot be null")));
	relid = PG_GETARG_OID(0);

	if (PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("on_error_stop cannot be null")));
	on_error_stop = PG_GETARG_BOOL(1);

	if (PG_ARGISNULL(2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("check_toast cannot be null")));
	check_toast = PG_GETARG_BOOL(2);

	if (PG_ARGISNULL(3))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("skip cannot be null")));
	skip = text_to_cstring(PG_GETARG_TEXT_PP(3));
	if (pg_strcasecmp(skip, "all-visible") == 0)
		skip_option = SKIP_PAGES_ALL_VISIBLE;
	else if (pg_strcasecmp(skip, "all-frozen") == 0)
		skip_option = SKIP_PAGES_ALL_FROZEN;
	else if (pg_strcasecmp(skip, "none") == 0)
		skip_option = SKIP_PAGES_NONE;
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid skip option"),
				 errhint("Valid skip options are \"all-visible\", \"all-frozen\", and \"none\".")));

	memset(&ctx, 0, sizeof(HeapCheckContext));
	ctx.cached_xid = InvalidTransactionId;

	/*
	 * If we report corruption when not examining some individual attribute,
	 * we need attnum to be reported as NULL.  Set that up before any
	 * corruption reporting might happen.
	 */
	ctx.attnum = -1;

	/* The tupdesc and tuplestore must be created in ecxt_per_query_memory */
	old_context = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);
	random_access = (rsinfo->allowedModes & SFRM_Materialize_Random) != 0;
	ctx.tupdesc = verify_heapam_tupdesc();
	ctx.tupstore = tuplestore_begin_heap(random_access, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = ctx.tupstore;
	rsinfo->setDesc = ctx.tupdesc;
	MemoryContextSwitchTo(old_context);

	/* Open relation, check relkind and access method, and check privileges */
	ctx.rel = relation_open(relid, AccessShareLock);
	sanity_check_relation(ctx.rel);

	/* Early exit if the relation is empty */
	nblocks = RelationGetNumberOfBlocks(ctx.rel);
	if (!nblocks)
	{
		relation_close(ctx.rel, AccessShareLock);
		PG_RETURN_NULL();
	}

	ctx.bstrategy = GetAccessStrategy(BAS_BULKREAD);
	ctx.buffer = InvalidBuffer;
	ctx.page = NULL;

	/* Validate block numbers, or handle nulls. */
	if (PG_ARGISNULL(4))
		first_block = 0;
	else
	{
		int64		fb = PG_GETARG_INT64(4);

		if (fb < 0 || fb >= nblocks)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("starting block number must be between 0 and %u",
							nblocks - 1)));
		first_block = (BlockNumber) fb;
	}
	if (PG_ARGISNULL(5))
		last_block = nblocks - 1;
	else
	{
		int64		lb = PG_GETARG_INT64(5);

		if (lb < 0 || lb >= nblocks)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("ending block number must be between 0 and %u",
							nblocks - 1)));
		last_block = (BlockNumber) lb;
	}

	/* Optionally open the toast relation, if any. */
	if (ctx.rel->rd_rel->reltoastrelid && check_toast)
	{
		int			offset;

		/* Main relation has associated toast relation */
		ctx.toast_rel = table_open(ctx.rel->rd_rel->reltoastrelid,
								   AccessShareLock);
		offset = toast_open_indexes(ctx.toast_rel,
									AccessShareLock,
									&(ctx.toast_indexes),
									&(ctx.num_toast_indexes));
		ctx.valid_toast_index = ctx.toast_indexes[offset];
	}
	else
	{
		/*
		 * Main relation has no associated toast relation, or we're
		 * intentionally skipping it.
		 */
		ctx.toast_rel = NULL;
		ctx.toast_indexes = NULL;
		ctx.num_toast_indexes = 0;
	}

	update_cached_xid_range(&ctx);
	update_cached_mxid_range(&ctx);
	ctx.relfrozenxid = ctx.rel->rd_rel->relfrozenxid;
	ctx.relfrozenfxid = FullTransactionIdFromXidAndCtx(ctx.relfrozenxid, &ctx);
	ctx.relminmxid = ctx.rel->rd_rel->relminmxid;

	if (TransactionIdIsNormal(ctx.relfrozenxid))
		ctx.oldest_xid = ctx.relfrozenxid;

	for (ctx.blkno = first_block; ctx.blkno <= last_block; ctx.blkno++)
	{
		OffsetNumber maxoff;

		/* Optionally skip over all-frozen or all-visible blocks */
		if (skip_option != SKIP_PAGES_NONE)
		{
			int32		mapbits;

			mapbits = (int32) visibilitymap_get_status(ctx.rel, ctx.blkno,
													   &vmbuffer);
			if (skip_option == SKIP_PAGES_ALL_FROZEN)
			{
				if ((mapbits & VISIBILITYMAP_ALL_FROZEN) != 0)
					continue;
			}

			if (skip_option == SKIP_PAGES_ALL_VISIBLE)
			{
				if ((mapbits & VISIBILITYMAP_ALL_VISIBLE) != 0)
					continue;
			}
		}

		/* Read and lock the next page. */
		ctx.buffer = ReadBufferExtended(ctx.rel, MAIN_FORKNUM, ctx.blkno,
										RBM_NORMAL, ctx.bstrategy);
		LockBuffer(ctx.buffer, BUFFER_LOCK_SHARE);
		ctx.page = BufferGetPage(ctx.buffer);

		/* Perform tuple checks */
		maxoff = PageGetMaxOffsetNumber(ctx.page);
		for (ctx.offnum = FirstOffsetNumber; ctx.offnum <= maxoff;
			 ctx.offnum = OffsetNumberNext(ctx.offnum))
		{
			ctx.itemid = PageGetItemId(ctx.page, ctx.offnum);

			/* Skip over unused/dead line pointers */
			if (!ItemIdIsUsed(ctx.itemid) || ItemIdIsDead(ctx.itemid))
				continue;

			/*
			 * If this line pointer has been redirected, check that it
			 * redirects to a valid offset within the line pointer array
			 */
			if (ItemIdIsRedirected(ctx.itemid))
			{
				OffsetNumber rdoffnum = ItemIdGetRedirect(ctx.itemid);
				ItemId		rditem;

				if (rdoffnum < FirstOffsetNumber)
				{
					report_corruption(&ctx,
									  psprintf("line pointer redirection to item at offset %u precedes minimum offset %u",
											   (unsigned) rdoffnum,
											   (unsigned) FirstOffsetNumber));
					continue;
				}
				if (rdoffnum > maxoff)
				{
					report_corruption(&ctx,
									  psprintf("line pointer redirection to item at offset %u exceeds maximum offset %u",
											   (unsigned) rdoffnum,
											   (unsigned) maxoff));
					continue;
				}
				rditem = PageGetItemId(ctx.page, rdoffnum);
				if (!ItemIdIsUsed(rditem))
					report_corruption(&ctx,
									  psprintf("line pointer redirection to unused item at offset %u",
											   (unsigned) rdoffnum));
				continue;
			}

			/* Sanity-check the line pointer's offset and length values */
			ctx.lp_len = ItemIdGetLength(ctx.itemid);
			ctx.lp_off = ItemIdGetOffset(ctx.itemid);

			if (ctx.lp_off != MAXALIGN(ctx.lp_off))
			{
				report_corruption(&ctx,
								  psprintf("line pointer to page offset %u is not maximally aligned",
										   ctx.lp_off));
				continue;
			}
			if (ctx.lp_len < MAXALIGN(SizeofHeapTupleHeader))
			{
				report_corruption(&ctx,
								  psprintf("line pointer length %u is less than the minimum tuple header size %u",
										   ctx.lp_len,
										   (unsigned) MAXALIGN(SizeofHeapTupleHeader)));
				continue;
			}
			if (ctx.lp_off + ctx.lp_len > BLCKSZ)
			{
				report_corruption(&ctx,
								  psprintf("line pointer to page offset %u with length %u ends beyond maximum page offset %u",
										   ctx.lp_off,
										   ctx.lp_len,
										   (unsigned) BLCKSZ));
				continue;
			}

			/* It should be safe to examine the tuple's header, at least */
			ctx.tuphdr = (HeapTupleHeader) PageGetItem(ctx.page, ctx.itemid);
			ctx.natts = HeapTupleHeaderGetNatts(ctx.tuphdr);

			/* Ok, ready to check this next tuple */
			check_tuple(&ctx);
		}

		/* clean up */
		UnlockReleaseBuffer(ctx.buffer);

		if (on_error_stop && ctx.is_corrupt)
			break;
	}

	if (vmbuffer != InvalidBuffer)
		ReleaseBuffer(vmbuffer);

	/* Close the associated toast table and indexes, if any. */
	if (ctx.toast_indexes)
		toast_close_indexes(ctx.toast_indexes, ctx.num_toast_indexes,
							AccessShareLock);
	if (ctx.toast_rel)
		table_close(ctx.toast_rel, AccessShareLock);

	/* Close the main relation */
	relation_close(ctx.rel, AccessShareLock);

	PG_RETURN_NULL();
}

/*
 * Check that a relation's relkind and access method are both supported,
 * and that the caller has select privilege on the relation.
 */
static void
sanity_check_relation(Relation rel)
{
	if (rel->rd_rel->relkind != RELKIND_RELATION &&
		rel->rd_rel->relkind != RELKIND_MATVIEW &&
		rel->rd_rel->relkind != RELKIND_TOASTVALUE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table, materialized view, or TOAST table",
						RelationGetRelationName(rel))));
	if (rel->rd_rel->relam != HEAP_TABLE_AM_OID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("only heap AM is supported")));
}

/*
 * Record a single corruption found in the table.  The values in ctx should
 * reflect the location of the corruption, and the msg argument should contain
 * a human-readable description of the corruption.
 *
 * The msg argument is pfree'd by this function.
 */
static void
report_corruption(HeapCheckContext *ctx, char *msg)
{
	Datum		values[HEAPCHECK_RELATION_COLS];
	bool		nulls[HEAPCHECK_RELATION_COLS];
	HeapTuple	tuple;

	MemSet(values, 0, sizeof(values));
	MemSet(nulls, 0, sizeof(nulls));
	values[0] = Int64GetDatum(ctx->blkno);
	values[1] = Int32GetDatum(ctx->offnum);
	values[2] = Int32GetDatum(ctx->attnum);
	nulls[2] = (ctx->attnum < 0);
	values[3] = CStringGetTextDatum(msg);

	/*
	 * In principle, there is nothing to prevent a scan over a large, highly
	 * corrupted table from using work_mem worth of memory building up the
	 * tuplestore.  That's ok, but if we also leak the msg argument memory
	 * until the end of the query, we could exceed work_mem by more than a
	 * trivial amount.  Therefore, free the msg argument each time we are
	 * called rather than waiting for our current memory context to be freed.
	 */
	pfree(msg);

	tuple = heap_form_tuple(ctx->tupdesc, values, nulls);
	tuplestore_puttuple(ctx->tupstore, tuple);
	ctx->is_corrupt = true;
}

/*
 * Construct the TupleDesc used to report messages about corruptions found
 * while scanning the heap.
 */
static TupleDesc
verify_heapam_tupdesc(void)
{
	TupleDesc	tupdesc;
	AttrNumber	a = 0;

	tupdesc = CreateTemplateTupleDesc(HEAPCHECK_RELATION_COLS);
	TupleDescInitEntry(tupdesc, ++a, "blkno", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, ++a, "offnum", INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, ++a, "attnum", INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, ++a, "msg", TEXTOID, -1, 0);
	Assert(a == HEAPCHECK_RELATION_COLS);

	return BlessTupleDesc(tupdesc);
}

/*
 * Check for tuple header corruption and tuple visibility.
 *
 * Since we do not hold a snapshot, tuple visibility is not a question of
 * whether we should be able to see the tuple relative to any particular
 * snapshot, but rather a question of whether it is safe and reasonable to
 * check the tuple attributes.
 *
 * Some kinds of corruption make it unsafe to check the tuple attributes, for
 * example when the line pointer refers to a range of bytes outside the page.
 * In such cases, we return false (not visible) after recording appropriate
 * corruption messages.
 *
 * Some other kinds of tuple header corruption confuse the question of where
 * the tuple attributes begin, or how long the nulls bitmap is, etc., making it
 * unreasonable to attempt to check attributes, even if all candidate answers
 * to those questions would not result in reading past the end of the line
 * pointer or page.  In such cases, like above, we record corruption messages
 * about the header and then return false.
 *
 * Other kinds of tuple header corruption do not bear on the question of
 * whether the tuple attributes can be checked, so we record corruption
 * messages for them but do not base our visibility determination on them.  (In
 * other words, we do not return false merely because we detected them.)
 *
 * For visibility determination not specifically related to corruption, what we
 * want to know is if a tuple is potentially visible to any running
 * transaction.  If you are tempted to replace this function's visibility logic
 * with a call to another visibility checking function, keep in mind that this
 * function does not update hint bits, as it seems imprudent to write hint bits
 * (or anything at all) to a table during a corruption check.  Nor does this
 * function bother classifying tuple visibility beyond a boolean visible vs.
 * not visible.
 *
 * The caller should already have checked that xmin and xmax are not out of
 * bounds for the relation.
 *
 * Returns whether the tuple is both visible and sufficiently sensible to
 * undergo attribute checks.
 */
static bool
check_tuple_header_and_visibilty(HeapTupleHeader tuphdr, HeapCheckContext *ctx)
{
	uint16		infomask = tuphdr->t_infomask;
	bool		header_garbled = false;
	unsigned	expected_hoff;

	if (ctx->tuphdr->t_hoff > ctx->lp_len)
	{
		report_corruption(ctx,
						  psprintf("data begins at offset %u beyond the tuple length %u",
								   ctx->tuphdr->t_hoff, ctx->lp_len));
		header_garbled = true;
	}
	if ((ctx->tuphdr->t_infomask & HEAP_XMAX_LOCK_ONLY) &&
		(ctx->tuphdr->t_infomask2 & HEAP_KEYS_UPDATED))
	{
		report_corruption(ctx,
						  pstrdup("tuple is marked as only locked, but also claims key columns were updated"));
		header_garbled = true;
	}

	if ((ctx->tuphdr->t_infomask & HEAP_XMAX_COMMITTED) &&
		(ctx->tuphdr->t_infomask & HEAP_XMAX_IS_MULTI))
	{
		report_corruption(ctx,
						  pstrdup("multixact should not be marked committed"));

		/*
		 * This condition is clearly wrong, but we do not consider the header
		 * garbled, because we don't rely on this property for determining if
		 * the tuple is visible or for interpreting other relevant header
		 * fields.
		 */
	}

	if (infomask & HEAP_HASNULL)
		expected_hoff = MAXALIGN(SizeofHeapTupleHeader + BITMAPLEN(ctx->natts));
	else
		expected_hoff = MAXALIGN(SizeofHeapTupleHeader);
	if (ctx->tuphdr->t_hoff != expected_hoff)
	{
		if ((infomask & HEAP_HASNULL) && ctx->natts == 1)
			report_corruption(ctx,
							  psprintf("tuple data should begin at byte %u, but actually begins at byte %u (1 attribute, has nulls)",
									   expected_hoff, ctx->tuphdr->t_hoff));
		else if ((infomask & HEAP_HASNULL))
			report_corruption(ctx,
							  psprintf("tuple data should begin at byte %u, but actually begins at byte %u (%u attributes, has nulls)",
									   expected_hoff, ctx->tuphdr->t_hoff, ctx->natts));
		else if (ctx->natts == 1)
			report_corruption(ctx,
							  psprintf("tuple data should begin at byte %u, but actually begins at byte %u (1 attribute, no nulls)",
									   expected_hoff, ctx->tuphdr->t_hoff));
		else
			report_corruption(ctx,
							  psprintf("tuple data should begin at byte %u, but actually begins at byte %u (%u attributes, no nulls)",
									   expected_hoff, ctx->tuphdr->t_hoff, ctx->natts));
		header_garbled = true;
	}

	if (header_garbled)
		return false;			/* checking of this tuple should not continue */

	/*
	 * Ok, we can examine the header for tuple visibility purposes, though we
	 * still need to be careful about a few remaining types of header
	 * corruption.  This logic roughly follows that of
	 * HeapTupleSatisfiesVacuum.  Where possible the comments indicate which
	 * HTSV_Result we think that function might return for this tuple.
	 */
	if (!HeapTupleHeaderXminCommitted(tuphdr))
	{
		TransactionId raw_xmin = HeapTupleHeaderGetRawXmin(tuphdr);

		if (HeapTupleHeaderXminInvalid(tuphdr))
			return false;		/* HEAPTUPLE_DEAD */
		/* Used by pre-9.0 binary upgrades */
		else if (infomask & HEAP_MOVED_OFF ||
				 infomask & HEAP_MOVED_IN)
		{
			XidCommitStatus status;
			TransactionId xvac = HeapTupleHeaderGetXvac(tuphdr);

			switch (get_xid_status(xvac, ctx, &status))
			{
				case XID_INVALID:
					report_corruption(ctx,
									  pstrdup("old-style VACUUM FULL transaction ID is invalid"));
					return false;	/* corrupt */
				case XID_IN_FUTURE:
					report_corruption(ctx,
									  psprintf("old-style VACUUM FULL transaction ID %u equals or exceeds next valid transaction ID %u:%u",
											   xvac,
											   EpochFromFullTransactionId(ctx->next_fxid),
											   XidFromFullTransactionId(ctx->next_fxid)));
					return false;	/* corrupt */
				case XID_PRECEDES_RELMIN:
					report_corruption(ctx,
									  psprintf("old-style VACUUM FULL transaction ID %u precedes relation freeze threshold %u:%u",
											   xvac,
											   EpochFromFullTransactionId(ctx->relfrozenfxid),
											   XidFromFullTransactionId(ctx->relfrozenfxid)));
					return false;	/* corrupt */
					break;
				case XID_PRECEDES_CLUSTERMIN:
					report_corruption(ctx,
									  psprintf("old-style VACUUM FULL transaction ID %u precedes oldest valid transaction ID %u:%u",
											   xvac,
											   EpochFromFullTransactionId(ctx->oldest_fxid),
											   XidFromFullTransactionId(ctx->oldest_fxid)));
					return false;	/* corrupt */
					break;
				case XID_BOUNDS_OK:
					switch (status)
					{
						case XID_IN_PROGRESS:
							return true;	/* HEAPTUPLE_DELETE_IN_PROGRESS */
						case XID_COMMITTED:
						case XID_ABORTED:
							return false;	/* HEAPTUPLE_DEAD */
					}
			}
		}
		else
		{
			XidCommitStatus status;

			switch (get_xid_status(raw_xmin, ctx, &status))
			{
				case XID_INVALID:
					report_corruption(ctx,
									  pstrdup("raw xmin is invalid"));
					return false;
				case XID_IN_FUTURE:
					report_corruption(ctx,
									  psprintf("raw xmin %u equals or exceeds next valid transaction ID %u:%u",
											   raw_xmin,
											   EpochFromFullTransactionId(ctx->next_fxid),
											   XidFromFullTransactionId(ctx->next_fxid)));
					return false;	/* corrupt */
				case XID_PRECEDES_RELMIN:
					report_corruption(ctx,
									  psprintf("raw xmin %u precedes relation freeze threshold %u:%u",
											   raw_xmin,
											   EpochFromFullTransactionId(ctx->relfrozenfxid),
											   XidFromFullTransactionId(ctx->relfrozenfxid)));
					return false;	/* corrupt */
				case XID_PRECEDES_CLUSTERMIN:
					report_corruption(ctx,
									  psprintf("raw xmin %u precedes oldest valid transaction ID %u:%u",
											   raw_xmin,
											   EpochFromFullTransactionId(ctx->oldest_fxid),
											   XidFromFullTransactionId(ctx->oldest_fxid)));
					return false;	/* corrupt */
				case XID_BOUNDS_OK:
					switch (status)
					{
						case XID_COMMITTED:
							break;
						case XID_IN_PROGRESS:
							return true;	/* insert or delete in progress */
						case XID_ABORTED:
							return false;	/* HEAPTUPLE_DEAD */
					}
			}
		}
	}

	if (!(infomask & HEAP_XMAX_INVALID) && !HEAP_XMAX_IS_LOCKED_ONLY(infomask))
	{
		if (infomask & HEAP_XMAX_IS_MULTI)
		{
			XidCommitStatus status;
			TransactionId xmax = HeapTupleGetUpdateXid(tuphdr);

			switch (get_xid_status(xmax, ctx, &status))
			{
					/* not LOCKED_ONLY, so it has to have an xmax */
				case XID_INVALID:
					report_corruption(ctx,
									  pstrdup("xmax is invalid"));
					return false;	/* corrupt */
				case XID_IN_FUTURE:
					report_corruption(ctx,
									  psprintf("xmax %u equals or exceeds next valid transaction ID %u:%u",
											   xmax,
											   EpochFromFullTransactionId(ctx->next_fxid),
											   XidFromFullTransactionId(ctx->next_fxid)));
					return false;	/* corrupt */
				case XID_PRECEDES_RELMIN:
					report_corruption(ctx,
									  psprintf("xmax %u precedes relation freeze threshold %u:%u",
											   xmax,
											   EpochFromFullTransactionId(ctx->relfrozenfxid),
											   XidFromFullTransactionId(ctx->relfrozenfxid)));
					return false;	/* corrupt */
				case XID_PRECEDES_CLUSTERMIN:
					report_corruption(ctx,
									  psprintf("xmax %u precedes oldest valid transaction ID %u:%u",
											   xmax,
											   EpochFromFullTransactionId(ctx->oldest_fxid),
											   XidFromFullTransactionId(ctx->oldest_fxid)));
					return false;	/* corrupt */
				case XID_BOUNDS_OK:
					switch (status)
					{
						case XID_IN_PROGRESS:
							return true;	/* HEAPTUPLE_DELETE_IN_PROGRESS */
						case XID_COMMITTED:
						case XID_ABORTED:
							return false;	/* HEAPTUPLE_RECENTLY_DEAD or
											 * HEAPTUPLE_DEAD */
					}
			}

			/* Ok, the tuple is live */
		}
		else if (!(infomask & HEAP_XMAX_COMMITTED))
			return true;		/* HEAPTUPLE_DELETE_IN_PROGRESS or
								 * HEAPTUPLE_LIVE */
		else
			return false;		/* HEAPTUPLE_RECENTLY_DEAD or HEAPTUPLE_DEAD */
	}
	return true;				/* not dead */
}

/*
 * Check the current toast tuple against the state tracked in ctx, recording
 * any corruption found in ctx->tupstore.
 *
 * This is not equivalent to running verify_heapam on the toast table itself,
 * and is not hardened against corruption of the toast table.  Rather, when
 * validating a toasted attribute in the main table, the sequence of toast
 * tuples that store the toasted value are retrieved and checked in order, with
 * each toast tuple being checked against where we are in the sequence, as well
 * as each toast tuple having its varlena structure sanity checked.
 */
static void
check_toast_tuple(HeapTuple toasttup, HeapCheckContext *ctx)
{
	int32		curchunk;
	Pointer		chunk;
	bool		isnull;
	int32		chunksize;
	int32		expected_size;

	/*
	 * Have a chunk, extract the sequence number and the data
	 */
	curchunk = DatumGetInt32(fastgetattr(toasttup, 2,
										 ctx->toast_rel->rd_att, &isnull));
	if (isnull)
	{
		report_corruption(ctx,
						  pstrdup("toast chunk sequence number is null"));
		return;
	}
	chunk = DatumGetPointer(fastgetattr(toasttup, 3,
										ctx->toast_rel->rd_att, &isnull));
	if (isnull)
	{
		report_corruption(ctx,
						  pstrdup("toast chunk data is null"));
		return;
	}
	if (!VARATT_IS_EXTENDED(chunk))
		chunksize = VARSIZE(chunk) - VARHDRSZ;
	else if (VARATT_IS_SHORT(chunk))
	{
		/*
		 * could happen due to heap_form_tuple doing its thing
		 */
		chunksize = VARSIZE_SHORT(chunk) - VARHDRSZ_SHORT;
	}
	else
	{
		/* should never happen */
		uint32		header = ((varattrib_4b *) chunk)->va_4byte.va_header;

		report_corruption(ctx,
						  psprintf("corrupt extended toast chunk has invalid varlena header: %0x (sequence number %d)",
								   header, curchunk));
		return;
	}

	/*
	 * Some checks on the data we've found
	 */
	if (curchunk != ctx->chunkno)
	{
		report_corruption(ctx,
						  psprintf("toast chunk sequence number %u does not match the expected sequence number %u",
								   curchunk, ctx->chunkno));
		return;
	}
	if (curchunk > ctx->endchunk)
	{
		report_corruption(ctx,
						  psprintf("toast chunk sequence number %u exceeds the end chunk sequence number %u",
								   curchunk, ctx->endchunk));
		return;
	}

	expected_size = curchunk < ctx->totalchunks - 1 ? TOAST_MAX_CHUNK_SIZE
		: ctx->attrsize - ((ctx->totalchunks - 1) * TOAST_MAX_CHUNK_SIZE);
	if (chunksize != expected_size)
	{
		report_corruption(ctx,
						  psprintf("toast chunk size %u differs from the expected size %u",
								   chunksize, expected_size));
		return;
	}
}

/*
 * Check the current attribute as tracked in ctx, recording any corruption
 * found in ctx->tupstore.
 *
 * This function follows the logic performed by heap_deform_tuple(), and in the
 * case of a toasted value, optionally continues along the logic of
 * detoast_external_attr(), checking for any conditions that would result in
 * either of those functions Asserting or crashing the backend.  The checks
 * performed by Asserts present in those two functions are also performed here.
 * In cases where those two functions are a bit cavalier in their assumptions
 * about data being correct, we perform additional checks not present in either
 * of those two functions.  Where some condition is checked in both of those
 * functions, we perform it here twice, as we parallel the logical flow of
 * those two functions.  The presence of duplicate checks seems a reasonable
 * price to pay for keeping this code tightly coupled with the code it
 * protects.
 *
 * Returns true if the tuple attribute is sane enough for processing to
 * continue on to the next attribute, false otherwise.
 */
static bool
check_tuple_attribute(HeapCheckContext *ctx)
{
	struct varatt_external toast_pointer;
	ScanKeyData toastkey;
	SysScanDesc toastscan;
	SnapshotData SnapshotToast;
	HeapTuple	toasttup;
	bool		found_toasttup;
	Datum		attdatum;
	struct varlena *attr;
	char	   *tp;				/* pointer to the tuple data */
	uint16		infomask;
	Form_pg_attribute thisatt;

	infomask = ctx->tuphdr->t_infomask;
	thisatt = TupleDescAttr(RelationGetDescr(ctx->rel), ctx->attnum);

	tp = (char *) ctx->tuphdr + ctx->tuphdr->t_hoff;

	if (ctx->tuphdr->t_hoff + ctx->offset > ctx->lp_len)
	{
		report_corruption(ctx,
						  psprintf("attribute %u with length %u starts at offset %u beyond total tuple length %u",
								   ctx->attnum,
								   thisatt->attlen,
								   ctx->tuphdr->t_hoff + ctx->offset,
								   ctx->lp_len));
		return false;
	}

	/* Skip null values */
	if (infomask & HEAP_HASNULL && att_isnull(ctx->attnum, ctx->tuphdr->t_bits))
		return true;

	/* Skip non-varlena values, but update offset first */
	if (thisatt->attlen != -1)
	{
		ctx->offset = att_align_nominal(ctx->offset, thisatt->attalign);
		ctx->offset = att_addlength_pointer(ctx->offset, thisatt->attlen,
											tp + ctx->offset);
		if (ctx->tuphdr->t_hoff + ctx->offset > ctx->lp_len)
		{
			report_corruption(ctx,
							  psprintf("attribute %u with length %u ends at offset %u beyond total tuple length %u",
									   ctx->attnum,
									   thisatt->attlen,
									   ctx->tuphdr->t_hoff + ctx->offset,
									   ctx->lp_len));
			return false;
		}
		return true;
	}

	/* Ok, we're looking at a varlena attribute. */
	ctx->offset = att_align_pointer(ctx->offset, thisatt->attalign, -1,
									tp + ctx->offset);

	/* Get the (possibly corrupt) varlena datum */
	attdatum = fetchatt(thisatt, tp + ctx->offset);

	/*
	 * We have the datum, but we cannot decode it carelessly, as it may still
	 * be corrupt.
	 */

	/*
	 * Check that VARTAG_SIZE won't hit a TrapMacro on a corrupt va_tag before
	 * risking a call into att_addlength_pointer
	 */
	if (VARATT_IS_EXTERNAL(tp + ctx->offset))
	{
		uint8		va_tag = VARTAG_EXTERNAL(tp + ctx->offset);

		if (va_tag != VARTAG_ONDISK)
		{
			report_corruption(ctx,
							  psprintf("toasted attribute %u has unexpected TOAST tag %u",
									   ctx->attnum,
									   va_tag));
			/* We can't know where the next attribute begins */
			return false;
		}
	}

	/* Ok, should be safe now */
	ctx->offset = att_addlength_pointer(ctx->offset, thisatt->attlen,
										tp + ctx->offset);

	if (ctx->tuphdr->t_hoff + ctx->offset > ctx->lp_len)
	{
		report_corruption(ctx,
						  psprintf("attribute %u with length %u ends at offset %u beyond total tuple length %u",
								   ctx->attnum,
								   thisatt->attlen,
								   ctx->tuphdr->t_hoff + ctx->offset,
								   ctx->lp_len));

		return false;
	}

	/*
	 * heap_deform_tuple would be done with this attribute at this point,
	 * having stored it in values[], and would continue to the next attribute.
	 * We go further, because we need to check if the toast datum is corrupt.
	 */

	attr = (struct varlena *) DatumGetPointer(attdatum);

	/*
	 * Now we follow the logic of detoast_external_attr(), with the same
	 * caveats about being paranoid about corruption.
	 */

	/* Skip values that are not external */
	if (!VARATT_IS_EXTERNAL(attr))
		return true;

	/* It is external, and we're looking at a page on disk */

	/* The tuple header better claim to contain toasted values */
	if (!(infomask & HEAP_HASEXTERNAL))
	{
		report_corruption(ctx,
						  psprintf("attribute %u is external but tuple header flag HEAP_HASEXTERNAL not set",
								   ctx->attnum));
		return true;
	}

	/* The relation better have a toast table */
	if (!ctx->rel->rd_rel->reltoastrelid)
	{
		report_corruption(ctx,
						  psprintf("attribute %u is external but relation has no toast relation",
								   ctx->attnum));
		return true;
	}

	/* If we were told to skip toast checking, then we're done. */
	if (ctx->toast_rel == NULL)
		return true;

	/*
	 * Must copy attr into toast_pointer for alignment considerations
	 */
	VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);

	ctx->attrsize = toast_pointer.va_extsize;
	ctx->endchunk = (ctx->attrsize - 1) / TOAST_MAX_CHUNK_SIZE;
	ctx->totalchunks = ctx->endchunk + 1;

	/*
	 * Setup a scan key to find chunks in toast table with matching va_valueid
	 */
	ScanKeyInit(&toastkey,
				(AttrNumber) 1,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(toast_pointer.va_valueid));

	/*
	 * Check if any chunks for this toasted object exist in the toast table,
	 * accessible via the index.
	 */
	init_toast_snapshot(&SnapshotToast);
	toastscan = systable_beginscan_ordered(ctx->toast_rel,
										   ctx->valid_toast_index,
										   &SnapshotToast, 1,
										   &toastkey);
	ctx->chunkno = 0;
	found_toasttup = false;
	while ((toasttup =
			systable_getnext_ordered(toastscan,
									 ForwardScanDirection)) != NULL)
	{
		found_toasttup = true;
		check_toast_tuple(toasttup, ctx);
		ctx->chunkno++;
	}
	if (ctx->chunkno != (ctx->endchunk + 1))
		report_corruption(ctx,
						  psprintf("final toast chunk number %u differs from expected value %u",
								   ctx->chunkno, (ctx->endchunk + 1)));
	if (!found_toasttup)
		report_corruption(ctx,
						  psprintf("toasted value for attribute %u missing from toast table",
								   ctx->attnum));
	systable_endscan_ordered(toastscan);

	return true;
}

/*
 * Check the current tuple as tracked in ctx, recording any corruption found in
 * ctx->tupstore.
 */
static void
check_tuple(HeapCheckContext *ctx)
{
	TransactionId xmin;
	TransactionId xmax;
	bool		fatal = false;
	uint16		infomask = ctx->tuphdr->t_infomask;

	/* If xmin is normal, it should be within valid range */
	xmin = HeapTupleHeaderGetXmin(ctx->tuphdr);
	switch (get_xid_status(xmin, ctx, NULL))
	{
		case XID_INVALID:
		case XID_BOUNDS_OK:
			break;
		case XID_IN_FUTURE:
			report_corruption(ctx,
							  psprintf("xmin %u equals or exceeds next valid transaction ID %u:%u",
									   xmin,
									   EpochFromFullTransactionId(ctx->next_fxid),
									   XidFromFullTransactionId(ctx->next_fxid)));
			fatal = true;
			break;
		case XID_PRECEDES_CLUSTERMIN:
			report_corruption(ctx,
							  psprintf("xmin %u precedes oldest valid transaction ID %u:%u",
									   xmin,
									   EpochFromFullTransactionId(ctx->oldest_fxid),
									   XidFromFullTransactionId(ctx->oldest_fxid)));
			fatal = true;
			break;
		case XID_PRECEDES_RELMIN:
			report_corruption(ctx,
							  psprintf("xmin %u precedes relation freeze threshold %u:%u",
									   xmin,
									   EpochFromFullTransactionId(ctx->relfrozenfxid),
									   XidFromFullTransactionId(ctx->relfrozenfxid)));
			fatal = true;
			break;
	}

	xmax = HeapTupleHeaderGetRawXmax(ctx->tuphdr);

	if (infomask & HEAP_XMAX_IS_MULTI)
	{
		/* xmax is a multixact, so it should be within valid MXID range */
		switch (check_mxid_valid_in_rel(xmax, ctx))
		{
			case XID_INVALID:
				report_corruption(ctx,
								  pstrdup("multitransaction ID is invalid"));
				fatal = true;
				break;
			case XID_PRECEDES_RELMIN:
				report_corruption(ctx,
								  psprintf("multitransaction ID %u precedes relation minimum multitransaction ID threshold %u",
										   xmax, ctx->relminmxid));
				fatal = true;
				break;
			case XID_PRECEDES_CLUSTERMIN:
				report_corruption(ctx,
								  psprintf("multitransaction ID %u precedes oldest valid multitransaction ID threshold %u",
										   xmax, ctx->oldest_mxact));
				fatal = true;
				break;
			case XID_IN_FUTURE:
				report_corruption(ctx,
								  psprintf("multitransaction ID %u equals or exceeds next valid multitransaction ID %u",
										   xmax,
										   ctx->next_mxact));
				fatal = true;
				break;
			case XID_BOUNDS_OK:
				break;
		}
	}
	else
	{
		/*
		 * xmax is not a multixact and is normal, so it should be within the
		 * valid XID range.
		 */
		switch (get_xid_status(xmax, ctx, NULL))
		{
			case XID_INVALID:
			case XID_BOUNDS_OK:
				break;
			case XID_IN_FUTURE:
				report_corruption(ctx,
								  psprintf("xmax %u equals or exceeds next valid transaction ID %u:%u",
										   xmax,
										   EpochFromFullTransactionId(ctx->next_fxid),
										   XidFromFullTransactionId(ctx->next_fxid)));
				fatal = true;
				break;
			case XID_PRECEDES_CLUSTERMIN:
				report_corruption(ctx,
								  psprintf("xmax %u precedes oldest valid transaction ID %u:%u",
										   xmax,
										   EpochFromFullTransactionId(ctx->oldest_fxid),
										   XidFromFullTransactionId(ctx->oldest_fxid)));
				fatal = true;
				break;
			case XID_PRECEDES_RELMIN:
				report_corruption(ctx,
								  psprintf("xmax %u precedes relation freeze threshold %u:%u",
										   xmax,
										   EpochFromFullTransactionId(ctx->relfrozenfxid),
										   XidFromFullTransactionId(ctx->relfrozenfxid)));
				fatal = true;
		}
	}

	/*
	 * Cannot process tuple data if tuple header was corrupt, as the offsets
	 * within the page cannot be trusted, leaving too much risk of reading
	 * garbage if we continue.
	 *
	 * We also cannot process the tuple if the xmin or xmax were invalid
	 * relative to relfrozenxid or relminmxid, as clog entries for the xids
	 * may already be gone.
	 */
	if (fatal)
		return;

	/*
	 * Check various forms of tuple header corruption.  If the header is too
	 * corrupt to continue checking, or if the tuple is not visible to anyone,
	 * we cannot continue with other checks.
	 */
	if (!check_tuple_header_and_visibilty(ctx->tuphdr, ctx))
		return;

	/*
	 * The tuple is visible, so it must be compatible with the current version
	 * of the relation descriptor. It might have fewer columns than are
	 * present in the relation descriptor, but it cannot have more.
	 */
	if (RelationGetDescr(ctx->rel)->natts < ctx->natts)
	{
		report_corruption(ctx,
						  psprintf("number of attributes %u exceeds maximum expected for table %u",
								   ctx->natts,
								   RelationGetDescr(ctx->rel)->natts));
		return;
	}

	/*
	 * Check each attribute unless we hit corruption that confuses what to do
	 * next, at which point we abort further attribute checks for this tuple.
	 * Note that we don't abort for all types of corruption, only for those
	 * types where we don't know how to continue.
	 */
	ctx->offset = 0;
	for (ctx->attnum = 0; ctx->attnum < ctx->natts; ctx->attnum++)
		if (!check_tuple_attribute(ctx))
			break;				/* cannot continue */

	/* revert attnum to -1 until we again examine individual attributes */
	ctx->attnum = -1;
}

/*
 * Convert a TransactionId into a FullTransactionId using our cached values of
 * the valid transaction ID range.  It is the caller's responsibility to have
 * already updated the cached values, if necessary.
 */
static FullTransactionId
FullTransactionIdFromXidAndCtx(TransactionId xid, const HeapCheckContext *ctx)
{
	uint32		epoch;

	if (!TransactionIdIsNormal(xid))
		return FullTransactionIdFromEpochAndXid(0, xid);
	epoch = EpochFromFullTransactionId(ctx->next_fxid);
	if (xid > ctx->next_xid)
		epoch--;
	return FullTransactionIdFromEpochAndXid(epoch, xid);
}

/*
 * Update our cached range of valid transaction IDs.
 */
static void
update_cached_xid_range(HeapCheckContext *ctx)
{
	/* Make cached copies */
	LWLockAcquire(XidGenLock, LW_SHARED);
	ctx->next_fxid = ShmemVariableCache->nextXid;
	ctx->oldest_xid = ShmemVariableCache->oldestXid;
	LWLockRelease(XidGenLock);

	/* And compute alternate versions of the same */
	ctx->oldest_fxid = FullTransactionIdFromXidAndCtx(ctx->oldest_xid, ctx);
	ctx->next_xid = XidFromFullTransactionId(ctx->next_fxid);
}

/*
 * Update our cached range of valid multitransaction IDs.
 */
static void
update_cached_mxid_range(HeapCheckContext *ctx)
{
	ReadMultiXactIdRange(&ctx->oldest_mxact, &ctx->next_mxact);
}

/*
 * Return whether the given FullTransactionId is within our cached valid
 * transaction ID range.
 */
static inline bool
fxid_in_cached_range(FullTransactionId fxid, const HeapCheckContext *ctx)
{
	return (FullTransactionIdPrecedesOrEquals(ctx->oldest_fxid, fxid) &&
			FullTransactionIdPrecedes(fxid, ctx->next_fxid));
}

/*
 * Checks whether a multitransaction ID is in the cached valid range, returning
 * the nature of the range violation, if any.
 */
static XidBoundsViolation
check_mxid_in_range(MultiXactId mxid, HeapCheckContext *ctx)
{
	if (!TransactionIdIsValid(mxid))
		return XID_INVALID;
	if (MultiXactIdPrecedes(mxid, ctx->relminmxid))
		return XID_PRECEDES_RELMIN;
	if (MultiXactIdPrecedes(mxid, ctx->oldest_mxact))
		return XID_PRECEDES_CLUSTERMIN;
	if (MultiXactIdPrecedesOrEquals(ctx->next_mxact, mxid))
		return XID_IN_FUTURE;
	return XID_BOUNDS_OK;
}

/*
 * Checks whether the given mxid is valid to appear in the heap being checked,
 * returning the nature of the range violation, if any.
 *
 * This function attempts to return quickly by caching the known valid mxid
 * range in ctx.  Callers should already have performed the initial setup of
 * the cache prior to the first call to this function.
 */
static XidBoundsViolation
check_mxid_valid_in_rel(MultiXactId mxid, HeapCheckContext *ctx)
{
	XidBoundsViolation result;

	result = check_mxid_in_range(mxid, ctx);
	if (result == XID_BOUNDS_OK)
		return XID_BOUNDS_OK;

	/* The range may have advanced.  Recheck. */
	update_cached_mxid_range(ctx);
	return check_mxid_in_range(mxid, ctx);
}

/*
 * Checks whether the given transaction ID is (or was recently) valid to appear
 * in the heap being checked, or whether it is too old or too new to appear in
 * the relation, returning information about the nature of the bounds violation.
 *
 * We cache the range of valid transaction IDs.  If xid is in that range, we
 * conclude that it is valid, even though concurrent changes to the table might
 * invalidate it under certain corrupt conditions.  (For example, if the table
 * contains corrupt all-frozen bits, a concurrent vacuum might skip the page(s)
 * containing the xid and then truncate clog and advance the relfrozenxid
 * beyond xid.) Reporting the xid as valid under such conditions seems
 * acceptable, since if we had checked it earlier in our scan it would have
 * truly been valid at that time.
 *
 * If the status argument is not NULL, and if and only if the transaction ID
 * appears to be valid in this relation, the status argument will be set with
 * the commit status of the transaction ID.
 */
static XidBoundsViolation
get_xid_status(TransactionId xid, HeapCheckContext *ctx,
			   XidCommitStatus *status)
{
	FullTransactionId fxid;
	FullTransactionId clog_horizon;

	/* Quick check for special xids */
	if (!TransactionIdIsValid(xid))
		return XID_INVALID;
	else if (xid == BootstrapTransactionId || xid == FrozenTransactionId)
	{
		if (status != NULL)
			*status = XID_COMMITTED;
		return XID_BOUNDS_OK;
	}

	/* Check if the xid is within bounds */
	fxid = FullTransactionIdFromXidAndCtx(xid, ctx);
	if (!fxid_in_cached_range(fxid, ctx))
	{
		/*
		 * We may have been checking against stale values.  Update the cached
		 * range to be sure, and since we relied on the cached range when we
		 * performed the full xid conversion, reconvert.
		 */
		update_cached_xid_range(ctx);
		fxid = FullTransactionIdFromXidAndCtx(xid, ctx);
	}

	if (FullTransactionIdPrecedesOrEquals(ctx->next_fxid, fxid))
		return XID_IN_FUTURE;
	if (FullTransactionIdPrecedes(fxid, ctx->oldest_fxid))
		return XID_PRECEDES_CLUSTERMIN;
	if (FullTransactionIdPrecedes(fxid, ctx->relfrozenfxid))
		return XID_PRECEDES_RELMIN;

	/* Early return if the caller does not request clog checking */
	if (status == NULL)
		return XID_BOUNDS_OK;

	/* Early return if we just checked this xid in a prior call */
	if (xid == ctx->cached_xid)
	{
		*status = ctx->cached_status;
		return XID_BOUNDS_OK;
	}

	*status = XID_COMMITTED;
	LWLockAcquire(XactTruncationLock, LW_SHARED);
	clog_horizon =
		FullTransactionIdFromXidAndCtx(ShmemVariableCache->oldestClogXid,
									   ctx);
	if (FullTransactionIdPrecedesOrEquals(clog_horizon, fxid))
	{
		if (TransactionIdIsCurrentTransactionId(xid))
			*status = XID_IN_PROGRESS;
		else if (TransactionIdDidCommit(xid))
			*status = XID_COMMITTED;
		else if (TransactionIdDidAbort(xid))
			*status = XID_ABORTED;
		else
			*status = XID_IN_PROGRESS;
	}
	LWLockRelease(XactTruncationLock);
	ctx->cached_xid = xid;
	ctx->cached_status = *status;
	return XID_BOUNDS_OK;
}
