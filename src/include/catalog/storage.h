/*-------------------------------------------------------------------------
 *
 * storage.h
 *	  prototypes for functions in backend/catalog/storage.c
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/catalog/storage.h,v 1.5 2010/02/07 20:48:13 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef STORAGE_H
#define STORAGE_H

#include "access/xlog.h"
#include "lib/stringinfo.h"
#include "storage/block.h"
#include "storage/relfilenode.h"
#include "utils/relcache.h"

extern void RelationCreateStorage(RelFileNode rnode, bool istemp);
extern void RelationDropStorage(Relation rel);
extern void RelationPreserveStorage(RelFileNode rnode);
extern void RelationTruncate(Relation rel, BlockNumber nblocks);

/*
 * These functions used to be in storage/smgr/smgr.c, which explains the
 * naming
 */
extern void smgrDoPendingDeletes(bool isCommit);
extern int smgrGetPendingDeletes(bool forCommit, RelFileNode **ptr,
					  bool *haveNonTemp);
extern void AtSubCommit_smgr(void);
extern void AtSubAbort_smgr(void);
extern void PostPrepare_smgr(void);

extern void smgr_redo(XLogRecPtr lsn, XLogRecord *record);
extern void smgr_desc(StringInfo buf, uint8 xl_info, char *rec);

#endif   /* STORAGE_H */
