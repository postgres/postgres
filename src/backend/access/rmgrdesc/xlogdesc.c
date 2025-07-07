/*-------------------------------------------------------------------------
 *
 * xlogdesc.c
 *	  rmgr descriptor routines for access/transam/xlog.c
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/xlogdesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/transam.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "catalog/pg_control.h"
#include "utils/guc.h"
#include "utils/timestamp.h"

/*
 * GUC support
 */
const struct config_enum_entry wal_level_options[] = {
	{"minimal", WAL_LEVEL_MINIMAL, false},
	{"replica", WAL_LEVEL_REPLICA, false},
	{"archive", WAL_LEVEL_REPLICA, true},	/* deprecated */
	{"hot_standby", WAL_LEVEL_REPLICA, true},	/* deprecated */
	{"logical", WAL_LEVEL_LOGICAL, false},
	{NULL, 0, false}
};

/*
 * Find a string representation for wal_level
 */
static const char *
get_wal_level_string(int wal_level)
{
	const struct config_enum_entry *entry;
	const char *wal_level_str = "?";

	for (entry = wal_level_options; entry->name; entry++)
	{
		if (entry->val == wal_level)
		{
			wal_level_str = entry->name;
			break;
		}
	}

	return wal_level_str;
}

void
xlog_desc(StringInfo buf, XLogReaderState *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	if (info == XLOG_CHECKPOINT_SHUTDOWN ||
		info == XLOG_CHECKPOINT_ONLINE)
	{
		CheckPoint *checkpoint = (CheckPoint *) rec;

		appendStringInfo(buf, "redo %X/%08X; "
						 "tli %u; prev tli %u; fpw %s; wal_level %s; xid %u:%u; oid %u; multi %u; offset %u; "
						 "oldest xid %u in DB %u; oldest multi %u in DB %u; "
						 "oldest/newest commit timestamp xid: %u/%u; "
						 "oldest running xid %u; %s",
						 LSN_FORMAT_ARGS(checkpoint->redo),
						 checkpoint->ThisTimeLineID,
						 checkpoint->PrevTimeLineID,
						 checkpoint->fullPageWrites ? "true" : "false",
						 get_wal_level_string(checkpoint->wal_level),
						 EpochFromFullTransactionId(checkpoint->nextXid),
						 XidFromFullTransactionId(checkpoint->nextXid),
						 checkpoint->nextOid,
						 checkpoint->nextMulti,
						 checkpoint->nextMultiOffset,
						 checkpoint->oldestXid,
						 checkpoint->oldestXidDB,
						 checkpoint->oldestMulti,
						 checkpoint->oldestMultiDB,
						 checkpoint->oldestCommitTsXid,
						 checkpoint->newestCommitTsXid,
						 checkpoint->oldestActiveXid,
						 (info == XLOG_CHECKPOINT_SHUTDOWN) ? "shutdown" : "online");
	}
	else if (info == XLOG_NEXTOID)
	{
		Oid			nextOid;

		memcpy(&nextOid, rec, sizeof(Oid));
		appendStringInfo(buf, "%u", nextOid);
	}
	else if (info == XLOG_RESTORE_POINT)
	{
		xl_restore_point *xlrec = (xl_restore_point *) rec;

		appendStringInfoString(buf, xlrec->rp_name);
	}
	else if (info == XLOG_FPI || info == XLOG_FPI_FOR_HINT)
	{
		/* no further information to print */
	}
	else if (info == XLOG_BACKUP_END)
	{
		XLogRecPtr	startpoint;

		memcpy(&startpoint, rec, sizeof(XLogRecPtr));
		appendStringInfo(buf, "%X/%08X", LSN_FORMAT_ARGS(startpoint));
	}
	else if (info == XLOG_PARAMETER_CHANGE)
	{
		xl_parameter_change xlrec;
		const char *wal_level_str;

		memcpy(&xlrec, rec, sizeof(xl_parameter_change));
		wal_level_str = get_wal_level_string(xlrec.wal_level);

		appendStringInfo(buf, "max_connections=%d max_worker_processes=%d "
						 "max_wal_senders=%d max_prepared_xacts=%d "
						 "max_locks_per_xact=%d wal_level=%s "
						 "wal_log_hints=%s track_commit_timestamp=%s",
						 xlrec.MaxConnections,
						 xlrec.max_worker_processes,
						 xlrec.max_wal_senders,
						 xlrec.max_prepared_xacts,
						 xlrec.max_locks_per_xact,
						 wal_level_str,
						 xlrec.wal_log_hints ? "on" : "off",
						 xlrec.track_commit_timestamp ? "on" : "off");
	}
	else if (info == XLOG_FPW_CHANGE)
	{
		bool		fpw;

		memcpy(&fpw, rec, sizeof(bool));
		appendStringInfoString(buf, fpw ? "true" : "false");
	}
	else if (info == XLOG_END_OF_RECOVERY)
	{
		xl_end_of_recovery xlrec;

		memcpy(&xlrec, rec, sizeof(xl_end_of_recovery));
		appendStringInfo(buf, "tli %u; prev tli %u; time %s; wal_level %s",
						 xlrec.ThisTimeLineID, xlrec.PrevTimeLineID,
						 timestamptz_to_str(xlrec.end_time),
						 get_wal_level_string(xlrec.wal_level));
	}
	else if (info == XLOG_OVERWRITE_CONTRECORD)
	{
		xl_overwrite_contrecord xlrec;

		memcpy(&xlrec, rec, sizeof(xl_overwrite_contrecord));
		appendStringInfo(buf, "lsn %X/%08X; time %s",
						 LSN_FORMAT_ARGS(xlrec.overwritten_lsn),
						 timestamptz_to_str(xlrec.overwrite_time));
	}
	else if (info == XLOG_CHECKPOINT_REDO)
	{
		int			wal_level;

		memcpy(&wal_level, rec, sizeof(int));
		appendStringInfo(buf, "wal_level %s", get_wal_level_string(wal_level));
	}
}

