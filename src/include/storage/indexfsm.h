/*-------------------------------------------------------------------------
 *
 * indexfsm.h
 *	  POSTGRES free space map for quickly finding an unused page in index
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/indexfsm.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef INDEXFSM_H_
#define INDEXFSM_H_

#include "storage/block.h"
#include "utils/relcache.h"

extern BlockNumber GetFreeIndexPage(Relation rel);
extern void RecordFreeIndexPage(Relation rel, BlockNumber freeBlock);
extern void RecordUsedIndexPage(Relation rel, BlockNumber usedBlock);

extern void IndexFreeSpaceMapVacuum(Relation rel);

#endif							/* INDEXFSM_H_ */
