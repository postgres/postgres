/*-------------------------------------------------------------------------
 *
 * waitlsn.h
 *	  Declarations for LSN replay waiting routines.
 *
 * Copyright (c) 2024, PostgreSQL Global Development Group
 *
 * src/include/commands/waitlsn.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WAIT_LSN_H
#define WAIT_LSN_H

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

extern PGDLLIMPORT WaitLSNState *waitLSNState;

extern Size WaitLSNShmemSize(void);
extern void WaitLSNShmemInit(void);
extern void WaitLSNSetLatches(XLogRecPtr currentLSN);
extern void WaitLSNCleanup(void);

#endif							/* WAIT_LSN_H */
