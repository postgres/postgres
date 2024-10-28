/*-------------------------------------------------------------------------
 *
 * verify_heapam.c
 *	  Functions to check postgresql heap relations for corruption
 *
 * Copyright (c) 2016-2024, PostgreSQL Global Development Group
 *
 *	  contrib/amcheck/verify_heapam.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/genam.h"
#include "access/heaptoast.h"
#include "access/multixact.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/toast_internals.h"
#include "access/visibilitymap.h"
#include "access/xact.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"

PG_FUNCTION_INFO_V1(verify_heapam);

/* The number of columns in tuples returned by verify_heapam */
#define HEAPCHECK_RELATION_COLS 4

/* The largest valid toast va_rawsize */
#define VARLENA_SIZE_LIMIT 0x3FFFFFFF

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
	XID_BOUNDS_OK,
} XidBoundsViolation;

typedef enum XidCommitStatus
{
	XID_COMMITTED,
	XID_IS_CURRENT_XID,
	XID_IN_PROGRESS,
	XID_ABORTED,
} XidCommitStatus;

typedef enum SkipPages
{
	SKIP_PAGES_ALL_FROZEN,
	SKIP_PAGES_ALL_VISIBLE,
	SKIP_PAGES_NONE,
} SkipPages;

/*
 * Struct holding information about a toasted attribute sufficient to both
 * check the toasted attribute and, if found to be corrupt, to report where it
 * was encountered in the main table.
 */
typedef struct ToastedAttribute
{
	struct varatt_external toast_pointer;
	BlockNumber blkno;			/* block in main table */
	OffsetNumber offnum;		/* offset in main table */
	AttrNumber	attnum;			/* attribute in main table */
} ToastedAttribute;

/*
 * Struct holding the running context information during
 * a lifetime of a verify_heapam execution.
 */
typedef struct HeapCheckContext
{
	/*
	 * Cached copies of values from TransamVariables and computed values from
	 * them.
	 */
	FullTransactionId next_fxid;	/* TransamVariables->nextXid */
	TransactionId next_xid;		/* 32-bit version of next_fxid */
	TransactionId oldest_xid;	/* TransamVariables->oldestXid */
	FullTransactionId oldest_fxid;	/* 64-bit version of oldest_xid, computed
									 * relative to next_fxid */
	TransactionId safe_xmin;	/* this XID and newer ones can't become
								 * all-visible while we're running */

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

	/* True if tuple's xmax makes it eligible for pruning */
	bool		tuple_could_be_pruned;

	/*
	 * List of ToastedAttribute structs for toasted attributes which are not
	 * eligible for pruning and should be checked
	 */
	List	   *toasted_attributes;

	/* Whether verify_heapam has yet encountered any corrupt tuples */
	bool		is_corrupt;

	/* The descriptor and tuplestore for verify_heapam's result tuples */
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
} HeapCheckContext;

/* Internal implementation */
static void check_tuple(HeapCheckContext *ctx,
						bool *xmin_commit_status_ok,
						XidCommitStatus *xmin_commit_status);
static void check_toast_tuple(HeapTuple toasttup, HeapCheckContext *ctx,
							  ToastedAttribute *ta, int32 *expected_chunk_seq,
							  uint32 extsize);

static bool check_tuple_attribute(HeapCheckContext *ctx);
static void check_toasted_attribute(HeapCheckContext *ctx,
									ToastedAttribute *ta);

static bool check_tuple_header(HeapCheckContext *ctx);
static bool check_tuple_visibility(HeapCheckContext *ctx,
								   bool *xmin_commit_status_ok,
								   XidCommitStatus *xmin_commit_status);

