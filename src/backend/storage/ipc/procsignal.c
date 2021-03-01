/*-------------------------------------------------------------------------
 *
 * procsignal.c
 *	  Routines for interprocess signaling
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
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
#include "port/pg_bitutils.h"
#include "commands/async.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "replication/walsender.h"
#include "storage/condition_variable.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "storage/sinval.h"
#include "tcop/tcopprot.h"

/*
 * The SIGUSR1 signal is multiplexed to support signaling multiple event
 * types. The specific reason is communicated via flags in shared memory.
 * We keep a boolean flag for each possible "reason", so that different
 * reasons can be signaled to a process concurrently.  (However, if the same
 * reason is signaled more than once nearly simultaneously, the process may
 * observe it only once.)
 *
 * Each process that wants to receive signals registers its process ID
 * in the ProcSignalSlots array. The array is indexed by backend ID to make
 * slot allocation simple, and to avoid having to search the array when you
 * know the backend ID of the process you're signaling.  (We do support
 * signaling without backend ID, but it's a bit less efficient.)
 *
 * The flags are actually declared as "volatile sig_atomic_t" for maximum
 * portability.  This should ensure that loads and stores of the flag
 * values are atomic, allowing us to dispense with any explicit locking.
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
	volatile pid_t		pss_pid;
	volatile sig_atomic_t pss_signalFlags[NUM_PROCSIGNALS];
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
typedef struct
{
	pg_atomic_uint64 psh_barrierGeneration;
	ProcSignalSlot psh_slot[FLEXIBLE_ARRAY_MEMBER];
} ProcSignalHeader;

/*
 * We reserve a slot for each possible BackendId, plus one for each
 * possible auxiliary process type.  (This scheme assumes there is not
 * more than one of any auxiliary process type at a time.)
 */
#define NumProcSignalSlots	(MaxBackends + NUM_AUXPROCTYPES)

/* Check whether the relevant type bit is set in the flags. */
#define BARRIER_SHOULD_CHECK(flags, type) \
	(((flags) & (((uint32) 1) << (uint32) (type))) != 0)

/* Clear the relevant type bit from the flags. */
#define BARRIER_CLEAR_BIT(flags, type) \
	((flags) &= ~(((uint32) 1) << (uint32) (type)))

static ProcSignalHeader *ProcSignal = NULL;
static ProcSignalSlot *MyProcSignalSlot = NULL;

static bool CheckProcSignal(ProcSignalReason reason);
static void CleanupProcSignalState(int status, Datum arg);
static void ResetProcSignalBarrierBits(uint32 flags);
static bool ProcessBarrierPlaceholder(void);

