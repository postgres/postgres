/*-------------------------------------------------------------------------
 *
 * spgdesc.c
 *	  rmgr descriptor routines for access/spgist/spgxlog.c
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/spgdesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/spgist_private.h"

static void
out_target(StringInfo buf, RelFileNode node)
{
	appendStringInfo(buf, "rel %u/%u/%u ",
					 node.spcNode, node.dbNode, node.relNode);
}

void
spg_desc(StringInfo buf, uint8 xl_info, char *rec)
{
	uint8		info = xl_info & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_SPGIST_CREATE_INDEX:
			appendStringInfo(buf, "create_index: rel %u/%u/%u",
							 ((RelFileNode *) rec)->spcNode,
							 ((RelFileNode *) rec)->dbNode,
							 ((RelFileNode *) rec)->relNode);
			break;
		case XLOG_SPGIST_ADD_LEAF:
			out_target(buf, ((spgxlogAddLeaf *) rec)->node);
			appendStringInfo(buf, "add leaf to page: %u",
							 ((spgxlogAddLeaf *) rec)->blknoLeaf);
			break;
		case XLOG_SPGIST_MOVE_LEAFS:
			out_target(buf, ((spgxlogMoveLeafs *) rec)->node);
			appendStringInfo(buf, "move %u leafs from page %u to page %u",
							 ((spgxlogMoveLeafs *) rec)->nMoves,
							 ((spgxlogMoveLeafs *) rec)->blknoSrc,
							 ((spgxlogMoveLeafs *) rec)->blknoDst);
			break;
		case XLOG_SPGIST_ADD_NODE:
			out_target(buf, ((spgxlogAddNode *) rec)->node);
			appendStringInfo(buf, "add node to %u:%u",
							 ((spgxlogAddNode *) rec)->blkno,
							 ((spgxlogAddNode *) rec)->offnum);
			break;
		case XLOG_SPGIST_SPLIT_TUPLE:
			out_target(buf, ((spgxlogSplitTuple *) rec)->node);
			appendStringInfo(buf, "split node %u:%u to %u:%u",
							 ((spgxlogSplitTuple *) rec)->blknoPrefix,
							 ((spgxlogSplitTuple *) rec)->offnumPrefix,
							 ((spgxlogSplitTuple *) rec)->blknoPostfix,
							 ((spgxlogSplitTuple *) rec)->offnumPostfix);
			break;
		case XLOG_SPGIST_PICKSPLIT:
			out_target(buf, ((spgxlogPickSplit *) rec)->node);
			appendStringInfo(buf, "split leaf page");
			break;
		case XLOG_SPGIST_VACUUM_LEAF:
			out_target(buf, ((spgxlogVacuumLeaf *) rec)->node);
			appendStringInfo(buf, "vacuum leaf tuples on page %u",
							 ((spgxlogVacuumLeaf *) rec)->blkno);
			break;
		case XLOG_SPGIST_VACUUM_ROOT:
			out_target(buf, ((spgxlogVacuumRoot *) rec)->node);
			appendStringInfo(buf, "vacuum leaf tuples on root page %u",
							 ((spgxlogVacuumRoot *) rec)->blkno);
			break;
		case XLOG_SPGIST_VACUUM_REDIRECT:
			out_target(buf, ((spgxlogVacuumRedirect *) rec)->node);
			appendStringInfo(buf, "vacuum redirect tuples on page %u, newest XID %u",
							 ((spgxlogVacuumRedirect *) rec)->blkno,
						 ((spgxlogVacuumRedirect *) rec)->newestRedirectXid);
			break;
		default:
			appendStringInfo(buf, "unknown spgist op code %u", info);
			break;
	}
}
