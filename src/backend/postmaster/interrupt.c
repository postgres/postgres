/*-------------------------------------------------------------------------
 *
 * interrupt.c
 *	  Interrupt handling routines.
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/interrupt.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>

#include "miscadmin.h"
#include "postmaster/interrupt.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/procsignal.h"
#include "utils/guc.h"

volatile sig_atomic_t ConfigReloadPending = false;
volatile sig_atomic_t ShutdownRequestPending = false;

/*
 * Simple interrupt handler for main loops of background processes.
 */
void
HandleMainLoopInterrupts(void)
{
	if (ProcSignalBarrierPending)
		ProcessProcSignalBarrier();

	if (ConfigReloadPending)
	{
		ConfigReloadPending = false;
		ProcessConfigFile(PGC_SIGHUP);
	}

	if (ShutdownRequestPending)
		proc_exit(0);
}

/*
 * Simple signal handler for triggering a configuration reload.
 *
 * Normally, this handler would be used for SIGHUP. The idea is that code
 * which uses it would arrange to check the ConfigReloadPending flag at
 * convenient places inside main loops, or else call HandleMainLoopInterrupts.
 */
void
SignalHandlerForConfigReload(SIGNAL_ARGS)
{
	int			save_errno = errno;

	ConfigReloadPending = true;
	SetLatch(MyLatch);

	errno = save_errno;
}

/*
 * Simple signal handler for exiting quickly as if due to a crash.
 *
 * Normally, this would be used for handling SIGQUIT.
 */
void
SignalHandlerForCrashExit(SIGNAL_ARGS)
{
	/*
	 * We DO NOT want to run proc_exit() or atexit() callbacks -- we're here
	 * because shared memory may be corrupted, so we don't want to try to
	 * clean up our transaction.  Just nail the windows shut and get out of
	 * town.  The callbacks wouldn't be safe to run from a signal handler,
	 * anyway.
	 *
	 * Note we do _exit(2) not _exit(0).  This is to force the postmaster into
	 * a system reset cycle if someone sends a manual SIGQUIT to a random
	 * backend.  This is necessary precisely because we don't clean up our
	 * shared memory state.  (The "dead man switch" mechanism in pmsignal.c
	 * should ensure the postmaster sees this as a crash, too, but no harm in
	 * being doubly sure.)
	 */
	_exit(2);
}

/*
 * Simple signal handler for triggering a long-running background process to
 * shut down and exit.
 *
 * Typically, this handler would be used for SIGTERM, but some processes use
 * other signals. In particular, the checkpointer exits on SIGUSR2, the
 * stats collector on SIGQUIT, and the WAL writer exits on either SIGINT
 * or SIGTERM.
 *
 * ShutdownRequestPending should be checked at a convenient place within the
 * main loop, or else the main loop should call HandleMainLoopInterrupts.
 */
void
SignalHandlerForShutdownRequest(SIGNAL_ARGS)
{
	int			save_errno = errno;

	ShutdownRequestPending = true;
	SetLatch(MyLatch);

	errno = save_errno;
}