static void report_corruption(HeapCheckContext *ctx, char *msg);
static void report_toast_corruption(HeapCheckContext *ctx,
									ToastedAttribute *ta, char *msg);
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
	ctx.toasted_attributes = NIL;

	/*
	 * Any xmin newer than the xmin of our snapshot can't become all-visible
	 * while we're running.
	 */
	ctx.safe_xmin = GetTransactionSnapshot()->xmin;

	/*
	 * If we report corruption when not examining some individual attribute,
	 * we need attnum to be reported as NULL.  Set that up before any
	 * corruption reporting might happen.
	 */
	ctx.attnum = -1;

	/* Construct the tuplestore and tuple descriptor */
	InitMaterializedSRF(fcinfo, 0);
	ctx.tupdesc = rsinfo->setDesc;
	ctx.tupstore = rsinfo->setResult;

	/* Open relation, check relkind and access method */
	ctx.rel = relation_open(relid, AccessShareLock);

	/*
	 * Check that a relation's relkind and access method are both supported.
	 */
	if (!RELKIND_HAS_TABLE_AM(ctx.rel->rd_rel->relkind) &&
		ctx.rel->rd_rel->relkind != RELKIND_SEQUENCE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot check relation \"%s\"",
						RelationGetRelationName(ctx.rel)),
				 errdetail_relkind_not_supported(ctx.rel->rd_rel->relkind)));

	/*
	 * Sequences always use heap AM, but they don't show that in the catalogs.
	 * Other relkinds might be using a different AM, so check.
	 */
	if (ctx.rel->rd_rel->relkind != RELKIND_SEQUENCE &&
		ctx.rel->rd_rel->relam != HEAP_TABLE_AM_OID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("only heap AM is supported")));

	/*
	 * Early exit for unlogged relations during recovery.  These will have no
	 * relation fork, so there won't be anything to check.  We behave as if
	 * the relation is empty.
	 */
	if (ctx.rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED &&
		RecoveryInProgress())
	{
		ereport(DEBUG1,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("cannot verify unlogged relation \"%s\" during recovery, skipping",
						RelationGetRelationName(ctx.rel))));
		relation_close(ctx.rel, AccessShareLock);
		PG_RETURN_NULL();
	}

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
		OffsetNumber predecessor[MaxOffsetNumber];
		OffsetNumber successor[MaxOffsetNumber];
		bool		lp_valid[MaxOffsetNumber];
		bool		xmin_commit_status_ok[MaxOffsetNumber];
		XidCommitStatus xmin_commit_status[MaxOffsetNumber];

		CHECK_FOR_INTERRUPTS();

		memset(predecessor, 0, sizeof(OffsetNumber) * MaxOffsetNumber);

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
			BlockNumber nextblkno;
			OffsetNumber nextoffnum;

			successor[ctx.offnum] = InvalidOffsetNumber;
			lp_valid[ctx.offnum] = false;
			xmin_commit_status_ok[ctx.offnum] = false;
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

				/*
				 * Since we've checked that this redirect points to a line
				 * pointer between FirstOffsetNumber and maxoff, it should now
				 * be safe to fetch the referenced line pointer. We expect it
				 * to be LP_NORMAL; if not, that's corruption.
				 */
				rditem = PageGetItemId(ctx.page, rdoffnum);
				if (!ItemIdIsUsed(rditem))
				{
					report_corruption(&ctx,
									  psprintf("redirected line pointer points to an unused item at offset %u",
											   (unsigned) rdoffnum));
					continue;
				}
				else if (ItemIdIsDead(rditem))
				{
					report_corruption(&ctx,
									  psprintf("redirected line pointer points to a dead item at offset %u",
											   (unsigned) rdoffnum));
					continue;
				}
				else if (ItemIdIsRedirected(rditem))
				{
					report_corruption(&ctx,
									  psprintf("redirected line pointer points to another redirected line pointer at offset %u",
											   (unsigned) rdoffnum));
					continue;
				}

				/*
				 * Record the fact that this line pointer has passed basic
				 * sanity checking, and also the offset number to which it
				 * points.
				 */
				lp_valid[ctx.offnum] = true;
				successor[ctx.offnum] = rdoffnum;
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
			lp_valid[ctx.offnum] = true;
			ctx.tuphdr = (HeapTupleHeader) PageGetItem(ctx.page, ctx.itemid);
			ctx.natts = HeapTupleHeaderGetNatts(ctx.tuphdr);

			/* Ok, ready to check this next tuple */
			check_tuple(&ctx,
						&xmin_commit_status_ok[ctx.offnum],
						&xmin_commit_status[ctx.offnum]);

			/*
			 * If the CTID field of this tuple seems to point to another tuple
			 * on the same page, record that tuple as the successor of this
			 * one.
			 */
			nextblkno = ItemPointerGetBlockNumber(&(ctx.tuphdr)->t_ctid);
			nextoffnum = ItemPointerGetOffsetNumber(&(ctx.tuphdr)->t_ctid);
			if (nextblkno == ctx.blkno && nextoffnum != ctx.offnum &&
				nextoffnum >= FirstOffsetNumber && nextoffnum <= maxoff)
				successor[ctx.offnum] = nextoffnum;
		}

		/*
		 * Update chain validation. Check each line pointer that's got a valid
		 * successor against that successor.
		 */
		ctx.attnum = -1;
		for (ctx.offnum = FirstOffsetNumber; ctx.offnum <= maxoff;
			 ctx.offnum = OffsetNumberNext(ctx.offnum))
		{
			ItemId		curr_lp;
			ItemId		next_lp;
			HeapTupleHeader curr_htup;
			HeapTupleHeader next_htup;
			TransactionId curr_xmin;
			TransactionId curr_xmax;
			TransactionId next_xmin;
			OffsetNumber nextoffnum = successor[ctx.offnum];

			/*
			 * The current line pointer may not have a successor, either
			 * because it's not valid or because it didn't point to anything.
			 * In either case, we have to give up.
			 *
			 * If the current line pointer does point to something, it's
			 * possible that the target line pointer isn't valid. We have to
			 * give up in that case, too.
			 */
			if (nextoffnum == InvalidOffsetNumber || !lp_valid[nextoffnum])
				continue;

			/* We have two valid line pointers that we can examine. */
			curr_lp = PageGetItemId(ctx.page, ctx.offnum);
			next_lp = PageGetItemId(ctx.page, nextoffnum);

			/* Handle the cases where the current line pointer is a redirect. */
			if (ItemIdIsRedirected(curr_lp))
			{
				/*
				 * We should not have set successor[ctx.offnum] to a value
				 * other than InvalidOffsetNumber unless that line pointer is
				 * LP_NORMAL.
				 */
				Assert(ItemIdIsNormal(next_lp));

				/* Can only redirect to a HOT tuple. */
				next_htup = (HeapTupleHeader) PageGetItem(ctx.page, next_lp);
				if (!HeapTupleHeaderIsHeapOnly(next_htup))
				{
					report_corruption(&ctx,
									  psprintf("redirected line pointer points to a non-heap-only tuple at offset %u",
											   (unsigned) nextoffnum));
				}

				/* HOT chains should not intersect. */
				if (predecessor[nextoffnum] != InvalidOffsetNumber)
				{
					report_corruption(&ctx,
									  psprintf("redirect line pointer points to offset %u, but offset %u also points there",
											   (unsigned) nextoffnum, (unsigned) predecessor[nextoffnum]));
					continue;
				}

				/*
				 * This redirect and the tuple to which it points seem to be
				 * part of an update chain.
				 */
				predecessor[nextoffnum] = ctx.offnum;
				continue;
			}

			/*
			 * If the next line pointer is a redirect, or if it's a tuple but
			 * the XMAX of this tuple doesn't match the XMIN of the next
			 * tuple, then the two aren't part of the same update chain and
			 * there is nothing more to do.
			 */
			if (ItemIdIsRedirected(next_lp))
				continue;
			curr_htup = (HeapTupleHeader) PageGetItem(ctx.page, curr_lp);
			curr_xmax = HeapTupleHeaderGetUpdateXid(curr_htup);
			next_htup = (HeapTupleHeader) PageGetItem(ctx.page, next_lp);
			next_xmin = HeapTupleHeaderGetXmin(next_htup);
			if (!TransactionIdIsValid(curr_xmax) ||
				!TransactionIdEquals(curr_xmax, next_xmin))
				continue;

			/* HOT chains should not intersect. */
			if (predecessor[nextoffnum] != InvalidOffsetNumber)
			{
				report_corruption(&ctx,
								  psprintf("tuple points to new version at offset %u, but offset %u also points there",
										   (unsigned) nextoffnum, (unsigned) predecessor[nextoffnum]));
				continue;
			}

			/*
			 * This tuple and the tuple to which it points seem to be part of
			 * an update chain.
			 */
			predecessor[nextoffnum] = ctx.offnum;

			/*
			 * If the current tuple is marked as HOT-updated, then the next
			 * tuple should be marked as a heap-only tuple. Conversely, if the
			 * current tuple isn't marked as HOT-updated, then the next tuple
			 * shouldn't be marked as a heap-only tuple.
			 *
			 * NB: Can't use HeapTupleHeaderIsHotUpdated() as it checks if
			 * hint bits indicate xmin/xmax aborted.
			 */
			if (!(curr_htup->t_infomask2 & HEAP_HOT_UPDATED) &&
				HeapTupleHeaderIsHeapOnly(next_htup))
			{
				report_corruption(&ctx,
								  psprintf("non-heap-only update produced a heap-only tuple at offset %u",
										   (unsigned) nextoffnum));
			}
			if ((curr_htup->t_infomask2 & HEAP_HOT_UPDATED) &&
				!HeapTupleHeaderIsHeapOnly(next_htup))
			{
				report_corruption(&ctx,
								  psprintf("heap-only update produced a non-heap only tuple at offset %u",
										   (unsigned) nextoffnum));
			}

			/*
			 * If the current tuple's xmin is still in progress but the
			 * successor tuple's xmin is committed, that's corruption.
			 *
			 * NB: We recheck the commit status of the current tuple's xmin
			 * here, because it might have committed after we checked it and
			 * before we checked the commit status of the successor tuple's
			 * xmin. This should be safe because the xmin itself can't have
			 * changed, only its commit status.
			 */
			curr_xmin = HeapTupleHeaderGetXmin(curr_htup);
			if (xmin_commit_status_ok[ctx.offnum] &&
				xmin_commit_status[ctx.offnum] == XID_IN_PROGRESS &&
				xmin_commit_status_ok[nextoffnum] &&
				xmin_commit_status[nextoffnum] == XID_COMMITTED &&
				TransactionIdIsInProgress(curr_xmin))
			{
				report_corruption(&ctx,
								  psprintf("tuple with in-progress xmin %u was updated to produce a tuple at offset %u with committed xmin %u",
										   (unsigned) curr_xmin,
										   (unsigned) ctx.offnum,
										   (unsigned) next_xmin));
			}

			/*
			 * If the current tuple's xmin is aborted but the successor
			 * tuple's xmin is in-progress or committed, that's corruption.
			 */
			if (xmin_commit_status_ok[ctx.offnum] &&
				xmin_commit_status[ctx.offnum] == XID_ABORTED &&
				xmin_commit_status_ok[nextoffnum])
			{
				if (xmin_commit_status[nextoffnum] == XID_IN_PROGRESS)
					report_corruption(&ctx,
									  psprintf("tuple with aborted xmin %u was updated to produce a tuple at offset %u with in-progress xmin %u",
											   (unsigned) curr_xmin,
											   (unsigned) ctx.offnum,
											   (unsigned) next_xmin));
				else if (xmin_commit_status[nextoffnum] == XID_COMMITTED)
					report_corruption(&ctx,
									  psprintf("tuple with aborted xmin %u was updated to produce a tuple at offset %u with committed xmin %u",
											   (unsigned) curr_xmin,
											   (unsigned) ctx.offnum,
											   (unsigned) next_xmin));
			}
		}

		/*
		 * An update chain can start either with a non-heap-only tuple or with
		 * a redirect line pointer, but not with a heap-only tuple.
		 *
		 * (This check is in a separate loop because we need the predecessor
		 * array to be fully populated before we can perform it.)
		 */
		for (ctx.offnum = FirstOffsetNumber;
			 ctx.offnum <= maxoff;
			 ctx.offnum = OffsetNumberNext(ctx.offnum))
		{
			if (xmin_commit_status_ok[ctx.offnum] &&
				(xmin_commit_status[ctx.offnum] == XID_COMMITTED ||
				 xmin_commit_status[ctx.offnum] == XID_IN_PROGRESS) &&
				predecessor[ctx.offnum] == InvalidOffsetNumber)
			{
				ItemId		curr_lp;

				curr_lp = PageGetItemId(ctx.page, ctx.offnum);
				if (!ItemIdIsRedirected(curr_lp))
				{
					HeapTupleHeader curr_htup;

					curr_htup = (HeapTupleHeader)
						PageGetItem(ctx.page, curr_lp);
					if (HeapTupleHeaderIsHeapOnly(curr_htup))
						report_corruption(&ctx,
										  psprintf("tuple is root of chain but is marked as heap-only tuple"));
				}
			}
		}

		/* clean up */
		UnlockReleaseBuffer(ctx.buffer);

		/*
		 * Check any toast pointers from the page whose lock we just released
		 */
		if (ctx.toasted_attributes != NIL)
		{
			ListCell   *cell;

			foreach(cell, ctx.toasted_attributes)
				check_toasted_attribute(&ctx, lfirst(cell));
			list_free_deep(ctx.toasted_attributes);
			ctx.toasted_attributes = NIL;
		}

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
 * Shared internal implementation for report_corruption and
 * report_toast_corruption.
 */
