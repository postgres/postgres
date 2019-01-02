/*-------------------------------------------------------------------------
 *
 * logicalworker.h
 *	  Exports for logical replication workers.
 *
 * Portions Copyright (c) 2016-2019, PostgreSQL Global Development Group
 *
 * src/include/replication/logicalworker.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LOGICALWORKER_H
#define LOGICALWORKER_H

extern void ApplyWorkerMain(Datum main_arg);

extern bool IsLogicalWorker(void);

#endif							/* LOGICALWORKER_H */
