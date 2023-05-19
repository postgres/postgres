/*-------------------------------------------------------------------------
 *
 * gindesc.c
 *	  rmgr descriptor routines for access/transam/gin/ginxlog.c
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/gindesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/ginxlog.h"
#include "access/xlogutils.h"
#include "lib/stringinfo.h"
#include "storage/relfilelocator.h"

static void
desc_recompress_leaf(StringInfo buf, ginxlogRecompressDataLeaf *insertData)
{
	int			i;
	char	   *walbuf = ((char *) insertData) + sizeof(ginxlogRecompressDataLeaf);

	appendStringInfo(buf, " %d segments:", (int) insertData->nactions);

	for (i = 0; i < insertData->nactions; i++)
	{
		uint8		a_segno = *((uint8 *) (walbuf++));
		uint8		a_action = *((uint8 *) (walbuf++));
		uint16		nitems = 0;
		int			newsegsize = 0;

		if (a_action == GIN_SEGMENT_INSERT ||
			a_action == GIN_SEGMENT_REPLACE)
		{
			newsegsize = SizeOfGinPostingList((GinPostingList *) walbuf);
			walbuf += SHORTALIGN(newsegsize);
		}

		if (a_action == GIN_SEGMENT_ADDITEMS)
		{
			memcpy(&nitems, walbuf, sizeof(uint16));
			walbuf += sizeof(uint16);
			walbuf += nitems * sizeof(ItemPointerData);
		}

		switch (a_action)
		{
			case GIN_SEGMENT_ADDITEMS:
				appendStringInfo(buf, " %d (add %d items)", a_segno, nitems);
				break;
			case GIN_SEGMENT_DELETE:
				appendStringInfo(buf, " %d (delete)", a_segno);
				break;
			case GIN_SEGMENT_INSERT:
				appendStringInfo(buf, " %d (insert)", a_segno);
				break;
			case GIN_SEGMENT_REPLACE:
				appendStringInfo(buf, " %d (replace)", a_segno);
				break;
			default:
				appendStringInfo(buf, " %d unknown action %d ???", a_segno, a_action);
				/* cannot decode unrecognized actions further */
				return;
		}
	}
}

void
gin_desc(StringInfo buf, XLogReaderState *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_GIN_CREATE_PTREE:
			/* no further information */
			break;
		case XLOG_GIN_INSERT:
			{
				ginxlogInsert *xlrec = (ginxlogInsert *) rec;

				appendStringInfo(buf, "isdata: %c isleaf: %c",
								 (xlrec->flags & GIN_INSERT_ISDATA) ? 'T' : 'F',
								 (xlrec->flags & GIN_INSERT_ISLEAF) ? 'T' : 'F');
				if (!(xlrec->flags & GIN_INSERT_ISLEAF))
				{
					char	   *payload = rec + sizeof(ginxlogInsert);
					BlockNumber leftChildBlkno;
					BlockNumber rightChildBlkno;

					leftChildBlkno = BlockIdGetBlockNumber((BlockId) payload);
					payload += sizeof(BlockIdData);
					rightChildBlkno = BlockIdGetBlockNumber((BlockId) payload);
					payload += sizeof(BlockNumber);
					appendStringInfo(buf, " children: %u/%u",
									 leftChildBlkno, rightChildBlkno);
				}
				if (XLogRecHasBlockImage(record, 0))
				{
					if (XLogRecBlockImageApply(record, 0))
						appendStringInfoString(buf, " (full page image)");
					else
						appendStringInfoString(buf, " (full page image, for WAL verification)");
				}
				else
				{
					char	   *payload = XLogRecGetBlockData(record, 0, NULL);

					if (!(xlrec->flags & GIN_INSERT_ISDATA))
						appendStringInfo(buf, " isdelete: %c",
										 (((ginxlogInsertEntry *) payload)->isDelete) ? 'T' : 'F');
					else if (xlrec->flags & GIN_INSERT_ISLEAF)
						desc_recompress_leaf(buf, (ginxlogRecompressDataLeaf *) payload);
					else
					{
						ginxlogInsertDataInternal *insertData =
							(ginxlogInsertDataInternal *) payload;

						appendStringInfo(buf, " pitem: %u-%u/%u",
										 PostingItemGetBlockNumber(&insertData->newitem),
										 ItemPointerGetBlockNumber(&insertData->newitem.key),
										 ItemPointerGetOffsetNumber(&insertData->newitem.key));
					}
				}
			}
			break;
		case XLOG_GIN_SPLIT:
			{
				ginxlogSplit *xlrec = (ginxlogSplit *) rec;

				appendStringInfo(buf, "isrootsplit: %c",
								 (((ginxlogSplit *) rec)->flags & GIN_SPLIT_ROOT) ? 'T' : 'F');
				appendStringInfo(buf, " isdata: %c isleaf: %c",
								 (xlrec->flags & GIN_INSERT_ISDATA) ? 'T' : 'F',
								 (xlrec->flags & GIN_INSERT_ISLEAF) ? 'T' : 'F');
			}
			break;
		case XLOG_GIN_VACUUM_PAGE:
			/* no further information */
			break;
		case XLOG_GIN_VACUUM_DATA_LEAF_PAGE:
			{
				if (XLogRecHasBlockImage(record, 0))
				{
					if (XLogRecBlockImageApply(record, 0))
						appendStringInfoString(buf, " (full page image)");
					else
						appendStringInfoString(buf, " (full page image, for WAL verification)");
				}
				else
				{
					ginxlogVacuumDataLeafPage *xlrec =
						(ginxlogVacuumDataLeafPage *) XLogRecGetBlockData(record, 0, NULL);

					desc_recompress_leaf(buf, &xlrec->data);
				}
			}
			break;
		case XLOG_GIN_DELETE_PAGE:
			/* no further information */
			break;
		case XLOG_GIN_UPDATE_META_PAGE:
			/* no further information */
			break;
		case XLOG_GIN_INSERT_LISTPAGE:
			/* no further information */
			break;
		case XLOG_GIN_DELETE_LISTPAGE:
			appendStringInfo(buf, "ndeleted: %d",
							 ((ginxlogDeleteListPages *) rec)->ndeleted);
			break;
	}
}

const char *
gin_identify(uint8 info)
{
	const char *id = NULL;

	switch (info & ~XLR_INFO_MASK)
	{
		case XLOG_GIN_CREATE_PTREE:
			id = "CREATE_PTREE";
			break;
		case XLOG_GIN_INSERT:
			id = "INSERT";
			break;
		case XLOG_GIN_SPLIT:
			id = "SPLIT";
			break;
		case XLOG_GIN_VACUUM_PAGE:
			id = "VACUUM_PAGE";
			break;
		case XLOG_GIN_VACUUM_DATA_LEAF_PAGE:
			id = "VACUUM_DATA_LEAF_PAGE";
			break;
		case XLOG_GIN_DELETE_PAGE:
			id = "DELETE_PAGE";
			break;
		case XLOG_GIN_UPDATE_META_PAGE:
			id = "UPDATE_META_PAGE";
			break;
		case XLOG_GIN_INSERT_LISTPAGE:
			id = "INSERT_LISTPAGE";
			break;
		case XLOG_GIN_DELETE_LISTPAGE:
			id = "DELETE_LISTPAGE";
			break;
	}

	return id;
}
