/*-------------------------------------------------------------------------
 *
 * visibilitymap.h
 *		visibility map interface
 *
 *
 * Portions Copyright (c) 2007-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/access/visibilitymap.h,v 1.4.2.1 2009/08/24 02:18:40 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef VISIBILITYMAP_H
#define VISIBILITYMAP_H

#include "access/xlogdefs.h"
#include "storage/block.h"
#include "storage/buf.h"
#include "utils/relcache.h"

extern void visibilitymap_clear(Relation rel, BlockNumber heapBlk);
extern void visibilitymap_pin(Relation rel, BlockNumber heapBlk,
				  Buffer *vmbuf);
extern void visibilitymap_set(Relation rel, BlockNumber heapBlk,
				  XLogRecPtr recptr, Buffer *vmbuf);
extern bool visibilitymap_test(Relation rel, BlockNumber heapBlk, Buffer *vmbuf);
extern void visibilitymap_truncate(Relation rel, BlockNumber heapblk);

#endif   /* VISIBILITYMAP_H */
