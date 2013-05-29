/*-------------------------------------------------------------------------
 *
 * standbydesc.c
 *	  rmgr descriptor routines for storage/ipc/standby.c
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/standbydesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/standby.h"

static void
standby_desc_running_xacts(StringInfo buf, xl_running_xacts *xlrec)
{
	int			i;

	appendStringInfo(buf, " nextXid %u latestCompletedXid %u oldestRunningXid %u",
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
		appendStringInfo(buf, "; subxid ovf");
}

void
standby_desc(StringInfo buf, uint8 xl_info, char *rec)
{
	uint8		info = xl_info & ~XLR_INFO_MASK;

	if (info == XLOG_STANDBY_LOCK)
	{
		xl_standby_locks *xlrec = (xl_standby_locks *) rec;
		int			i;

		appendStringInfo(buf, "AccessExclusive locks:");

		for (i = 0; i < xlrec->nlocks; i++)
			appendStringInfo(buf, " xid %u db %u rel %u",
							 xlrec->locks[i].xid, xlrec->locks[i].dbOid,
							 xlrec->locks[i].relOid);
	}
	else if (info == XLOG_RUNNING_XACTS)
	{
		xl_running_xacts *xlrec = (xl_running_xacts *) rec;

		appendStringInfo(buf, "running xacts:");
		standby_desc_running_xacts(buf, xlrec);
	}
	else
		appendStringInfo(buf, "UNKNOWN");
}
