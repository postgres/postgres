/*-------------------------------------------------------------------------
 *
 * mxactdesc.c
 *	  rmgr descriptor routines for access/transam/multixact.c
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/mxactdesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/multixact.h"

static void
out_member(StringInfo buf, MultiXactMember *member)
{
	appendStringInfo(buf, "%u ", member->xid);
	switch (member->status)
	{
		case MultiXactStatusForKeyShare:
			appendStringInfoString(buf, "(keysh) ");
			break;
		case MultiXactStatusForShare:
			appendStringInfoString(buf, "(sh) ");
			break;
		case MultiXactStatusForNoKeyUpdate:
			appendStringInfoString(buf, "(fornokeyupd) ");
			break;
		case MultiXactStatusForUpdate:
			appendStringInfoString(buf, "(forupd) ");
			break;
		case MultiXactStatusNoKeyUpdate:
			appendStringInfoString(buf, "(nokeyupd) ");
			break;
		case MultiXactStatusUpdate:
			appendStringInfoString(buf, "(upd) ");
			break;
		default:
			appendStringInfoString(buf, "(unk) ");
			break;
	}
}

void
multixact_desc(StringInfo buf, XLogReaderState *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	if (info == XLOG_MULTIXACT_ZERO_OFF_PAGE ||
		info == XLOG_MULTIXACT_ZERO_MEM_PAGE)
	{
		int			pageno;

		memcpy(&pageno, rec, sizeof(int));
		appendStringInfo(buf, "%d", pageno);
	}
	else if (info == XLOG_MULTIXACT_CREATE_ID)
	{
		xl_multixact_create *xlrec = (xl_multixact_create *) rec;
		int			i;

		appendStringInfo(buf, "%u offset %u nmembers %d: ", xlrec->mid,
						 xlrec->moff, xlrec->nmembers);
		for (i = 0; i < xlrec->nmembers; i++)
			out_member(buf, &xlrec->members[i]);
	}
	else if (info == XLOG_MULTIXACT_TRUNCATE_ID)
	{
		xl_multixact_truncate *xlrec = (xl_multixact_truncate *) rec;

		appendStringInfo(buf, "offsets [%u, %u), members [%u, %u)",
						 xlrec->startTruncOff, xlrec->endTruncOff,
						 xlrec->startTruncMemb, xlrec->endTruncMemb);
	}
}

const char *
multixact_identify(uint8 info)
{
	const char *id = NULL;

	switch (info & ~XLR_INFO_MASK)
	{
		case XLOG_MULTIXACT_ZERO_OFF_PAGE:
			id = "ZERO_OFF_PAGE";
			break;
		case XLOG_MULTIXACT_ZERO_MEM_PAGE:
			id = "ZERO_MEM_PAGE";
			break;
		case XLOG_MULTIXACT_CREATE_ID:
			id = "CREATE_ID";
			break;
		case XLOG_MULTIXACT_TRUNCATE_ID:
			id = "TRUNCATE_ID";
			break;
	}

	return id;
}
