/*-------------------------------------------------------------------------
 *
 * freespace.h
 *	  POSTGRES free space map for quickly finding free space in relations
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: freespace.h,v 1.8 2002/09/20 19:56:01 tgl Exp $
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
	BlockNumber		blkno;		/* which page in relation */
	Size			avail;		/* space available on this page */
} PageFreeSpaceInfo;


extern int	MaxFSMRelations;
extern int	MaxFSMPages;


/*
 * function prototypes
 */
extern void InitFreeSpaceMap(void);
extern int	FreeSpaceShmemSize(void);

extern BlockNumber GetPageWithFreeSpace(RelFileNode *rel, Size spaceNeeded);
extern void RecordFreeSpace(RelFileNode *rel, BlockNumber page,
				Size spaceAvail);
extern BlockNumber RecordAndGetPageWithFreeSpace(RelFileNode *rel,
							  BlockNumber oldPage,
							  Size oldSpaceAvail,
							  Size spaceNeeded);
extern void MultiRecordFreeSpace(RelFileNode *rel,
					 BlockNumber minPage,
					 int nPages,
					 PageFreeSpaceInfo *pageSpaces);
extern void FreeSpaceMapForgetRel(RelFileNode *rel);
extern void FreeSpaceMapForgetDatabase(Oid dbid);

#ifdef FREESPACE_DEBUG
extern void DumpFreeSpace(void);
#endif

#endif   /* FREESPACE_H */
