/*-------------------------------------------------------------------------
 *
 * procsignal.c
 *	  Routines for interprocess signaling
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
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
#include "pgstat.h"
#include "port/pg_bitutils.h"
#include "replication/logicalworker.h"
#include "replication/walsender.h"
#include "storage/condition_variable.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/shmem.h"
#include "storage/sinval.h"
#include "storage/smgr.h"
#include "tcop/tcopprot.h"
#include "utils/memutils.h"

/*
 * The SIGUSR1 signal is multiplexed to support signaling multiple event
 * types. The specific reason is communicated via flags in shared memory.
 * We keep a boolean flag for each possible "reason", so that different
 * reasons can be signaled to a process concurrently.  (However, if the same
 * reason is signaled more than once nearly simultaneously, the process may
 * observe it only once.)
 *
 * Each process that wants to receive signals registers its process ID
 * in the ProcSignalSlots array. The array is indexed by ProcNumber to make
 * slot allocation simple, and to avoid having to search the array when you
 * know the ProcNumber of the process you're signaling.  (We do support
 * signaling without ProcNumber, but it's a bit less efficient.)
 *
 * The fields in each slot are protected by a spinlock, pss_mutex. pss_pid can
 * also be read without holding the spinlock, as a quick preliminary check
 * when searching for a particular PID in the array.
 *
 * pss_signalFlags are intended to be set in cases where we don't need to
 * keep track of whether or not the target process has handled the signal,
 * but sometimes we need confirmation, as when making a global state change
 * that cannot be considered complete until all backends have taken notice
 * of it. For such use cases, we set a bit in pss_barrierCheckMask and then
 * increment the current "barrier generation"; when the new barrier generation
 * (or greater) appears in the pss_barrierGeneration flag of every process,
 * we know that the message has been received everywhere.
 */
typedef struct
{
	pg_atomic_uint32 pss_pid;
	bool		pss_cancel_key_valid;
	int32		pss_cancel_key;
	volatile sig_atomic_t pss_signalFlags[NUM_PROCSIGNALS];
	slock_t		pss_mutex;		/* protects the above fields */

	/* Barrier-related fields (not protected by pss_mutex) */
	pg_atomic_uint64 pss_barrierGeneration;
	pg_atomic_uint32 pss_barrierCheckMask;
	ConditionVariable pss_barrierCV;
} ProcSignalSlot;

/*
 * Information that is global to the entire ProcSignal system can be stored
 * here.
 *
 * psh_barrierGeneration is the highest barrier generation in existence.
 */
struct ProcSignalHeader
{
	pg_atomic_uint64 psh_barrierGeneration;
	ProcSignalSlot psh_slot[FLEXIBLE_ARRAY_MEMBER];
};

/*
 * We reserve a slot for each possible ProcNumber, plus one for each
 * possible auxiliary process type.  (This scheme assumes there is not
 * more than one of any auxiliary process type at a time.)
 */
#define NumProcSignalSlots	(MaxBackends + NUM_AUXILIARY_PROCS)

/* Check whether the relevant type bit is set in the flags. */
#define BARRIER_SHOULD_CHECK(flags, type) \
	(((flags) & (((uint32) 1) << (uint32) (type))) != 0)

/* Clear the relevant type bit from the flags. */
#define BARRIER_CLEAR_BIT(flags, type) \
	((flags) &= ~(((uint32) 1) << (uint32) (type)))

NON_EXEC_STATIC ProcSignalHeader *ProcSignal = NULL;
static ProcSignalSlot *MyProcSignalSlot = NULL;

static bool CheckProcSignal(ProcSignalReason reason);
static void CleanupProcSignalState(int status, Datum arg);
static void ResetProcSignalBarrierBits(uint32 flags);

/*
 * ProcSignalShmemSize
 *		Compute space needed for ProcSignal's shared memory
 */
Size
ProcSignalShmemSize(void)
{
	Size		size;

	size = mul_size(NumProcSignalSlots, sizeof(ProcSignalSlot));
	size = add_size(size, offsetof(ProcSignalHeader, psh_slot));
	return size;
}

/*
 * ProcSignalShmemInit
 *		Allocate and initialize ProcSignal's shared memory
 */
