/*-------------------------------------------------------------------------
 *
 * pmsignal.c
 *	  routines for signaling the postmaster from its child processes
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
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

#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "replication/walsender.h"
#include "storage/pmsignal.h"
#include "storage/shmem.h"


/*
 * The postmaster is signaled by its children by sending SIGUSR1.  The
 * specific reason is communicated via flags in shared memory.	We keep
 * a boolean flag for each possible "reason", so that different reasons
 * can be signaled by different backends at the same time.	(However,
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
 * child process, but either the process has not touched shared memory yet,
 * or it has successfully cleaned up after itself.	A ACTIVE slot means the
 * process is actively using shared memory.  The slots are assigned to
 * child processes at random, and postmaster.c is responsible for tracking
 * which one goes with which PID.
 *
 * Actually there is a fourth state, WALSENDER.  This is just like ACTIVE,
 * but carries the extra information that the child is a WAL sender.
 * WAL senders too start in ACTIVE state, but switch to WALSENDER once they
 * start streaming the WAL (and they never go back to ACTIVE after that).
 */

#define PM_CHILD_UNUSED		0	/* these values must fit in sig_atomic_t */
#define PM_CHILD_ASSIGNED	1
#define PM_CHILD_ACTIVE		2
#define PM_CHILD_WALSENDER	3

/* "typedef struct PMSignalData PMSignalData" appears in pmsignal.h */
struct PMSignalData
{
	/* per-reason flags */
	sig_atomic_t PMSignalFlags[NUM_PMSIGNALS];
	/* per-child-process flags */
	int			num_child_flags;	/* # of entries in PMChildFlags[] */
	int			next_child_flag;	/* next slot to try to assign */
	sig_atomic_t PMChildFlags[1];		/* VARIABLE LENGTH ARRAY */
};

NON_EXEC_STATIC volatile PMSignalData *PMSignalState = NULL;


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
		MemSet(PMSignalState, 0, PMSignalShmemSize());
		PMSignalState->num_child_flags = MaxLivePostmasterChildren();
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
 * AssignPostmasterChildSlot - select an unused slot for a new postmaster
 * child process, and set its state to ASSIGNED.  Returns a slot number
 * (one to N).
 *
 * Only the postmaster is allowed to execute this routine, so we need no
 * special locking.
 */
int
AssignPostmasterChildSlot(void)
{
	int			slot = PMSignalState->next_child_flag;
	int			n;

	/*
	 * Scan for a free slot.  We track the last slot assigned so as not to
	 * waste time repeatedly rescanning low-numbered slots.
	 */
	for (n = PMSignalState->num_child_flags; n > 0; n--)
	{
		if (--slot < 0)
			slot = PMSignalState->num_child_flags - 1;
		if (PMSignalState->PMChildFlags[slot] == PM_CHILD_UNUSED)
		{
			PMSignalState->PMChildFlags[slot] = PM_CHILD_ASSIGNED;
			PMSignalState->next_child_flag = slot;
			return slot + 1;
		}
	}

	/* Out of slots ... should never happen, else postmaster.c messed up */
	elog(FATAL, "no free slots in PMChildFlags array");
	return 0;					/* keep compiler quiet */
}

/*
 * ReleasePostmasterChildSlot - release a slot after death of a postmaster
 * child process.  This must be called in the postmaster process.
 *
 * Returns true if the slot had been in ASSIGNED state (the expected case),
 * false otherwise (implying that the child failed to clean itself up).
 */
bool
ReleasePostmasterChildSlot(int slot)
{
	bool		result;

	Assert(slot > 0 && slot <= PMSignalState->num_child_flags);
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
 * walsender process.
 */
bool
IsPostmasterChildWalSender(int slot)
{
	Assert(slot > 0 && slot <= PMSignalState->num_child_flags);
	slot--;

	if (PMSignalState->PMChildFlags[slot] == PM_CHILD_WALSENDER)
		return true;
	else
		return false;
}

/*
 * MarkPostmasterChildActive - mark a postmaster child as about to begin
 * actively using shared memory.  This is called in the child process.
 */
void
MarkPostmasterChildActive(void)
{
	int			slot = MyPMChildSlot;

	Assert(slot > 0 && slot <= PMSignalState->num_child_flags);
	slot--;
	Assert(PMSignalState->PMChildFlags[slot] == PM_CHILD_ASSIGNED);
	PMSignalState->PMChildFlags[slot] = PM_CHILD_ACTIVE;
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
void
MarkPostmasterChildInactive(void)
{
	int			slot = MyPMChildSlot;

	Assert(slot > 0 && slot <= PMSignalState->num_child_flags);
	slot--;
	Assert(PMSignalState->PMChildFlags[slot] == PM_CHILD_ACTIVE ||
		   PMSignalState->PMChildFlags[slot] == PM_CHILD_WALSENDER);
	PMSignalState->PMChildFlags[slot] = PM_CHILD_ASSIGNED;
}


/*
 * PostmasterIsAlive - check whether postmaster process is still alive
 */
bool
PostmasterIsAlive(void)
{
#ifndef WIN32
	char		c;
	ssize_t		rc;

	rc = read(postmaster_alive_fds[POSTMASTER_FD_WATCH], &c, 1);
	if (rc < 0)
	{
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return true;
		else
			elog(FATAL, "read on postmaster death monitoring pipe failed: %m");
	}
	else if (rc > 0)
		elog(FATAL, "unexpected data in postmaster death monitoring pipe");

	return false;
#else							/* WIN32 */
	return (WaitForSingleObject(PostmasterHandle, 0) == WAIT_TIMEOUT);
#endif   /* WIN32 */
}
