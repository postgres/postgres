/*-------------------------------------------------------------------------
 *
 * heapdesc.c
 *	  rmgr descriptor routines for access/heap/heapam.c
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/heapdesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam_xlog.h"

static void
out_infobits(StringInfo buf, uint8 infobits)
{
	if (infobits & XLHL_XMAX_IS_MULTI)
		appendStringInfoString(buf, "IS_MULTI ");
	if (infobits & XLHL_XMAX_LOCK_ONLY)
		appendStringInfoString(buf, "LOCK_ONLY ");
	if (infobits & XLHL_XMAX_EXCL_LOCK)
		appendStringInfoString(buf, "EXCL_LOCK ");
	if (infobits & XLHL_XMAX_KEYSHR_LOCK)
		appendStringInfoString(buf, "KEYSHR_LOCK ");
	if (infobits & XLHL_KEYS_UPDATED)
		appendStringInfoString(buf, "KEYS_UPDATED ");
}

void
heap_desc(StringInfo buf, XLogReaderState *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	info &= XLOG_HEAP_OPMASK;
	if (info == XLOG_HEAP_INSERT)
	{
		xl_heap_insert *xlrec = (xl_heap_insert *) rec;

		appendStringInfo(buf, "off %u", xlrec->offnum);
	}
	else if (info == XLOG_HEAP_DELETE)
	{
		xl_heap_delete *xlrec = (xl_heap_delete *) rec;

		appendStringInfo(buf, "off %u ", xlrec->offnum);
		out_infobits(buf, xlrec->infobits_set);
	}
	else if (info == XLOG_HEAP_UPDATE)
	{
		xl_heap_update *xlrec = (xl_heap_update *) rec;

		appendStringInfo(buf, "off %u xmax %u ",
						 xlrec->old_offnum,
						 xlrec->old_xmax);
		out_infobits(buf, xlrec->old_infobits_set);
		appendStringInfo(buf, "; new off %u xmax %u",
						 xlrec->new_offnum,
						 xlrec->new_xmax);
	}
	else if (info == XLOG_HEAP_HOT_UPDATE)
	{
		xl_heap_update *xlrec = (xl_heap_update *) rec;

		appendStringInfo(buf, "off %u xmax %u ",
						 xlrec->old_offnum,
						 xlrec->old_xmax);
		out_infobits(buf, xlrec->old_infobits_set);
		appendStringInfo(buf, "; new off %u xmax %u",
						 xlrec->new_offnum,
						 xlrec->new_xmax);
	}
	else if (info == XLOG_HEAP_CONFIRM)
	{
		xl_heap_confirm *xlrec = (xl_heap_confirm *) rec;

		appendStringInfo(buf, "off %u", xlrec->offnum);
	}
	else if (info == XLOG_HEAP_LOCK)
	{
		xl_heap_lock *xlrec = (xl_heap_lock *) rec;

		appendStringInfo(buf, "off %u: xid %u: flags %u ",
						 xlrec->offnum, xlrec->locking_xid, xlrec->flags);
		out_infobits(buf, xlrec->infobits_set);
	}
	else if (info == XLOG_HEAP_INPLACE)
	{
		xl_heap_inplace *xlrec = (xl_heap_inplace *) rec;

		appendStringInfo(buf, "off %u", xlrec->offnum);
	}
}
void
heap2_desc(StringInfo buf, XLogReaderState *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	info &= XLOG_HEAP_OPMASK;
	if (info == XLOG_HEAP2_CLEAN)
	{
		xl_heap_clean *xlrec = (xl_heap_clean *) rec;

		appendStringInfo(buf, "remxid %u", xlrec->latestRemovedXid);
	}
	else if (info == XLOG_HEAP2_FREEZE_PAGE)
	{
		xl_heap_freeze_page *xlrec = (xl_heap_freeze_page *) rec;

		appendStringInfo(buf, "cutoff xid %u ntuples %u",
						 xlrec->cutoff_xid, xlrec->ntuples);
	}
	else if (info == XLOG_HEAP2_CLEANUP_INFO)
	{
		xl_heap_cleanup_info *xlrec = (xl_heap_cleanup_info *) rec;

		appendStringInfo(buf, "remxid %u", xlrec->latestRemovedXid);
	}
	else if (info == XLOG_HEAP2_VISIBLE)
	{
		xl_heap_visible *xlrec = (xl_heap_visible *) rec;

		appendStringInfo(buf, "cutoff xid %u flags %d",
						 xlrec->cutoff_xid, xlrec->flags);
	}
	else if (info == XLOG_HEAP2_MULTI_INSERT)
	{
		xl_heap_multi_insert *xlrec = (xl_heap_multi_insert *) rec;

		appendStringInfo(buf, "%d tuples", xlrec->ntuples);
	}
	else if (info == XLOG_HEAP2_LOCK_UPDATED)
	{
		xl_heap_lock_updated *xlrec = (xl_heap_lock_updated *) rec;

		appendStringInfo(buf, "off %u: xmax %u: flags %u ",
						 xlrec->offnum, xlrec->xmax, xlrec->flags);
		out_infobits(buf, xlrec->infobits_set);
	}
	else if (info == XLOG_HEAP2_NEW_CID)
	{
		xl_heap_new_cid *xlrec = (xl_heap_new_cid *) rec;

		appendStringInfo(buf, "rel %u/%u/%u; tid %u/%u",
						 xlrec->target_node.spcNode,
						 xlrec->target_node.dbNode,
						 xlrec->target_node.relNode,
						 ItemPointerGetBlockNumber(&(xlrec->target_tid)),
						 ItemPointerGetOffsetNumber(&(xlrec->target_tid)));
		appendStringInfo(buf, "; cmin: %u, cmax: %u, combo: %u",
						 xlrec->cmin, xlrec->cmax, xlrec->combocid);
	}
}

const char *
heap_identify(uint8 info)
{
	const char *id = NULL;

	switch (info & ~XLR_INFO_MASK)
	{
		case XLOG_HEAP_INSERT:
			id = "INSERT";
			break;
		case XLOG_HEAP_INSERT | XLOG_HEAP_INIT_PAGE:
			id = "INSERT+INIT";
			break;
		case XLOG_HEAP_DELETE:
			id = "DELETE";
			break;
		case XLOG_HEAP_UPDATE:
			id = "UPDATE";
			break;
		case XLOG_HEAP_UPDATE | XLOG_HEAP_INIT_PAGE:
			id = "UPDATE+INIT";
			break;
		case XLOG_HEAP_HOT_UPDATE:
			id = "HOT_UPDATE";
			break;
		case XLOG_HEAP_HOT_UPDATE | XLOG_HEAP_INIT_PAGE:
			id = "HOT_UPDATE+INIT";
			break;
		case XLOG_HEAP_CONFIRM:
			id = "HEAP_CONFIRM";
			break;
		case XLOG_HEAP_LOCK:
			id = "LOCK";
			break;
		case XLOG_HEAP_INPLACE:
			id = "INPLACE";
			break;
	}

	return id;
}

const char *
heap2_identify(uint8 info)
{
	const char *id = NULL;

	switch (info & ~XLR_INFO_MASK)
	{
		case XLOG_HEAP2_CLEAN:
			id = "CLEAN";
			break;
		case XLOG_HEAP2_FREEZE_PAGE:
			id = "FREEZE_PAGE";
			break;
		case XLOG_HEAP2_CLEANUP_INFO:
			id = "CLEANUP_INFO";
			break;
		case XLOG_HEAP2_VISIBLE:
			id = "VISIBLE";
			break;
		case XLOG_HEAP2_MULTI_INSERT:
			id = "MULTI_INSERT";
			break;
		case XLOG_HEAP2_MULTI_INSERT | XLOG_HEAP_INIT_PAGE:
			id = "MULTI_INSERT+INIT";
			break;
		case XLOG_HEAP2_LOCK_UPDATED:
			id = "LOCK_UPDATED";
			break;
		case XLOG_HEAP2_NEW_CID:
			id = "NEW_CID";
			break;
		case XLOG_HEAP2_REWRITE:
			id = "REWRITE";
			break;
	}

	return id;
}
