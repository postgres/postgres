/*-------------------------------------------------------------------------
 *
 * syncrep.h
 *	  Exports from replication/syncrep.c.
 *
 * Portions Copyright (c) 2010-2010, PostgreSQL Global Development Group
 *
 * $PostgreSQL$
 *
 *-------------------------------------------------------------------------
 */
#ifndef _SYNCREP_H
#define _SYNCREP_H

#include "access/xlog.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "utils/guc.h"

#define SyncRepRequested()				(sync_rep_mode)

/* syncRepState */
#define SYNC_REP_NOT_WAITING		0
#define SYNC_REP_WAITING			1
#define SYNC_REP_WAIT_COMPLETE		2
#define SYNC_REP_MUST_DISCONNECT	3

/* user-settable parameters for synchronous replication */
extern bool sync_rep_mode;
extern int 	sync_rep_timeout;
extern char *SyncRepStandbyNames;

/* called by user backend */
extern void SyncRepWaitForLSN(XLogRecPtr XactCommitLSN);

/* callback at backend exit */
extern void SyncRepCleanupAtProcExit(int code, Datum arg);

/* called by wal sender */
extern void SyncRepInitConfig(void);
extern void SyncRepReleaseWaiters(void);

/* called by various procs */
extern int SyncRepWakeQueue(bool all);
const char *assign_synchronous_standby_names(const char *newval, bool doit, GucSource source);

#endif   /* _SYNCREP_H */
