/*-------------------------------------------------------------------------
 *
 * xlogfuncs.c
 *
 * PostgreSQL transaction log manager user interface functions
 *
 * This file contains WAL control and information functions.
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/xlogfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/xlog.h"
#include "access/xlog_fn.h"
#include "access/xlog_internal.h"
#include "access/xlogutils.h"
#include "catalog/catalog.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "replication/walreceiver.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/numeric.h"
#include "utils/guc.h"
#include "utils/timestamp.h"
#include "storage/fd.h"

static void validate_xlog_location(char *str);


/*
 * pg_start_backup: set up for taking an on-line backup dump
 *
 * Essentially what this does is to create a backup label file in $PGDATA,
 * where it will be archived as part of the backup dump.  The label file
 * contains the user-supplied label string (typically this would be used
 * to tell where the backup dump will be stored) and the starting time and
 * starting WAL location for the dump.
 */
Datum
pg_start_backup(PG_FUNCTION_ARGS)
{
	text	   *backupid = PG_GETARG_TEXT_P(0);
	bool		fast = PG_GETARG_BOOL(1);
	char	   *backupidstr;
	XLogRecPtr	startpoint;
	char		startxlogstr[MAXFNAMELEN];

	backupidstr = text_to_cstring(backupid);

	startpoint = do_pg_start_backup(backupidstr, fast, NULL, NULL);

	snprintf(startxlogstr, sizeof(startxlogstr), "%X/%X",
			 (uint32) (startpoint >> 32), (uint32) startpoint);
	PG_RETURN_TEXT_P(cstring_to_text(startxlogstr));
}

/*
 * pg_stop_backup: finish taking an on-line backup dump
 *
 * We write an end-of-backup WAL record, and remove the backup label file
 * created by pg_start_backup, creating a backup history file in pg_xlog
 * instead (whence it will immediately be archived). The backup history file
 * contains the same info found in the label file, plus the backup-end time
 * and WAL location. Before 9.0, the backup-end time was read from the backup
 * history file at the beginning of archive recovery, but we now use the WAL
 * record for that and the file is for informational and debug purposes only.
 *
 * Note: different from CancelBackup which just cancels online backup mode.
 */
Datum
pg_stop_backup(PG_FUNCTION_ARGS)
{
	XLogRecPtr	stoppoint;
	char		stopxlogstr[MAXFNAMELEN];

	stoppoint = do_pg_stop_backup(NULL, true, NULL);

	snprintf(stopxlogstr, sizeof(stopxlogstr), "%X/%X",
			 (uint32) (stoppoint >> 32), (uint32) stoppoint);
	PG_RETURN_TEXT_P(cstring_to_text(stopxlogstr));
}

/*
 * pg_switch_xlog: switch to next xlog file
 */
Datum
pg_switch_xlog(PG_FUNCTION_ARGS)
{
	XLogRecPtr	switchpoint;
	char		location[MAXFNAMELEN];

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
			 (errmsg("must be superuser to switch transaction log files"))));

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is in progress"),
				 errhint("WAL control functions cannot be executed during recovery.")));

	switchpoint = RequestXLogSwitch();

	/*
	 * As a convenience, return the WAL location of the switch record
	 */
	snprintf(location, sizeof(location), "%X/%X",
			 (uint32) (switchpoint >> 32), (uint32) switchpoint);
	PG_RETURN_TEXT_P(cstring_to_text(location));
}

/*
 * pg_create_restore_point: a named point for restore
 */
Datum
pg_create_restore_point(PG_FUNCTION_ARGS)
{
	text	   *restore_name = PG_GETARG_TEXT_P(0);
	char	   *restore_name_str;
	XLogRecPtr	restorepoint;
	char		location[MAXFNAMELEN];

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to create a restore point"))));

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 (errmsg("recovery is in progress"),
				  errhint("WAL control functions cannot be executed during recovery."))));

	if (!XLogIsNeeded())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			 errmsg("WAL level not sufficient for creating a restore point"),
				 errhint("wal_level must be set to \"archive\" or \"hot_standby\" at server start.")));

	restore_name_str = text_to_cstring(restore_name);

	if (strlen(restore_name_str) >= MAXFNAMELEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("value too long for restore point (maximum %d characters)", MAXFNAMELEN - 1)));

	restorepoint = XLogRestorePoint(restore_name_str);

	/*
	 * As a convenience, return the WAL location of the restore point record
	 */
	snprintf(location, sizeof(location), "%X/%X",
			 (uint32) (restorepoint >> 32), (uint32) restorepoint);
	PG_RETURN_TEXT_P(cstring_to_text(location));
}

