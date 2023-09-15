/*-------------------------------------------------------------------------
 *
 * pg_tde_visibilitymap.h
 *		visibility map interface
 *
 *
 * Portions Copyright (c) 2007-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/pg_tde_visibilitymap.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TDE_VISIBILITYMAP_H
#define PG_TDE_VISIBILITYMAP_H

#include "access/visibilitymapdefs.h"
#include "access/xlogdefs.h"
#include "storage/block.h"
#include "storage/buf.h"
#include "utils/relcache.h"

/* Macros for pg_tde_visibilitymap test */
#define VM_ALL_VISIBLE(r, b, v) \
	((pg_tde_visibilitymap_get_status((r), (b), (v)) & VISIBILITYMAP_ALL_VISIBLE) != 0)
#define VM_ALL_FROZEN(r, b, v) \
	((pg_tde_visibilitymap_get_status((r), (b), (v)) & VISIBILITYMAP_ALL_FROZEN) != 0)

extern bool pg_tde_visibilitymap_clear(Relation rel, BlockNumber heapBlk,
								Buffer vmbuf, uint8 flags);
extern void pg_tde_visibilitymap_pin(Relation rel, BlockNumber heapBlk,
							  Buffer *vmbuf);
extern bool pg_tde_visibilitymap_pin_ok(BlockNumber heapBlk, Buffer vmbuf);
extern void pg_tde_visibilitymap_set(Relation rel, BlockNumber heapBlk, Buffer heapBuf,
							  XLogRecPtr recptr, Buffer vmBuf, TransactionId cutoff_xid,
							  uint8 flags);
extern uint8 pg_tde_visibilitymap_get_status(Relation rel, BlockNumber heapBlk, Buffer *vmbuf);
extern void pg_tde_visibilitymap_count(Relation rel, BlockNumber *all_visible, BlockNumber *all_frozen);
extern BlockNumber pg_tde_visibilitymap_prepare_truncate(Relation rel,
												  BlockNumber nheapblocks);

#endif							/* PG_TDE_VISIBILITYMAP_H */
