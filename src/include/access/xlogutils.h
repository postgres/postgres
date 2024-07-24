/*
 * xlogutils.h
 *
 * Utilities for replaying WAL records.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/xlogutils.h
 */
#ifndef XLOG_UTILS_H
#define XLOG_UTILS_H

#include "access/xlogreader.h"
#include "storage/bufmgr.h"

/* GUC variable */
extern PGDLLIMPORT bool ignore_invalid_pages;

/*
 * Prior to 8.4, all activity during recovery was carried out by the startup
 * process. This local variable continues to be used in many parts of the
 * code to indicate actions taken by RecoveryManagers. Other processes that
 * potentially perform work during recovery should check RecoveryInProgress().
 * See XLogCtl notes in xlog.c.
 */
extern PGDLLIMPORT bool InRecovery;

/*
 * Like InRecovery, standbyState is only valid in the startup process.
 * In all other processes it will have the value STANDBY_DISABLED (so
 * InHotStandby will read as false).
 *
 * In DISABLED state, we're performing crash recovery or hot standby was
 * disabled in postgresql.conf.
 *
 * In INITIALIZED state, we've run InitRecoveryTransactionEnvironment, but
 * we haven't yet processed a RUNNING_XACTS or shutdown-checkpoint WAL record
 * to initialize our primary-transaction tracking system.
 *
 * When the transaction tracking is initialized, we enter the SNAPSHOT_PENDING
 * state. The tracked information might still be incomplete, so we can't allow
 * connections yet, but redo functions must update the in-memory state when
 * appropriate.
 *
 * In SNAPSHOT_READY mode, we have full knowledge of transactions that are
 * (or were) running on the primary at the current WAL location. Snapshots
 * can be taken, and read-only queries can be run.
 */
typedef enum
{
	STANDBY_DISABLED,
	STANDBY_INITIALIZED,
	STANDBY_SNAPSHOT_PENDING,
	STANDBY_SNAPSHOT_READY,
} HotStandbyState;

extern PGDLLIMPORT HotStandbyState standbyState;

#define InHotStandby (standbyState >= STANDBY_SNAPSHOT_PENDING)


extern bool XLogHaveInvalidPages(void);
extern void XLogCheckInvalidPages(void);

extern void XLogDropRelation(RelFileLocator rlocator, ForkNumber forknum);
extern void XLogDropDatabase(Oid dbid);
extern void XLogTruncateRelation(RelFileLocator rlocator, ForkNumber forkNum,
								 BlockNumber nblocks);

/* Result codes for XLogReadBufferForRedo[Extended] */
typedef enum
{
	BLK_NEEDS_REDO,				/* changes from WAL record need to be applied */
	BLK_DONE,					/* block is already up-to-date */
	BLK_RESTORED,				/* block was restored from a full-page image */
	BLK_NOTFOUND,				/* block was not found (and hence does not
								 * need to be replayed) */
} XLogRedoAction;

/* Private data of the read_local_xlog_page_no_wait callback. */
typedef struct ReadLocalXLogPageNoWaitPrivate
{
	bool		end_of_wal;		/* true, when end of WAL is reached */
} ReadLocalXLogPageNoWaitPrivate;

extern XLogRedoAction XLogReadBufferForRedo(XLogReaderState *record,
											uint8 block_id, Buffer *buf);
extern Buffer XLogInitBufferForRedo(XLogReaderState *record, uint8 block_id);
extern XLogRedoAction XLogReadBufferForRedoExtended(XLogReaderState *record,
													uint8 block_id,
													ReadBufferMode mode, bool get_cleanup_lock,
													Buffer *buf);

extern Buffer XLogReadBufferExtended(RelFileLocator rlocator, ForkNumber forknum,
									 BlockNumber blkno, ReadBufferMode mode,
									 Buffer recent_buffer);

extern Relation CreateFakeRelcacheEntry(RelFileLocator rlocator);
extern void FreeFakeRelcacheEntry(Relation fakerel);

extern int	read_local_xlog_page(XLogReaderState *state,
								 XLogRecPtr targetPagePtr, int reqLen,
								 XLogRecPtr targetRecPtr, char *cur_page);
extern int	read_local_xlog_page_no_wait(XLogReaderState *state,
										 XLogRecPtr targetPagePtr, int reqLen,
										 XLogRecPtr targetRecPtr,
										 char *cur_page);
extern void wal_segment_open(XLogReaderState *state,
							 XLogSegNo nextSegNo,
							 TimeLineID *tli_p);
extern void wal_segment_close(XLogReaderState *state);

extern void XLogReadDetermineTimeline(XLogReaderState *state,
									  XLogRecPtr wantPage,
									  uint32 wantLength,
									  TimeLineID currTLI);

extern void WALReadRaiseError(WALReadError *errinfo);

#endif
