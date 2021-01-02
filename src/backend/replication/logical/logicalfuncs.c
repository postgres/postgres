/*-------------------------------------------------------------------------
 *
 * logicalfuncs.c
 *
 *	   Support functions for using logical decoding and management of
 *	   logical replication slots via SQL.
 *
 *
 * Copyright (c) 2012-2021, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logicalfuncs.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>

#include "access/xact.h"
#include "access/xlog_internal.h"
#include "access/xlogutils.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "replication/decode.h"
#include "replication/logical.h"
#include "replication/message.h"
#include "storage/fd.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_lsn.h"
#include "utils/regproc.h"
#include "utils/resowner.h"

/* private date for writing out data */
typedef struct DecodingOutputState
{
	Tuplestorestate *tupstore;
	TupleDesc	tupdesc;
	bool		binary_output;
	int64		returned_rows;
} DecodingOutputState;

/*
 * Prepare for an output plugin write.
 */
static void
LogicalOutputPrepareWrite(LogicalDecodingContext *ctx, XLogRecPtr lsn, TransactionId xid,
						  bool last_write)
{
	resetStringInfo(ctx->out);
}

/*
 * Perform output plugin write into tuplestore.
 */
static void
LogicalOutputWrite(LogicalDecodingContext *ctx, XLogRecPtr lsn, TransactionId xid,
				   bool last_write)
{
	Datum		values[3];
	bool		nulls[3];
	DecodingOutputState *p;

	/* SQL Datums can only be of a limited length... */
	if (ctx->out->len > MaxAllocSize - VARHDRSZ)
		elog(ERROR, "too much output for sql interface");

	p = (DecodingOutputState *) ctx->output_writer_private;

	memset(nulls, 0, sizeof(nulls));
	values[0] = LSNGetDatum(lsn);
	values[1] = TransactionIdGetDatum(xid);

	/*
	 * Assert ctx->out is in database encoding when we're writing textual
	 * output.
	 */
	if (!p->binary_output)
		Assert(pg_verify_mbstr(GetDatabaseEncoding(),
							   ctx->out->data, ctx->out->len,
							   false));

	/* ick, but cstring_to_text_with_len works for bytea perfectly fine */
	values[2] = PointerGetDatum(cstring_to_text_with_len(ctx->out->data, ctx->out->len));

	tuplestore_putvalues(p->tupstore, p->tupdesc, values, nulls);
	p->returned_rows++;
}

static void
check_permissions(void)
{
	if (!superuser() && !has_rolreplication(GetUserId()))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser or replication role to use replication slots")));
}

/*
 * Helper function for the various SQL callable logical decoding functions.
 */
