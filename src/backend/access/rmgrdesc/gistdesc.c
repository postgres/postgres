/*-------------------------------------------------------------------------
 *
 * gistdesc.c
 *	  rmgr descriptor routines for access/gist/gistxlog.c
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/gistdesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/gistxlog.h"
#include "lib/stringinfo.h"
#include "storage/relfilelocator.h"

static void
out_gistxlogPageUpdate(StringInfo buf, gistxlogPageUpdate *xlrec)
{
}

static void
out_gistxlogPageReuse(StringInfo buf, gistxlogPageReuse *xlrec)
{
	appendStringInfo(buf, "rel %u/%u/%u; blk %u; snapshotConflictHorizon %u:%u",
					 xlrec->locator.spcOid, xlrec->locator.dbOid,
					 xlrec->locator.relNumber, xlrec->block,
					 EpochFromFullTransactionId(xlrec->snapshotConflictHorizon),
					 XidFromFullTransactionId(xlrec->snapshotConflictHorizon));
}

static void
out_gistxlogDelete(StringInfo buf, gistxlogDelete *xlrec)
{
	appendStringInfo(buf, "delete: snapshotConflictHorizon %u, nitems: %u",
					 xlrec->snapshotConflictHorizon, xlrec->ntodelete);
}

static void
out_gistxlogPageSplit(StringInfo buf, gistxlogPageSplit *xlrec)
{
	appendStringInfo(buf, "page_split: splits to %d pages",
					 xlrec->npage);
}

static void
out_gistxlogPageDelete(StringInfo buf, gistxlogPageDelete *xlrec)
{
	appendStringInfo(buf, "deleteXid %u:%u; downlink %u",
					 EpochFromFullTransactionId(xlrec->deleteXid),
					 XidFromFullTransactionId(xlrec->deleteXid),
					 xlrec->downlinkOffset);
}

void
gist_desc(StringInfo buf, XLogReaderState *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_GIST_PAGE_UPDATE:
			out_gistxlogPageUpdate(buf, (gistxlogPageUpdate *) rec);
			break;
		case XLOG_GIST_PAGE_REUSE:
			out_gistxlogPageReuse(buf, (gistxlogPageReuse *) rec);
			break;
		case XLOG_GIST_DELETE:
			out_gistxlogDelete(buf, (gistxlogDelete *) rec);
			break;
		case XLOG_GIST_PAGE_SPLIT:
			out_gistxlogPageSplit(buf, (gistxlogPageSplit *) rec);
			break;
		case XLOG_GIST_PAGE_DELETE:
			out_gistxlogPageDelete(buf, (gistxlogPageDelete *) rec);
			break;
		case XLOG_GIST_ASSIGN_LSN:
			/* No details to write out */
			break;
	}
}

const char *
gist_identify(uint8 info)
{
	const char *id = NULL;

	switch (info & ~XLR_INFO_MASK)
	{
		case XLOG_GIST_PAGE_UPDATE:
			id = "PAGE_UPDATE";
			break;
		case XLOG_GIST_DELETE:
			id = "DELETE";
			break;
		case XLOG_GIST_PAGE_REUSE:
			id = "PAGE_REUSE";
			break;
		case XLOG_GIST_PAGE_SPLIT:
			id = "PAGE_SPLIT";
			break;
		case XLOG_GIST_PAGE_DELETE:
			id = "PAGE_DELETE";
			break;
		case XLOG_GIST_ASSIGN_LSN:
			id = "ASSIGN_LSN";
			break;
	}

	return id;
}
