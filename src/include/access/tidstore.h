/*-------------------------------------------------------------------------
 *
 * tidstore.h
 *	  TidStore interface.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/tidstore.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TIDSTORE_H
#define TIDSTORE_H

#include "storage/itemptr.h"
#include "utils/dsa.h"

typedef struct TidStore TidStore;
typedef struct TidStoreIter TidStoreIter;

/*
 * Result struct for TidStoreIterateNext.  This is copyable, but should be
 * treated as opaque.  Call TidStoreGetBlockOffsets() to obtain the offsets.
 */
typedef struct TidStoreIterResult
{
	BlockNumber blkno;
	void	   *internal_page;
} TidStoreIterResult;

extern TidStore *TidStoreCreateLocal(size_t max_bytes, bool insert_only);
extern TidStore *TidStoreCreateShared(size_t max_bytes, int tranche_id);
extern TidStore *TidStoreAttach(dsa_handle area_handle, dsa_pointer handle);
extern void TidStoreDetach(TidStore *ts);
extern void TidStoreLockExclusive(TidStore *ts);
extern void TidStoreLockShare(TidStore *ts);
extern void TidStoreUnlock(TidStore *ts);
extern void TidStoreDestroy(TidStore *ts);
extern void TidStoreSetBlockOffsets(TidStore *ts, BlockNumber blkno, OffsetNumber *offsets,
									int num_offsets);
extern bool TidStoreIsMember(TidStore *ts, ItemPointer tid);
extern TidStoreIter *TidStoreBeginIterate(TidStore *ts);
extern TidStoreIterResult *TidStoreIterateNext(TidStoreIter *iter);
extern int	TidStoreGetBlockOffsets(TidStoreIterResult *result,
									OffsetNumber *offsets,
									int max_offsets);
extern void TidStoreEndIterate(TidStoreIter *iter);
extern size_t TidStoreMemoryUsage(TidStore *ts);
extern dsa_pointer TidStoreGetHandle(TidStore *ts);
extern dsa_area *TidStoreGetDSA(TidStore *ts);

#endif							/* TIDSTORE_H */