void
ProcSignalShmemInit(void)
{
	Size		size = ProcSignalShmemSize();
	bool		found;

	ProcSignal = (ProcSignalHeader *)
		ShmemInitStruct("ProcSignal", size, &found);

	/* If we're first, initialize. */
	if (!found)
	{
		int			i;

		pg_atomic_init_u64(&ProcSignal->psh_barrierGeneration, 0);

		for (i = 0; i < NumProcSignalSlots; ++i)
		{
			ProcSignalSlot *slot = &ProcSignal->psh_slot[i];

			SpinLockInit(&slot->pss_mutex);
			pg_atomic_init_u32(&slot->pss_pid, 0);
			slot->pss_cancel_key_valid = false;
			slot->pss_cancel_key = 0;
			MemSet(slot->pss_signalFlags, 0, sizeof(slot->pss_signalFlags));
			pg_atomic_init_u64(&slot->pss_barrierGeneration, PG_UINT64_MAX);
			pg_atomic_init_u32(&slot->pss_barrierCheckMask, 0);
			ConditionVariableInit(&slot->pss_barrierCV);
		}
	}
}

/*
 * ProcSignalInit
 *		Register the current process in the ProcSignal array
 */
void
ProcSignalInit(bool cancel_key_valid, int32 cancel_key)
{
	ProcSignalSlot *slot;
	uint64		barrier_generation;
	uint32		old_pss_pid;

	if (MyProcNumber < 0)
		elog(ERROR, "MyProcNumber not set");
	if (MyProcNumber >= NumProcSignalSlots)
		elog(ERROR, "unexpected MyProcNumber %d in ProcSignalInit (max %d)", MyProcNumber, NumProcSignalSlots);
	slot = &ProcSignal->psh_slot[MyProcNumber];

	SpinLockAcquire(&slot->pss_mutex);

	/* Value used for sanity check below */
	old_pss_pid = pg_atomic_read_u32(&slot->pss_pid);

	/* Clear out any leftover signal reasons */
	MemSet(slot->pss_signalFlags, 0, NUM_PROCSIGNALS * sizeof(sig_atomic_t));

	/*
	 * Initialize barrier state. Since we're a brand-new process, there
	 * shouldn't be any leftover backend-private state that needs to be
	 * updated. Therefore, we can broadcast the latest barrier generation and
	 * disregard any previously-set check bits.
	 *
	 * NB: This only works if this initialization happens early enough in the
	 * startup sequence that we haven't yet cached any state that might need
	 * to be invalidated. That's also why we have a memory barrier here, to be
	 * sure that any later reads of memory happen strictly after this.
	 */
	pg_atomic_write_u32(&slot->pss_barrierCheckMask, 0);
	barrier_generation =
		pg_atomic_read_u64(&ProcSignal->psh_barrierGeneration);
	pg_atomic_write_u64(&slot->pss_barrierGeneration, barrier_generation);

	slot->pss_cancel_key_valid = cancel_key_valid;
	slot->pss_cancel_key = cancel_key;
	pg_atomic_write_u32(&slot->pss_pid, MyProcPid);

	SpinLockRelease(&slot->pss_mutex);

	/* Spinlock is released, do the check */
	if (old_pss_pid != 0)
		elog(LOG, "process %d taking over ProcSignal slot %d, but it's not empty",
			 MyProcPid, MyProcNumber);

	/* Remember slot location for CheckProcSignal */
	MyProcSignalSlot = slot;

	/* Set up to release the slot on process exit */
	on_shmem_exit(CleanupProcSignalState, (Datum) 0);
}

/*
 * CleanupProcSignalState
 *		Remove current process from ProcSignal mechanism
 *
 * This function is called via on_shmem_exit() during backend shutdown.
 */
static void
CleanupProcSignalState(int status, Datum arg)
{
	pid_t		old_pid;
	ProcSignalSlot *slot = MyProcSignalSlot;

	/*
	 * Clear MyProcSignalSlot, so that a SIGUSR1 received after this point
	 * won't try to access it after it's no longer ours (and perhaps even
	 * after we've unmapped the shared memory segment).
	 */
	Assert(MyProcSignalSlot != NULL);
	MyProcSignalSlot = NULL;

	/* sanity check */
	SpinLockAcquire(&slot->pss_mutex);
	old_pid = pg_atomic_read_u32(&slot->pss_pid);
	if (old_pid != MyProcPid)
	{
		/*
		 * don't ERROR here. We're exiting anyway, and don't want to get into
		 * infinite loop trying to exit
		 */
		SpinLockRelease(&slot->pss_mutex);
		elog(LOG, "process %d releasing ProcSignal slot %d, but it contains %d",
			 MyProcPid, (int) (slot - ProcSignal->psh_slot), (int) old_pid);
		return;					/* XXX better to zero the slot anyway? */
	}

	/* Mark the slot as unused */
	pg_atomic_write_u32(&slot->pss_pid, 0);
	slot->pss_cancel_key_valid = false;
	slot->pss_cancel_key = 0;

	/*
	 * Make this slot look like it's absorbed all possible barriers, so that
	 * no barrier waits block on it.
	 */
	pg_atomic_write_u64(&slot->pss_barrierGeneration, PG_UINT64_MAX);

	SpinLockRelease(&slot->pss_mutex);

	ConditionVariableBroadcast(&slot->pss_barrierCV);
}