/*
 * Report the current WAL write location (same format as pg_start_backup etc)
 *
 * This is useful for determining how much of WAL is visible to an external
 * archiving process.  Note that the data before this point is written out
 * to the kernel, but is not necessarily synced to disk.
 */
Datum
pg_current_xlog_location(PG_FUNCTION_ARGS)
{
	XLogRecPtr	current_recptr;
	char		location[MAXFNAMELEN];

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is in progress"),
				 errhint("WAL control functions cannot be executed during recovery.")));

	current_recptr = GetXLogWriteRecPtr();

	snprintf(location, sizeof(location), "%X/%X",
			 (uint32) (current_recptr >> 32), (uint32) current_recptr);
	PG_RETURN_TEXT_P(cstring_to_text(location));
}

/*
 * Report the current WAL insert location (same format as pg_start_backup etc)
 *
 * This function is mostly for debugging purposes.
 */
Datum
pg_current_xlog_insert_location(PG_FUNCTION_ARGS)
{
	XLogRecPtr	current_recptr;
	char		location[MAXFNAMELEN];

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is in progress"),
				 errhint("WAL control functions cannot be executed during recovery.")));

	current_recptr = GetXLogInsertRecPtr();

	snprintf(location, sizeof(location), "%X/%X",
			 (uint32) (current_recptr >> 32), (uint32) current_recptr);
	PG_RETURN_TEXT_P(cstring_to_text(location));
}

/*
 * Report the last WAL receive location (same format as pg_start_backup etc)
 *
 * This is useful for determining how much of WAL is guaranteed to be received
 * and synced to disk by walreceiver.
 */
Datum
pg_last_xlog_receive_location(PG_FUNCTION_ARGS)
{
	XLogRecPtr	recptr;
	char		location[MAXFNAMELEN];

	recptr = GetWalRcvWriteRecPtr(NULL, NULL);

	if (recptr == 0)
		PG_RETURN_NULL();

	snprintf(location, sizeof(location), "%X/%X",
			 (uint32) (recptr >> 32), (uint32) recptr);
	PG_RETURN_TEXT_P(cstring_to_text(location));
}

/*
 * Report the last WAL replay location (same format as pg_start_backup etc)
 *
 * This is useful for determining how much of WAL is visible to read-only
 * connections during recovery.
 */
Datum
pg_last_xlog_replay_location(PG_FUNCTION_ARGS)
{
	XLogRecPtr	recptr;
	char		location[MAXFNAMELEN];

	recptr = GetXLogReplayRecPtr(NULL);

	if (recptr == 0)
		PG_RETURN_NULL();

	snprintf(location, sizeof(location), "%X/%X",
			 (uint32) (recptr >> 32), (uint32) recptr);
	PG_RETURN_TEXT_P(cstring_to_text(location));
}

/*
 * Compute an xlog file name and decimal byte offset given a WAL location,
 * such as is returned by pg_stop_backup() or pg_xlog_switch().
 *
 * Note that a location exactly at a segment boundary is taken to be in
 * the previous segment.  This is usually the right thing, since the
 * expected usage is to determine which xlog file(s) are ready to archive.
 */