static void
report_corruption_internal(Tuplestorestate *tupstore, TupleDesc tupdesc,
						   BlockNumber blkno, OffsetNumber offnum,
						   AttrNumber attnum, char *msg)
{
	Datum		values[HEAPCHECK_RELATION_COLS] = {0};
	bool		nulls[HEAPCHECK_RELATION_COLS] = {0};
	HeapTuple	tuple;

	values[0] = Int64GetDatum(blkno);
	values[1] = Int32GetDatum(offnum);
	values[2] = Int32GetDatum(attnum);
	nulls[2] = (attnum < 0);
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

	tuple = heap_form_tuple(tupdesc, values, nulls);
	tuplestore_puttuple(tupstore, tuple);
}

/*
 * Record a single corruption found in the main table.  The values in ctx should
 * indicate the location of the corruption, and the msg argument should contain
 * a human-readable description of the corruption.
 *
 * The msg argument is pfree'd by this function.
 */
static void
report_corruption(HeapCheckContext *ctx, char *msg)
{
	report_corruption_internal(ctx->tupstore, ctx->tupdesc, ctx->blkno,
							   ctx->offnum, ctx->attnum, msg);
	ctx->is_corrupt = true;
}

/*
 * Record corruption found in the toast table.  The values in ta should
 * indicate the location in the main table where the toast pointer was
 * encountered, and the msg argument should contain a human-readable
 * description of the toast table corruption.
 *
 * As above, the msg argument is pfree'd by this function.
 */
static void
report_toast_corruption(HeapCheckContext *ctx, ToastedAttribute *ta,
						char *msg)
{
	report_corruption_internal(ctx->tupstore, ctx->tupdesc, ta->blkno,
							   ta->offnum, ta->attnum, msg);
	ctx->is_corrupt = true;
}

/*
 * Check for tuple header corruption.
 *
 * Some kinds of corruption make it unsafe to check the tuple attributes, for
 * example when the line pointer refers to a range of bytes outside the page.
 * In such cases, we return false (not checkable) after recording appropriate
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
 * messages for them but we do not return false merely because we detected
 * them.
 *
 * Returns whether the tuple is sufficiently sensible to undergo visibility and
 * attribute checks.
 */
