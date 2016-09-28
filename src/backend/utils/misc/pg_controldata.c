/*-------------------------------------------------------------------------
 *
 * pg_controldata.c
 *
 * Routines to expose the contents of the control data file via
 * a set of SQL functions.
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/misc/pg_controldata.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "funcapi.h"
#include "miscadmin.h"
#include "access/htup_details.h"
#include "access/xlog_internal.h"
#include "catalog/pg_control.h"
#include "catalog/pg_type.h"
#include "common/controldata_utils.h"
#include "utils/builtins.h"
#include "utils/pg_lsn.h"
#include "utils/timestamp.h"

Datum
pg_control_system(PG_FUNCTION_ARGS)
{
	Datum		values[4];
	bool		nulls[4];
	TupleDesc	tupdesc;
	HeapTuple	htup;
	ControlFileData *ControlFile;
	bool		crc_ok;

	/*
	 * Construct a tuple descriptor for the result row.  This must match this
	 * function's pg_proc entry!
	 */
	tupdesc = CreateTemplateTupleDesc(4, false);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "pg_control_version",
					   INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "catalog_version_no",
					   INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "system_identifier",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4, "pg_control_last_modified",
					   TIMESTAMPTZOID, -1, 0);
	tupdesc = BlessTupleDesc(tupdesc);

	/* read the control file */
	ControlFile = get_controlfile(DataDir, NULL, &crc_ok);
	if (!crc_ok)
		ereport(ERROR,
				(errmsg("calculated CRC checksum does not match value stored in file")));

	values[0] = Int32GetDatum(ControlFile->pg_control_version);
	nulls[0] = false;

	values[1] = Int32GetDatum(ControlFile->catalog_version_no);
	nulls[1] = false;

	values[2] = Int64GetDatum(ControlFile->system_identifier);
	nulls[2] = false;

	values[3] = TimestampTzGetDatum(time_t_to_timestamptz(ControlFile->time));
	nulls[3] = false;

	htup = heap_form_tuple(tupdesc, values, nulls);

	PG_RETURN_DATUM(HeapTupleGetDatum(htup));
}

Datum
pg_control_checkpoint(PG_FUNCTION_ARGS)
{
	Datum		values[19];
	bool		nulls[19];
	TupleDesc	tupdesc;
	HeapTuple	htup;
	ControlFileData *ControlFile;
	XLogSegNo	segno;
	char		xlogfilename[MAXFNAMELEN];
	bool		crc_ok;

	/*
	 * Construct a tuple descriptor for the result row.  This must match this
	 * function's pg_proc entry!
	 */
	tupdesc = CreateTemplateTupleDesc(19, false);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "checkpoint_location",
					   LSNOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "prior_location",
					   LSNOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "redo_location",
					   LSNOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4, "redo_wal_file",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 5, "timeline_id",
					   INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 6, "prev_timeline_id",
					   INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 7, "full_page_writes",
					   BOOLOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 8, "next_xid",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 9, "next_oid",
					   OIDOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 10, "next_multixact_id",
					   XIDOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 11, "next_multi_offset",
					   XIDOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 12, "oldest_xid",
					   XIDOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 13, "oldest_xid_dbid",
					   OIDOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 14, "oldest_active_xid",
					   XIDOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 15, "oldest_multi_xid",
					   XIDOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 16, "oldest_multi_dbid",
					   OIDOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 17, "oldest_commit_ts_xid",
					   XIDOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 18, "newest_commit_ts_xid",
					   XIDOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 19, "checkpoint_time",
					   TIMESTAMPTZOID, -1, 0);
	tupdesc = BlessTupleDesc(tupdesc);

	/* Read the control file. */
	ControlFile = get_controlfile(DataDir, NULL, &crc_ok);
	if (!crc_ok)
		ereport(ERROR,
				(errmsg("calculated CRC checksum does not match value stored in file")));

	/*
	 * Calculate name of the WAL file containing the latest checkpoint's REDO
	 * start point.
	 */
	XLByteToSeg(ControlFile->checkPointCopy.redo, segno);
	XLogFileName(xlogfilename, ControlFile->checkPointCopy.ThisTimeLineID, segno);

	/* Populate the values and null arrays */
	values[0] = LSNGetDatum(ControlFile->checkPoint);
	nulls[0] = false;

	values[1] = LSNGetDatum(ControlFile->prevCheckPoint);
	nulls[1] = false;

	values[2] = LSNGetDatum(ControlFile->checkPointCopy.redo);
	nulls[2] = false;

	values[3] = CStringGetTextDatum(xlogfilename);
	nulls[3] = false;

	values[4] = Int32GetDatum(ControlFile->checkPointCopy.ThisTimeLineID);
	nulls[4] = false;

	values[5] = Int32GetDatum(ControlFile->checkPointCopy.PrevTimeLineID);
	nulls[5] = false;

	values[6] = BoolGetDatum(ControlFile->checkPointCopy.fullPageWrites);
	nulls[6] = false;

	values[7] = CStringGetTextDatum(psprintf("%u:%u",
									ControlFile->checkPointCopy.nextXidEpoch,
									   ControlFile->checkPointCopy.nextXid));
	nulls[7] = false;

	values[8] = ObjectIdGetDatum(ControlFile->checkPointCopy.nextOid);
	nulls[8] = false;

	values[9] = TransactionIdGetDatum(ControlFile->checkPointCopy.nextMulti);
	nulls[9] = false;

	values[10] = TransactionIdGetDatum(ControlFile->checkPointCopy.nextMultiOffset);
	nulls[10] = false;

	values[11] = TransactionIdGetDatum(ControlFile->checkPointCopy.oldestXid);
	nulls[11] = false;

	values[12] = ObjectIdGetDatum(ControlFile->checkPointCopy.oldestXidDB);
	nulls[12] = false;

	values[13] = TransactionIdGetDatum(ControlFile->checkPointCopy.oldestActiveXid);
	nulls[13] = false;

	values[14] = TransactionIdGetDatum(ControlFile->checkPointCopy.oldestMulti);
	nulls[14] = false;

	values[15] = ObjectIdGetDatum(ControlFile->checkPointCopy.oldestMultiDB);
	nulls[15] = false;

	values[16] = TransactionIdGetDatum(ControlFile->checkPointCopy.oldestCommitTsXid);
	nulls[16] = false;

	values[17] = TransactionIdGetDatum(ControlFile->checkPointCopy.newestCommitTsXid);
	nulls[17] = false;

	values[18] = TimestampTzGetDatum(
					time_t_to_timestamptz(ControlFile->checkPointCopy.time));
	nulls[18] = false;

	htup = heap_form_tuple(tupdesc, values, nulls);

	PG_RETURN_DATUM(HeapTupleGetDatum(htup));
}

