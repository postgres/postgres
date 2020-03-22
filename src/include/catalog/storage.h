/*-------------------------------------------------------------------------
 *
 * storage.h
 *	  prototypes for functions in backend/catalog/storage.c
 *
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/storage.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef STORAGE_H
#define STORAGE_H

#include "storage/block.h"
#include "storage/relfilenode.h"
#include "utils/relcache.h"

extern void RelationCreateStorage(RelFileNode rnode, char relpersistence);
extern void RelationDropStorage(Relation rel);
extern void RelationPreserveStorage(RelFileNode rnode, bool atCommit);
extern void RelationTruncate(Relation rel, BlockNumber nblocks);

/*
 * These functions used to be in storage/smgr/smgr.c, which explains the
 * naming
 */
extern void smgrDoPendingDeletes(bool isCommit);
extern int	smgrGetPendingDeletes(bool forCommit, RelFileNode **ptr);
extern void AtSubCommit_smgr(void);
extern void AtSubAbort_smgr(void);
extern void PostPrepare_smgr(void);

#endif   /* STORAGE_H */
