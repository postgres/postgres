/*-------------------------------------------------------------------------
 *
 * spgdesc.c
 *	  rmgr descriptor routines for access/spgist/spgxlog.c
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
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

				appendStringInfo(buf, "off: %u, headoff: %u, parentoff: %u, nodeI: %u",
								 xlrec->offnumLeaf, xlrec->offnumHeadLeaf,
								 xlrec->offnumParent, xlrec->nodeI);
				if (xlrec->newPage)
					appendStringInfoString(buf, " (newpage)");
				if (xlrec->storesNulls)
					appendStringInfoString(buf, " (nulls)");
			}
			break;
		case XLOG_SPGIST_MOVE_LEAFS:
			{
				spgxlogMoveLeafs *xlrec = (spgxlogMoveLeafs *) rec;

				appendStringInfo(buf, "nmoves: %u, parentoff: %u, nodeI: %u",
								 xlrec->nMoves,
								 xlrec->offnumParent, xlrec->nodeI);
				if (xlrec->newPage)
					appendStringInfoString(buf, " (newpage)");
				if (xlrec->replaceDead)
					appendStringInfoString(buf, " (replacedead)");
				if (xlrec->storesNulls)
					appendStringInfoString(buf, " (nulls)");
			}
			break;
		case XLOG_SPGIST_ADD_NODE:
			{
				spgxlogAddNode *xlrec = (spgxlogAddNode *) rec;

				appendStringInfo(buf, "off: %u, newoff: %u, parentBlk: %d, "
								 "parentoff: %u, nodeI: %u",
								 xlrec->offnum,
								 xlrec->offnumNew,
								 xlrec->parentBlk,
								 xlrec->offnumParent,
								 xlrec->nodeI);
				if (xlrec->newPage)
					appendStringInfoString(buf, " (newpage)");
			}
			break;
		case XLOG_SPGIST_SPLIT_TUPLE:
			{
				spgxlogSplitTuple *xlrec = (spgxlogSplitTuple *) rec;

				appendStringInfo(buf, "prefixoff: %u, postfixoff: %u",
								 xlrec->offnumPrefix,
								 xlrec->offnumPostfix);
				if (xlrec->newPage)
					appendStringInfoString(buf, " (newpage)");
				if (xlrec->postfixBlkSame)
					appendStringInfoString(buf, " (same)");
			}
			break;
		case XLOG_SPGIST_PICKSPLIT:
			{
				spgxlogPickSplit *xlrec = (spgxlogPickSplit *) rec;

				appendStringInfo(buf, "ndelete: %u, ninsert: %u, inneroff: %u, "
								 "parentoff: %u, nodeI: %u",
								 xlrec->nDelete, xlrec->nInsert,
								 xlrec->offnumInner,
								 xlrec->offnumParent, xlrec->nodeI);
				if (xlrec->innerIsParent)
					appendStringInfoString(buf, " (innerIsParent)");
				if (xlrec->storesNulls)
					appendStringInfoString(buf, " (nulls)");
				if (xlrec->isRootSplit)
					appendStringInfoString(buf, " (isRootSplit)");
			}
			break;
		case XLOG_SPGIST_VACUUM_LEAF:
			{
				spgxlogVacuumLeaf *xlrec = (spgxlogVacuumLeaf *) rec;

				appendStringInfo(buf, "ndead: %u, nplaceholder: %u, nmove: %u, nchain: %u",
								 xlrec->nDead, xlrec->nPlaceholder,
								 xlrec->nMove, xlrec->nChain);
			}
			break;
		case XLOG_SPGIST_VACUUM_ROOT:
			{
				spgxlogVacuumRoot *xlrec = (spgxlogVacuumRoot *) rec;

				appendStringInfo(buf, "ndelete: %u",
								 xlrec->nDelete);
			}
			break;
		case XLOG_SPGIST_VACUUM_REDIRECT:
			{
				spgxlogVacuumRedirect *xlrec = (spgxlogVacuumRedirect *) rec;

				appendStringInfo(buf, "ntoplaceholder: %u, firstplaceholder: %u, newestredirectxid: %u",
								 xlrec->nToPlaceholder,
								 xlrec->firstPlaceholder,
								 xlrec->newestRedirectXid);
			}
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
