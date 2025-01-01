/*-------------------------------------------------------------------------
 *
 * pmsignal.c
 *	  routines for signaling between the postmaster and its child processes
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/pmsignal.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <unistd.h>

#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif

#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "replication/walsender.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "storage/shmem.h"
#include "utils/memutils.h"


/*
 * The postmaster is signaled by its children by sending SIGUSR1.  The
 * specific reason is communicated via flags in shared memory.  We keep
 * a boolean flag for each possible "reason", so that different reasons
 * can be signaled by different backends at the same time.  (However,
 * if the same reason is signaled more than once simultaneously, the
 * postmaster will observe it only once.)
 *
 * The flags are actually declared as "volatile sig_atomic_t" for maximum
 * portability.  This should ensure that loads and stores of the flag
 * values are atomic, allowing us to dispense with any explicit locking.
 *
 * In addition to the per-reason flags, we store a set of per-child-process
 * flags that are currently used only for detecting whether a backend has
 * exited without performing proper shutdown.  The per-child-process flags
 * have three possible states: UNUSED, ASSIGNED, ACTIVE.  An UNUSED slot is
 * available for assignment.  An ASSIGNED slot is associated with a postmaster
 * child process, but either the process has not touched shared memory yet, or
 * it has successfully cleaned up after itself.  An ACTIVE slot means the
 * process is actively using shared memory.  The slots are assigned to child
 * processes by postmaster, and pmchild.c is responsible for tracking which
 * one goes with which PID.
 *
 * Actually there is a fourth state, WALSENDER.  This is just like ACTIVE,
 * but carries the extra information that the child is a WAL sender.
 * WAL senders too start in ACTIVE state, but switch to WALSENDER once they
 * start streaming the WAL (and they never go back to ACTIVE after that).
 *
 * We also have a shared-memory field that is used for communication in
 * the opposite direction, from postmaster to children: it tells why the
 * postmaster has broadcasted SIGQUIT signals, if indeed it has done so.
 */

#define PM_CHILD_UNUSED		0	/* these values must fit in sig_atomic_t */
#define PM_CHILD_ASSIGNED	1
#define PM_CHILD_ACTIVE		2
#define PM_CHILD_WALSENDER	3

/* "typedef struct PMSignalData PMSignalData" appears in pmsignal.h */
struct PMSignalData
{
	/* per-reason flags for signaling the postmaster */
	sig_atomic_t PMSignalFlags[NUM_PMSIGNALS];
	/* global flags for signals from postmaster to children */
	QuitSignalReason sigquit_reason;	/* why SIGQUIT was sent */
	/* per-child-process flags */
	int			num_child_flags;	/* # of entries in PMChildFlags[] */
	sig_atomic_t PMChildFlags[FLEXIBLE_ARRAY_MEMBER];
};

/* PMSignalState pointer is valid in both postmaster and child processes */
NON_EXEC_STATIC volatile PMSignalData *PMSignalState = NULL;

/*
 * Local copy of PMSignalState->num_child_flags, only valid in the
 * postmaster.  Postmaster keeps a local copy so that it doesn't need to
 * trust the value in shared memory.
 */
static int	num_child_flags;

/*
 * Signal handler to be notified if postmaster dies.
 */
#ifdef USE_POSTMASTER_DEATH_SIGNAL
volatile sig_atomic_t postmaster_possibly_dead = false;

static void
postmaster_death_handler(SIGNAL_ARGS)
{
	postmaster_possibly_dead = true;
}

/*
 * The available signals depend on the OS.  SIGUSR1 and SIGUSR2 are already
 * used for other things, so choose another one.
 *
 * Currently, we assume that we can always find a signal to use.  That
 * seems like a reasonable assumption for all platforms that are modern
 * enough to have a parent-death signaling mechanism.
 */
#if defined(SIGINFO)
#define POSTMASTER_DEATH_SIGNAL SIGINFO
#elif defined(SIGPWR)
#define POSTMASTER_DEATH_SIGNAL SIGPWR
#else
#error "cannot find a signal to use for postmaster death"
#endif

#endif							/* USE_POSTMASTER_DEATH_SIGNAL */

static void MarkPostmasterChildInactive(int code, Datum arg);

/*
 * PMSignalShmemSize
 *		Compute space needed for pmsignal.c's shared memory
 */
Size
PMSignalShmemSize(void)
{
	Size		size;

	size = offsetof(PMSignalData, PMChildFlags);
	size = add_size(size, mul_size(MaxLivePostmasterChildren(),
								   sizeof(sig_atomic_t)));

	return size;
}

/*
 * PMSignalShmemInit - initialize during shared-memory creation
 */
