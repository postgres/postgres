/*-------------------------------------------------------------------------
 *
 * indexfsm.c
 *	  POSTGRES free space map for quickly finding free pages in relations
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/freespace/indexfsm.c,v 1.2 2008/10/06 08:04:11 heikki Exp $
 *
 *
 * NOTES:
 *
 *  This is similar to the FSM used for heap, in freespace.c, but instead
 *  of tracking the amount of free space on pages, we only track whether
 *  pages are completely free or in-use. We use the same FSM implementation
 *  as for heaps, using BLCKSZ - 1 to denote used pages, and 0 for unused.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/freespace.h"
#include "storage/indexfsm.h"
#include "storage/smgr.h"

/*
 * Exported routines
 */

/*
 * InitIndexFreeSpaceMap - Create or reset the FSM fork for relation.
 */
void
InitIndexFreeSpaceMap(Relation rel)
{
	/* Create FSM fork if it doesn't exist yet, or truncate it if it does */
	RelationOpenSmgr(rel);
	if (!smgrexists(rel->rd_smgr, FSM_FORKNUM))
		smgrcreate(rel->rd_smgr, FSM_FORKNUM, rel->rd_istemp, false);
	else
		smgrtruncate(rel->rd_smgr, FSM_FORKNUM, 0, rel->rd_istemp);
}

/*
 * GetFreeIndexPage - return a free page from the FSM
 *
 * As a side effect, the page is marked as used in the FSM.
 */
BlockNumber
GetFreeIndexPage(Relation rel)
{
	BlockNumber blkno = GetPageWithFreeSpace(rel, BLCKSZ/2);

	if (blkno != InvalidBlockNumber)
		RecordUsedIndexPage(rel, blkno);

	return blkno;
}

/*
 * RecordFreeIndexPage - mark a page as free in the FSM
 */
void
RecordFreeIndexPage(Relation rel, BlockNumber freeBlock)
{
	RecordPageWithFreeSpace(rel, freeBlock, BLCKSZ - 1);
}


/*
 * RecordUsedIndexPage - mark a page as used in the FSM
 */
void
RecordUsedIndexPage(Relation rel, BlockNumber usedBlock)
{
	RecordPageWithFreeSpace(rel, usedBlock, 0);
}

/*
 * IndexFreeSpaceMapTruncate - adjust for truncation of a relation.
 *
 * We need to delete any stored data past the new relation length, so that
 * we don't bogusly return removed block numbers.
 */
void
IndexFreeSpaceMapTruncate(Relation rel, BlockNumber nblocks)
{
	FreeSpaceMapTruncateRel(rel, nblocks);
}

/*
 * IndexFreeSpaceMapVacuum - scan and fix any inconsistencies in the FSM
 */
void
IndexFreeSpaceMapVacuum(Relation rel)
{
	FreeSpaceMapVacuum(rel);
}