/*
 * SendProcSignal
 *		Send a signal to a Postgres process
 *
 * Providing procNumber is optional, but it will speed up the operation.
 *
 * On success (a signal was sent), zero is returned.
 * On error, -1 is returned, and errno is set (typically to ESRCH or EPERM).
 *
 * Not to be confused with ProcSendSignal
 */
int
SendProcSignal(pid_t pid, ProcSignalReason reason, ProcNumber procNumber)
{
	volatile ProcSignalSlot *slot;

	if (procNumber != INVALID_PROC_NUMBER)
	{
		Assert(procNumber < NumProcSignalSlots);
		slot = &ProcSignal->psh_slot[procNumber];

		SpinLockAcquire(&slot->pss_mutex);
		if (pg_atomic_read_u32(&slot->pss_pid) == pid)
		{
			/* Atomically set the proper flag */
			slot->pss_signalFlags[reason] = true;
			SpinLockRelease(&slot->pss_mutex);
			/* Send signal */
			return kill(pid, SIGUSR1);
		}
		SpinLockRelease(&slot->pss_mutex);
	}
	else
	{
		/*
		 * procNumber not provided, so search the array using pid.  We search
		 * the array back to front so as to reduce search overhead.  Passing
		 * INVALID_PROC_NUMBER means that the target is most likely an
		 * auxiliary process, which will have a slot near the end of the
		 * array.
		 */
		int			i;

		for (i = NumProcSignalSlots - 1; i >= 0; i--)
		{
			slot = &ProcSignal->psh_slot[i];

			if (pg_atomic_read_u32(&slot->pss_pid) == pid)
			{
				SpinLockAcquire(&slot->pss_mutex);
				if (pg_atomic_read_u32(&slot->pss_pid) == pid)
				{
					/* Atomically set the proper flag */
					slot->pss_signalFlags[reason] = true;
					SpinLockRelease(&slot->pss_mutex);
					/* Send signal */
					return kill(pid, SIGUSR1);
				}
				SpinLockRelease(&slot->pss_mutex);
			}
		}
	}

	errno = ESRCH;
	return -1;
}

/*
 * EmitProcSignalBarrier
 *		Send a signal to every Postgres process
 *
 * The return value of this function is the barrier "generation" created
 * by this operation. This value can be passed to WaitForProcSignalBarrier
 * to wait until it is known that every participant in the ProcSignal
 * mechanism has absorbed the signal (or started afterwards).
 *
 * Note that it would be a bad idea to use this for anything that happens
 * frequently, as interrupting every backend could cause a noticeable
 * performance hit.
 *
 * Callers are entitled to assume that this function will not throw ERROR
 * or FATAL.
 */
uint64
EmitProcSignalBarrier(ProcSignalBarrierType type)
{
	uint32		flagbit = 1 << (uint32) type;
	uint64		generation;

	/*
	 * Set all the flags.
	 *
	 * Note that pg_atomic_fetch_or_u32 has full barrier semantics, so this is
	 * totally ordered with respect to anything the caller did before, and
	 * anything that we do afterwards. (This is also true of the later call to
	 * pg_atomic_add_fetch_u64.)
	 */
	for (int i = 0; i < NumProcSignalSlots; i++)
	{
		volatile ProcSignalSlot *slot = &ProcSignal->psh_slot[i];

		pg_atomic_fetch_or_u32(&slot->pss_barrierCheckMask, flagbit);
	}

	/*
	 * Increment the generation counter.
	 */
	generation =
		pg_atomic_add_fetch_u64(&ProcSignal->psh_barrierGeneration, 1);

	/*
	 * Signal all the processes, so that they update their advertised barrier
	 * generation.
	 *
	 * Concurrency is not a problem here. Backends that have exited don't
	 * matter, and new backends that have joined since we entered this
	 * function must already have current state, since the caller is
	 * responsible for making sure that the relevant state is entirely visible
	 * before calling this function in the first place. We still have to wake
	 * them up - because we can't distinguish between such backends and older
	 * backends that need to update state - but they won't actually need to
	 * change any state.
	 */
	for (int i = NumProcSignalSlots - 1; i >= 0; i--)
	{
		volatile ProcSignalSlot *slot = &ProcSignal->psh_slot[i];
		pid_t		pid = pg_atomic_read_u32(&slot->pss_pid);

		if (pid != 0)
		{
			SpinLockAcquire(&slot->pss_mutex);
			pid = pg_atomic_read_u32(&slot->pss_pid);
			if (pid != 0)
			{
				/* see SendProcSignal for details */
				slot->pss_signalFlags[PROCSIG_BARRIER] = true;
				SpinLockRelease(&slot->pss_mutex);
				kill(pid, SIGUSR1);
			}
			else
				SpinLockRelease(&slot->pss_mutex);
		}
	}

	return generation;
}

