/*-------------------------------------------------------------------------
 *
 * heapdesc.c
 *	  rmgr descriptor routines for access/heap/heapam.c
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
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
#include "access/rmgrdesc_utils.h"

/*
 * NOTE: "keyname" argument cannot have trailing spaces or punctuation
 * characters
 */
static void
infobits_desc(StringInfo buf, uint8 infobits, const char *keyname)
{
	appendStringInfo(buf, "%s: [", keyname);

	Assert(buf->data[buf->len - 1] != ' ');

	if (infobits & XLHL_XMAX_IS_MULTI)
		appendStringInfoString(buf, "IS_MULTI, ");
	if (infobits & XLHL_XMAX_LOCK_ONLY)
		appendStringInfoString(buf, "LOCK_ONLY, ");
	if (infobits & XLHL_XMAX_EXCL_LOCK)
		appendStringInfoString(buf, "EXCL_LOCK, ");
	if (infobits & XLHL_XMAX_KEYSHR_LOCK)
		appendStringInfoString(buf, "KEYSHR_LOCK, ");
	if (infobits & XLHL_KEYS_UPDATED)
		appendStringInfoString(buf, "KEYS_UPDATED, ");

	if (buf->data[buf->len - 1] == ' ')
	{
		/* Truncate-away final unneeded ", "  */
		Assert(buf->data[buf->len - 2] == ',');
		buf->len -= 2;
		buf->data[buf->len] = '\0';
	}

	appendStringInfoString(buf, "]");
}

static void
truncate_flags_desc(StringInfo buf, uint8 flags)
{
	appendStringInfoString(buf, "flags: [");

	if (flags & XLH_TRUNCATE_CASCADE)
		appendStringInfoString(buf, "CASCADE, ");
	if (flags & XLH_TRUNCATE_RESTART_SEQS)
		appendStringInfoString(buf, "RESTART_SEQS, ");

	if (buf->data[buf->len - 1] == ' ')
	{
		/* Truncate-away final unneeded ", "  */
		Assert(buf->data[buf->len - 2] == ',');
		buf->len -= 2;
		buf->data[buf->len] = '\0';
	}

	appendStringInfoString(buf, "]");
}

