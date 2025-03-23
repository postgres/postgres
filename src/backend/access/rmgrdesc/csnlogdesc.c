/*-------------------------------------------------------------------------
 *
 * clogdesc.c
 *	  rmgr descriptor routines for access/transam/csn_log.c
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/csnlogdesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/csn_log.h"


void
csnlog_desc(StringInfo buf, XLogReaderState *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	if (info == XLOG_CSN_ZEROPAGE)
	{
		int pageno;

		memcpy(&pageno, XLogRecGetData(record), sizeof(int));
		appendStringInfo(buf, "pageno %d", pageno);
	}
	else if (info == XLOG_CSN_TRUNCATE)
	{
		int pageno;

		memcpy(&pageno, XLogRecGetData(record), sizeof(int));
		appendStringInfo(buf, "pageno %d", pageno);
	}
	else if (info == XLOG_CSN_ASSIGNMENT)
	{
		CSN csn;

		memcpy(&csn, XLogRecGetData(record), sizeof(CSN));
		appendStringInfo(buf, "assign "INT64_FORMAT"", csn);
	}
	else if (info == XLOG_CSN_SETCSN)
	{
		xl_csn_set *xlrec = (xl_csn_set *) rec;
		int			  nsubxids;

		appendStringInfo(buf, "set "INT64_FORMAT" for: %u",
						 xlrec->csn,
						 xlrec->xtop);
		nsubxids = ((XLogRecGetDataLen(record) - MinSizeOfCSNSet) /
					sizeof(TransactionId));
		if (nsubxids > 0)
		{
			int			i;
			TransactionId *subxids;

			subxids = palloc(sizeof(TransactionId) * nsubxids);
			memcpy(subxids,
				   XLogRecGetData(record) + MinSizeOfCSNSet,
				   sizeof(TransactionId) * nsubxids);
			for (i = 0; i < nsubxids; i++)
				appendStringInfo(buf, ", %u", subxids[i]);
			pfree(subxids);
		}
	}
}

const char *
csnlog_identify(uint8 info)
{
	const char *id = NULL;

	switch (info & ~XLR_INFO_MASK)
	{
		case XLOG_CSN_ASSIGNMENT:
			id = "ASSIGNMENT";
			break;
		case XLOG_CSN_SETCSN:
			id = "SETCSN";
			break;
		case XLOG_CSN_ZEROPAGE:
			id = "ZEROPAGE";
			break;
		case XLOG_CSN_TRUNCATE:
			id = "TRUNCATE";
			break;
	}

	return id;
}
