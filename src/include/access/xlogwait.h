/*-------------------------------------------------------------------------
 *
 * xlogwait.h
 *	  Declarations for LSN replay waiting routines.
 *
 * Copyright (c) 2024, PostgreSQL Global Development Group
 *
 * src/include/access/xlogwait.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef XLOG_WAIT_H
#define XLOG_WAIT_H

#include "lib/pairingheap.h"
#include "postgres.h"
#include "port/atomics.h"
#include "storage/latch.h"
#include "storage/spin.h"
#include "tcop/dest.h"

/*
 * WaitLSNProcInfo - the shared memory structure representing information
 * about the single process, which may wait for LSN replay.  An item of
 * waitLSN->procInfos array.
 */
typedef struct WaitLSNProcInfo
{
	/* LSN, which this process is waiting for */
	XLogRecPtr	waitLSN;

	/*
	 * A pointer to the latch, which should be set once the waitLSN is
	 * replayed.
	 */
	Latch	   *latch;

	/* A pairing heap node for participation in waitLSNState->waitersHeap */
	pairingheap_node phNode;

	/*
	 * A flag indicating that this item is present in
	 * waitLSNState->waitersHeap
	 */
	bool		inHeap;
} WaitLSNProcInfo;

/*
 * WaitLSNState - the shared memory state for the replay LSN waiting facility.
 */
typedef struct WaitLSNState
{
	/*
	 * The minimum LSN value some process is waiting for.  Used for the
	 * fast-path checking if we need to wake up any waiters after replaying a
	 * WAL record.  Could be read lock-less.  Update protected by WaitLSNLock.
	 */
	pg_atomic_uint64 minWaitedLSN;

	/*
	 * A pairing heap of waiting processes order by LSN values (least LSN is
	 * on top).  Protected by WaitLSNLock.
	 */
	pairingheap waitersHeap;

	/*
	 * An array with per-process information, indexed by the process number.
	 * Protected by WaitLSNLock.
	 */
	WaitLSNProcInfo procInfos[FLEXIBLE_ARRAY_MEMBER];
} WaitLSNState;

/*
 * Result statuses for WaitForLSNReplay().
 */
typedef enum
{
	WAIT_LSN_RESULT_SUCCESS,	/* Target LSN is reached */
	WAIT_LSN_RESULT_TIMEOUT,	/* Timeout occurred */
	WAIT_LSN_RESULT_NOT_IN_RECOVERY,	/* Recovery ended before or during our
										 * wait */
} WaitLSNResult;

extern PGDLLIMPORT WaitLSNState *waitLSNState;

extern Size WaitLSNShmemSize(void);
extern void WaitLSNShmemInit(void);
extern void WaitLSNSetLatches(XLogRecPtr currentLSN);
extern void WaitLSNCleanup(void);
extern WaitLSNResult WaitForLSNReplay(XLogRecPtr targetLSN, int64 timeout);

#endif							/* XLOG_WAIT_H */
