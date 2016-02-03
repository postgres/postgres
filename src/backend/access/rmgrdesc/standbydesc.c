/*-------------------------------------------------------------------------
 *
 * standbydesc.c
 *	  rmgr descriptor routines for storage/ipc/standby.c
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/standbydesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/standbydefs.h"

static void
standby_desc_running_xacts(StringInfo buf, xl_running_xacts *xlrec)
{
	int			i;

	appendStringInfo(buf, "nextXid %u latestCompletedXid %u oldestRunningXid %u",
					 xlrec->nextXid,
					 xlrec->latestCompletedXid,
					 xlrec->oldestRunningXid);
	if (xlrec->xcnt > 0)
	{
		appendStringInfo(buf, "; %d xacts:", xlrec->xcnt);
		for (i = 0; i < xlrec->xcnt; i++)
			appendStringInfo(buf, " %u", xlrec->xids[i]);
	}

	if (xlrec->subxid_overflow)
		appendStringInfoString(buf, "; subxid ovf");
}

void
standby_desc(StringInfo buf, XLogReaderState *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	if (info == XLOG_STANDBY_LOCK)
	{
		xl_standby_locks *xlrec = (xl_standby_locks *) rec;
		int			i;

		for (i = 0; i < xlrec->nlocks; i++)
			appendStringInfo(buf, "xid %u db %u rel %u ",
							 xlrec->locks[i].xid, xlrec->locks[i].dbOid,
							 xlrec->locks[i].relOid);
	}
	else if (info == XLOG_RUNNING_XACTS)
	{
		xl_running_xacts *xlrec = (xl_running_xacts *) rec;

		standby_desc_running_xacts(buf, xlrec);
	}
}

const char *
standby_identify(uint8 info)
{
	const char *id = NULL;

	switch (info & ~XLR_INFO_MASK)
	{
		case XLOG_STANDBY_LOCK:
			id = "LOCK";
			break;
		case XLOG_RUNNING_XACTS:
			id = "RUNNING_XACTS";
			break;
	}

	return id;
}
