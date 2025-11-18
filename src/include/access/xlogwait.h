/*-------------------------------------------------------------------------
 *
 * xlogwait.h
 *	  Declarations for LSN replay waiting routines.
 *
 * Copyright (c) 2025, PostgreSQL Global Development Group
 *
 * src/include/access/xlogwait.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef XLOG_WAIT_H
#define XLOG_WAIT_H

#include "access/xlogdefs.h"
#include "lib/pairingheap.h"
#include "port/atomics.h"
#include "storage/procnumber.h"
#include "storage/spin.h"
#include "tcop/dest.h"

/*
 * Result statuses for WaitForLSN().
 */
typedef enum
{
	WAIT_LSN_RESULT_SUCCESS,	/* Target LSN is reached */
	WAIT_LSN_RESULT_NOT_IN_RECOVERY,	/* Recovery ended before or during our
										 * wait */
	WAIT_LSN_RESULT_TIMEOUT		/* Timeout occurred */
} WaitLSNResult;

/*
 * LSN type for waiting facility.
 */
typedef enum WaitLSNType
{
	WAIT_LSN_TYPE_REPLAY = 0,	/* Waiting for replay on standby */
	WAIT_LSN_TYPE_FLUSH = 1,	/* Waiting for flush on primary */
	WAIT_LSN_TYPE_COUNT = 2
} WaitLSNType;

/*
 * WaitLSNProcInfo - the shared memory structure representing information
 * about the single process, which may wait for LSN operations.  An item of
 * waitLSNState->procInfos array.
 */
typedef struct WaitLSNProcInfo
{
	/* LSN, which this process is waiting for */
	XLogRecPtr	waitLSN;

	/* The type of LSN to wait */
	WaitLSNType lsnType;

	/* Process to wake up once the waitLSN is reached */
	ProcNumber	procno;

	/*
	 * Heap membership flag.  A process can wait for only one LSN type at a
	 * time, so a single flag suffices (tracked by the lsnType field).
	 */
	bool		inHeap;

	/* Pairing heap node for the waiters' heap (one per process) */
	pairingheap_node heapNode;
} WaitLSNProcInfo;

/*
 * WaitLSNState - the shared memory state for the LSN waiting facility.
 */
typedef struct WaitLSNState
{
	/*
	 * The minimum LSN values some process is waiting for.  Used for the
	 * fast-path checking if we need to wake up any waiters after replaying a
	 * WAL record.  Could be read lock-less.  Update protected by WaitLSNLock.
	 */
	pg_atomic_uint64 minWaitedLSN[WAIT_LSN_TYPE_COUNT];

	/*
	 * A pairing heaps of waiting processes ordered by LSN values (least LSN
	 * is on top).  Protected by WaitLSNLock.
	 */
	pairingheap waitersHeap[WAIT_LSN_TYPE_COUNT];

	/*
	 * An array with per-process information, indexed by the process number.
	 * Protected by WaitLSNLock.
	 */
	WaitLSNProcInfo procInfos[FLEXIBLE_ARRAY_MEMBER];
} WaitLSNState;


extern PGDLLIMPORT WaitLSNState *waitLSNState;

extern Size WaitLSNShmemSize(void);
extern void WaitLSNShmemInit(void);
extern void WaitLSNWakeup(WaitLSNType lsnType, XLogRecPtr currentLSN);
extern void WaitLSNCleanup(void);
extern WaitLSNResult WaitForLSN(WaitLSNType lsnType, XLogRecPtr targetLSN,
								int64 timeout);

#endif							/* XLOG_WAIT_H */
