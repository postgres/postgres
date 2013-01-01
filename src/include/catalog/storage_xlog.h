/*-------------------------------------------------------------------------
 *
 * storage_xlog.h
 *	  prototypes for XLog support for backend/catalog/storage.c
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/storage_xlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef STORAGE_XLOG_H
#define STORAGE_XLOG_H

#include "access/xlog.h"
#include "storage/block.h"
#include "storage/relfilenode.h"

/*
 * Declarations for smgr-related XLOG records
 *
 * Note: we log file creation and truncation here, but logging of deletion
 * actions is handled by xact.c, because it is part of transaction commit.
 */

/* XLOG gives us high 4 bits */
#define XLOG_SMGR_CREATE	0x10
#define XLOG_SMGR_TRUNCATE	0x20

typedef struct xl_smgr_create
{
	RelFileNode rnode;
	ForkNumber	forkNum;
} xl_smgr_create;

typedef struct xl_smgr_truncate
{
	BlockNumber blkno;
	RelFileNode rnode;
} xl_smgr_truncate;

extern void log_smgrcreate(RelFileNode *rnode, ForkNumber forkNum);

extern void smgr_redo(XLogRecPtr lsn, XLogRecord *record);
extern void smgr_desc(StringInfo buf, uint8 xl_info, char *rec);

#endif   /* STORAGE_XLOG_H */