/*
 * WaitForProcSignalBarrier - wait until it is guaranteed that all changes
 * requested by a specific call to EmitProcSignalBarrier() have taken effect.
 */
void
WaitForProcSignalBarrier(uint64 generation)
{
	Assert(generation <= pg_atomic_read_u64(&ProcSignal->psh_barrierGeneration));

	elog(DEBUG1,
		 "waiting for all backends to process ProcSignalBarrier generation "
		 UINT64_FORMAT,
		 generation);

	for (int i = NumProcSignalSlots - 1; i >= 0; i--)
	{
		ProcSignalSlot *slot = &ProcSignal->psh_slot[i];
		uint64		oldval;

		/*
		 * It's important that we check only pss_barrierGeneration here and
		 * not pss_barrierCheckMask. Bits in pss_barrierCheckMask get cleared
		 * before the barrier is actually absorbed, but pss_barrierGeneration
		 * is updated only afterward.
		 */
		oldval = pg_atomic_read_u64(&slot->pss_barrierGeneration);
		while (oldval < generation)
		{
			if (ConditionVariableTimedSleep(&slot->pss_barrierCV,
											5000,
											WAIT_EVENT_PROC_SIGNAL_BARRIER))
				ereport(LOG,
						(errmsg("still waiting for backend with PID %d to accept ProcSignalBarrier",
								(int) pg_atomic_read_u32(&slot->pss_pid))));
			oldval = pg_atomic_read_u64(&slot->pss_barrierGeneration);
		}
		ConditionVariableCancelSleep();
	}

	elog(DEBUG1,
		 "finished waiting for all backends to process ProcSignalBarrier generation "
		 UINT64_FORMAT,
		 generation);

	/*
	 * The caller is probably calling this function because it wants to read
	 * the shared state or perform further writes to shared state once all
	 * backends are known to have absorbed the barrier. However, the read of
	 * pss_barrierGeneration was performed unlocked; insert a memory barrier
	 * to separate it from whatever follows.
	 */
	pg_memory_barrier();
}

/*
 * Handle receipt of an interrupt indicating a global barrier event.
 *
 * All the actual work is deferred to ProcessProcSignalBarrier(), because we
 * cannot safely access the barrier generation inside the signal handler as
 * 64bit atomics might use spinlock based emulation, even for reads. As this
 * routine only gets called when PROCSIG_BARRIER is sent that won't cause a
 * lot of unnecessary work.
 */
static void
HandleProcSignalBarrierInterrupt(void)
{
	InterruptPending = true;
	ProcSignalBarrierPending = true;
	/* latch will be set by procsignal_sigusr1_handler */
}

/*
 * Perform global barrier related interrupt checking.
 *
 * Any backend that participates in ProcSignal signaling must arrange to
 * call this function periodically. It is called from CHECK_FOR_INTERRUPTS(),
 * which is enough for normal backends, but not necessarily for all types of
 * background processes.
 */
