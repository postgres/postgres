/*-------------------------------------------------------------------------
 *
 * brindesc.c
 *	  rmgr descriptor routines for BRIN indexes
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/brindesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/brin_xlog.h"

void
brin_desc(StringInfo buf, XLogRecord *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		info = record->xl_info & ~XLR_INFO_MASK;

	info &= XLOG_BRIN_OPMASK;
	if (info == XLOG_BRIN_CREATE_INDEX)
	{
		xl_brin_createidx *xlrec = (xl_brin_createidx *) rec;

		appendStringInfo(buf, "v%d pagesPerRange %u rel %u/%u/%u",
						 xlrec->version, xlrec->pagesPerRange,
						 xlrec->node.spcNode, xlrec->node.dbNode,
						 xlrec->node.relNode);
	}
	else if (info == XLOG_BRIN_INSERT)
	{
		xl_brin_insert *xlrec = (xl_brin_insert *) rec;

		appendStringInfo(buf, "rel %u/%u/%u heapBlk %u revmapBlk %u pagesPerRange %u TID (%u,%u)",
						 xlrec->node.spcNode, xlrec->node.dbNode,
						 xlrec->node.relNode,
						 xlrec->heapBlk, xlrec->revmapBlk,
						 xlrec->pagesPerRange,
						 ItemPointerGetBlockNumber(&xlrec->tid),
						 ItemPointerGetOffsetNumber(&xlrec->tid));
	}
	else if (info == XLOG_BRIN_UPDATE)
	{
		xl_brin_update *xlrec = (xl_brin_update *) rec;

		appendStringInfo(buf, "rel %u/%u/%u heapBlk %u revmapBlk %u pagesPerRange %u old TID (%u,%u) TID (%u,%u)",
						 xlrec->new.node.spcNode, xlrec->new.node.dbNode,
						 xlrec->new.node.relNode,
						 xlrec->new.heapBlk, xlrec->new.revmapBlk,
						 xlrec->new.pagesPerRange,
						 ItemPointerGetBlockNumber(&xlrec->oldtid),
						 ItemPointerGetOffsetNumber(&xlrec->oldtid),
						 ItemPointerGetBlockNumber(&xlrec->new.tid),
						 ItemPointerGetOffsetNumber(&xlrec->new.tid));
	}
	else if (info == XLOG_BRIN_SAMEPAGE_UPDATE)
	{
		xl_brin_samepage_update *xlrec = (xl_brin_samepage_update *) rec;

		appendStringInfo(buf, "rel %u/%u/%u TID (%u,%u)",
						 xlrec->node.spcNode, xlrec->node.dbNode,
						 xlrec->node.relNode,
						 ItemPointerGetBlockNumber(&xlrec->tid),
						 ItemPointerGetOffsetNumber(&xlrec->tid));
	}
	else if (info == XLOG_BRIN_REVMAP_EXTEND)
	{
		xl_brin_revmap_extend *xlrec = (xl_brin_revmap_extend *) rec;

		appendStringInfo(buf, "rel %u/%u/%u targetBlk %u",
						 xlrec->node.spcNode, xlrec->node.dbNode,
						 xlrec->node.relNode, xlrec->targetBlk);
	}
}

const char *
brin_identify(uint8 info)
{
	const char *id = NULL;

	switch (info & ~XLR_INFO_MASK)
	{
		case XLOG_BRIN_CREATE_INDEX:
			id = "CREATE_INDEX";
			break;
		case XLOG_BRIN_INSERT:
			id = "INSERT";
			break;
		case XLOG_BRIN_INSERT | XLOG_BRIN_INIT_PAGE:
			id = "INSERT+INIT";
			break;
		case XLOG_BRIN_UPDATE:
			id = "UPDATE";
			break;
		case XLOG_BRIN_UPDATE | XLOG_BRIN_INIT_PAGE:
			id = "UPDATE+INIT";
			break;
		case XLOG_BRIN_SAMEPAGE_UPDATE:
			id = "SAMEPAGE_UPDATE";
			break;
		case XLOG_BRIN_REVMAP_EXTEND:
			id = "REVMAP_EXTEND";
			break;
	}

	return id;
}
