/*-------------------------------------------------------------------------
 *
 * procsignal.c
 *	  Routines for interprocess signalling
 *
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/procsignal.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <unistd.h>

#include "access/parallel.h"
#include "commands/async.h"
#include "miscadmin.h"
#include "storage/latch.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "storage/sinval.h"
#include "tcop/tcopprot.h"


/*
 * The SIGUSR1 signal is multiplexed to support signalling multiple event
 * types. The specific reason is communicated via flags in shared memory.
 * We keep a boolean flag for each possible "reason", so that different
 * reasons can be signaled to a process concurrently.  (However, if the same
 * reason is signaled more than once nearly simultaneously, the process may
 * observe it only once.)
 *
 * Each process that wants to receive signals registers its process ID
 * in the ProcSignalSlots array. The array is indexed by backend ID to make
 * slot allocation simple, and to avoid having to search the array when you
 * know the backend ID of the process you're signalling.  (We do support
 * signalling without backend ID, but it's a bit less efficient.)
 *
 * The flags are actually declared as "volatile sig_atomic_t" for maximum
 * portability.  This should ensure that loads and stores of the flag
 * values are atomic, allowing us to dispense with any explicit locking.
 */
typedef struct
{
	pid_t		pss_pid;
	sig_atomic_t pss_signalFlags[NUM_PROCSIGNALS];
} ProcSignalSlot;

/*
 * We reserve a slot for each possible BackendId, plus one for each
 * possible auxiliary process type.  (This scheme assumes there is not
 * more than one of any auxiliary process type at a time.)
 */
#define NumProcSignalSlots	(MaxBackends + NUM_AUXPROCTYPES)

/*
 * If this flag is set, the process latch will be set whenever SIGUSR1
 * is received.  This is useful when waiting for a signal from the postmaster.
 * Spurious wakeups must be expected.  Make sure that the flag is cleared
 * in the error path.
 */
bool		set_latch_on_sigusr1;

static ProcSignalSlot *ProcSignalSlots = NULL;
static volatile ProcSignalSlot *MyProcSignalSlot = NULL;

static bool CheckProcSignal(ProcSignalReason reason);
static void CleanupProcSignalState(int status, Datum arg);

/*
 * ProcSignalShmemSize
 *		Compute space needed for procsignal's shared memory
 */
Size
ProcSignalShmemSize(void)
{
	return NumProcSignalSlots * sizeof(ProcSignalSlot);
}

/*
 * ProcSignalShmemInit
 *		Allocate and initialize procsignal's shared memory
 */
void
ProcSignalShmemInit(void)
{
	Size		size = ProcSignalShmemSize();
	bool		found;

	ProcSignalSlots = (ProcSignalSlot *)
		ShmemInitStruct("ProcSignalSlots", size, &found);

	/* If we're first, set everything to zeroes */
	if (!found)
		MemSet(ProcSignalSlots, 0, size);
}

/*
 * ProcSignalInit
 *		Register the current process in the procsignal array
 *
 * The passed index should be my BackendId if the process has one,
 * or MaxBackends + aux process type if not.
 */
void
ProcSignalInit(int pss_idx)
{
	volatile ProcSignalSlot *slot;

	Assert(pss_idx >= 1 && pss_idx <= NumProcSignalSlots);

	slot = &ProcSignalSlots[pss_idx - 1];

	/* sanity check */
	if (slot->pss_pid != 0)
		elog(LOG, "process %d taking over ProcSignal slot %d, but it's not empty",
			 MyProcPid, pss_idx);

	/* Clear out any leftover signal reasons */
	MemSet(slot->pss_signalFlags, 0, NUM_PROCSIGNALS * sizeof(sig_atomic_t));

	/* Mark slot with my PID */
	slot->pss_pid = MyProcPid;

	/* Remember slot location for CheckProcSignal */
	MyProcSignalSlot = slot;

	/* Set up to release the slot on process exit */
	on_shmem_exit(CleanupProcSignalState, Int32GetDatum(pss_idx));
}

/*
 * CleanupProcSignalState
 *		Remove current process from ProcSignalSlots
 *
 * This function is called via on_shmem_exit() during backend shutdown.
 */
static void
CleanupProcSignalState(int status, Datum arg)
{
	int			pss_idx = DatumGetInt32(arg);
	volatile ProcSignalSlot *slot;

	slot = &ProcSignalSlots[pss_idx - 1];
	Assert(slot == MyProcSignalSlot);

	/*
	 * Clear MyProcSignalSlot, so that a SIGUSR1 received after this point
	 * won't try to access it after it's no longer ours (and perhaps even
	 * after we've unmapped the shared memory segment).
	 */
	MyProcSignalSlot = NULL;

	/* sanity check */
	if (slot->pss_pid != MyProcPid)
	{
		/*
		 * don't ERROR here. We're exiting anyway, and don't want to get into
		 * infinite loop trying to exit
		 */
		elog(LOG, "process %d releasing ProcSignal slot %d, but it contains %d",
			 MyProcPid, pss_idx, (int) slot->pss_pid);
		return;					/* XXX better to zero the slot anyway? */
	}

	slot->pss_pid = 0;
}

