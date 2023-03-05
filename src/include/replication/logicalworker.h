/*-------------------------------------------------------------------------
 *
 * logicalworker.h
 *	  Exports for logical replication workers.
 *
 * Portions Copyright (c) 2016-2023, PostgreSQL Global Development Group
 *
 * src/include/replication/logicalworker.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LOGICALWORKER_H
#define LOGICALWORKER_H

#include <signal.h>

extern PGDLLIMPORT volatile sig_atomic_t ParallelApplyMessagePending;

extern void ApplyWorkerMain(Datum main_arg);
extern void ParallelApplyWorkerMain(Datum main_arg);

extern bool IsLogicalWorker(void);
extern bool IsLogicalParallelApplyWorker(void);

extern void HandleParallelApplyMessageInterrupt(void);
extern void HandleParallelApplyMessages(void);

extern void LogicalRepWorkersWakeupAtCommit(Oid subid);

extern void AtEOXact_LogicalRepWorkers(bool isCommit);

#endif							/* LOGICALWORKER_H */