static void
plan_elem_desc(StringInfo buf, void *plan, void *data)
{
	xl_heap_freeze_plan *new_plan = (xl_heap_freeze_plan *) plan;
	OffsetNumber **offsets = data;

	appendStringInfo(buf, "{ xmax: %u, infomask: %u, infomask2: %u, ntuples: %u",
					 new_plan->xmax,
					 new_plan->t_infomask, new_plan->t_infomask2,
					 new_plan->ntuples);

	appendStringInfoString(buf, ", offsets:");
	array_desc(buf, *offsets, sizeof(OffsetNumber), new_plan->ntuples,
			   &offset_elem_desc, NULL);

	*offsets += new_plan->ntuples;

	appendStringInfo(buf, " }");
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

		appendStringInfo(buf, "off: %u, flags: 0x%02X",
						 xlrec->offnum,
						 xlrec->flags);
	}
	else if (info == XLOG_HEAP_DELETE)
	{
		xl_heap_delete *xlrec = (xl_heap_delete *) rec;

		appendStringInfo(buf, "xmax: %u, off: %u, ",
						 xlrec->xmax, xlrec->offnum);
		infobits_desc(buf, xlrec->infobits_set, "infobits");
		appendStringInfo(buf, ", flags: 0x%02X", xlrec->flags);
	}
	else if (info == XLOG_HEAP_UPDATE)
	{
		xl_heap_update *xlrec = (xl_heap_update *) rec;

		appendStringInfo(buf, "old_xmax: %u, old_off: %u, ",
						 xlrec->old_xmax, xlrec->old_offnum);
		infobits_desc(buf, xlrec->old_infobits_set, "old_infobits");
		appendStringInfo(buf, ", flags: 0x%02X, new_xmax: %u, new_off: %u",
						 xlrec->flags, xlrec->new_xmax, xlrec->new_offnum);
	}
	else if (info == XLOG_HEAP_HOT_UPDATE)
	{
		xl_heap_update *xlrec = (xl_heap_update *) rec;

		appendStringInfo(buf, "old_xmax: %u, old_off: %u, ",
						 xlrec->old_xmax, xlrec->old_offnum);
		infobits_desc(buf, xlrec->old_infobits_set, "old_infobits");
		appendStringInfo(buf, ", flags: 0x%02X, new_xmax: %u, new_off: %u",
						 xlrec->flags, xlrec->new_xmax, xlrec->new_offnum);
	}
	else if (info == XLOG_HEAP_TRUNCATE)
	{
		xl_heap_truncate *xlrec = (xl_heap_truncate *) rec;

		truncate_flags_desc(buf, xlrec->flags);
		appendStringInfo(buf, ", nrelids: %u", xlrec->nrelids);
		appendStringInfoString(buf, ", relids:");
		array_desc(buf, xlrec->relids, sizeof(Oid), xlrec->nrelids,
				   &oid_elem_desc, NULL);
	}
	else if (info == XLOG_HEAP_CONFIRM)
	{
		xl_heap_confirm *xlrec = (xl_heap_confirm *) rec;

		appendStringInfo(buf, "off: %u", xlrec->offnum);
	}
	else if (info == XLOG_HEAP_LOCK)
	{
		xl_heap_lock *xlrec = (xl_heap_lock *) rec;

		appendStringInfo(buf, "xmax: %u, off: %u, ",
						 xlrec->xmax, xlrec->offnum);
		infobits_desc(buf, xlrec->infobits_set, "infobits");
		appendStringInfo(buf, ", flags: 0x%02X", xlrec->flags);
	}
	else if (info == XLOG_HEAP_INPLACE)
	{
		xl_heap_inplace *xlrec = (xl_heap_inplace *) rec;

		appendStringInfo(buf, "off: %u", xlrec->offnum);
	}
}

