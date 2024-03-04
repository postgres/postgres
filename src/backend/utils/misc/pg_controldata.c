/*-------------------------------------------------------------------------
 *
 * pg_controldata.c
 *
 * Routines to expose the contents of the control data file via
 * a set of SQL functions.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/misc/pg_controldata.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "catalog/pg_control.h"
#include "common/controldata_utils.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/lwlock.h"
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

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	/* read the control file */
	LWLockAcquire(ControlFileLock, LW_SHARED);
	ControlFile = get_controlfile(DataDir, &crc_ok);
	LWLockRelease(ControlFileLock);
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
	Datum		values[18];
	bool		nulls[18];
	TupleDesc	tupdesc;
	HeapTuple	htup;
	ControlFileData *ControlFile;
	XLogSegNo	segno;
	char		xlogfilename[MAXFNAMELEN];
	bool		crc_ok;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	/* Read the control file. */
	LWLockAcquire(ControlFileLock, LW_SHARED);
	ControlFile = get_controlfile(DataDir, &crc_ok);
	LWLockRelease(ControlFileLock);
	if (!crc_ok)
		ereport(ERROR,
				(errmsg("calculated CRC checksum does not match value stored in file")));

	/*
	 * Calculate name of the WAL file containing the latest checkpoint's REDO
	 * start point.
	 */
	XLByteToSeg(ControlFile->checkPointCopy.redo, segno, wal_segment_size);
	XLogFileName(xlogfilename, ControlFile->checkPointCopy.ThisTimeLineID,
				 segno, wal_segment_size);

	/* Populate the values and null arrays */
	values[0] = LSNGetDatum(ControlFile->checkPoint);
	nulls[0] = false;

	values[1] = LSNGetDatum(ControlFile->checkPointCopy.redo);
	nulls[1] = false;

	values[2] = CStringGetTextDatum(xlogfilename);
	nulls[2] = false;

	values[3] = Int32GetDatum(ControlFile->checkPointCopy.ThisTimeLineID);
	nulls[3] = false;

	values[4] = Int32GetDatum(ControlFile->checkPointCopy.PrevTimeLineID);
	nulls[4] = false;

	values[5] = BoolGetDatum(ControlFile->checkPointCopy.fullPageWrites);
	nulls[5] = false;

	values[6] = CStringGetTextDatum(psprintf("%u:%u",
											 EpochFromFullTransactionId(ControlFile->checkPointCopy.nextXid),
											 XidFromFullTransactionId(ControlFile->checkPointCopy.nextXid)));
	nulls[6] = false;

	values[7] = ObjectIdGetDatum(ControlFile->checkPointCopy.nextOid);
	nulls[7] = false;

	values[8] = TransactionIdGetDatum(ControlFile->checkPointCopy.nextMulti);
	nulls[8] = false;

	values[9] = TransactionIdGetDatum(ControlFile->checkPointCopy.nextMultiOffset);
	nulls[9] = false;

	values[10] = TransactionIdGetDatum(ControlFile->checkPointCopy.oldestXid);
	nulls[10] = false;

	values[11] = ObjectIdGetDatum(ControlFile->checkPointCopy.oldestXidDB);
	nulls[11] = false;

	values[12] = TransactionIdGetDatum(ControlFile->checkPointCopy.oldestActiveXid);
	nulls[12] = false;

	values[13] = TransactionIdGetDatum(ControlFile->checkPointCopy.oldestMulti);
	nulls[13] = false;

	values[14] = ObjectIdGetDatum(ControlFile->checkPointCopy.oldestMultiDB);
	nulls[14] = false;

	values[15] = TransactionIdGetDatum(ControlFile->checkPointCopy.oldestCommitTsXid);
	nulls[15] = false;

	values[16] = TransactionIdGetDatum(ControlFile->checkPointCopy.newestCommitTsXid);
	nulls[16] = false;

	values[17] = TimestampTzGetDatum(time_t_to_timestamptz(ControlFile->checkPointCopy.time));
	nulls[17] = false;

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

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	/* read the control file */
	LWLockAcquire(ControlFileLock, LW_SHARED);
	ControlFile = get_controlfile(DataDir, &crc_ok);
	LWLockRelease(ControlFileLock);
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
	Datum		values[11];
	bool		nulls[11];
	TupleDesc	tupdesc;
	HeapTuple	htup;
	ControlFileData *ControlFile;
	bool		crc_ok;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	/* read the control file */
	LWLockAcquire(ControlFileLock, LW_SHARED);
	ControlFile = get_controlfile(DataDir, &crc_ok);
	LWLockRelease(ControlFileLock);
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

	values[9] = BoolGetDatum(ControlFile->float8ByVal);
	nulls[9] = false;

	values[10] = Int32GetDatum(ControlFile->data_checksum_version);
	nulls[10] = false;

	htup = heap_form_tuple(tupdesc, values, nulls);

	PG_RETURN_DATUM(HeapTupleGetDatum(htup));
}