static Datum
pg_logical_slot_get_changes_guts(FunctionCallInfo fcinfo, bool confirm, bool binary)
{
	Name		name;
	XLogRecPtr	upto_lsn;
	int32		upto_nchanges;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	XLogRecPtr	end_of_wal;
	LogicalDecodingContext *ctx;
	ResourceOwner old_resowner = CurrentResourceOwner;
	ArrayType  *arr;
	Size		ndim;
	List	   *options = NIL;
	DecodingOutputState *p;

	check_permissions();

	CheckLogicalDecodingRequirements();

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("slot name must not be null")));
	name = PG_GETARG_NAME(0);

	if (PG_ARGISNULL(1))
		upto_lsn = InvalidXLogRecPtr;
	else
		upto_lsn = PG_GETARG_LSN(1);

	if (PG_ARGISNULL(2))
		upto_nchanges = InvalidXLogRecPtr;
	else
		upto_nchanges = PG_GETARG_INT32(2);

	if (PG_ARGISNULL(3))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("options array must not be null")));
	arr = PG_GETARG_ARRAYTYPE_P(3);

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	/* state to write output to */
	p = palloc0(sizeof(DecodingOutputState));

	p->binary_output = binary;

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &p->tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Deconstruct options array */
	ndim = ARR_NDIM(arr);
	if (ndim > 1)
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("array must be one-dimensional")));
	}
	else if (array_contains_nulls(arr))
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("array must not contain nulls")));
	}
	else if (ndim == 1)
	{
		int			nelems;
		Datum	   *datum_opts;
		int			i;

		Assert(ARR_ELEMTYPE(arr) == TEXTOID);

		deconstruct_array(arr, TEXTOID, -1, false, TYPALIGN_INT,
						  &datum_opts, NULL, &nelems);

		if (nelems % 2 != 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("array must have even number of elements")));

		for (i = 0; i < nelems; i += 2)
		{
			char	   *name = TextDatumGetCString(datum_opts[i]);
			char	   *opt = TextDatumGetCString(datum_opts[i + 1]);

			options = lappend(options, makeDefElem(name, (Node *) makeString(opt), -1));
		}
	}

	p->tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = p->tupstore;
	rsinfo->setDesc = p->tupdesc;

	/*
	 * Compute the current end-of-wal and maintain ThisTimeLineID.
	 * RecoveryInProgress() will update ThisTimeLineID on promotion.
	 */
	if (!RecoveryInProgress())
		end_of_wal = GetFlushRecPtr();
	else
		end_of_wal = GetXLogReplayRecPtr(&ThisTimeLineID);

	(void) ReplicationSlotAcquire(NameStr(*name), SAB_Error);

	PG_TRY();
	{
		/* restart at slot's confirmed_flush */
		ctx = CreateDecodingContext(InvalidXLogRecPtr,
									options,
									false,
									XL_ROUTINE(.page_read = read_local_xlog_page,
											   .segment_open = wal_segment_open,
											   .segment_close = wal_segment_close),
									LogicalOutputPrepareWrite,
									LogicalOutputWrite, NULL);

		/*
		 * After the sanity checks in CreateDecodingContext, make sure the
		 * restart_lsn is valid.  Avoid "cannot get changes" wording in this
		 * errmsg because that'd be confusingly ambiguous about no changes
		 * being available.
		 */
		if (XLogRecPtrIsInvalid(MyReplicationSlot->data.restart_lsn))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("can no longer get changes from replication slot \"%s\"",
							NameStr(*name)),
					 errdetail("This slot has never previously reserved WAL, or has been invalidated.")));

		MemoryContextSwitchTo(oldcontext);

		/*
		 * Check whether the output plugin writes textual output if that's
		 * what we need.
		 */
		if (!binary &&
			ctx->options.output_type !=OUTPUT_PLUGIN_TEXTUAL_OUTPUT)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("logical decoding output plugin \"%s\" produces binary output, but function \"%s\" expects textual data",
							NameStr(MyReplicationSlot->data.plugin),
							format_procedure(fcinfo->flinfo->fn_oid))));

		ctx->output_writer_private = p;

		/*
		 * Decoding of WAL must start at restart_lsn so that the entirety of
		 * xacts that committed after the slot's confirmed_flush can be
		 * accumulated into reorder buffers.
		 */
		XLogBeginRead(ctx->reader, MyReplicationSlot->data.restart_lsn);

		/* invalidate non-timetravel entries */
		InvalidateSystemCaches();

		/* Decode until we run out of records */
		while (ctx->reader->EndRecPtr < end_of_wal)
		{
			XLogRecord *record;
			char	   *errm = NULL;

			record = XLogReadRecord(ctx->reader, &errm);
			if (errm)
				elog(ERROR, "%s", errm);

			/*
			 * The {begin_txn,change,commit_txn}_wrapper callbacks above will
			 * store the description into our tuplestore.
			 */
			if (record != NULL)
				LogicalDecodingProcessRecord(ctx, ctx->reader);

			/* check limits */
			if (upto_lsn != InvalidXLogRecPtr &&
				upto_lsn <= ctx->reader->EndRecPtr)
				break;
			if (upto_nchanges != 0 &&
				upto_nchanges <= p->returned_rows)
				break;
			CHECK_FOR_INTERRUPTS();
		}

		tuplestore_donestoring(tupstore);

		/*
		 * Logical decoding could have clobbered CurrentResourceOwner during
		 * transaction management, so restore the executor's value.  (This is
		 * a kluge, but it's not worth cleaning up right now.)
		 */
		CurrentResourceOwner = old_resowner;

		/*
		 * Next time, start where we left off. (Hunting things, the family
		 * business..)
		 */
		if (ctx->reader->EndRecPtr != InvalidXLogRecPtr && confirm)
		{
			LogicalConfirmReceivedLocation(ctx->reader->EndRecPtr);

			/*
			 * If only the confirmed_flush_lsn has changed the slot won't get
			 * marked as dirty by the above. Callers on the walsender
			 * interface are expected to keep track of their own progress and
			 * don't need it written out. But SQL-interface users cannot
			 * specify their own start positions and it's harder for them to
			 * keep track of their progress, so we should make more of an
			 * effort to save it for them.
			 *
			 * Dirty the slot so it's written out at the next checkpoint.
			 * We'll still lose its position on crash, as documented, but it's
			 * better than always losing the position even on clean restart.
			 */
			ReplicationSlotMarkDirty();
		}

		/* free context, call shutdown callback */
		FreeDecodingContext(ctx);

		ReplicationSlotRelease();
		InvalidateSystemCaches();
	}
	PG_CATCH();
	{
		/* clear all timetravel entries */
		InvalidateSystemCaches();

		PG_RE_THROW();
	}
	PG_END_TRY();

	return (Datum) 0;
}

/*
 * SQL function returning the changestream as text, consuming the data.
 */
Datum
pg_logical_slot_get_changes(PG_FUNCTION_ARGS)
{
	return pg_logical_slot_get_changes_guts(fcinfo, true, false);
}

/*
 * SQL function returning the changestream as text, only peeking ahead.
 */
Datum
pg_logical_slot_peek_changes(PG_FUNCTION_ARGS)
{
	return pg_logical_slot_get_changes_guts(fcinfo, false, false);
}

/*
 * SQL function returning the changestream in binary, consuming the data.
 */
Datum
pg_logical_slot_get_binary_changes(PG_FUNCTION_ARGS)
{
	return pg_logical_slot_get_changes_guts(fcinfo, true, true);
}

/*
 * SQL function returning the changestream in binary, only peeking ahead.
 */
Datum
pg_logical_slot_peek_binary_changes(PG_FUNCTION_ARGS)
{
	return pg_logical_slot_get_changes_guts(fcinfo, false, true);
}


/*
 * SQL function for writing logical decoding message into WAL.
 */
Datum
pg_logical_emit_message_bytea(PG_FUNCTION_ARGS)
{
	bool		transactional = PG_GETARG_BOOL(0);
	char	   *prefix = text_to_cstring(PG_GETARG_TEXT_PP(1));
	bytea	   *data = PG_GETARG_BYTEA_PP(2);
	XLogRecPtr	lsn;

	lsn = LogLogicalMessage(prefix, VARDATA_ANY(data), VARSIZE_ANY_EXHDR(data),
							transactional);
	PG_RETURN_LSN(lsn);
}

Datum
pg_logical_emit_message_text(PG_FUNCTION_ARGS)
{
	/* bytea and text are compatible */
	return pg_logical_emit_message_bytea(fcinfo);
}
