/*-------------------------------------------------------------------------
 *
 * indexfsm.h
 *	  POSTGRES free space map for quickly finding an unused page in index
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/storage/indexfsm.h,v 1.1 2008/09/30 10:52:14 heikki Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef INDEXFSM_H_
#define INDEXFSM_H_

#include "utils/rel.h"

extern void InitIndexFreeSpaceMap(Relation rel);

extern BlockNumber GetFreeIndexPage(Relation rel);
extern void RecordFreeIndexPage(Relation rel, BlockNumber page);
extern void RecordUsedIndexPage(Relation rel, BlockNumber page);

extern void IndexFreeSpaceMapTruncate(Relation rel, BlockNumber nblocks);

#endif   /* INDEXFSM_H */
