/*-------------------------------------------------------------------------
 *
 * waitlsn.h
 *	  Declarations for LSN waiting routines.
 *
 * Copyright (c) 2024, PostgreSQL Global Development Group
 *
 * src/include/commands/waitlsn.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WAIT_LSN_H
#define WAIT_LSN_H

#include "postgres.h"
#include "port/atomics.h"
#include "storage/spin.h"
#include "tcop/dest.h"

/* Shared memory structures */
typedef struct WaitLSNProcInfo
{
	int			procnum;
	XLogRecPtr	waitLSN;
} WaitLSNProcInfo;

typedef struct WaitLSNState
{
	pg_atomic_uint64 minLSN;
	slock_t		mutex;
	int			numWaitedProcs;
	WaitLSNProcInfo procInfos[FLEXIBLE_ARRAY_MEMBER];
} WaitLSNState;

extern PGDLLIMPORT struct WaitLSNState *waitLSN;

extern void WaitForLSN(XLogRecPtr targetLSN, int64 timeout);
extern Size WaitLSNShmemSize(void);
extern void WaitLSNShmemInit(void);
extern void WaitLSNSetLatches(XLogRecPtr currentLSN);
extern void WaitLSNCleanup(void);

#endif							/* WAIT_LSN_H */
