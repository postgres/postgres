/*-------------------------------------------------------------------------
 *
 * logicalfuncs.c
 *
 *	   Support functions for using logical decoding and management of
 *	   logical replication slots via SQL.
 *
 *
 * Copyright (c) 2012-2015, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logicalfuncs.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>

#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"

#include "access/xlog_internal.h"

#include "catalog/pg_type.h"

#include "nodes/makefuncs.h"

#include "mb/pg_wchar.h"

#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/pg_lsn.h"
#include "utils/resowner.h"
#include "utils/lsyscache.h"

#include "replication/decode.h"
#include "replication/logical.h"
#include "replication/logicalfuncs.h"

#include "storage/fd.h"

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
	values[2] = PointerGetDatum(
					cstring_to_text_with_len(ctx->out->data, ctx->out->len));

	tuplestore_putvalues(p->tupstore, p->tupdesc, values, nulls);
	p->returned_rows++;
}

/*
 * TODO: This is duplicate code with pg_xlogdump, similar to walsender.c, but
 * we currently don't have the infrastructure (elog!) to share it.
 */
static void
XLogRead(char *buf, TimeLineID tli, XLogRecPtr startptr, Size count)
{
	char	   *p;
	XLogRecPtr	recptr;
	Size		nbytes;

	static int	sendFile = -1;
	static XLogSegNo sendSegNo = 0;
	static uint32 sendOff = 0;

	p = buf;
	recptr = startptr;
	nbytes = count;

	while (nbytes > 0)
	{
		uint32		startoff;
		int			segbytes;
		int			readbytes;

		startoff = recptr % XLogSegSize;

		if (sendFile < 0 || !XLByteInSeg(recptr, sendSegNo))
		{
			char		path[MAXPGPATH];

			/* Switch to another logfile segment */
			if (sendFile >= 0)
				close(sendFile);

			XLByteToSeg(recptr, sendSegNo);

			XLogFilePath(path, tli, sendSegNo);

			sendFile = BasicOpenFile(path, O_RDONLY | PG_BINARY, 0);

			if (sendFile < 0)
			{
				if (errno == ENOENT)
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("requested WAL segment %s has already been removed",
									path)));
				else
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not open file \"%s\": %m",
									path)));
			}
			sendOff = 0;
		}

		/* Need to seek in the file? */
		if (sendOff != startoff)
		{
			if (lseek(sendFile, (off_t) startoff, SEEK_SET) < 0)
			{
				char		path[MAXPGPATH];

				XLogFilePath(path, tli, sendSegNo);

				ereport(ERROR,
						(errcode_for_file_access(),
				  errmsg("could not seek in log segment %s to offset %u: %m",
						 path, startoff)));
			}
			sendOff = startoff;
		}

		/* How many bytes are within this segment? */
		if (nbytes > (XLogSegSize - startoff))
			segbytes = XLogSegSize - startoff;
		else
			segbytes = nbytes;

		readbytes = read(sendFile, p, segbytes);
		if (readbytes <= 0)
		{
			char		path[MAXPGPATH];

			XLogFilePath(path, tli, sendSegNo);

			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read from log segment %s, offset %u, length %lu: %m",
							path, sendOff, (unsigned long) segbytes)));
		}

		/* Update state for read */
		recptr += readbytes;

		sendOff += readbytes;
		nbytes -= readbytes;
		p += readbytes;
	}
}

static void
check_permissions(void)
{
	if (!superuser() && !has_rolreplication(GetUserId()))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser or replication role to use replication slots"))));
}

/*
 * read_page callback for logical decoding contexts.
 *
 * Public because it would likely be very helpful for someone writing another
 * output method outside walsender, e.g. in a bgworker.
 *
 * TODO: The walsender has it's own version of this, but it relies on the
 * walsender's latch being set whenever WAL is flushed. No such infrastructure
 * exists for normal backends, so we have to do a check/sleep/repeat style of
 * loop for now.
 */
int
logical_read_local_xlog_page(XLogReaderState *state, XLogRecPtr targetPagePtr,
	int reqLen, XLogRecPtr targetRecPtr, char *cur_page, TimeLineID *pageTLI)
{
	XLogRecPtr	flushptr,
				loc;
	int			count;

	loc = targetPagePtr + reqLen;
	while (1)
	{
		/*
		 * TODO: we're going to have to do something more intelligent about
		 * timelines on standbys. Use readTimeLineHistory() and
		 * tliOfPointInHistory() to get the proper LSN? For now we'll catch
		 * that case earlier, but the code and TODO is left in here for when
		 * that changes.
		 */
		if (!RecoveryInProgress())
		{
			*pageTLI = ThisTimeLineID;
			flushptr = GetFlushRecPtr();
		}
		else
			flushptr = GetXLogReplayRecPtr(pageTLI);

		if (loc <= flushptr)
			break;

		CHECK_FOR_INTERRUPTS();
		pg_usleep(1000L);
	}

	/* more than one block available */
	if (targetPagePtr + XLOG_BLCKSZ <= flushptr)
		count = XLOG_BLCKSZ;
	/* not enough data there */
	else if (targetPagePtr + reqLen > flushptr)
		return -1;
	/* part of the page available */
	else
		count = flushptr - targetPagePtr;

	XLogRead(cur_page, *pageTLI, targetPagePtr, XLOG_BLCKSZ);

	return count;
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
	XLogRecPtr	startptr;
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

		deconstruct_array(arr, TEXTOID, -1, false, 'i',
						  &datum_opts, NULL, &nelems);

		if (nelems % 2 != 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("array must have even number of elements")));

		for (i = 0; i < nelems; i += 2)
		{
			char	   *name = TextDatumGetCString(datum_opts[i]);
			char	   *opt = TextDatumGetCString(datum_opts[i + 1]);

			options = lappend(options, makeDefElem(name, (Node *) makeString(opt)));
		}
	}

	p->tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = p->tupstore;
	rsinfo->setDesc = p->tupdesc;

	/* compute the current end-of-wal */
	if (!RecoveryInProgress())
		end_of_wal = GetFlushRecPtr();
	else
		end_of_wal = GetXLogReplayRecPtr(NULL);

	ReplicationSlotAcquire(NameStr(*name));

	PG_TRY();
	{
		ctx = CreateDecodingContext(InvalidXLogRecPtr,
									options,
									logical_read_local_xlog_page,
									LogicalOutputPrepareWrite,
									LogicalOutputWrite);

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

		startptr = MyReplicationSlot->data.restart_lsn;

		CurrentResourceOwner = ResourceOwnerCreate(CurrentResourceOwner, "logical decoding");

		/* invalidate non-timetravel entries */
		InvalidateSystemCaches();

		while ((startptr != InvalidXLogRecPtr && startptr < end_of_wal) ||
			 (ctx->reader->EndRecPtr && ctx->reader->EndRecPtr < end_of_wal))
		{
			XLogRecord *record;
			char	   *errm = NULL;

			record = XLogReadRecord(ctx->reader, startptr, &errm);
			if (errm)
				elog(ERROR, "%s", errm);

			startptr = InvalidXLogRecPtr;

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

		CurrentResourceOwner = old_resowner;

		/*
		 * Next time, start where we left off. (Hunting things, the family
		 * business..)
		 */
		if (ctx->reader->EndRecPtr != InvalidXLogRecPtr && confirm)
			LogicalConfirmReceivedLocation(ctx->reader->EndRecPtr);

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
