/*-------------------------------------------------------------------------
 *
 * hashdesc.c
 *	  rmgr descriptor routines for access/hash/hash.c
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/hashdesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash_xlog.h"

void
hash_desc(StringInfo buf, XLogReaderState *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_HASH_INIT_META_PAGE:
			{
				xl_hash_init_meta_page *xlrec = (xl_hash_init_meta_page *) rec;

				appendStringInfo(buf, "num_tuples %g, fillfactor %d",
								 xlrec->num_tuples, xlrec->ffactor);
				break;
			}
		case XLOG_HASH_INIT_BITMAP_PAGE:
			{
				xl_hash_init_bitmap_page *xlrec = (xl_hash_init_bitmap_page *) rec;

				appendStringInfo(buf, "bmsize %d", xlrec->bmsize);
				break;
			}
		case XLOG_HASH_INSERT:
			{
				xl_hash_insert *xlrec = (xl_hash_insert *) rec;

				appendStringInfo(buf, "off %u", xlrec->offnum);
				break;
			}
		case XLOG_HASH_ADD_OVFL_PAGE:
			{
				xl_hash_add_ovfl_page *xlrec = (xl_hash_add_ovfl_page *) rec;

				appendStringInfo(buf, "bmsize %d, bmpage_found %c",
								 xlrec->bmsize, (xlrec->bmpage_found) ? 'T' : 'F');
				break;
			}
		case XLOG_HASH_SPLIT_ALLOCATE_PAGE:
			{
				xl_hash_split_allocate_page *xlrec = (xl_hash_split_allocate_page *) rec;

				appendStringInfo(buf, "new_bucket %u, meta_page_masks_updated %c, issplitpoint_changed %c",
								 xlrec->new_bucket,
								 (xlrec->flags & XLH_SPLIT_META_UPDATE_MASKS) ? 'T' : 'F',
								 (xlrec->flags & XLH_SPLIT_META_UPDATE_SPLITPOINT) ? 'T' : 'F');
				break;
			}
		case XLOG_HASH_SPLIT_COMPLETE:
			{
				xl_hash_split_complete *xlrec = (xl_hash_split_complete *) rec;

				appendStringInfo(buf, "old_bucket_flag %u, new_bucket_flag %u",
								 xlrec->old_bucket_flag, xlrec->new_bucket_flag);
				break;
			}
		case XLOG_HASH_MOVE_PAGE_CONTENTS:
			{
				xl_hash_move_page_contents *xlrec = (xl_hash_move_page_contents *) rec;

				appendStringInfo(buf, "ntups %d, is_primary %c",
								 xlrec->ntups,
								 xlrec->is_prim_bucket_same_wrt ? 'T' : 'F');
				break;
			}
		case XLOG_HASH_SQUEEZE_PAGE:
			{
				xl_hash_squeeze_page *xlrec = (xl_hash_squeeze_page *) rec;

				appendStringInfo(buf, "prevblkno %u, nextblkno %u, ntups %d, is_primary %c",
								 xlrec->prevblkno,
								 xlrec->nextblkno,
								 xlrec->ntups,
								 xlrec->is_prim_bucket_same_wrt ? 'T' : 'F');
				break;
			}
		case XLOG_HASH_DELETE:
			{
				xl_hash_delete *xlrec = (xl_hash_delete *) rec;

				appendStringInfo(buf, "clear_dead_marking %c, is_primary %c",
								 xlrec->clear_dead_marking ? 'T' : 'F',
								 xlrec->is_primary_bucket_page ? 'T' : 'F');
				break;
			}
		case XLOG_HASH_UPDATE_META_PAGE:
			{
				xl_hash_update_meta_page *xlrec = (xl_hash_update_meta_page *) rec;

				appendStringInfo(buf, "ntuples %g",
								 xlrec->ntuples);
				break;
			}
		case XLOG_HASH_VACUUM_ONE_PAGE:
			{
				xl_hash_vacuum_one_page *xlrec = (xl_hash_vacuum_one_page *) rec;

				appendStringInfo(buf, "ntuples %d, latest removed xid %u",
								 xlrec->ntuples,
								 xlrec->latestRemovedXid);
				break;
			}
	}
}

const char *
hash_identify(uint8 info)
{
	const char *id = NULL;

	switch (info & ~XLR_INFO_MASK)
	{
		case XLOG_HASH_INIT_META_PAGE:
			id = "INIT_META_PAGE";
			break;
		case XLOG_HASH_INIT_BITMAP_PAGE:
			id = "INIT_BITMAP_PAGE";
			break;
		case XLOG_HASH_INSERT:
			id = "INSERT";
			break;
		case XLOG_HASH_ADD_OVFL_PAGE:
			id = "ADD_OVFL_PAGE";
			break;
		case XLOG_HASH_SPLIT_ALLOCATE_PAGE:
			id = "SPLIT_ALLOCATE_PAGE";
			break;
		case XLOG_HASH_SPLIT_PAGE:
			id = "SPLIT_PAGE";
			break;
		case XLOG_HASH_SPLIT_COMPLETE:
			id = "SPLIT_COMPLETE";
			break;
		case XLOG_HASH_MOVE_PAGE_CONTENTS:
			id = "MOVE_PAGE_CONTENTS";
			break;
		case XLOG_HASH_SQUEEZE_PAGE:
			id = "SQUEEZE_PAGE";
			break;
		case XLOG_HASH_DELETE:
			id = "DELETE";
			break;
		case XLOG_HASH_SPLIT_CLEANUP:
			id = "SPLIT_CLEANUP";
			break;
		case XLOG_HASH_UPDATE_META_PAGE:
			id = "UPDATE_META_PAGE";
			break;
		case XLOG_HASH_VACUUM_ONE_PAGE:
			id = "VACUUM_ONE_PAGE";
	}

	return id;
}