Datum
pg_control_recovery(PG_FUNCTION_ARGS)
{
	Datum		values[5];
	bool		nulls[5];
	TupleDesc	tupdesc;
	HeapTuple	htup;
	ControlFileData *ControlFile;
	bool		crc_ok;

	/*
	 * Construct a tuple descriptor for the result row.  This must match this
	 * function's pg_proc entry!
	 */
	tupdesc = CreateTemplateTupleDesc(5, false);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "min_recovery_end_location",
					   LSNOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "min_recovery_end_timeline",
					   INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "backup_start_location",
					   LSNOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4, "backup_end_location",
					   LSNOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 5, "end_of_backup_record_required",
					   BOOLOID, -1, 0);
	tupdesc = BlessTupleDesc(tupdesc);

	/* read the control file */
	ControlFile = get_controlfile(DataDir, NULL, &crc_ok);
	if (!crc_ok)
		ereport(ERROR,
				(errmsg("calculated CRC checksum does not match value stored in file")));

	values[0] = LSNGetDatum(ControlFile->minRecoveryPoint);
	nulls[0] = false;

	values[1] = Int32GetDatum(ControlFile->minRecoveryPointTLI);
	nulls[1] = false;

	values[2] = LSNGetDatum(ControlFile->backupStartPoint);
	nulls[2] = false;

	values[3] = LSNGetDatum(ControlFile->backupEndPoint);
	nulls[3] = false;

	values[4] = BoolGetDatum(ControlFile->backupEndRequired);
	nulls[4] = false;

	htup = heap_form_tuple(tupdesc, values, nulls);

	PG_RETURN_DATUM(HeapTupleGetDatum(htup));
}

Datum
pg_control_init(PG_FUNCTION_ARGS)
{
	Datum		values[13];
	bool		nulls[13];
	TupleDesc	tupdesc;
	HeapTuple	htup;
	ControlFileData *ControlFile;
	bool		crc_ok;

	/*
	 * Construct a tuple descriptor for the result row.  This must match this
	 * function's pg_proc entry!
	 */
	tupdesc = CreateTemplateTupleDesc(13, false);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "max_data_alignment",
					   INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "database_block_size",
					   INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "blocks_per_segment",
					   INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4, "wal_block_size",
					   INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 5, "bytes_per_wal_segment",
					   INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 6, "max_identifier_length",
					   INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 7, "max_index_columns",
					   INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 8, "max_toast_chunk_size",
					   INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 9, "large_object_chunk_size",
					   INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 10, "bigint_timestamps",
					   BOOLOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 11, "float4_pass_by_value",
					   BOOLOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 12, "float8_pass_by_value",
					   BOOLOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 13, "data_page_checksum_version",
					   INT4OID, -1, 0);
	tupdesc = BlessTupleDesc(tupdesc);

	/* read the control file */
	ControlFile = get_controlfile(DataDir, NULL, &crc_ok);
	if (!crc_ok)
		ereport(ERROR,
				(errmsg("calculated CRC checksum does not match value stored in file")));

	values[0] = Int32GetDatum(ControlFile->maxAlign);
	nulls[0] = false;

	values[1] = Int32GetDatum(ControlFile->blcksz);
	nulls[1] = false;

	values[2] = Int32GetDatum(ControlFile->relseg_size);
	nulls[2] = false;

	values[3] = Int32GetDatum(ControlFile->xlog_blcksz);
	nulls[3] = false;

	values[4] = Int32GetDatum(ControlFile->xlog_seg_size);
	nulls[4] = false;

	values[5] = Int32GetDatum(ControlFile->nameDataLen);
	nulls[5] = false;

	values[6] = Int32GetDatum(ControlFile->indexMaxKeys);
	nulls[6] = false;

	values[7] = Int32GetDatum(ControlFile->toast_max_chunk_size);
	nulls[7] = false;

	values[8] = Int32GetDatum(ControlFile->loblksize);
	nulls[8] = false;

	values[9] = BoolGetDatum(ControlFile->enableIntTimes);
	nulls[9] = false;

	values[10] = BoolGetDatum(ControlFile->float4ByVal);
	nulls[10] = false;

	values[11] = BoolGetDatum(ControlFile->float8ByVal);
	nulls[11] = false;

	values[12] = Int32GetDatum(ControlFile->data_checksum_version);
	nulls[12] = false;

	htup = heap_form_tuple(tupdesc, values, nulls);

	PG_RETURN_DATUM(HeapTupleGetDatum(htup));
}
