/*-------------------------------------------------------------------------
 *
 * spgdesc.c
 *	  rmgr descriptor routines for access/spgist/spgxlog.c
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/spgdesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/spgxlog.h"

void
spg_desc(StringInfo buf, XLogReaderState *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_SPGIST_ADD_LEAF:
			{
				spgxlogAddLeaf *xlrec = (spgxlogAddLeaf *) rec;

				appendStringInfoString(buf, "add leaf to page");
				appendStringInfo(buf, "; off %u; headoff %u; parentoff %u",
								 xlrec->offnumLeaf, xlrec->offnumHeadLeaf,
								 xlrec->offnumParent);
				if (xlrec->newPage)
					appendStringInfoString(buf, " (newpage)");
				if (xlrec->storesNulls)
					appendStringInfoString(buf, " (nulls)");
			}
			break;
		case XLOG_SPGIST_MOVE_LEAFS:
			appendStringInfo(buf, "%u leafs",
							 ((spgxlogMoveLeafs *) rec)->nMoves);
			break;
		case XLOG_SPGIST_ADD_NODE:
			appendStringInfo(buf, "off %u",
							 ((spgxlogAddNode *) rec)->offnum);
			break;
		case XLOG_SPGIST_SPLIT_TUPLE:
			appendStringInfo(buf, "prefix off: %u, postfix off: %u (same %d, new %d)",
							 ((spgxlogSplitTuple *) rec)->offnumPrefix,
							 ((spgxlogSplitTuple *) rec)->offnumPostfix,
							 ((spgxlogSplitTuple *) rec)->postfixBlkSame,
							 ((spgxlogSplitTuple *) rec)->newPage
				);
			break;
		case XLOG_SPGIST_PICKSPLIT:
			{
				spgxlogPickSplit *xlrec = (spgxlogPickSplit *) rec;

				appendStringInfo(buf, "ndel %u; nins %u",
								 xlrec->nDelete, xlrec->nInsert);
				if (xlrec->innerIsParent)
					appendStringInfoString(buf, " (innerIsParent)");
				if (xlrec->isRootSplit)
					appendStringInfoString(buf, " (isRootSplit)");
			}
			break;
		case XLOG_SPGIST_VACUUM_LEAF:
			/* no further information */
			break;
		case XLOG_SPGIST_VACUUM_ROOT:
			/* no further information */
			break;
		case XLOG_SPGIST_VACUUM_REDIRECT:
			appendStringInfo(buf, "newest XID %u",
							 ((spgxlogVacuumRedirect *) rec)->newestRedirectXid);
			break;
	}
}

const char *
spg_identify(uint8 info)
{
	const char *id = NULL;

	switch (info & ~XLR_INFO_MASK)
	{
		case XLOG_SPGIST_ADD_LEAF:
			id = "ADD_LEAF";
			break;
		case XLOG_SPGIST_MOVE_LEAFS:
			id = "MOVE_LEAFS";
			break;
		case XLOG_SPGIST_ADD_NODE:
			id = "ADD_NODE";
			break;
		case XLOG_SPGIST_SPLIT_TUPLE:
			id = "SPLIT_TUPLE";
			break;
		case XLOG_SPGIST_PICKSPLIT:
			id = "PICKSPLIT";
			break;
		case XLOG_SPGIST_VACUUM_LEAF:
			id = "VACUUM_LEAF";
			break;
		case XLOG_SPGIST_VACUUM_ROOT:
			id = "VACUUM_ROOT";
			break;
		case XLOG_SPGIST_VACUUM_REDIRECT:
			id = "VACUUM_REDIRECT";
			break;
	}

	return id;
}
