/*-------------------------------------------------------------------------
 *
 * walsummaryfuncs.c
 *	  SQL-callable functions for accessing WAL summary data.
 *
 * Portions Copyright (c) 2010-2025, PostgreSQL Global Development Group
 *
 * src/backend/backup/walsummaryfuncs.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "backup/walsummary.h"
#include "common/blkreftable.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "postmaster/walsummarizer.h"
#include "utils/fmgrprotos.h"
#include "utils/pg_lsn.h"

#define NUM_WS_ATTS			3
#define NUM_SUMMARY_ATTS	6
#define NUM_STATE_ATTS		4
#define MAX_BLOCKS_PER_CALL	256

/*
 * List the WAL summary files available in pg_wal/summaries.
 */
Datum
pg_available_wal_summaries(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi;
	List	   *wslist;
	ListCell   *lc;
	Datum		values[NUM_WS_ATTS];
	bool		nulls[NUM_WS_ATTS];

	InitMaterializedSRF(fcinfo, 0);
	rsi = (ReturnSetInfo *) fcinfo->resultinfo;

	memset(nulls, 0, sizeof(nulls));

	wslist = GetWalSummaries(0, InvalidXLogRecPtr, InvalidXLogRecPtr);
	foreach(lc, wslist)
	{
		WalSummaryFile *ws = (WalSummaryFile *) lfirst(lc);
		HeapTuple	tuple;

		CHECK_FOR_INTERRUPTS();

		values[0] = Int64GetDatum((int64) ws->tli);
		values[1] = LSNGetDatum(ws->start_lsn);
		values[2] = LSNGetDatum(ws->end_lsn);

		tuple = heap_form_tuple(rsi->setDesc, values, nulls);
		tuplestore_puttuple(rsi->setResult, tuple);
	}

	return (Datum) 0;
}

/*
 * List the contents of a WAL summary file identified by TLI, start LSN,
 * and end LSN.
 */
Datum
pg_wal_summary_contents(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi;
	Datum		values[NUM_SUMMARY_ATTS];
	bool		nulls[NUM_SUMMARY_ATTS];
	WalSummaryFile ws;
	WalSummaryIO io;
	BlockRefTableReader *reader;
	int64		raw_tli;
	RelFileLocator rlocator;
	ForkNumber	forknum;
	BlockNumber limit_block;

	InitMaterializedSRF(fcinfo, 0);
	rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	memset(nulls, 0, sizeof(nulls));

	/*
	 * Since the timeline could at least in theory be more than 2^31, and
	 * since we don't have unsigned types at the SQL level, it is passed as a
	 * 64-bit integer. Test whether it's out of range.
	 */
	raw_tli = PG_GETARG_INT64(0);
	if (raw_tli < 1 || raw_tli > PG_INT32_MAX)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("invalid timeline %" PRId64, raw_tli));

	/* Prepare to read the specified WAL summary file. */
	ws.tli = (TimeLineID) raw_tli;
	ws.start_lsn = PG_GETARG_LSN(1);
	ws.end_lsn = PG_GETARG_LSN(2);
	io.filepos = 0;
	io.file = OpenWalSummaryFile(&ws, false);
	reader = CreateBlockRefTableReader(ReadWalSummary, &io,
									   FilePathName(io.file),
									   ReportWalSummaryError, NULL);

	/* Loop over relation forks. */
	while (BlockRefTableReaderNextRelation(reader, &rlocator, &forknum,
										   &limit_block))
	{
		BlockNumber blocks[MAX_BLOCKS_PER_CALL];
		HeapTuple	tuple;

		CHECK_FOR_INTERRUPTS();

		values[0] = ObjectIdGetDatum(rlocator.relNumber);
		values[1] = ObjectIdGetDatum(rlocator.spcOid);
		values[2] = ObjectIdGetDatum(rlocator.dbOid);
		values[3] = Int16GetDatum((int16) forknum);

		/*
		 * If the limit block is not InvalidBlockNumber, emit an extra row
		 * with that block number and limit_block = true.
		 *
		 * There is no point in doing this when the limit_block is
		 * InvalidBlockNumber, because no block with that number or any higher
		 * number can ever exist.
		 */
		if (BlockNumberIsValid(limit_block))
		{
			values[4] = Int64GetDatum((int64) limit_block);
			values[5] = BoolGetDatum(true);

			tuple = heap_form_tuple(rsi->setDesc, values, nulls);
			tuplestore_puttuple(rsi->setResult, tuple);
		}

		/* Loop over blocks within the current relation fork. */
		while (1)
		{
			unsigned	nblocks;
			unsigned	i;

			CHECK_FOR_INTERRUPTS();

			nblocks = BlockRefTableReaderGetBlocks(reader, blocks,
												   MAX_BLOCKS_PER_CALL);
			if (nblocks == 0)
				break;

			/*
			 * For each block that we specifically know to have been modified,
			 * emit a row with that block number and limit_block = false.
			 */
			values[5] = BoolGetDatum(false);
			for (i = 0; i < nblocks; ++i)
			{
				values[4] = Int64GetDatum((int64) blocks[i]);

				tuple = heap_form_tuple(rsi->setDesc, values, nulls);
				tuplestore_puttuple(rsi->setResult, tuple);
			}
		}
	}

	/* Cleanup */
	DestroyBlockRefTableReader(reader);
	FileClose(io.file);

	return (Datum) 0;
}

/*
 * Returns information about the state of the WAL summarizer process.
 */
Datum
pg_get_wal_summarizer_state(PG_FUNCTION_ARGS)
{
	Datum		values[NUM_STATE_ATTS];
	bool		nulls[NUM_STATE_ATTS];
	TimeLineID	summarized_tli;
	XLogRecPtr	summarized_lsn;
	XLogRecPtr	pending_lsn;
	int			summarizer_pid;
	TupleDesc	tupdesc;
	HeapTuple	htup;

	GetWalSummarizerState(&summarized_tli, &summarized_lsn, &pending_lsn,
						  &summarizer_pid);

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	memset(nulls, 0, sizeof(nulls));

	values[0] = Int64GetDatum((int64) summarized_tli);
	values[1] = LSNGetDatum(summarized_lsn);
	values[2] = LSNGetDatum(pending_lsn);

	if (summarizer_pid < 0)
		nulls[3] = true;
	else
		values[3] = Int32GetDatum(summarizer_pid);

	htup = heap_form_tuple(tupdesc, values, nulls);

	PG_RETURN_DATUM(HeapTupleGetDatum(htup));
}
