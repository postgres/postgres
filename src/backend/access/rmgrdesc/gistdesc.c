/*-------------------------------------------------------------------------
 *
 * gistdesc.c
 *	  rmgr descriptor routines for access/gist/gistxlog.c
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/gistdesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/gist_private.h"
#include "lib/stringinfo.h"
#include "storage/relfilenode.h"

static void
out_gistxlogPageUpdate(StringInfo buf, gistxlogPageUpdate *xlrec)
{
}

static void
out_gistxlogPageSplit(StringInfo buf, gistxlogPageSplit *xlrec)
{
	appendStringInfo(buf, "page_split: splits to %d pages",
					 xlrec->npage);
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
		case XLOG_GIST_PAGE_SPLIT:
			out_gistxlogPageSplit(buf, (gistxlogPageSplit *) rec);
			break;
		case XLOG_GIST_CREATE_INDEX:
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
		case XLOG_GIST_PAGE_SPLIT:
			id = "PAGE_SPLIT";
			break;
		case XLOG_GIST_CREATE_INDEX:
			id = "CREATE_INDEX";
			break;
	}

	return id;
}
