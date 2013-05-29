/*-------------------------------------------------------------------------
 *
 * heapdesc.c
 *	  rmgr descriptor routines for access/heap/heapam.c
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
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
out_target(StringInfo buf, xl_heaptid *target)
{
	appendStringInfo(buf, "rel %u/%u/%u; tid %u/%u",
			 target->node.spcNode, target->node.dbNode, target->node.relNode,
					 ItemPointerGetBlockNumber(&(target->tid)),
					 ItemPointerGetOffsetNumber(&(target->tid)));
}

static void
out_infobits(StringInfo buf, uint8 infobits)
{
	if (infobits & XLHL_XMAX_IS_MULTI)
		appendStringInfo(buf, "IS_MULTI ");
	if (infobits & XLHL_XMAX_LOCK_ONLY)
		appendStringInfo(buf, "LOCK_ONLY ");
	if (infobits & XLHL_XMAX_EXCL_LOCK)
		appendStringInfo(buf, "EXCL_LOCK ");
	if (infobits & XLHL_XMAX_KEYSHR_LOCK)
		appendStringInfo(buf, "KEYSHR_LOCK ");
	if (infobits & XLHL_KEYS_UPDATED)
		appendStringInfo(buf, "KEYS_UPDATED ");
}

void
heap_desc(StringInfo buf, uint8 xl_info, char *rec)
{
	uint8		info = xl_info & ~XLR_INFO_MASK;

	info &= XLOG_HEAP_OPMASK;
	if (info == XLOG_HEAP_INSERT)
	{
		xl_heap_insert *xlrec = (xl_heap_insert *) rec;

		if (xl_info & XLOG_HEAP_INIT_PAGE)
			appendStringInfo(buf, "insert(init): ");
		else
			appendStringInfo(buf, "insert: ");
		out_target(buf, &(xlrec->target));
	}
	else if (info == XLOG_HEAP_DELETE)
	{
		xl_heap_delete *xlrec = (xl_heap_delete *) rec;

		appendStringInfo(buf, "delete: ");
		out_target(buf, &(xlrec->target));
		appendStringInfoChar(buf, ' ');
		out_infobits(buf, xlrec->infobits_set);
	}
	else if (info == XLOG_HEAP_UPDATE)
	{
		xl_heap_update *xlrec = (xl_heap_update *) rec;

		if (xl_info & XLOG_HEAP_INIT_PAGE)
			appendStringInfo(buf, "update(init): ");
		else
			appendStringInfo(buf, "update: ");
		out_target(buf, &(xlrec->target));
		appendStringInfo(buf, " xmax %u ", xlrec->old_xmax);
		out_infobits(buf, xlrec->old_infobits_set);
		appendStringInfo(buf, "; new tid %u/%u xmax %u",
						 ItemPointerGetBlockNumber(&(xlrec->newtid)),
						 ItemPointerGetOffsetNumber(&(xlrec->newtid)),
						 xlrec->new_xmax);
	}
	else if (info == XLOG_HEAP_HOT_UPDATE)
	{
		xl_heap_update *xlrec = (xl_heap_update *) rec;

		if (xl_info & XLOG_HEAP_INIT_PAGE)		/* can this case happen? */
			appendStringInfo(buf, "hot_update(init): ");
		else
			appendStringInfo(buf, "hot_update: ");
		out_target(buf, &(xlrec->target));
		appendStringInfo(buf, " xmax %u ", xlrec->old_xmax);
		out_infobits(buf, xlrec->old_infobits_set);
		appendStringInfo(buf, "; new tid %u/%u xmax %u",
						 ItemPointerGetBlockNumber(&(xlrec->newtid)),
						 ItemPointerGetOffsetNumber(&(xlrec->newtid)),
						 xlrec->new_xmax);
	}
	else if (info == XLOG_HEAP_NEWPAGE)
	{
		xl_heap_newpage *xlrec = (xl_heap_newpage *) rec;

		appendStringInfo(buf, "newpage: rel %u/%u/%u; fork %u, blk %u",
						 xlrec->node.spcNode, xlrec->node.dbNode,
						 xlrec->node.relNode, xlrec->forknum,
						 xlrec->blkno);
	}
	else if (info == XLOG_HEAP_LOCK)
	{
		xl_heap_lock *xlrec = (xl_heap_lock *) rec;

		appendStringInfo(buf, "lock %u: ", xlrec->locking_xid);
		out_target(buf, &(xlrec->target));
		appendStringInfoChar(buf, ' ');
		out_infobits(buf, xlrec->infobits_set);
	}
	else if (info == XLOG_HEAP_INPLACE)
	{
		xl_heap_inplace *xlrec = (xl_heap_inplace *) rec;

		appendStringInfo(buf, "inplace: ");
		out_target(buf, &(xlrec->target));
	}
	else
		appendStringInfo(buf, "UNKNOWN");
}
void
heap2_desc(StringInfo buf, uint8 xl_info, char *rec)
{
	uint8		info = xl_info & ~XLR_INFO_MASK;

	info &= XLOG_HEAP_OPMASK;
	if (info == XLOG_HEAP2_FREEZE)
	{
		xl_heap_freeze *xlrec = (xl_heap_freeze *) rec;

		appendStringInfo(buf, "freeze: rel %u/%u/%u; blk %u; cutoff xid %u multi %u",
						 xlrec->node.spcNode, xlrec->node.dbNode,
						 xlrec->node.relNode, xlrec->block,
						 xlrec->cutoff_xid, xlrec->cutoff_multi);
	}
	else if (info == XLOG_HEAP2_CLEAN)
	{
		xl_heap_clean *xlrec = (xl_heap_clean *) rec;

		appendStringInfo(buf, "clean: rel %u/%u/%u; blk %u remxid %u",
						 xlrec->node.spcNode, xlrec->node.dbNode,
						 xlrec->node.relNode, xlrec->block,
						 xlrec->latestRemovedXid);
	}
	else if (info == XLOG_HEAP2_CLEANUP_INFO)
	{
		xl_heap_cleanup_info *xlrec = (xl_heap_cleanup_info *) rec;

		appendStringInfo(buf, "cleanup info: remxid %u",
						 xlrec->latestRemovedXid);
	}
	else if (info == XLOG_HEAP2_VISIBLE)
	{
		xl_heap_visible *xlrec = (xl_heap_visible *) rec;

		appendStringInfo(buf, "visible: rel %u/%u/%u; blk %u",
						 xlrec->node.spcNode, xlrec->node.dbNode,
						 xlrec->node.relNode, xlrec->block);
	}
	else if (info == XLOG_HEAP2_MULTI_INSERT)
	{
		xl_heap_multi_insert *xlrec = (xl_heap_multi_insert *) rec;

		if (xl_info & XLOG_HEAP_INIT_PAGE)
			appendStringInfo(buf, "multi-insert (init): ");
		else
			appendStringInfo(buf, "multi-insert: ");
		appendStringInfo(buf, "rel %u/%u/%u; blk %u; %d tuples",
				xlrec->node.spcNode, xlrec->node.dbNode, xlrec->node.relNode,
						 xlrec->blkno, xlrec->ntuples);
	}
	else if (info == XLOG_HEAP2_LOCK_UPDATED)
	{
		xl_heap_lock_updated *xlrec = (xl_heap_lock_updated *) rec;

		appendStringInfo(buf, "lock updated: xmax %u msk %04x; ", xlrec->xmax,
						 xlrec->infobits_set);
		out_target(buf, &(xlrec->target));
	}
	else
		appendStringInfo(buf, "UNKNOWN");
}