Datum
pg_xlogfile_name_offset(PG_FUNCTION_ARGS)
{
	text	   *location = PG_GETARG_TEXT_P(0);
	char	   *locationstr;
	uint32		hi,
				lo;
	XLogSegNo	xlogsegno;
	uint32		xrecoff;
	XLogRecPtr	locationpoint;
	char		xlogfilename[MAXFNAMELEN];
	Datum		values[2];
	bool		isnull[2];
	TupleDesc	resultTupleDesc;
	HeapTuple	resultHeapTuple;
	Datum		result;

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is in progress"),
				 errhint("pg_xlogfile_name_offset() cannot be executed during recovery.")));

	/*
	 * Read input and parse
	 */
	locationstr = text_to_cstring(location);

	validate_xlog_location(locationstr);

	if (sscanf(locationstr, "%X/%X", &hi, &lo) != 2)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("could not parse transaction log location \"%s\"",
						locationstr)));
	locationpoint = ((uint64) hi) << 32 | lo;

	/*
	 * Construct a tuple descriptor for the result row.  This must match this
	 * function's pg_proc entry!
	 */
	resultTupleDesc = CreateTemplateTupleDesc(2, false);
	TupleDescInitEntry(resultTupleDesc, (AttrNumber) 1, "file_name",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(resultTupleDesc, (AttrNumber) 2, "file_offset",
					   INT4OID, -1, 0);

	resultTupleDesc = BlessTupleDesc(resultTupleDesc);

	/*
	 * xlogfilename
	 */
	XLByteToPrevSeg(locationpoint, xlogsegno);
	XLogFileName(xlogfilename, ThisTimeLineID, xlogsegno);

	values[0] = CStringGetTextDatum(xlogfilename);
	isnull[0] = false;

	/*
	 * offset
	 */
	xrecoff = locationpoint % XLogSegSize;

	values[1] = UInt32GetDatum(xrecoff);
	isnull[1] = false;

	/*
	 * Tuple jam: Having first prepared your Datums, then squash together
	 */
	resultHeapTuple = heap_form_tuple(resultTupleDesc, values, isnull);

	result = HeapTupleGetDatum(resultHeapTuple);

	PG_RETURN_DATUM(result);
}

/*
 * Compute an xlog file name given a WAL location,
 * such as is returned by pg_stop_backup() or pg_xlog_switch().
 */
Datum
pg_xlogfile_name(PG_FUNCTION_ARGS)
{
	text	   *location = PG_GETARG_TEXT_P(0);
	char	   *locationstr;
	uint32		hi,
				lo;
	XLogSegNo	xlogsegno;
	XLogRecPtr	locationpoint;
	char		xlogfilename[MAXFNAMELEN];

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is in progress"),
		 errhint("pg_xlogfile_name() cannot be executed during recovery.")));

	locationstr = text_to_cstring(location);

	validate_xlog_location(locationstr);

	if (sscanf(locationstr, "%X/%X", &hi, &lo) != 2)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("could not parse transaction log location \"%s\"",
						locationstr)));
	locationpoint = ((uint64) hi) << 32 | lo;

	XLByteToPrevSeg(locationpoint, xlogsegno);
	XLogFileName(xlogfilename, ThisTimeLineID, xlogsegno);

	PG_RETURN_TEXT_P(cstring_to_text(xlogfilename));
}

/*
 * pg_xlog_replay_pause - pause recovery now
 */
Datum
pg_xlog_replay_pause(PG_FUNCTION_ARGS)
{
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to control recovery"))));

	if (!RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is not in progress"),
				 errhint("Recovery control functions can only be executed during recovery.")));

	SetRecoveryPause(true);

	PG_RETURN_VOID();
}

/*
 * pg_xlog_replay_resume - resume recovery now
 */
Datum
pg_xlog_replay_resume(PG_FUNCTION_ARGS)
{
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to control recovery"))));

	if (!RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is not in progress"),
				 errhint("Recovery control functions can only be executed during recovery.")));

	SetRecoveryPause(false);

	PG_RETURN_VOID();
}

/*
 * pg_is_xlog_replay_paused
 */
Datum
pg_is_xlog_replay_paused(PG_FUNCTION_ARGS)
{
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to control recovery"))));

	if (!RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is not in progress"),
				 errhint("Recovery control functions can only be executed during recovery.")));

	PG_RETURN_BOOL(RecoveryIsPaused());
}

/*
 * Returns timestamp of latest processed commit/abort record.
 *
 * When the server has been started normally without recovery the function
 * returns NULL.
 */
Datum
pg_last_xact_replay_timestamp(PG_FUNCTION_ARGS)
{
	TimestampTz xtime;

	xtime = GetLatestXTime();
	if (xtime == 0)
		PG_RETURN_NULL();

	PG_RETURN_TIMESTAMPTZ(xtime);
}

/*
 * Returns bool with current recovery mode, a global state.
 */
Datum
pg_is_in_recovery(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(RecoveryInProgress());
}