void
ProcessProcSignalBarrier(void)
{
	uint64		local_gen;
	uint64		shared_gen;
	volatile uint32 flags;

	Assert(MyProcSignalSlot);

	/* Exit quickly if there's no work to do. */
	if (!ProcSignalBarrierPending)
		return;
	ProcSignalBarrierPending = false;

	/*
	 * It's not unlikely to process multiple barriers at once, before the
	 * signals for all the barriers have arrived. To avoid unnecessary work in
	 * response to subsequent signals, exit early if we already have processed
	 * all of them.
	 */
	local_gen = pg_atomic_read_u64(&MyProcSignalSlot->pss_barrierGeneration);
	shared_gen = pg_atomic_read_u64(&ProcSignal->psh_barrierGeneration);

	Assert(local_gen <= shared_gen);

	if (local_gen == shared_gen)
		return;

	/*
	 * Get and clear the flags that are set for this backend. Note that
	 * pg_atomic_exchange_u32 is a full barrier, so we're guaranteed that the
	 * read of the barrier generation above happens before we atomically
	 * extract the flags, and that any subsequent state changes happen
	 * afterward.
	 *
	 * NB: In order to avoid race conditions, we must zero
	 * pss_barrierCheckMask first and only afterwards try to do barrier
	 * processing. If we did it in the other order, someone could send us
	 * another barrier of some type right after we called the
	 * barrier-processing function but before we cleared the bit. We would
	 * have no way of knowing that the bit needs to stay set in that case, so
	 * the need to call the barrier-processing function again would just get
	 * forgotten. So instead, we tentatively clear all the bits and then put
	 * back any for which we don't manage to successfully absorb the barrier.
	 */
	flags = pg_atomic_exchange_u32(&MyProcSignalSlot->pss_barrierCheckMask, 0);

	/*
	 * If there are no flags set, then we can skip doing any real work.
	 * Otherwise, establish a PG_TRY block, so that we don't lose track of
	 * which types of barrier processing are needed if an ERROR occurs.
	 */
	if (flags != 0)
	{
		bool		success = true;

		PG_TRY();
		{
			/*
			 * Process each type of barrier. The barrier-processing functions
			 * should normally return true, but may return false if the
			 * barrier can't be absorbed at the current time. This should be
			 * rare, because it's pretty expensive.  Every single
			 * CHECK_FOR_INTERRUPTS() will return here until we manage to
			 * absorb the barrier, and that cost will add up in a hurry.
			 *
			 * NB: It ought to be OK to call the barrier-processing functions
			 * unconditionally, but it's more efficient to call only the ones
			 * that might need us to do something based on the flags.
			 */
			while (flags != 0)
			{
				ProcSignalBarrierType type;
				bool		processed = true;

				type = (ProcSignalBarrierType) pg_rightmost_one_pos32(flags);
				switch (type)
				{
					case PROCSIGNAL_BARRIER_SMGRRELEASE:
						processed = ProcessBarrierSmgrRelease();
						break;
				}

				/*
				 * To avoid an infinite loop, we must always unset the bit in
				 * flags.
				 */
				BARRIER_CLEAR_BIT(flags, type);

				/*
				 * If we failed to process the barrier, reset the shared bit
				 * so we try again later, and set a flag so that we don't bump
				 * our generation.
				 */
				if (!processed)
				{
					ResetProcSignalBarrierBits(((uint32) 1) << type);
					success = false;
				}
			}
		}
		PG_CATCH();
		{
			/*
			 * If an ERROR occurred, we'll need to try again later to handle
			 * that barrier type and any others that haven't been handled yet
			 * or weren't successfully absorbed.
			 */
			ResetProcSignalBarrierBits(flags);
			PG_RE_THROW();
		}
		PG_END_TRY();

		/*
		 * If some barrier types were not successfully absorbed, we will have
		 * to try again later.
		 */
		if (!success)
			return;
	}

	/*
	 * State changes related to all types of barriers that might have been
	 * emitted have now been handled, so we can update our notion of the
	 * generation to the one we observed before beginning the updates. If
	 * things have changed further, it'll get fixed up when this function is
	 * next called.
	 */
	pg_atomic_write_u64(&MyProcSignalSlot->pss_barrierGeneration, shared_gen);
	ConditionVariableBroadcast(&MyProcSignalSlot->pss_barrierCV);
}

/*
 * If it turns out that we couldn't absorb one or more barrier types, either
 * because the barrier-processing functions returned false or due to an error,
 * arrange for processing to be retried later.
 */
