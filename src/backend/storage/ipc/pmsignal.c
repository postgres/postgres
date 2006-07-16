/*-------------------------------------------------------------------------
 *
 * pmsignal.c
 *	  routines for signaling the postmaster from its child processes
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/ipc/pmsignal.c,v 1.23 2006/07/16 01:05:23 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <unistd.h>

#include "miscadmin.h"
#include "postmaster/postmaster.h"
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
 */

static volatile sig_atomic_t *PMSignalFlags;


/*
 * PMSignalInit - initialize during shared-memory creation
 */
void
PMSignalInit(void)
{
	bool		found;

	PMSignalFlags = (sig_atomic_t *)
		ShmemInitStruct("PMSignalFlags",
						NUM_PMSIGNALS * sizeof(sig_atomic_t),
						&found);

	if (!found)
		MemSet(PMSignalFlags, 0, NUM_PMSIGNALS * sizeof(sig_atomic_t));
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
	PMSignalFlags[reason] = true;
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
	if (PMSignalFlags[reason])
	{
		PMSignalFlags[reason] = false;
		return true;
	}
	return false;
}

/*
 * PostmasterIsAlive - check whether postmaster process is still alive
 *
 * amDirectChild should be passed as "true" by code that knows it is
 * executing in a direct child process of the postmaster; pass "false"
 * if an indirect child or not sure.  The "true" case uses a faster and
 * more reliable test, so use it when possible.
 */
bool
PostmasterIsAlive(bool amDirectChild)
{
#ifndef WIN32
	if (amDirectChild)
	{
		/*
		 * If the postmaster is alive, we'll still be its child.  If it's
		 * died, we'll be reassigned as a child of the init process.
		 */
		return (getppid() == PostmasterPid);
	}
	else
	{
		/*
		 * Use kill() to see if the postmaster is still alive.	This can
		 * sometimes give a false positive result, since the postmaster's PID
		 * may get recycled, but it is good enough for existing uses by
		 * indirect children.
		 */
		return (kill(PostmasterPid, 0) == 0);
	}
#else							/* WIN32 */
	return (WaitForSingleObject(PostmasterHandle, 0) == WAIT_TIMEOUT);
#endif   /* WIN32 */
}
