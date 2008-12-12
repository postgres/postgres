/*-------------------------------------------------------------------------
 *
 * indexfsm.h
 *	  POSTGRES free space map for quickly finding an unused page in index
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/storage/indexfsm.h,v 1.4 2008/12/12 22:56:00 alvherre Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef INDEXFSM_H_
#define INDEXFSM_H_

#include "storage/block.h"
#include "utils/relcache.h"

extern BlockNumber GetFreeIndexPage(Relation rel);
extern void RecordFreeIndexPage(Relation rel, BlockNumber page);
extern void RecordUsedIndexPage(Relation rel, BlockNumber page);

extern void IndexFreeSpaceMapVacuum(Relation rel);

#endif   /* INDEXFSM_H_ */