/*
 * SendProcSignal
 *		Send a signal to a Postgres process
 *
 * Providing backendId is optional, but it will speed up the operation.
 *
 * On success (a signal was sent), zero is returned.
 * On error, -1 is returned, and errno is set (typically to ESRCH or EPERM).
 *
 * Not to be confused with ProcSendSignal
 */
int
SendProcSignal(pid_t pid, ProcSignalReason reason, BackendId backendId)
{
	volatile ProcSignalSlot *slot;

	if (backendId != InvalidBackendId)
	{
		slot = &ProcSignalSlots[backendId - 1];

		/*
		 * Note: Since there's no locking, it's possible that the target
		 * process detaches from shared memory and exits right after this
		 * test, before we set the flag and send signal. And the signal slot
		 * might even be recycled by a new process, so it's remotely possible
		 * that we set a flag for a wrong process. That's OK, all the signals
		 * are such that no harm is done if they're mistakenly fired.
		 */
		if (slot->pss_pid == pid)
		{
			/* Atomically set the proper flag */
			slot->pss_signalFlags[reason] = true;
			/* Send signal */
			return kill(pid, SIGUSR1);
		}
	}
	else
	{
		/*
		 * BackendId not provided, so search the array using pid.  We search
		 * the array back to front so as to reduce search overhead.  Passing
		 * InvalidBackendId means that the target is most likely an auxiliary
		 * process, which will have a slot near the end of the array.
		 */
		int			i;

		for (i = NumProcSignalSlots - 1; i >= 0; i--)
		{
			slot = &ProcSignalSlots[i];

			if (slot->pss_pid == pid)
			{
				/* the above note about race conditions applies here too */

				/* Atomically set the proper flag */
				slot->pss_signalFlags[reason] = true;
				/* Send signal */
				return kill(pid, SIGUSR1);
			}
		}
	}

	errno = ESRCH;
	return -1;
}

/*
 * CheckProcSignal - check to see if a particular reason has been
 * signaled, and clear the signal flag.  Should be called after receiving
 * SIGUSR1.
 */
static bool
CheckProcSignal(ProcSignalReason reason)
{
	volatile ProcSignalSlot *slot = MyProcSignalSlot;

	if (slot != NULL)
	{
		/* Careful here --- don't clear flag if we haven't seen it set */
		if (slot->pss_signalFlags[reason])
		{
			slot->pss_signalFlags[reason] = false;
			return true;
		}
	}

	return false;
}

/*
 * procsignal_sigusr1_handler - handle SIGUSR1 signal.
 */
void
procsignal_sigusr1_handler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	if (CheckProcSignal(PROCSIG_CATCHUP_INTERRUPT))
		HandleCatchupInterrupt();

	if (CheckProcSignal(PROCSIG_NOTIFY_INTERRUPT))
		HandleNotifyInterrupt();

	if (CheckProcSignal(PROCSIG_PARALLEL_MESSAGE))
		HandleParallelMessageInterrupt();

	if (CheckProcSignal(PROCSIG_RECOVERY_CONFLICT_DATABASE))
		RecoveryConflictInterrupt(PROCSIG_RECOVERY_CONFLICT_DATABASE);

	if (CheckProcSignal(PROCSIG_RECOVERY_CONFLICT_TABLESPACE))
		RecoveryConflictInterrupt(PROCSIG_RECOVERY_CONFLICT_TABLESPACE);

	if (CheckProcSignal(PROCSIG_RECOVERY_CONFLICT_LOCK))
		RecoveryConflictInterrupt(PROCSIG_RECOVERY_CONFLICT_LOCK);

	if (CheckProcSignal(PROCSIG_RECOVERY_CONFLICT_SNAPSHOT))
		RecoveryConflictInterrupt(PROCSIG_RECOVERY_CONFLICT_SNAPSHOT);

	if (CheckProcSignal(PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK))
		RecoveryConflictInterrupt(PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK);

	if (CheckProcSignal(PROCSIG_RECOVERY_CONFLICT_BUFFERPIN))
		RecoveryConflictInterrupt(PROCSIG_RECOVERY_CONFLICT_BUFFERPIN);

	if (set_latch_on_sigusr1)
		SetLatch(MyLatch);

	latch_sigusr1_handler();

	errno = save_errno;
}
