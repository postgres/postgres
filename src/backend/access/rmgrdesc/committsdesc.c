/*-------------------------------------------------------------------------
 *
 * committsdesc.c
 *	  rmgr descriptor routines for access/transam/commit_ts.c
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
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
	else if (info == COMMIT_TS_SETTS)
	{
		xl_commit_ts_set *xlrec = (xl_commit_ts_set *) rec;
		int			nsubxids;

		appendStringInfo(buf, "set %s/%d for: %u",
						 timestamptz_to_str(xlrec->timestamp),
						 xlrec->nodeid,
						 xlrec->mainxid);
		nsubxids = ((XLogRecGetDataLen(record) - SizeOfCommitTsSet) /
					sizeof(TransactionId));
		if (nsubxids > 0)
		{
			int			i;
			TransactionId *subxids;

			subxids = palloc(sizeof(TransactionId) * nsubxids);
			memcpy(subxids,
				   XLogRecGetData(record) + SizeOfCommitTsSet,
				   sizeof(TransactionId) * nsubxids);
			for (i = 0; i < nsubxids; i++)
				appendStringInfo(buf, ", %u", subxids[i]);
			pfree(subxids);
		}
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
		case COMMIT_TS_SETTS:
			return "SETTS";
		default:
			return NULL;
	}
}
