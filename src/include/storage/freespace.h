/*-------------------------------------------------------------------------
 *
 * freespace.h
 *	  POSTGRES free space map for quickly finding free space in relations
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: freespace.h,v 1.13 2003/08/08 21:42:51 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef FREESPACE_H_
#define FREESPACE_H_

#include "storage/block.h"
#include "storage/relfilenode.h"


/*
 * exported types
 */
typedef struct PageFreeSpaceInfo
{
	BlockNumber blkno;			/* which page in relation */
	Size		avail;			/* space available on this page */
} PageFreeSpaceInfo;


/* GUC variables */
extern int	MaxFSMRelations;
extern int	MaxFSMPages;


/*
 * function prototypes
 */
extern void InitFreeSpaceMap(void);
extern int	FreeSpaceShmemSize(void);

extern BlockNumber GetPageWithFreeSpace(RelFileNode *rel, Size spaceNeeded);
extern BlockNumber RecordAndGetPageWithFreeSpace(RelFileNode *rel,
							  BlockNumber oldPage,
							  Size oldSpaceAvail,
							  Size spaceNeeded);
extern Size GetAvgFSMRequestSize(RelFileNode *rel);
extern void RecordRelationFreeSpace(RelFileNode *rel,
						int nPages,
						PageFreeSpaceInfo *pageSpaces);

extern BlockNumber GetFreeIndexPage(RelFileNode *rel);
extern void RecordIndexFreeSpace(RelFileNode *rel,
					 int nPages,
					 BlockNumber *pages);

extern void FreeSpaceMapTruncateRel(RelFileNode *rel, BlockNumber nblocks);
extern void FreeSpaceMapForgetRel(RelFileNode *rel);
extern void FreeSpaceMapForgetDatabase(Oid dbid);

extern void PrintFreeSpaceMapStatistics(int elevel);

extern void DumpFreeSpaceMap(void);
extern void LoadFreeSpaceMap(void);

#ifdef FREESPACE_DEBUG
extern void DumpFreeSpace(void);
#endif

#endif   /* FREESPACE_H */
