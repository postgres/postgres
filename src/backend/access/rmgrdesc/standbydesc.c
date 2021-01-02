/*-------------------------------------------------------------------------
 *
 * standbydesc.c
 *	  rmgr descriptor routines for storage/ipc/standby.c
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
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
	else if (info == XLOG_INVALIDATIONS)
	{
		xl_invalidations *xlrec = (xl_invalidations *) rec;

		standby_desc_invalidations(buf, xlrec->nmsgs, xlrec->msgs,
								   xlrec->dbId, xlrec->tsId,
								   xlrec->relcacheInitFileInval);
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
		case XLOG_INVALIDATIONS:
			id = "INVALIDATIONS";
			break;
	}

	return id;
}

/*
 * This routine is used by both standby_desc and xact_desc, because
 * transaction commits and XLOG_INVALIDATIONS messages contain invalidations;
 * it seems pointless to duplicate the code.
 */
void
standby_desc_invalidations(StringInfo buf,
						   int nmsgs, SharedInvalidationMessage *msgs,
						   Oid dbId, Oid tsId,
						   bool relcacheInitFileInval)
{
	int			i;

	/* Do nothing if there are no invalidation messages */
	if (nmsgs <= 0)
		return;

	if (relcacheInitFileInval)
		appendStringInfo(buf, "; relcache init file inval dbid %u tsid %u",
						 dbId, tsId);

	appendStringInfoString(buf, "; inval msgs:");
	for (i = 0; i < nmsgs; i++)
	{
		SharedInvalidationMessage *msg = &msgs[i];

		if (msg->id >= 0)
			appendStringInfo(buf, " catcache %d", msg->id);
		else if (msg->id == SHAREDINVALCATALOG_ID)
			appendStringInfo(buf, " catalog %u", msg->cat.catId);
		else if (msg->id == SHAREDINVALRELCACHE_ID)
			appendStringInfo(buf, " relcache %u", msg->rc.relId);
		/* not expected, but print something anyway */
		else if (msg->id == SHAREDINVALSMGR_ID)
			appendStringInfoString(buf, " smgr");
		/* not expected, but print something anyway */
		else if (msg->id == SHAREDINVALRELMAP_ID)
			appendStringInfo(buf, " relmap db %u", msg->rm.dbId);
		else if (msg->id == SHAREDINVALSNAPSHOT_ID)
			appendStringInfo(buf, " snapshot %u", msg->sn.relId);
		else
			appendStringInfo(buf, " unrecognized id %d", msg->id);
	}
}
