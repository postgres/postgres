/*
 * commit_ts.h
 *
 * PostgreSQL commit timestamp manager
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/commit_ts.h
 */
#ifndef COMMIT_TS_H
#define COMMIT_TS_H

#include "access/xlog.h"
#include "datatype/timestamp.h"
#include "utils/guc.h"


extern PGDLLIMPORT bool	track_commit_timestamp;

extern bool check_track_commit_timestamp(bool *newval, void **extra,
							 GucSource source);

typedef uint32 CommitTsNodeId;
#define InvalidCommitTsNodeId 0

extern void CommitTsSetDefaultNodeId(CommitTsNodeId nodeid);
extern CommitTsNodeId CommitTsGetDefaultNodeId(void);
extern void TransactionTreeSetCommitTsData(TransactionId xid, int nsubxids,
							   TransactionId *subxids, TimestampTz timestamp,
							   CommitTsNodeId nodeid, bool do_xlog);
extern bool TransactionIdGetCommitTsData(TransactionId xid,
							 TimestampTz *ts, CommitTsNodeId *nodeid);
extern TransactionId GetLatestCommitTsData(TimestampTz *ts,
					  CommitTsNodeId *nodeid);

extern Size CommitTsShmemBuffers(void);
extern Size CommitTsShmemSize(void);
extern void CommitTsShmemInit(void);
extern void BootStrapCommitTs(void);
extern void StartupCommitTs(void);
extern void CompleteCommitTsInitialization(void);
extern void ShutdownCommitTs(void);
extern void CheckPointCommitTs(void);
extern void ExtendCommitTs(TransactionId newestXact);
extern void TruncateCommitTs(TransactionId oldestXact);
extern void SetCommitTsLimit(TransactionId oldestXact,
				 TransactionId newestXact);
extern void AdvanceOldestCommitTs(TransactionId oldestXact);

/* XLOG stuff */
#define COMMIT_TS_ZEROPAGE		0x00
#define COMMIT_TS_TRUNCATE		0x10
#define COMMIT_TS_SETTS			0x20

typedef struct xl_commit_ts_set
{
	TimestampTz		timestamp;
	CommitTsNodeId	nodeid;
	TransactionId	mainxid;
	/* subxact Xids follow */
} xl_commit_ts_set;

#define SizeOfCommitTsSet	(offsetof(xl_commit_ts_set, mainxid) + \
							 sizeof(TransactionId))


extern void commit_ts_redo(XLogReaderState *record);
extern void commit_ts_desc(StringInfo buf, XLogReaderState *record);
extern const char *commit_ts_identify(uint8 info);

#endif   /* COMMITTS_H */
