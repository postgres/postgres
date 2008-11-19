/*-------------------------------------------------------------------------
 *
 * indexfsm.h
 *	  POSTGRES free space map for quickly finding an unused page in index
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/storage/indexfsm.h,v 1.3 2008/11/19 10:34:52 heikki Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef INDEXFSM_H_
#define INDEXFSM_H_

#include "utils/rel.h"

extern BlockNumber GetFreeIndexPage(Relation rel);
extern void RecordFreeIndexPage(Relation rel, BlockNumber page);
extern void RecordUsedIndexPage(Relation rel, BlockNumber page);

extern void IndexFreeSpaceMapVacuum(Relation rel);

#endif   /* INDEXFSM_H */