/*
 * ProcSignalShmemSize
 *		Compute space needed for procsignal's shared memory
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
 *		Allocate and initialize procsignal's shared memory
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

			slot->pss_pid = 0;
			MemSet(slot->pss_signalFlags, 0, sizeof(slot->pss_signalFlags));
			pg_atomic_init_u64(&slot->pss_barrierGeneration, PG_UINT64_MAX);
			pg_atomic_init_u32(&slot->pss_barrierCheckMask, 0);
			ConditionVariableInit(&slot->pss_barrierCV);
		}
	}
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
	ProcSignalSlot *slot;
	uint64		barrier_generation;

	Assert(pss_idx >= 1 && pss_idx <= NumProcSignalSlots);

	slot = &ProcSignal->psh_slot[pss_idx - 1];

	/* sanity check */
	if (slot->pss_pid != 0)
		elog(LOG, "process %d taking over ProcSignal slot %d, but it's not empty",
			 MyProcPid, pss_idx);

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
	pg_memory_barrier();

	/* Mark slot with my PID */
	slot->pss_pid = MyProcPid;

	/* Remember slot location for CheckProcSignal */
	MyProcSignalSlot = slot;

	/* Set up to release the slot on process exit */
	on_shmem_exit(CleanupProcSignalState, Int32GetDatum(pss_idx));
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
	int			pss_idx = DatumGetInt32(arg);
	ProcSignalSlot *slot;

	slot = &ProcSignal->psh_slot[pss_idx - 1];
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

	/*
	 * Make this slot look like it's absorbed all possible barriers, so that
	 * no barrier waits block on it.
	 */
	pg_atomic_write_u64(&slot->pss_barrierGeneration, PG_UINT64_MAX);
	ConditionVariableBroadcast(&slot->pss_barrierCV);

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
		slot = &ProcSignal->psh_slot[backendId - 1];

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
			slot = &ProcSignal->psh_slot[i];

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
		pid_t		pid = slot->pss_pid;

		if (pid != 0)
		{
			/* see SendProcSignal for details */
			slot->pss_signalFlags[PROCSIG_BARRIER] = true;
			kill(pid, SIGUSR1);
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
			ConditionVariableSleep(&slot->pss_barrierCV,
								   WAIT_EVENT_PROC_SIGNAL_BARRIER);
			oldval = pg_atomic_read_u64(&slot->pss_barrierGeneration);
		}
		ConditionVariableCancelSleep();
	}

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
	volatile uint32		flags;

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
	 * NB: In order to avoid race conditions, we must zero pss_barrierCheckMask
	 * first and only afterwards try to do barrier processing. If we did it
	 * in the other order, someone could send us another barrier of some
	 * type right after we called the barrier-processing function but before
	 * we cleared the bit. We would have no way of knowing that the bit needs
	 * to stay set in that case, so the need to call the barrier-processing
	 * function again would just get forgotten. So instead, we tentatively
	 * clear all the bits and then put back any for which we don't manage
	 * to successfully absorb the barrier.
	 */
	flags = pg_atomic_exchange_u32(&MyProcSignalSlot->pss_barrierCheckMask, 0);

	/*
	 * If there are no flags set, then we can skip doing any real work.
	 * Otherwise, establish a PG_TRY block, so that we don't lose track of
	 * which types of barrier processing are needed if an ERROR occurs.
	 */
	if (flags != 0)
	{
		bool	success = true;

		PG_TRY();
		{
			/*
			 * Process each type of barrier. The barrier-processing functions
			 * should normally return true, but may return false if the barrier
			 * can't be absorbed at the current time. This should be rare,
			 * because it's pretty expensive.  Every single
			 * CHECK_FOR_INTERRUPTS() will return here until we manage to
			 * absorb the barrier, and that cost will add up in a hurry.
			 *
			 * NB: It ought to be OK to call the barrier-processing functions
			 * unconditionally, but it's more efficient to call only the ones
			 * that might need us to do something based on the flags.
			 */
			while (flags != 0)
			{
				ProcSignalBarrierType	type;
				bool processed = true;

				type = (ProcSignalBarrierType) pg_rightmost_one_pos32(flags);
				switch (type)
				{
					case PROCSIGNAL_BARRIER_PLACEHOLDER:
						processed = ProcessBarrierPlaceholder();
						break;
				}

				/*
				 * To avoid an infinite loop, we must always unset the bit
				 * in flags.
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

static bool
ProcessBarrierPlaceholder(void)
{
	/*
	 * XXX. This is just a placeholder until the first real user of this
	 * machinery gets committed. Rename PROCSIGNAL_BARRIER_PLACEHOLDER to
	 * PROCSIGNAL_BARRIER_SOMETHING_ELSE where SOMETHING_ELSE is something
	 * appropriately descriptive. Get rid of this function and instead have
	 * ProcessBarrierSomethingElse. Most likely, that function should live in
	 * the file pertaining to that subsystem, rather than here.
	 *
	 * The return value should be 'true' if the barrier was successfully
	 * absorbed and 'false' if not. Note that returning 'false' can lead to
	 * very frequent retries, so try hard to make that an uncommon case.
	 */
	return true;
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

	if (CheckProcSignal(PROCSIG_WALSND_INIT_STOPPING))
		HandleWalSndInitStopping();

	if (CheckProcSignal(PROCSIG_BARRIER))
		HandleProcSignalBarrierInterrupt();

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

	SetLatch(MyLatch);

	errno = save_errno;
}