static bool
check_tuple_header(HeapCheckContext *ctx)
{
	HeapTupleHeader tuphdr = ctx->tuphdr;
	uint16		infomask = tuphdr->t_infomask;
	TransactionId curr_xmax = HeapTupleHeaderGetUpdateXid(tuphdr);
	bool		result = true;
	unsigned	expected_hoff;

	if (ctx->tuphdr->t_hoff > ctx->lp_len)
	{
		report_corruption(ctx,
						  psprintf("data begins at offset %u beyond the tuple length %u",
								   ctx->tuphdr->t_hoff, ctx->lp_len));
		result = false;
	}

	if ((ctx->tuphdr->t_infomask & HEAP_XMAX_COMMITTED) &&
		(ctx->tuphdr->t_infomask & HEAP_XMAX_IS_MULTI))
	{
		report_corruption(ctx,
						  pstrdup("multixact should not be marked committed"));

		/*
		 * This condition is clearly wrong, but it's not enough to justify
		 * skipping further checks, because we don't rely on this to determine
		 * whether the tuple is visible or to interpret other relevant header
		 * fields.
		 */
	}

	if (!TransactionIdIsValid(curr_xmax) &&
		HeapTupleHeaderIsHotUpdated(tuphdr))
	{
		report_corruption(ctx,
						  psprintf("tuple has been HOT updated, but xmax is 0"));

		/*
		 * As above, even though this shouldn't happen, it's not sufficient
		 * justification for skipping further checks, we should still be able
		 * to perform sensibly.
		 */
	}

	if (HeapTupleHeaderIsHeapOnly(tuphdr) &&
		((tuphdr->t_infomask & HEAP_UPDATED) == 0))
	{
		report_corruption(ctx,
						  psprintf("tuple is heap only, but not the result of an update"));

		/* Here again, we can still perform further checks. */
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
		result = false;
	}

	return result;
}

/*
 * Checks tuple visibility so we know which further checks are safe to
 * perform.
 *
 * If a tuple could have been inserted by a transaction that also added a
 * column to the table, but which ultimately did not commit, or which has not
 * yet committed, then the table's current TupleDesc might differ from the one
 * used to construct this tuple, so we must not check it.
 *
 * As a special case, if our own transaction inserted the tuple, even if we
 * added a column to the table, our TupleDesc should match.  We could check the
 * tuple, but choose not to do so.
 *
 * If a tuple has been updated or deleted, we can still read the old tuple for
 * corruption checking purposes, as long as we are careful about concurrent
 * vacuums.  The main table tuple itself cannot be vacuumed away because we
 * hold a buffer lock on the page, but if the deleting transaction is older
 * than our transaction snapshot's xmin, then vacuum could remove the toast at
 * any time, so we must not try to follow TOAST pointers.
 *
 * If xmin or xmax values are older than can be checked against clog, or appear
 * to be in the future (possibly due to wrap-around), then we cannot make a
 * determination about the visibility of the tuple, so we skip further checks.
 *
 * Returns true if the tuple itself should be checked, false otherwise.  Sets
 * ctx->tuple_could_be_pruned if the tuple -- and thus also any associated
 * TOAST tuples -- are eligible for pruning.
 *
 * Sets *xmin_commit_status_ok to true if the commit status of xmin is known
 * and false otherwise. If it's set to true, then also set *xmin_commit_status
 * to the actual commit status.
 */