const char *
xlog_identify(uint8 info)
{
	const char *id = NULL;

	switch (info & ~XLR_INFO_MASK)
	{
		case XLOG_CHECKPOINT_SHUTDOWN:
			id = "CHECKPOINT_SHUTDOWN";
			break;
		case XLOG_CHECKPOINT_ONLINE:
			id = "CHECKPOINT_ONLINE";
			break;
		case XLOG_NOOP:
			id = "NOOP";
			break;
		case XLOG_NEXTOID:
			id = "NEXTOID";
			break;
		case XLOG_SWITCH:
			id = "SWITCH";
			break;
		case XLOG_BACKUP_END:
			id = "BACKUP_END";
			break;
		case XLOG_PARAMETER_CHANGE:
			id = "PARAMETER_CHANGE";
			break;
		case XLOG_RESTORE_POINT:
			id = "RESTORE_POINT";
			break;
		case XLOG_FPW_CHANGE:
			id = "FPW_CHANGE";
			break;
		case XLOG_END_OF_RECOVERY:
			id = "END_OF_RECOVERY";
			break;
		case XLOG_OVERWRITE_CONTRECORD:
			id = "OVERWRITE_CONTRECORD";
			break;
		case XLOG_FPI:
			id = "FPI";
			break;
		case XLOG_FPI_FOR_HINT:
			id = "FPI_FOR_HINT";
			break;
		case XLOG_CHECKPOINT_REDO:
			id = "CHECKPOINT_REDO";
			break;
	}

	return id;
}

/*
 * Returns a string giving information about all the blocks in an
 * XLogRecord.
 */
