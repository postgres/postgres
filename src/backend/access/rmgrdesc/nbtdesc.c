/*-------------------------------------------------------------------------
 *
 * nbtdesc.c
 *	  rmgr descriptor routines for access/nbtree/nbtxlog.c
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/nbtdesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/nbtree.h"

static void
out_target(StringInfo buf, xl_btreetid *target)
{
	appendStringInfo(buf, "rel %u/%u/%u; tid %u/%u",
			 target->node.spcNode, target->node.dbNode, target->node.relNode,
					 ItemPointerGetBlockNumber(&(target->tid)),
					 ItemPointerGetOffsetNumber(&(target->tid)));
}

void
btree_desc(StringInfo buf, uint8 xl_info, char *rec)
{
	uint8		info = xl_info & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_BTREE_INSERT_LEAF:
			{
				xl_btree_insert *xlrec = (xl_btree_insert *) rec;

				appendStringInfo(buf, "insert: ");
				out_target(buf, &(xlrec->target));
				break;
			}
		case XLOG_BTREE_INSERT_UPPER:
			{
				xl_btree_insert *xlrec = (xl_btree_insert *) rec;

				appendStringInfo(buf, "insert_upper: ");
				out_target(buf, &(xlrec->target));
				break;
			}
		case XLOG_BTREE_INSERT_META:
			{
				xl_btree_insert *xlrec = (xl_btree_insert *) rec;

				appendStringInfo(buf, "insert_meta: ");
				out_target(buf, &(xlrec->target));
				break;
			}
		case XLOG_BTREE_SPLIT_L:
			{
				xl_btree_split *xlrec = (xl_btree_split *) rec;

				appendStringInfo(buf, "split_l: rel %u/%u/%u ",
								 xlrec->node.spcNode, xlrec->node.dbNode,
								 xlrec->node.relNode);
				appendStringInfo(buf, "left %u, right %u, next %u, level %u, firstright %d",
							   xlrec->leftsib, xlrec->rightsib, xlrec->rnext,
								 xlrec->level, xlrec->firstright);
				break;
			}
		case XLOG_BTREE_SPLIT_R:
			{
				xl_btree_split *xlrec = (xl_btree_split *) rec;

				appendStringInfo(buf, "split_r: rel %u/%u/%u ",
								 xlrec->node.spcNode, xlrec->node.dbNode,
								 xlrec->node.relNode);
				appendStringInfo(buf, "left %u, right %u, next %u, level %u, firstright %d",
							   xlrec->leftsib, xlrec->rightsib, xlrec->rnext,
								 xlrec->level, xlrec->firstright);
				break;
			}
		case XLOG_BTREE_SPLIT_L_ROOT:
			{
				xl_btree_split *xlrec = (xl_btree_split *) rec;

				appendStringInfo(buf, "split_l_root: rel %u/%u/%u ",
								 xlrec->node.spcNode, xlrec->node.dbNode,
								 xlrec->node.relNode);
				appendStringInfo(buf, "left %u, right %u, next %u, level %u, firstright %d",
							   xlrec->leftsib, xlrec->rightsib, xlrec->rnext,
								 xlrec->level, xlrec->firstright);
				break;
			}
		case XLOG_BTREE_SPLIT_R_ROOT:
			{
				xl_btree_split *xlrec = (xl_btree_split *) rec;

				appendStringInfo(buf, "split_r_root: rel %u/%u/%u ",
								 xlrec->node.spcNode, xlrec->node.dbNode,
								 xlrec->node.relNode);
				appendStringInfo(buf, "left %u, right %u, next %u, level %u, firstright %d",
							   xlrec->leftsib, xlrec->rightsib, xlrec->rnext,
								 xlrec->level, xlrec->firstright);
				break;
			}
		case XLOG_BTREE_VACUUM:
			{
				xl_btree_vacuum *xlrec = (xl_btree_vacuum *) rec;

				appendStringInfo(buf, "vacuum: rel %u/%u/%u; blk %u, lastBlockVacuumed %u",
								 xlrec->node.spcNode, xlrec->node.dbNode,
								 xlrec->node.relNode, xlrec->block,
								 xlrec->lastBlockVacuumed);
				break;
			}
		case XLOG_BTREE_DELETE:
			{
				xl_btree_delete *xlrec = (xl_btree_delete *) rec;

				appendStringInfo(buf, "delete: index %u/%u/%u; iblk %u, heap %u/%u/%u;",
				xlrec->node.spcNode, xlrec->node.dbNode, xlrec->node.relNode,
								 xlrec->block,
								 xlrec->hnode.spcNode, xlrec->hnode.dbNode, xlrec->hnode.relNode);
				break;
			}
		case XLOG_BTREE_DELETE_PAGE:
		case XLOG_BTREE_DELETE_PAGE_META:
		case XLOG_BTREE_DELETE_PAGE_HALF:
			{
				xl_btree_delete_page *xlrec = (xl_btree_delete_page *) rec;

				appendStringInfo(buf, "delete_page: ");
				out_target(buf, &(xlrec->target));
				appendStringInfo(buf, "; dead %u; left %u; right %u",
							xlrec->deadblk, xlrec->leftblk, xlrec->rightblk);
				break;
			}
		case XLOG_BTREE_NEWROOT:
			{
				xl_btree_newroot *xlrec = (xl_btree_newroot *) rec;

				appendStringInfo(buf, "newroot: rel %u/%u/%u; root %u lev %u",
								 xlrec->node.spcNode, xlrec->node.dbNode,
								 xlrec->node.relNode,
								 xlrec->rootblk, xlrec->level);
				break;
			}
		case XLOG_BTREE_REUSE_PAGE:
			{
				xl_btree_reuse_page *xlrec = (xl_btree_reuse_page *) rec;

				appendStringInfo(buf, "reuse_page: rel %u/%u/%u; latestRemovedXid %u",
								 xlrec->node.spcNode, xlrec->node.dbNode,
							   xlrec->node.relNode, xlrec->latestRemovedXid);
				break;
			}
		default:
			appendStringInfo(buf, "UNKNOWN");
			break;
	}
}
