/*-------------------------------------------------------------------------
 *
 * committsdesc.c
 *	  rmgr descriptor routines for access/transam/commit_ts.c
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/committsdesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/commit_ts.h"
#include "utils/timestamp.h"


void
commit_ts_desc(StringInfo buf, XLogReaderState *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	if (info == COMMIT_TS_ZEROPAGE)
	{
		int			pageno;

		memcpy(&pageno, rec, sizeof(int));
		appendStringInfo(buf, "%d", pageno);
	}
	else if (info == COMMIT_TS_TRUNCATE)
	{
		xl_commit_ts_truncate *trunc = (xl_commit_ts_truncate *) rec;

		appendStringInfo(buf, "pageno %d, oldestXid %u",
						 trunc->pageno, trunc->oldestXid);
	}
}

const char *
commit_ts_identify(uint8 info)
{
	switch (info)
	{
		case COMMIT_TS_ZEROPAGE:
			return "ZEROPAGE";
		case COMMIT_TS_TRUNCATE:
			return "TRUNCATE";
		default:
			return NULL;
	}
}
