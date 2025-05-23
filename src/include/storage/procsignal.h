/*-------------------------------------------------------------------------
 *
 * procsignal.h
 *	  Routines for interprocess signaling
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/procsignal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROCSIGNAL_H
#define PROCSIGNAL_H

#include "storage/procnumber.h"


/*
 * Reasons for signaling a Postgres child process (a backend or an auxiliary
 * process, like checkpointer).  We can cope with concurrent signals for different
 * reasons.  However, if the same reason is signaled multiple times in quick
 * succession, the process is likely to observe only one notification of it.
 * This is okay for the present uses.
 *
 * Also, because of race conditions, it's important that all the signals be
 * defined so that no harm is done if a process mistakenly receives one.
 */
typedef enum
{
	PROCSIG_CATCHUP_INTERRUPT,	/* sinval catchup interrupt */
	PROCSIG_NOTIFY_INTERRUPT,	/* listen/notify interrupt */
	PROCSIG_PARALLEL_MESSAGE,	/* message from cooperating parallel backend */
	PROCSIG_WALSND_INIT_STOPPING,	/* ask walsenders to prepare for shutdown  */
	PROCSIG_BARRIER,			/* global barrier interrupt  */
	PROCSIG_LOG_MEMORY_CONTEXT, /* ask backend to log the memory contexts */
	PROCSIG_PARALLEL_APPLY_MESSAGE, /* Message from parallel apply workers */

	/* Recovery conflict reasons */
	PROCSIG_RECOVERY_CONFLICT_FIRST,
	PROCSIG_RECOVERY_CONFLICT_DATABASE = PROCSIG_RECOVERY_CONFLICT_FIRST,
	PROCSIG_RECOVERY_CONFLICT_TABLESPACE,
	PROCSIG_RECOVERY_CONFLICT_LOCK,
	PROCSIG_RECOVERY_CONFLICT_SNAPSHOT,
	PROCSIG_RECOVERY_CONFLICT_LOGICALSLOT,
	PROCSIG_RECOVERY_CONFLICT_BUFFERPIN,
	PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK,
	PROCSIG_RECOVERY_CONFLICT_LAST = PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK,
} ProcSignalReason;

#define NUM_PROCSIGNALS (PROCSIG_RECOVERY_CONFLICT_LAST + 1)

typedef enum
{
	PROCSIGNAL_BARRIER_SMGRRELEASE, /* ask smgr to close files */
} ProcSignalBarrierType;

/*
 * Length of query cancel keys generated.
 *
 * Note that the protocol allows for longer keys, or shorter, but this is the
 * length we actually generate.  Client code, and the server code that handles
 * incoming cancellation packets from clients, mustn't use this hardcoded
 * length.
 */
#define MAX_CANCEL_KEY_LENGTH  32

/*
 * prototypes for functions in procsignal.c
 */
extern Size ProcSignalShmemSize(void);
extern void ProcSignalShmemInit(void);

extern void ProcSignalInit(const uint8 *cancel_key, int cancel_key_len);
extern int	SendProcSignal(pid_t pid, ProcSignalReason reason,
						   ProcNumber procNumber);
extern void SendCancelRequest(int backendPID, const uint8 *cancel_key, int cancel_key_len);

extern uint64 EmitProcSignalBarrier(ProcSignalBarrierType type);
extern void WaitForProcSignalBarrier(uint64 generation);
extern void ProcessProcSignalBarrier(void);

extern void procsignal_sigusr1_handler(SIGNAL_ARGS);

/* ProcSignalHeader is an opaque struct, details known only within procsignal.c */
typedef struct ProcSignalHeader ProcSignalHeader;

#ifdef EXEC_BACKEND
extern PGDLLIMPORT ProcSignalHeader *ProcSignal;
#endif

#endif							/* PROCSIGNAL_H */
