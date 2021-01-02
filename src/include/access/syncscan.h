/*-------------------------------------------------------------------------
 *
 * syncscan.h
 *    POSTGRES synchronous scan support functions.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/syncscan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SYNCSCAN_H
#define SYNCSCAN_H

#include "storage/block.h"
#include "utils/relcache.h"

extern void ss_report_location(Relation rel, BlockNumber location);
extern BlockNumber ss_get_location(Relation rel, BlockNumber relnblocks);
extern void SyncScanShmemInit(void);
extern Size SyncScanShmemSize(void);

#endif