void
PMSignalShmemInit(void)
{
	bool		found;

	PMSignalState = (PMSignalData *)
		ShmemInitStruct("PMSignalState", PMSignalShmemSize(), &found);

	if (!found)
	{
		/* initialize all flags to zeroes */
		MemSet(unvolatize(PMSignalData *, PMSignalState), 0, PMSignalShmemSize());
		num_child_flags = MaxLivePostmasterChildren();
		PMSignalState->num_child_flags = num_child_flags;
	}
}

/*
 * SendPostmasterSignal - signal the postmaster from a child process
 */
void
SendPostmasterSignal(PMSignalReason reason)
{
	/* If called in a standalone backend, do nothing */
	if (!IsUnderPostmaster)
		return;
	/* Atomically set the proper flag */
	PMSignalState->PMSignalFlags[reason] = true;
	/* Send signal to postmaster */
	kill(PostmasterPid, SIGUSR1);
}

/*
 * CheckPostmasterSignal - check to see if a particular reason has been
 * signaled, and clear the signal flag.  Should be called by postmaster
 * after receiving SIGUSR1.
 */
bool
CheckPostmasterSignal(PMSignalReason reason)
{
	/* Careful here --- don't clear flag if we haven't seen it set */
	if (PMSignalState->PMSignalFlags[reason])
	{
		PMSignalState->PMSignalFlags[reason] = false;
		return true;
	}
	return false;
}

/*
 * SetQuitSignalReason - broadcast the reason for a system shutdown.
 * Should be called by postmaster before sending SIGQUIT to children.
 *
 * Note: in a crash-and-restart scenario, the "reason" field gets cleared
 * as a part of rebuilding shared memory; the postmaster need not do it
 * explicitly.
 */
void
SetQuitSignalReason(QuitSignalReason reason)
{
	PMSignalState->sigquit_reason = reason;
}

/*
 * GetQuitSignalReason - obtain the reason for a system shutdown.
 * Called by child processes when they receive SIGQUIT.
 * If the postmaster hasn't actually sent SIGQUIT, will return PMQUIT_NOT_SENT.
 */
QuitSignalReason
GetQuitSignalReason(void)
{
	/* This is called in signal handlers, so be extra paranoid. */
	if (!IsUnderPostmaster || PMSignalState == NULL)
		return PMQUIT_NOT_SENT;
	return PMSignalState->sigquit_reason;
}


/*
 * MarkPostmasterChildSlotAssigned - mark the given slot as ASSIGNED for a
 * new postmaster child process.
 *
 * Only the postmaster is allowed to execute this routine, so we need no
 * special locking.
 */
void
MarkPostmasterChildSlotAssigned(int slot)
{
	Assert(slot > 0 && slot <= num_child_flags);
	slot--;

	if (PMSignalState->PMChildFlags[slot] != PM_CHILD_UNUSED)
		elog(FATAL, "postmaster child slot is already in use");

	PMSignalState->PMChildFlags[slot] = PM_CHILD_ASSIGNED;
}

/*
 * MarkPostmasterChildSlotUnassigned - release a slot after death of a
 * postmaster child process.  This must be called in the postmaster process.
 *
 * Returns true if the slot had been in ASSIGNED state (the expected case),
 * false otherwise (implying that the child failed to clean itself up).
 */
bool
MarkPostmasterChildSlotUnassigned(int slot)
{
	bool		result;

	Assert(slot > 0 && slot <= num_child_flags);
	slot--;

	/*
	 * Note: the slot state might already be unused, because the logic in
	 * postmaster.c is such that this might get called twice when a child
	 * crashes.  So we don't try to Assert anything about the state.
	 */
	result = (PMSignalState->PMChildFlags[slot] == PM_CHILD_ASSIGNED);
	PMSignalState->PMChildFlags[slot] = PM_CHILD_UNUSED;
	return result;
}

/*
 * IsPostmasterChildWalSender - check if given slot is in use by a
 * walsender process.  This is called only by the postmaster.
 */
bool
IsPostmasterChildWalSender(int slot)
{
	Assert(slot > 0 && slot <= num_child_flags);
	slot--;

	if (PMSignalState->PMChildFlags[slot] == PM_CHILD_WALSENDER)
		return true;
	else
		return false;
}

/*
 * RegisterPostmasterChildActive - mark a postmaster child as about to begin
 * actively using shared memory.  This is called in the child process.
 *
 * This register an shmem exit hook to mark us as inactive again when the
 * process exits normally.
 */
