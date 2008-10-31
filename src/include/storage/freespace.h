/*-------------------------------------------------------------------------
 *
 * freespace.h
 *	  POSTGRES free space map for quickly finding free space in relations
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/storage/freespace.h,v 1.30 2008/10/31 19:40:27 heikki Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef FREESPACE_H_
#define FREESPACE_H_

#include "utils/rel.h"
#include "storage/bufpage.h"
#include "access/xlog.h"

/* prototypes for public functions in freespace.c */
extern Size GetRecordedFreeSpace(Relation rel, BlockNumber heapBlk);
extern BlockNumber GetPageWithFreeSpace(Relation rel, Size spaceNeeded);
extern BlockNumber RecordAndGetPageWithFreeSpace(Relation rel,
							  BlockNumber oldPage,
							  Size oldSpaceAvail,
							  Size spaceNeeded);
extern void RecordPageWithFreeSpace(Relation rel, BlockNumber heapBlk,
									Size spaceAvail);
extern void XLogRecordPageWithFreeSpace(RelFileNode rnode, BlockNumber heapBlk,
										Size spaceAvail);

extern void FreeSpaceMapTruncateRel(Relation rel, BlockNumber nblocks);
extern void FreeSpaceMapVacuum(Relation rel);

/* WAL prototypes */
extern void fsm_desc(StringInfo buf, uint8 xl_info, char *rec);
extern void fsm_redo(XLogRecPtr lsn, XLogRecord *record);

#endif   /* FREESPACE_H */