void
XLogRecGetBlockRefInfo(XLogReaderState *record, bool pretty,
					   bool detailed_format, StringInfo buf,
					   uint32 *fpi_len)
{
	int			block_id;

	Assert(record != NULL);

	if (detailed_format && pretty)
		appendStringInfoChar(buf, '\n');

	for (block_id = 0; block_id <= XLogRecMaxBlockId(record); block_id++)
	{
		RelFileLocator rlocator;
		ForkNumber	forknum;
		BlockNumber blk;

		if (!XLogRecGetBlockTagExtended(record, block_id,
										&rlocator, &forknum, &blk, NULL))
			continue;

		if (detailed_format)
		{
			/* Get block references in detailed format. */

			if (pretty)
				appendStringInfoChar(buf, '\t');
			else if (block_id > 0)
				appendStringInfoChar(buf, ' ');

			appendStringInfo(buf,
							 "blkref #%d: rel %u/%u/%u fork %s blk %u",
							 block_id,
							 rlocator.spcOid, rlocator.dbOid, rlocator.relNumber,
							 forkNames[forknum],
							 blk);

			if (XLogRecHasBlockImage(record, block_id))
			{
				uint8		bimg_info = XLogRecGetBlock(record, block_id)->bimg_info;

				/* Calculate the amount of FPI data in the record. */
				if (fpi_len)
					*fpi_len += XLogRecGetBlock(record, block_id)->bimg_len;

				if (BKPIMAGE_COMPRESSED(bimg_info))
				{
					const char *method;

					if ((bimg_info & BKPIMAGE_COMPRESS_PGLZ) != 0)
						method = "pglz";
					else if ((bimg_info & BKPIMAGE_COMPRESS_LZ4) != 0)
						method = "lz4";
					else if ((bimg_info & BKPIMAGE_COMPRESS_ZSTD) != 0)
						method = "zstd";
					else
						method = "unknown";

					appendStringInfo(buf,
									 " (FPW%s); hole: offset: %u, length: %u, "
									 "compression saved: %u, method: %s",
									 XLogRecBlockImageApply(record, block_id) ?
									 "" : " for WAL verification",
									 XLogRecGetBlock(record, block_id)->hole_offset,
									 XLogRecGetBlock(record, block_id)->hole_length,
									 BLCKSZ -
									 XLogRecGetBlock(record, block_id)->hole_length -
									 XLogRecGetBlock(record, block_id)->bimg_len,
									 method);
				}
				else
				{
					appendStringInfo(buf,
									 " (FPW%s); hole: offset: %u, length: %u",
									 XLogRecBlockImageApply(record, block_id) ?
									 "" : " for WAL verification",
									 XLogRecGetBlock(record, block_id)->hole_offset,
									 XLogRecGetBlock(record, block_id)->hole_length);
				}
			}

			if (pretty)
				appendStringInfoChar(buf, '\n');
		}
		else
		{
			/* Get block references in short format. */

			if (forknum != MAIN_FORKNUM)
			{
				appendStringInfo(buf,
								 ", blkref #%d: rel %u/%u/%u fork %s blk %u",
								 block_id,
								 rlocator.spcOid, rlocator.dbOid, rlocator.relNumber,
								 forkNames[forknum],
								 blk);
			}
			else
			{
				appendStringInfo(buf,
								 ", blkref #%d: rel %u/%u/%u blk %u",
								 block_id,
								 rlocator.spcOid, rlocator.dbOid, rlocator.relNumber,
								 blk);
			}

			if (XLogRecHasBlockImage(record, block_id))
			{
				/* Calculate the amount of FPI data in the record. */
				if (fpi_len)
					*fpi_len += XLogRecGetBlock(record, block_id)->bimg_len;

				if (XLogRecBlockImageApply(record, block_id))
					appendStringInfoString(buf, " FPW");
				else
					appendStringInfoString(buf, " FPW for WAL verification");
			}
		}
	}

	if (!detailed_format && pretty)
		appendStringInfoChar(buf, '\n');
}
