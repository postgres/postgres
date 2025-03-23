/*-------------------------------------------------------------------------
 *
 * csn_snapshot.h
 *	  Support for cross-node snapshot isolation.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/csn_snapshot.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CSN_SNAPSHOT_H
#define CSN_SNAPSHOT_H

#include "access/csn_log.h"
#include "port/atomics.h"
#include "storage/lock.h"
#include "utils/snapshot.h"
#include "utils/guc.h"

/*
 * snapshot.h is used in frontend code so atomic variant of SnapshotCSN type
 * is defined here.
 */
typedef pg_atomic_uint64 CSN_atomic;


extern int csn_snapshot_defer_time;
extern int csn_time_shift;


extern Size CSNSnapshotShmemSize(void);
extern void CSNSnapshotShmemInit(void);
extern void CSNSnapshotStartup(TransactionId oldestActiveXID);

extern void CSNSnapshotMapXmin(SnapshotCSN snapshot_csn);
extern TransactionId CSNSnapshotToXmin(SnapshotCSN snapshot_csn);

extern bool XidInCSNSnapshot(TransactionId xid, Snapshot snapshot);

extern CSN TransactionIdGetCSN(TransactionId xid);

extern void CSNSnapshotAbort(PGPROC *proc, TransactionId xid, int nsubxids,
								TransactionId *subxids);
extern void CSNSnapshotPrecommit(PGPROC *proc, TransactionId xid, int nsubxids,
									TransactionId *subxids);
extern void CSNSnapshotCommit(PGPROC *proc, TransactionId xid, int nsubxids,
									TransactionId *subxids);
extern void CSNSnapshotAssignCurrent(SnapshotCSN snapshot_csn);
extern SnapshotCSN CSNSnapshotPrepareCurrent(void);
extern void CSNSnapshotSync(SnapshotCSN remote_csn);

#endif							/* CSN_SNAPSHOT_H */