void
RegisterPostmasterChildActive(void)
{
	int			slot = MyPMChildSlot;

	Assert(slot > 0 && slot <= PMSignalState->num_child_flags);
	slot--;
	Assert(PMSignalState->PMChildFlags[slot] == PM_CHILD_ASSIGNED);
	PMSignalState->PMChildFlags[slot] = PM_CHILD_ACTIVE;

	/* Arrange to clean up at exit. */
	on_shmem_exit(MarkPostmasterChildInactive, 0);
}

/*
 * MarkPostmasterChildWalSender - mark a postmaster child as a WAL sender
 * process.  This is called in the child process, sometime after marking the
 * child as active.
 */
void
MarkPostmasterChildWalSender(void)
{
	int			slot = MyPMChildSlot;

	Assert(am_walsender);

	Assert(slot > 0 && slot <= PMSignalState->num_child_flags);
	slot--;
	Assert(PMSignalState->PMChildFlags[slot] == PM_CHILD_ACTIVE);
	PMSignalState->PMChildFlags[slot] = PM_CHILD_WALSENDER;
}

/*
 * MarkPostmasterChildInactive - mark a postmaster child as done using
 * shared memory.  This is called in the child process.
 */
static void
MarkPostmasterChildInactive(int code, Datum arg)
{
	int			slot = MyPMChildSlot;

	Assert(slot > 0 && slot <= PMSignalState->num_child_flags);
	slot--;
	Assert(PMSignalState->PMChildFlags[slot] == PM_CHILD_ACTIVE ||
		   PMSignalState->PMChildFlags[slot] == PM_CHILD_WALSENDER);
	PMSignalState->PMChildFlags[slot] = PM_CHILD_ASSIGNED;
}


/*
 * PostmasterIsAliveInternal - check whether postmaster process is still alive
 *
 * This is the slow path of PostmasterIsAlive(), where the caller has already
 * checked 'postmaster_possibly_dead'.  (On platforms that don't support
 * a signal for parent death, PostmasterIsAlive() is just an alias for this.)
 */
bool
PostmasterIsAliveInternal(void)
{
#ifdef USE_POSTMASTER_DEATH_SIGNAL
	/*
	 * Reset the flag before checking, so that we don't miss a signal if
	 * postmaster dies right after the check.  If postmaster was indeed dead,
	 * we'll re-arm it before returning to caller.
	 */
	postmaster_possibly_dead = false;
#endif

#ifndef WIN32
	{
		char		c;
		ssize_t		rc;

		rc = read(postmaster_alive_fds[POSTMASTER_FD_WATCH], &c, 1);

		/*
		 * In the usual case, the postmaster is still alive, and there is no
		 * data in the pipe.
		 */
		if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return true;
		else
		{
			/*
			 * Postmaster is dead, or something went wrong with the read()
			 * call.
			 */

#ifdef USE_POSTMASTER_DEATH_SIGNAL
			postmaster_possibly_dead = true;
#endif

			if (rc < 0)
				elog(FATAL, "read on postmaster death monitoring pipe failed: %m");
			else if (rc > 0)
				elog(FATAL, "unexpected data in postmaster death monitoring pipe");

			return false;
		}
	}

#else							/* WIN32 */
	if (WaitForSingleObject(PostmasterHandle, 0) == WAIT_TIMEOUT)
		return true;
	else
	{
#ifdef USE_POSTMASTER_DEATH_SIGNAL
		postmaster_possibly_dead = true;
#endif
		return false;
	}
#endif							/* WIN32 */
}

/*
 * PostmasterDeathSignalInit - request signal on postmaster death if possible
 */
void
PostmasterDeathSignalInit(void)
{
#ifdef USE_POSTMASTER_DEATH_SIGNAL
	int			signum = POSTMASTER_DEATH_SIGNAL;

	/* Register our signal handler. */
	pqsignal(signum, postmaster_death_handler);

	/* Request a signal on parent exit. */
#if defined(PR_SET_PDEATHSIG)
	if (prctl(PR_SET_PDEATHSIG, signum) < 0)
		elog(ERROR, "could not request parent death signal: %m");
#elif defined(PROC_PDEATHSIG_CTL)
	if (procctl(P_PID, 0, PROC_PDEATHSIG_CTL, &signum) < 0)
		elog(ERROR, "could not request parent death signal: %m");
#else
#error "USE_POSTMASTER_DEATH_SIGNAL set, but there is no mechanism to request the signal"
#endif

	/*
	 * Just in case the parent was gone already and we missed it, we'd better
	 * check the slow way on the first call.
	 */
	postmaster_possibly_dead = true;
#endif							/* USE_POSTMASTER_DEATH_SIGNAL */
}