void
heap2_desc(StringInfo buf, XLogReaderState *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	info &= XLOG_HEAP_OPMASK;
	if (info == XLOG_HEAP2_PRUNE)
	{
		xl_heap_prune *xlrec = (xl_heap_prune *) rec;

		appendStringInfo(buf, "snapshotConflictHorizon: %u, nredirected: %u, ndead: %u",
						 xlrec->snapshotConflictHorizon,
						 xlrec->nredirected,
						 xlrec->ndead);

		if (XLogRecHasBlockData(record, 0))
		{
			OffsetNumber *end;
			OffsetNumber *redirected;
			OffsetNumber *nowdead;
			OffsetNumber *nowunused;
			int			nredirected;
			int			nunused;
			Size		datalen;

			redirected = (OffsetNumber *) XLogRecGetBlockData(record, 0,
															  &datalen);

			nredirected = xlrec->nredirected;
			end = (OffsetNumber *) ((char *) redirected + datalen);
			nowdead = redirected + (nredirected * 2);
			nowunused = nowdead + xlrec->ndead;
			nunused = (end - nowunused);
			Assert(nunused >= 0);

			appendStringInfo(buf, ", nunused: %d", nunused);

			appendStringInfoString(buf, ", redirected:");
			array_desc(buf, redirected, sizeof(OffsetNumber) * 2,
					   nredirected, &redirect_elem_desc, NULL);
			appendStringInfoString(buf, ", dead:");
			array_desc(buf, nowdead, sizeof(OffsetNumber), xlrec->ndead,
					   &offset_elem_desc, NULL);
			appendStringInfoString(buf, ", unused:");
			array_desc(buf, nowunused, sizeof(OffsetNumber), nunused,
					   &offset_elem_desc, NULL);
		}
	}
	else if (info == XLOG_HEAP2_VACUUM)
	{
		xl_heap_vacuum *xlrec = (xl_heap_vacuum *) rec;

		appendStringInfo(buf, "nunused: %u", xlrec->nunused);

		if (XLogRecHasBlockData(record, 0))
		{
			OffsetNumber *nowunused;

			nowunused = (OffsetNumber *) XLogRecGetBlockData(record, 0, NULL);

			appendStringInfoString(buf, ", unused:");
			array_desc(buf, nowunused, sizeof(OffsetNumber), xlrec->nunused,
					   &offset_elem_desc, NULL);
		}
	}
	else if (info == XLOG_HEAP2_FREEZE_PAGE)
	{
		xl_heap_freeze_page *xlrec = (xl_heap_freeze_page *) rec;

		appendStringInfo(buf, "snapshotConflictHorizon: %u, nplans: %u",
						 xlrec->snapshotConflictHorizon, xlrec->nplans);

		if (XLogRecHasBlockData(record, 0))
		{
			xl_heap_freeze_plan *plans;
			OffsetNumber *offsets;

			plans = (xl_heap_freeze_plan *) XLogRecGetBlockData(record, 0, NULL);
			offsets = (OffsetNumber *) ((char *) plans +
										(xlrec->nplans *
										 sizeof(xl_heap_freeze_plan)));
			appendStringInfoString(buf, ", plans:");
			array_desc(buf, plans, sizeof(xl_heap_freeze_plan), xlrec->nplans,
					   &plan_elem_desc, &offsets);
		}
	}
	else if (info == XLOG_HEAP2_VISIBLE)
	{
		xl_heap_visible *xlrec = (xl_heap_visible *) rec;

		appendStringInfo(buf, "snapshotConflictHorizon: %u, flags: 0x%02X",
						 xlrec->snapshotConflictHorizon, xlrec->flags);
	}
	else if (info == XLOG_HEAP2_MULTI_INSERT)
	{
		xl_heap_multi_insert *xlrec = (xl_heap_multi_insert *) rec;
		bool		isinit = (XLogRecGetInfo(record) & XLOG_HEAP_INIT_PAGE) != 0;

		appendStringInfo(buf, "ntuples: %d, flags: 0x%02X", xlrec->ntuples,
						 xlrec->flags);

		if (XLogRecHasBlockData(record, 0) && !isinit)
		{
			appendStringInfoString(buf, ", offsets:");
			array_desc(buf, xlrec->offsets, sizeof(OffsetNumber),
					   xlrec->ntuples, &offset_elem_desc, NULL);
		}
	}
	else if (info == XLOG_HEAP2_LOCK_UPDATED)
	{
		xl_heap_lock_updated *xlrec = (xl_heap_lock_updated *) rec;

		appendStringInfo(buf, "xmax: %u, off: %u, ",
						 xlrec->xmax, xlrec->offnum);
		infobits_desc(buf, xlrec->infobits_set, "infobits");
		appendStringInfo(buf, ", flags: 0x%02X", xlrec->flags);
	}
	else if (info == XLOG_HEAP2_NEW_CID)
	{
		xl_heap_new_cid *xlrec = (xl_heap_new_cid *) rec;

		appendStringInfo(buf, "rel: %u/%u/%u, tid: %u/%u",
						 xlrec->target_locator.spcOid,
						 xlrec->target_locator.dbOid,
						 xlrec->target_locator.relNumber,
						 ItemPointerGetBlockNumber(&(xlrec->target_tid)),
						 ItemPointerGetOffsetNumber(&(xlrec->target_tid)));
		appendStringInfo(buf, ", cmin: %u, cmax: %u, combo: %u",
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
		case XLOG_HEAP_TRUNCATE:
			id = "TRUNCATE";
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
		case XLOG_HEAP2_PRUNE:
			id = "PRUNE";
			break;
		case XLOG_HEAP2_VACUUM:
			id = "VACUUM";
			break;
		case XLOG_HEAP2_FREEZE_PAGE:
			id = "FREEZE_PAGE";
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
