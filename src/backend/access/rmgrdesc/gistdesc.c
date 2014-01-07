/*-------------------------------------------------------------------------
 *
 * gistdesc.c
 *	  rmgr descriptor routines for access/gist/gistxlog.c
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
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
out_target(StringInfo buf, RelFileNode node)
{
	appendStringInfo(buf, "rel %u/%u/%u",
					 node.spcNode, node.dbNode, node.relNode);
}

static void
out_gistxlogPageUpdate(StringInfo buf, gistxlogPageUpdate *xlrec)
{
	out_target(buf, xlrec->node);
	appendStringInfo(buf, "; block number %u", xlrec->blkno);
}

static void
out_gistxlogPageSplit(StringInfo buf, gistxlogPageSplit *xlrec)
{
	appendStringInfoString(buf, "page_split: ");
	out_target(buf, xlrec->node);
	appendStringInfo(buf, "; block number %u splits to %d pages",
					 xlrec->origblkno, xlrec->npage);
}

void
gist_desc(StringInfo buf, uint8 xl_info, char *rec)
{
	uint8		info = xl_info & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_GIST_PAGE_UPDATE:
			appendStringInfoString(buf, "page_update: ");
			out_gistxlogPageUpdate(buf, (gistxlogPageUpdate *) rec);
			break;
		case XLOG_GIST_PAGE_SPLIT:
			out_gistxlogPageSplit(buf, (gistxlogPageSplit *) rec);
			break;
		case XLOG_GIST_CREATE_INDEX:
			appendStringInfo(buf, "create_index: rel %u/%u/%u",
							 ((RelFileNode *) rec)->spcNode,
							 ((RelFileNode *) rec)->dbNode,
							 ((RelFileNode *) rec)->relNode);
			break;
		default:
			appendStringInfo(buf, "unknown gist op code %u", info);
			break;
	}
}