static void
ResetProcSignalBarrierBits(uint32 flags)
{
	pg_atomic_fetch_or_u32(&MyProcSignalSlot->pss_barrierCheckMask, flags);
	ProcSignalBarrierPending = true;
	InterruptPending = true;
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
		/*
		 * Careful here --- don't clear flag if we haven't seen it set.
		 * pss_signalFlags is of type "volatile sig_atomic_t" to allow us to
		 * read it here safely, without holding the spinlock.
		 */
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
	if (CheckProcSignal(PROCSIG_CATCHUP_INTERRUPT))
		HandleCatchupInterrupt();

	if (CheckProcSignal(PROCSIG_NOTIFY_INTERRUPT))
		HandleNotifyInterrupt();

	if (CheckProcSignal(PROCSIG_PARALLEL_MESSAGE))
		HandleParallelMessageInterrupt();

	if (CheckProcSignal(PROCSIG_WALSND_INIT_STOPPING))
		HandleWalSndInitStopping();

	if (CheckProcSignal(PROCSIG_BARRIER))
		HandleProcSignalBarrierInterrupt();

	if (CheckProcSignal(PROCSIG_LOG_MEMORY_CONTEXT))
		HandleLogMemoryContextInterrupt();

	if (CheckProcSignal(PROCSIG_PARALLEL_APPLY_MESSAGE))
		HandleParallelApplyMessageInterrupt();

	if (CheckProcSignal(PROCSIG_RECOVERY_CONFLICT_DATABASE))
		HandleRecoveryConflictInterrupt(PROCSIG_RECOVERY_CONFLICT_DATABASE);

	if (CheckProcSignal(PROCSIG_RECOVERY_CONFLICT_TABLESPACE))
		HandleRecoveryConflictInterrupt(PROCSIG_RECOVERY_CONFLICT_TABLESPACE);

	if (CheckProcSignal(PROCSIG_RECOVERY_CONFLICT_LOCK))
		HandleRecoveryConflictInterrupt(PROCSIG_RECOVERY_CONFLICT_LOCK);

	if (CheckProcSignal(PROCSIG_RECOVERY_CONFLICT_SNAPSHOT))
		HandleRecoveryConflictInterrupt(PROCSIG_RECOVERY_CONFLICT_SNAPSHOT);

	if (CheckProcSignal(PROCSIG_RECOVERY_CONFLICT_LOGICALSLOT))
		HandleRecoveryConflictInterrupt(PROCSIG_RECOVERY_CONFLICT_LOGICALSLOT);

	if (CheckProcSignal(PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK))
		HandleRecoveryConflictInterrupt(PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK);

	if (CheckProcSignal(PROCSIG_RECOVERY_CONFLICT_BUFFERPIN))
		HandleRecoveryConflictInterrupt(PROCSIG_RECOVERY_CONFLICT_BUFFERPIN);

	SetLatch(MyLatch);
}

/*
 * Send a query cancellation signal to backend.
 *
 * Note: This is called from a backend process before authentication.  We
 * cannot take LWLocks yet, but that's OK; we rely on atomic reads of the
 * fields in the ProcSignal slots.
 */
void
SendCancelRequest(int backendPID, int32 cancelAuthCode)
{
	Assert(backendPID != 0);

	/*
	 * See if we have a matching backend. Reading the pss_pid and
	 * pss_cancel_key fields is racy, a backend might die and remove itself
	 * from the array at any time.  The probability of the cancellation key
	 * matching wrong process is miniscule, however, so we can live with that.
	 * PIDs are reused too, so sending the signal based on PID is inherently
	 * racy anyway, although OS's avoid reusing PIDs too soon.
	 */
	for (int i = 0; i < NumProcSignalSlots; i++)
	{
		ProcSignalSlot *slot = &ProcSignal->psh_slot[i];
		bool		match;

		if (pg_atomic_read_u32(&slot->pss_pid) != backendPID)
			continue;

		/* Acquire the spinlock and re-check */
		SpinLockAcquire(&slot->pss_mutex);
		if (pg_atomic_read_u32(&slot->pss_pid) != backendPID)
		{
			SpinLockRelease(&slot->pss_mutex);
			continue;
		}
		else
		{
			match = slot->pss_cancel_key_valid && slot->pss_cancel_key == cancelAuthCode;

			SpinLockRelease(&slot->pss_mutex);

			if (match)
			{
				/* Found a match; signal that backend to cancel current op */
				ereport(DEBUG2,
						(errmsg_internal("processing cancel request: sending SIGINT to process %d",
										 backendPID)));

				/*
				 * If we have setsid(), signal the backend's whole process
				 * group
				 */
#ifdef HAVE_SETSID
				kill(-backendPID, SIGINT);
#else
				kill(backendPID, SIGINT);
#endif
			}
			else
			{
				/* Right PID, wrong key: no way, Jose */
				ereport(LOG,
						(errmsg("wrong key in cancel request for process %d",
								backendPID)));
			}
			return;
		}
	}

	/* No matching backend */
	ereport(LOG,
			(errmsg("PID %d in cancel request did not match any process",
					backendPID)));
}
