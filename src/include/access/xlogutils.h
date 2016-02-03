/*
 * xlogutils.h
 *
 * Utilities for replaying WAL records.
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/xlogutils.h
 */
#ifndef XLOG_UTILS_H
#define XLOG_UTILS_H

#include "access/xlogreader.h"
#include "storage/bufmgr.h"


extern bool XLogHaveInvalidPages(void);
extern void XLogCheckInvalidPages(void);

extern void XLogDropRelation(RelFileNode rnode, ForkNumber forknum);
extern void XLogDropDatabase(Oid dbid);
extern void XLogTruncateRelation(RelFileNode rnode, ForkNumber forkNum,
					 BlockNumber nblocks);

/* Result codes for XLogReadBufferForRedo[Extended] */
typedef enum
{
	BLK_NEEDS_REDO,				/* changes from WAL record need to be applied */
	BLK_DONE,					/* block is already up-to-date */
	BLK_RESTORED,				/* block was restored from a full-page image */
	BLK_NOTFOUND				/* block was not found (and hence does not
								 * need to be replayed) */
} XLogRedoAction;

extern XLogRedoAction XLogReadBufferForRedo(XLogReaderState *record,
					  uint8 buffer_id, Buffer *buf);
extern Buffer XLogInitBufferForRedo(XLogReaderState *record, uint8 block_id);
extern XLogRedoAction XLogReadBufferForRedoExtended(XLogReaderState *record,
							  uint8 buffer_id,
							  ReadBufferMode mode, bool get_cleanup_lock,
							  Buffer *buf);

extern Buffer XLogReadBufferExtended(RelFileNode rnode, ForkNumber forknum,
					   BlockNumber blkno, ReadBufferMode mode);

extern Relation CreateFakeRelcacheEntry(RelFileNode rnode);
extern void FreeFakeRelcacheEntry(Relation fakerel);

extern int read_local_xlog_page(XLogReaderState *state, XLogRecPtr targetPagePtr,
	int reqLen, XLogRecPtr targetRecPtr, char *cur_page, TimeLineID *pageTLI);

#endif
