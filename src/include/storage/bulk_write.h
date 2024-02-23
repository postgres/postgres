/*-------------------------------------------------------------------------
 *
 * bulk_write.h
 *	  Efficiently and reliably populate a new relation
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/bulk_write.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BULK_WRITE_H
#define BULK_WRITE_H

#include "storage/smgr.h"
#include "utils/rel.h"

/* Bulk writer state, contents are private to bulk_write.c */
typedef struct BulkWriteState BulkWriteState;

/*
 * Temporary buffer to hold a page to until it's written out. Use
 * smgr_bulk_get_buf() to reserve one of these.  This is a separate typedef to
 * distinguish it from other block-sized buffers passed around in the system.
 */
typedef PGIOAlignedBlock *BulkWriteBuffer;

/* forward declared from smgr.h */
struct SMgrRelationData;

extern BulkWriteState *smgr_bulk_start_rel(Relation rel, ForkNumber forknum);
extern BulkWriteState *smgr_bulk_start_smgr(struct SMgrRelationData *smgr, ForkNumber forknum, bool use_wal);

extern BulkWriteBuffer smgr_bulk_get_buf(BulkWriteState *bulkstate);
extern void smgr_bulk_write(BulkWriteState *bulkstate, BlockNumber blocknum, BulkWriteBuffer buf, bool page_std);

extern void smgr_bulk_finish(BulkWriteState *bulkstate);

#endif							/* BULK_WRITE_H */