/*
 * Validate the text form of a transaction log location.
 * (Just using sscanf() input allows incorrect values such as
 * negatives, so we have to be a bit more careful about that).
 */
static void
validate_xlog_location(char *str)
{
#define MAXLSNCOMPONENT		8

	int			len1,
				len2;

	len1 = strspn(str, "0123456789abcdefABCDEF");
	if (len1 < 1 || len1 > MAXLSNCOMPONENT || str[len1] != '/')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for transaction log location: \"%s\"", str)));

	len2 = strspn(str + len1 + 1, "0123456789abcdefABCDEF");
	if (len2 < 1 || len2 > MAXLSNCOMPONENT || str[len1 + 1 + len2] != '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for transaction log location: \"%s\"", str)));
}

/*
 * Compute the difference in bytes between two WAL locations.
 */
Datum
pg_xlog_location_diff(PG_FUNCTION_ARGS)
{
	text	   *location1 = PG_GETARG_TEXT_P(0);
	text	   *location2 = PG_GETARG_TEXT_P(1);
	char	   *str1,
			   *str2;
	XLogRecPtr	loc1,
				loc2;
	Numeric		result;
	uint64		bytes1,
				bytes2;
	uint32		hi,
				lo;

	/*
	 * Read and parse input
	 */
	str1 = text_to_cstring(location1);
	str2 = text_to_cstring(location2);

	validate_xlog_location(str1);
	validate_xlog_location(str2);

	if (sscanf(str1, "%X/%X", &hi, &lo) != 2)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		   errmsg("could not parse transaction log location \"%s\"", str1)));
	loc1 = ((uint64) hi) << 32 | lo;

	if (sscanf(str2, "%X/%X", &hi, &lo) != 2)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		   errmsg("could not parse transaction log location \"%s\"", str2)));
	loc2 = ((uint64) hi) << 32 | lo;

	bytes1 = (uint64) loc1;
	bytes2 = (uint64) loc2;

	/*
	 * result = bytes1 - bytes2.
	 *
	 * XXX: this won't handle values higher than 2^63 correctly.
	 */
	result = DatumGetNumeric(DirectFunctionCall2(numeric_sub,
			DirectFunctionCall1(int8_numeric, Int64GetDatum((int64) bytes1)),
		  DirectFunctionCall1(int8_numeric, Int64GetDatum((int64) bytes2))));

	PG_RETURN_NUMERIC(result);
}

/*
 * Returns bool with current on-line backup mode, a global state.
 */
Datum
pg_is_in_backup(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(BackupInProgress());
}

/*
 * Returns start time of an online exclusive backup.
 *
 * When there's no exclusive backup in progress, the function
 * returns NULL.
 */
Datum
pg_backup_start_time(PG_FUNCTION_ARGS)
{
	Datum		xtime;
	FILE	   *lfp;
	char		fline[MAXPGPATH];
	char		backup_start_time[30];

	/*
	 * See if label file is present
	 */
	lfp = AllocateFile(BACKUP_LABEL_FILE, "r");
	if (lfp == NULL)
	{
		if (errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m",
							BACKUP_LABEL_FILE)));
		PG_RETURN_NULL();
	}

	/*
	 * Parse the file to find the the START TIME line.
	 */
	backup_start_time[0] = '\0';
	while (fgets(fline, sizeof(fline), lfp) != NULL)
	{
		if (sscanf(fline, "START TIME: %25[^\n]\n", backup_start_time) == 1)
			break;
	}

	/* Check for a read error. */
	if (ferror(lfp))
		ereport(ERROR,
				(errcode_for_file_access(),
			   errmsg("could not read file \"%s\": %m", BACKUP_LABEL_FILE)));

	/* Close the backup label file. */
	if (FreeFile(lfp))
		ereport(ERROR,
				(errcode_for_file_access(),
			  errmsg("could not close file \"%s\": %m", BACKUP_LABEL_FILE)));

	if (strlen(backup_start_time) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("invalid data in file \"%s\"", BACKUP_LABEL_FILE)));

	/*
	 * Convert the time string read from file to TimestampTz form.
	 */
	xtime = DirectFunctionCall3(timestamptz_in,
								CStringGetDatum(backup_start_time),
								ObjectIdGetDatum(InvalidOid),
								Int32GetDatum(-1));

	PG_RETURN_DATUM(xtime);
}