static bool
check_tuple_visibility(HeapCheckContext *ctx, bool *xmin_commit_status_ok,
					   XidCommitStatus *xmin_commit_status)
{
	TransactionId xmin;
	TransactionId xvac;
	TransactionId xmax;
	XidCommitStatus xmin_status;
	XidCommitStatus xvac_status;
	XidCommitStatus xmax_status;
	HeapTupleHeader tuphdr = ctx->tuphdr;

	ctx->tuple_could_be_pruned = true;	/* have not yet proven otherwise */
	*xmin_commit_status_ok = false; /* have not yet proven otherwise */

	/* If xmin is normal, it should be within valid range */
	xmin = HeapTupleHeaderGetXmin(tuphdr);
	switch (get_xid_status(xmin, ctx, &xmin_status))
	{
		case XID_INVALID:
			/* Could be the result of a speculative insertion that aborted. */
			return false;
		case XID_BOUNDS_OK:
			*xmin_commit_status_ok = true;
			*xmin_commit_status = xmin_status;
			break;
		case XID_IN_FUTURE:
			report_corruption(ctx,
							  psprintf("xmin %u equals or exceeds next valid transaction ID %u:%u",
									   xmin,
									   EpochFromFullTransactionId(ctx->next_fxid),
									   XidFromFullTransactionId(ctx->next_fxid)));
			return false;
		case XID_PRECEDES_CLUSTERMIN:
			report_corruption(ctx,
							  psprintf("xmin %u precedes oldest valid transaction ID %u:%u",
									   xmin,
									   EpochFromFullTransactionId(ctx->oldest_fxid),
									   XidFromFullTransactionId(ctx->oldest_fxid)));
			return false;
		case XID_PRECEDES_RELMIN:
			report_corruption(ctx,
							  psprintf("xmin %u precedes relation freeze threshold %u:%u",
									   xmin,
									   EpochFromFullTransactionId(ctx->relfrozenfxid),
									   XidFromFullTransactionId(ctx->relfrozenfxid)));
			return false;
	}

	/*
	 * Has inserting transaction committed?
	 */
	if (!HeapTupleHeaderXminCommitted(tuphdr))
	{
		if (HeapTupleHeaderXminInvalid(tuphdr))
			return false;		/* inserter aborted, don't check */
		/* Used by pre-9.0 binary upgrades */
		else if (tuphdr->t_infomask & HEAP_MOVED_OFF)
		{
			xvac = HeapTupleHeaderGetXvac(tuphdr);

			switch (get_xid_status(xvac, ctx, &xvac_status))
			{
				case XID_INVALID:
					report_corruption(ctx,
									  pstrdup("old-style VACUUM FULL transaction ID for moved off tuple is invalid"));
					return false;
				case XID_IN_FUTURE:
					report_corruption(ctx,
									  psprintf("old-style VACUUM FULL transaction ID %u for moved off tuple equals or exceeds next valid transaction ID %u:%u",
											   xvac,
											   EpochFromFullTransactionId(ctx->next_fxid),
											   XidFromFullTransactionId(ctx->next_fxid)));
					return false;
				case XID_PRECEDES_RELMIN:
					report_corruption(ctx,
									  psprintf("old-style VACUUM FULL transaction ID %u for moved off tuple precedes relation freeze threshold %u:%u",
											   xvac,
											   EpochFromFullTransactionId(ctx->relfrozenfxid),
											   XidFromFullTransactionId(ctx->relfrozenfxid)));
					return false;
				case XID_PRECEDES_CLUSTERMIN:
					report_corruption(ctx,
									  psprintf("old-style VACUUM FULL transaction ID %u for moved off tuple precedes oldest valid transaction ID %u:%u",
											   xvac,
											   EpochFromFullTransactionId(ctx->oldest_fxid),
											   XidFromFullTransactionId(ctx->oldest_fxid)));
					return false;
				case XID_BOUNDS_OK:
					break;
			}

			switch (xvac_status)
			{
				case XID_IS_CURRENT_XID:
					report_corruption(ctx,
									  psprintf("old-style VACUUM FULL transaction ID %u for moved off tuple matches our current transaction ID",
											   xvac));
					return false;
				case XID_IN_PROGRESS:
					report_corruption(ctx,
									  psprintf("old-style VACUUM FULL transaction ID %u for moved off tuple appears to be in progress",
											   xvac));
					return false;

				case XID_COMMITTED:

					/*
					 * The tuple is dead, because the xvac transaction moved
					 * it off and committed. It's checkable, but also
					 * prunable.
					 */
					return true;

				case XID_ABORTED:

					/*
					 * The original xmin must have committed, because the xvac
					 * transaction tried to move it later. Since xvac is
					 * aborted, whether it's still alive now depends on the
					 * status of xmax.
					 */
					break;
			}
		}
		/* Used by pre-9.0 binary upgrades */
		else if (tuphdr->t_infomask & HEAP_MOVED_IN)
		{
			xvac = HeapTupleHeaderGetXvac(tuphdr);

			switch (get_xid_status(xvac, ctx, &xvac_status))
			{
				case XID_INVALID:
					report_corruption(ctx,
									  pstrdup("old-style VACUUM FULL transaction ID for moved in tuple is invalid"));
					return false;
				case XID_IN_FUTURE:
					report_corruption(ctx,
									  psprintf("old-style VACUUM FULL transaction ID %u for moved in tuple equals or exceeds next valid transaction ID %u:%u",
											   xvac,
											   EpochFromFullTransactionId(ctx->next_fxid),
											   XidFromFullTransactionId(ctx->next_fxid)));
					return false;
				case XID_PRECEDES_RELMIN:
					report_corruption(ctx,
									  psprintf("old-style VACUUM FULL transaction ID %u for moved in tuple precedes relation freeze threshold %u:%u",
											   xvac,
											   EpochFromFullTransactionId(ctx->relfrozenfxid),
											   XidFromFullTransactionId(ctx->relfrozenfxid)));
					return false;
				case XID_PRECEDES_CLUSTERMIN:
					report_corruption(ctx,
									  psprintf("old-style VACUUM FULL transaction ID %u for moved in tuple precedes oldest valid transaction ID %u:%u",
											   xvac,
											   EpochFromFullTransactionId(ctx->oldest_fxid),
											   XidFromFullTransactionId(ctx->oldest_fxid)));
					return false;
				case XID_BOUNDS_OK:
					break;
			}

			switch (xvac_status)
			{
				case XID_IS_CURRENT_XID:
					report_corruption(ctx,
									  psprintf("old-style VACUUM FULL transaction ID %u for moved in tuple matches our current transaction ID",
											   xvac));
					return false;
				case XID_IN_PROGRESS:
					report_corruption(ctx,
									  psprintf("old-style VACUUM FULL transaction ID %u for moved in tuple appears to be in progress",
											   xvac));
					return false;

				case XID_COMMITTED:

					/*
					 * The original xmin must have committed, because the xvac
					 * transaction moved it later. Whether it's still alive
					 * now depends on the status of xmax.
					 */
					break;

				case XID_ABORTED:

					/*
					 * The tuple is dead, because the xvac transaction moved
					 * it off and committed. It's checkable, but also
					 * prunable.
					 */
					return true;
			}
		}
		else if (xmin_status != XID_COMMITTED)
		{
			/*
			 * Inserting transaction is not in progress, and not committed, so
			 * it might have changed the TupleDesc in ways we don't know
			 * about. Thus, don't try to check the tuple structure.
			 *
			 * If xmin_status happens to be XID_IS_CURRENT_XID, then in theory
			 * any such DDL changes ought to be visible to us, so perhaps we
			 * could check anyway in that case. But, for now, let's be
			 * conservative and treat this like any other uncommitted insert.
			 */
			return false;
		}
	}

	/*
	 * Okay, the inserter committed, so it was good at some point.  Now what
	 * about the deleting transaction?
	 */

	if (tuphdr->t_infomask & HEAP_XMAX_IS_MULTI)
	{
		/*
		 * xmax is a multixact, so sanity-check the MXID. Note that we do this
		 * prior to checking for HEAP_XMAX_INVALID or
		 * HEAP_XMAX_IS_LOCKED_ONLY. This might therefore complain about
		 * things that wouldn't actually be a problem during a normal scan,
		 * but eventually we're going to have to freeze, and that process will
		 * ignore hint bits.
		 *
		 * Even if the MXID is out of range, we still know that the original
		 * insert committed, so we can check the tuple itself. However, we
		 * can't rule out the possibility that this tuple is dead, so don't
		 * clear ctx->tuple_could_be_pruned. Possibly we should go ahead and
		 * clear that flag anyway if HEAP_XMAX_INVALID is set or if
		 * HEAP_XMAX_IS_LOCKED_ONLY is true, but for now we err on the side of
		 * avoiding possibly-bogus complaints about missing TOAST entries.
		 */
		xmax = HeapTupleHeaderGetRawXmax(tuphdr);
		switch (check_mxid_valid_in_rel(xmax, ctx))
		{
			case XID_INVALID:
				report_corruption(ctx,
								  pstrdup("multitransaction ID is invalid"));
				return true;
			case XID_PRECEDES_RELMIN:
				report_corruption(ctx,
								  psprintf("multitransaction ID %u precedes relation minimum multitransaction ID threshold %u",
										   xmax, ctx->relminmxid));
				return true;
			case XID_PRECEDES_CLUSTERMIN:
				report_corruption(ctx,
								  psprintf("multitransaction ID %u precedes oldest valid multitransaction ID threshold %u",
										   xmax, ctx->oldest_mxact));
				return true;
			case XID_IN_FUTURE:
				report_corruption(ctx,
								  psprintf("multitransaction ID %u equals or exceeds next valid multitransaction ID %u",
										   xmax,
										   ctx->next_mxact));
				return true;
			case XID_BOUNDS_OK:
				break;
		}
	}

	if (tuphdr->t_infomask & HEAP_XMAX_INVALID)
	{
		/*
		 * This tuple is live.  A concurrently running transaction could
		 * delete it before we get around to checking the toast, but any such
		 * running transaction is surely not less than our safe_xmin, so the
		 * toast cannot be vacuumed out from under us.
		 */
		ctx->tuple_could_be_pruned = false;
		return true;
	}

	if (HEAP_XMAX_IS_LOCKED_ONLY(tuphdr->t_infomask))
	{
		/*
		 * "Deleting" xact really only locked it, so the tuple is live in any
		 * case.  As above, a concurrently running transaction could delete
		 * it, but it cannot be vacuumed out from under us.
		 */
		ctx->tuple_could_be_pruned = false;
		return true;
	}

	if (tuphdr->t_infomask & HEAP_XMAX_IS_MULTI)
	{
		/*
		 * We already checked above that this multixact is within limits for
		 * this table.  Now check the update xid from this multixact.
		 */
		xmax = HeapTupleGetUpdateXid(tuphdr);
		switch (get_xid_status(xmax, ctx, &xmax_status))
		{
			case XID_INVALID:
				/* not LOCKED_ONLY, so it has to have an xmax */
				report_corruption(ctx,
								  pstrdup("update xid is invalid"));
				return true;
			case XID_IN_FUTURE:
				report_corruption(ctx,
								  psprintf("update xid %u equals or exceeds next valid transaction ID %u:%u",
										   xmax,
										   EpochFromFullTransactionId(ctx->next_fxid),
										   XidFromFullTransactionId(ctx->next_fxid)));
				return true;
			case XID_PRECEDES_RELMIN:
				report_corruption(ctx,
								  psprintf("update xid %u precedes relation freeze threshold %u:%u",
										   xmax,
										   EpochFromFullTransactionId(ctx->relfrozenfxid),
										   XidFromFullTransactionId(ctx->relfrozenfxid)));
				return true;
			case XID_PRECEDES_CLUSTERMIN:
				report_corruption(ctx,
								  psprintf("update xid %u precedes oldest valid transaction ID %u:%u",
										   xmax,
										   EpochFromFullTransactionId(ctx->oldest_fxid),
										   XidFromFullTransactionId(ctx->oldest_fxid)));
				return true;
			case XID_BOUNDS_OK:
				break;
		}

		switch (xmax_status)
		{
			case XID_IS_CURRENT_XID:
			case XID_IN_PROGRESS:

				/*
				 * The delete is in progress, so it cannot be visible to our
				 * snapshot.
				 */
				ctx->tuple_could_be_pruned = false;
				break;
			case XID_COMMITTED:

				/*
				 * The delete committed.  Whether the toast can be vacuumed
				 * away depends on how old the deleting transaction is.
				 */
				ctx->tuple_could_be_pruned = TransactionIdPrecedes(xmax,
																   ctx->safe_xmin);
				break;
			case XID_ABORTED:

				/*
				 * The delete aborted or crashed.  The tuple is still live.
				 */
				ctx->tuple_could_be_pruned = false;
				break;
		}

		/* Tuple itself is checkable even if it's dead. */
		return true;
	}

	/* xmax is an XID, not a MXID. Sanity check it. */
	xmax = HeapTupleHeaderGetRawXmax(tuphdr);
	switch (get_xid_status(xmax, ctx, &xmax_status))
	{
		case XID_INVALID:
			ctx->tuple_could_be_pruned = false;
			return true;
		case XID_IN_FUTURE:
			report_corruption(ctx,
							  psprintf("xmax %u equals or exceeds next valid transaction ID %u:%u",
									   xmax,
									   EpochFromFullTransactionId(ctx->next_fxid),
									   XidFromFullTransactionId(ctx->next_fxid)));
			return false;		/* corrupt */
		case XID_PRECEDES_RELMIN:
			report_corruption(ctx,
							  psprintf("xmax %u precedes relation freeze threshold %u:%u",
									   xmax,
									   EpochFromFullTransactionId(ctx->relfrozenfxid),
									   XidFromFullTransactionId(ctx->relfrozenfxid)));
			return false;		/* corrupt */
		case XID_PRECEDES_CLUSTERMIN:
			report_corruption(ctx,
							  psprintf("xmax %u precedes oldest valid transaction ID %u:%u",
									   xmax,
									   EpochFromFullTransactionId(ctx->oldest_fxid),
									   XidFromFullTransactionId(ctx->oldest_fxid)));
			return false;		/* corrupt */
		case XID_BOUNDS_OK:
			break;
	}

	/*
	 * Whether the toast can be vacuumed away depends on how old the deleting
	 * transaction is.
	 */
	switch (xmax_status)
	{
		case XID_IS_CURRENT_XID:
		case XID_IN_PROGRESS:

			/*
			 * The delete is in progress, so it cannot be visible to our
			 * snapshot.
			 */
			ctx->tuple_could_be_pruned = false;
			break;

		case XID_COMMITTED:

			/*
			 * The delete committed.  Whether the toast can be vacuumed away
			 * depends on how old the deleting transaction is.
			 */
			ctx->tuple_could_be_pruned = TransactionIdPrecedes(xmax,
															   ctx->safe_xmin);
			break;

		case XID_ABORTED:

			/*
			 * The delete aborted or crashed.  The tuple is still live.
			 */
			ctx->tuple_could_be_pruned = false;
			break;
	}

	/* Tuple itself is checkable even if it's dead. */
	return true;
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
 *
 * On entry, *expected_chunk_seq should be the chunk_seq value that we expect
 * to find in toasttup. On exit, it will be updated to the value the next call
 * to this function should expect to see.
 */
static void
check_toast_tuple(HeapTuple toasttup, HeapCheckContext *ctx,
				  ToastedAttribute *ta, int32 *expected_chunk_seq,
				  uint32 extsize)
{
	int32		chunk_seq;
	int32		last_chunk_seq = (extsize - 1) / TOAST_MAX_CHUNK_SIZE;
	Pointer		chunk;
	bool		isnull;
	int32		chunksize;
	int32		expected_size;

	/* Sanity-check the sequence number. */
	chunk_seq = DatumGetInt32(fastgetattr(toasttup, 2,
										  ctx->toast_rel->rd_att, &isnull));
	if (isnull)
	{
		report_toast_corruption(ctx, ta,
								psprintf("toast value %u has toast chunk with null sequence number",
										 ta->toast_pointer.va_valueid));
		return;
	}
	if (chunk_seq != *expected_chunk_seq)
	{
		/* Either the TOAST index is corrupt, or we don't have all chunks. */
		report_toast_corruption(ctx, ta,
								psprintf("toast value %u index scan returned chunk %d when expecting chunk %d",
										 ta->toast_pointer.va_valueid,
										 chunk_seq, *expected_chunk_seq));
	}
	*expected_chunk_seq = chunk_seq + 1;

	/* Sanity-check the chunk data. */
	chunk = DatumGetPointer(fastgetattr(toasttup, 3,
										ctx->toast_rel->rd_att, &isnull));
	if (isnull)
	{
		report_toast_corruption(ctx, ta,
								psprintf("toast value %u chunk %d has null data",
										 ta->toast_pointer.va_valueid,
										 chunk_seq));
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

		report_toast_corruption(ctx, ta,
								psprintf("toast value %u chunk %d has invalid varlena header %0x",
										 ta->toast_pointer.va_valueid,
										 chunk_seq, header));
		return;
	}

	/*
	 * Some checks on the data we've found
	 */
	if (chunk_seq > last_chunk_seq)
	{
		report_toast_corruption(ctx, ta,
								psprintf("toast value %u chunk %d follows last expected chunk %d",
										 ta->toast_pointer.va_valueid,
										 chunk_seq, last_chunk_seq));
		return;
	}

	expected_size = chunk_seq < last_chunk_seq ? TOAST_MAX_CHUNK_SIZE
		: extsize - (last_chunk_seq * TOAST_MAX_CHUNK_SIZE);

	if (chunksize != expected_size)
		report_toast_corruption(ctx, ta,
								psprintf("toast value %u chunk %d has size %u, but expected size %u",
										 ta->toast_pointer.va_valueid,
										 chunk_seq, chunksize, expected_size));
}

/*
 * Check the current attribute as tracked in ctx, recording any corruption
 * found in ctx->tupstore.
 *
 * This function follows the logic performed by heap_deform_tuple(), and in the
 * case of a toasted value, optionally stores the toast pointer so later it can
 * be checked following the logic of detoast_external_attr(), checking for any
 * conditions that would result in either of those functions Asserting or
 * crashing the backend.  The checks performed by Asserts present in those two
 * functions are also performed here and in check_toasted_attribute.  In cases
 * where those two functions are a bit cavalier in their assumptions about data
 * being correct, we perform additional checks not present in either of those
 * two functions.  Where some condition is checked in both of those functions,
 * we perform it here twice, as we parallel the logical flow of those two
 * functions.  The presence of duplicate checks seems a reasonable price to pay
 * for keeping this code tightly coupled with the code it protects.
 *
 * Returns true if the tuple attribute is sane enough for processing to
 * continue on to the next attribute, false otherwise.
 */
static bool
check_tuple_attribute(HeapCheckContext *ctx)
{
	Datum		attdatum;
	struct varlena *attr;
	char	   *tp;				/* pointer to the tuple data */
	uint16		infomask;
	Form_pg_attribute thisatt;
	struct varatt_external toast_pointer;

	infomask = ctx->tuphdr->t_infomask;
	thisatt = TupleDescAttr(RelationGetDescr(ctx->rel), ctx->attnum);

	tp = (char *) ctx->tuphdr + ctx->tuphdr->t_hoff;

	if (ctx->tuphdr->t_hoff + ctx->offset > ctx->lp_len)
	{
		report_corruption(ctx,
						  psprintf("attribute with length %u starts at offset %u beyond total tuple length %u",
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
							  psprintf("attribute with length %u ends at offset %u beyond total tuple length %u",
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
	 * Check that VARTAG_SIZE won't hit an Assert on a corrupt va_tag before
	 * risking a call into att_addlength_pointer
	 */
	if (VARATT_IS_EXTERNAL(tp + ctx->offset))
	{
		uint8		va_tag = VARTAG_EXTERNAL(tp + ctx->offset);

		if (va_tag != VARTAG_ONDISK)
		{
			report_corruption(ctx,
							  psprintf("toasted attribute has unexpected TOAST tag %u",
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
						  psprintf("attribute with length %u ends at offset %u beyond total tuple length %u",
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

	/*
	 * Must copy attr into toast_pointer for alignment considerations
	 */
	VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);

	/* Toasted attributes too large to be untoasted should never be stored */
	if (toast_pointer.va_rawsize > VARLENA_SIZE_LIMIT)
		report_corruption(ctx,
						  psprintf("toast value %u rawsize %d exceeds limit %d",
								   toast_pointer.va_valueid,
								   toast_pointer.va_rawsize,
								   VARLENA_SIZE_LIMIT));

	if (VARATT_EXTERNAL_IS_COMPRESSED(toast_pointer))
	{
		ToastCompressionId cmid;
		bool		valid = false;

		/* Compressed attributes should have a valid compression method */
		cmid = TOAST_COMPRESS_METHOD(&toast_pointer);
		switch (cmid)
		{
				/* List of all valid compression method IDs */
			case TOAST_PGLZ_COMPRESSION_ID:
			case TOAST_LZ4_COMPRESSION_ID:
				valid = true;
				break;

				/* Recognized but invalid compression method ID */
			case TOAST_INVALID_COMPRESSION_ID:
				break;

				/* Intentionally no default here */
		}
		if (!valid)
			report_corruption(ctx,
							  psprintf("toast value %u has invalid compression method id %d",
									   toast_pointer.va_valueid, cmid));
	}

	/* The tuple header better claim to contain toasted values */
	if (!(infomask & HEAP_HASEXTERNAL))
	{
		report_corruption(ctx,
						  psprintf("toast value %u is external but tuple header flag HEAP_HASEXTERNAL not set",
								   toast_pointer.va_valueid));
		return true;
	}

	/* The relation better have a toast table */
	if (!ctx->rel->rd_rel->reltoastrelid)
	{
		report_corruption(ctx,
						  psprintf("toast value %u is external but relation has no toast relation",
								   toast_pointer.va_valueid));
		return true;
	}

	/* If we were told to skip toast checking, then we're done. */
	if (ctx->toast_rel == NULL)
		return true;

	/*
	 * If this tuple is eligible to be pruned, we cannot check the toast.
	 * Otherwise, we push a copy of the toast tuple so we can check it after
	 * releasing the main table buffer lock.
	 */
	if (!ctx->tuple_could_be_pruned)
	{
		ToastedAttribute *ta;

		ta = (ToastedAttribute *) palloc0(sizeof(ToastedAttribute));

		VARATT_EXTERNAL_GET_POINTER(ta->toast_pointer, attr);
		ta->blkno = ctx->blkno;
		ta->offnum = ctx->offnum;
		ta->attnum = ctx->attnum;
		ctx->toasted_attributes = lappend(ctx->toasted_attributes, ta);
	}

	return true;
}

/*
 * For each attribute collected in ctx->toasted_attributes, look up the value
 * in the toast table and perform checks on it.  This function should only be
 * called on toast pointers which cannot be vacuumed away during our
 * processing.
 */
static void
check_toasted_attribute(HeapCheckContext *ctx, ToastedAttribute *ta)
{
	SnapshotData SnapshotToast;
	ScanKeyData toastkey;
	SysScanDesc toastscan;
	bool		found_toasttup;
	HeapTuple	toasttup;
	uint32		extsize;
	int32		expected_chunk_seq = 0;
	int32		last_chunk_seq;

	extsize = VARATT_EXTERNAL_GET_EXTSIZE(ta->toast_pointer);
	last_chunk_seq = (extsize - 1) / TOAST_MAX_CHUNK_SIZE;

	/*
	 * Setup a scan key to find chunks in toast table with matching va_valueid
	 */
	ScanKeyInit(&toastkey,
				(AttrNumber) 1,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(ta->toast_pointer.va_valueid));

	/*
	 * Check if any chunks for this toasted object exist in the toast table,
	 * accessible via the index.
	 */
	init_toast_snapshot(&SnapshotToast);
	toastscan = systable_beginscan_ordered(ctx->toast_rel,
										   ctx->valid_toast_index,
										   &SnapshotToast, 1,
										   &toastkey);
	found_toasttup = false;
	while ((toasttup =
			systable_getnext_ordered(toastscan,
									 ForwardScanDirection)) != NULL)
	{
		found_toasttup = true;
		check_toast_tuple(toasttup, ctx, ta, &expected_chunk_seq, extsize);
	}
	systable_endscan_ordered(toastscan);

	if (!found_toasttup)
		report_toast_corruption(ctx, ta,
								psprintf("toast value %u not found in toast table",
										 ta->toast_pointer.va_valueid));
	else if (expected_chunk_seq <= last_chunk_seq)
		report_toast_corruption(ctx, ta,
								psprintf("toast value %u was expected to end at chunk %d, but ended while expecting chunk %d",
										 ta->toast_pointer.va_valueid,
										 last_chunk_seq, expected_chunk_seq));
}

/*
 * Check the current tuple as tracked in ctx, recording any corruption found in
 * ctx->tupstore.
 *
 * We return some information about the status of xmin to aid in validating
 * update chains.
 */
static void
check_tuple(HeapCheckContext *ctx, bool *xmin_commit_status_ok,
			XidCommitStatus *xmin_commit_status)
{
	/*
	 * Check various forms of tuple header corruption, and if the header is
	 * too corrupt, do not continue with other checks.
	 */
	if (!check_tuple_header(ctx))
		return;

	/*
	 * Check tuple visibility.  If the inserting transaction aborted, we
	 * cannot assume our relation description matches the tuple structure, and
	 * therefore cannot check it.
	 */
	if (!check_tuple_visibility(ctx, xmin_commit_status_ok,
								xmin_commit_status))
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
	 * types where we don't know how to continue.  We also don't abort the
	 * checking of toasted attributes collected from the tuple prior to
	 * aborting.  Those will still be checked later along with other toasted
	 * attributes collected from the page.
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
	uint64		nextfxid_i;
	int32		diff;
	FullTransactionId fxid;

	Assert(TransactionIdIsNormal(ctx->next_xid));
	Assert(FullTransactionIdIsNormal(ctx->next_fxid));
	Assert(XidFromFullTransactionId(ctx->next_fxid) == ctx->next_xid);

	if (!TransactionIdIsNormal(xid))
		return FullTransactionIdFromEpochAndXid(0, xid);

	nextfxid_i = U64FromFullTransactionId(ctx->next_fxid);

	/* compute the 32bit modulo difference */
	diff = (int32) (ctx->next_xid - xid);

	/*
	 * In cases of corruption we might see a 32bit xid that is before epoch 0.
	 * We can't represent that as a 64bit xid, due to 64bit xids being
	 * unsigned integers, without the modulo arithmetic of 32bit xid. There's
	 * no really nice way to deal with that, but it works ok enough to use
	 * FirstNormalFullTransactionId in that case, as a freshly initdb'd
	 * cluster already has a newer horizon.
	 */
	if (diff > 0 && (nextfxid_i - FirstNormalTransactionId) < (int64) diff)
	{
		Assert(EpochFromFullTransactionId(ctx->next_fxid) == 0);
		fxid = FirstNormalFullTransactionId;
	}
	else
		fxid = FullTransactionIdFromU64(nextfxid_i - diff);

	Assert(FullTransactionIdIsNormal(fxid));
	return fxid;
}

/*
 * Update our cached range of valid transaction IDs.
 */
static void
update_cached_xid_range(HeapCheckContext *ctx)
{
	/* Make cached copies */
	LWLockAcquire(XidGenLock, LW_SHARED);
	ctx->next_fxid = TransamVariables->nextXid;
	ctx->oldest_xid = TransamVariables->oldestXid;
	LWLockRelease(XidGenLock);

	/* And compute alternate versions of the same */
	ctx->next_xid = XidFromFullTransactionId(ctx->next_fxid);
	ctx->oldest_fxid = FullTransactionIdFromXidAndCtx(ctx->oldest_xid, ctx);
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
		FullTransactionIdFromXidAndCtx(TransamVariables->oldestClogXid,
									   ctx);
	if (FullTransactionIdPrecedesOrEquals(clog_horizon, fxid))
	{
		if (TransactionIdIsCurrentTransactionId(xid))
			*status = XID_IS_CURRENT_XID;
		else if (TransactionIdIsInProgress(xid))
			*status = XID_IN_PROGRESS;
		else if (TransactionIdDidCommit(xid))
			*status = XID_COMMITTED;
		else
			*status = XID_ABORTED;
	}
	LWLockRelease(XactTruncationLock);
	ctx->cached_xid = xid;
	ctx->cached_status = *status;
	return XID_BOUNDS_OK;
}
