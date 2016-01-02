/*-------------------------------------------------------------------------
 *
 * startup.c
 *
 * The Startup process initialises the server and performs any recovery
 * actions that have been specified. Notice that there is no "main loop"
 * since the Startup process ends as soon as initialisation is complete.
 * (in standby mode, one can think of the replay loop as a main loop,
 * though.)
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/startup.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <unistd.h>

#include "access/xlog.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/startup.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/pmsignal.h"
#include "storage/standby.h"
#include "utils/guc.h"
#include "utils/timeout.h"


/*
 * Flags set by interrupt handlers for later service in the redo loop.
 */
static volatile sig_atomic_t got_SIGHUP = false;
static volatile sig_atomic_t shutdown_requested = false;
static volatile sig_atomic_t promote_triggered = false;

/*
 * Flag set when executing a restore command, to tell SIGTERM signal handler
 * that it's safe to just proc_exit.
 */
static volatile sig_atomic_t in_restore_command = false;

/* Signal handlers */
static void startupproc_quickdie(SIGNAL_ARGS);
static void StartupProcSigUsr1Handler(SIGNAL_ARGS);
static void StartupProcTriggerHandler(SIGNAL_ARGS);
static void StartupProcSigHupHandler(SIGNAL_ARGS);


/* --------------------------------
 *		signal handler routines
 * --------------------------------
 */

/*
 * startupproc_quickdie() occurs when signalled SIGQUIT by the postmaster.
 *
 * Some backend has bought the farm,
 * so we need to stop what we're doing and exit.
 */
static void
startupproc_quickdie(SIGNAL_ARGS)
{
	PG_SETMASK(&BlockSig);

	/*
	 * We DO NOT want to run proc_exit() callbacks -- we're here because
	 * shared memory may be corrupted, so we don't want to try to clean up our
	 * transaction.  Just nail the windows shut and get out of town.  Now that
	 * there's an atexit callback to prevent third-party code from breaking
	 * things by calling exit() directly, we have to reset the callbacks
	 * explicitly to make this work as intended.
	 */
	on_exit_reset();

	/*
	 * Note we do exit(2) not exit(0).  This is to force the postmaster into a
	 * system reset cycle if some idiot DBA sends a manual SIGQUIT to a random
	 * backend.  This is necessary precisely because we don't clean up our
	 * shared memory state.  (The "dead man switch" mechanism in pmsignal.c
	 * should ensure the postmaster sees this as a crash, too, but no harm in
	 * being doubly sure.)
	 */
	exit(2);
}


/* SIGUSR1: let latch facility handle the signal */
static void
StartupProcSigUsr1Handler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	latch_sigusr1_handler();

	errno = save_errno;
}

/* SIGUSR2: set flag to finish recovery */
static void
StartupProcTriggerHandler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	promote_triggered = true;
	WakeupRecovery();

	errno = save_errno;
}

/* SIGHUP: set flag to re-read config file at next convenient time */
static void
StartupProcSigHupHandler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	got_SIGHUP = true;
	WakeupRecovery();

	errno = save_errno;
}

/* SIGTERM: set flag to abort redo and exit */
static void
StartupProcShutdownHandler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	if (in_restore_command)
		proc_exit(1);
	else
		shutdown_requested = true;
	WakeupRecovery();

	errno = save_errno;
}

/* Handle SIGHUP and SIGTERM signals of startup process */
void
HandleStartupProcInterrupts(void)
{
	/*
	 * Check if we were requested to re-read config file.
	 */
	if (got_SIGHUP)
	{
		got_SIGHUP = false;
		ProcessConfigFile(PGC_SIGHUP);
	}

	/*
	 * Check if we were requested to exit without finishing recovery.
	 */
	if (shutdown_requested)
		proc_exit(1);

	/*
	 * Emergency bailout if postmaster has died.  This is to avoid the
	 * necessity for manual cleanup of all postmaster children.
	 */
	if (IsUnderPostmaster && !PostmasterIsAlive())
		exit(1);
}


/* ----------------------------------
 *	Startup Process main entry point
 * ----------------------------------
 */
void
StartupProcessMain(void)
{
	/*
	 * Properly accept or ignore signals the postmaster might send us.
	 */
	pqsignal(SIGHUP, StartupProcSigHupHandler); /* reload config file */
	pqsignal(SIGINT, SIG_IGN);	/* ignore query cancel */
	pqsignal(SIGTERM, StartupProcShutdownHandler);		/* request shutdown */
	pqsignal(SIGQUIT, startupproc_quickdie);	/* hard crash time */
	InitializeTimeouts();		/* establishes SIGALRM handler */
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, StartupProcSigUsr1Handler);
	pqsignal(SIGUSR2, StartupProcTriggerHandler);

	/*
	 * Reset some signals that are accepted by postmaster but not here
	 */
	pqsignal(SIGCHLD, SIG_DFL);
	pqsignal(SIGTTIN, SIG_DFL);
	pqsignal(SIGTTOU, SIG_DFL);
	pqsignal(SIGCONT, SIG_DFL);
	pqsignal(SIGWINCH, SIG_DFL);

	/*
	 * Register timeouts needed for standby mode
	 */
	RegisterTimeout(STANDBY_DEADLOCK_TIMEOUT, StandbyDeadLockHandler);
	RegisterTimeout(STANDBY_TIMEOUT, StandbyTimeoutHandler);

	/*
	 * Unblock signals (they were blocked when the postmaster forked us)
	 */
	PG_SETMASK(&UnBlockSig);

	/*
	 * Do what we came for.
	 */
	StartupXLOG();

	/*
	 * Exit normally. Exit code 0 tells postmaster that we completed recovery
	 * successfully.
	 */
	proc_exit(0);
}

void
PreRestoreCommand(void)
{
	/*
	 * Set in_restore_command to tell the signal handler that we should exit
	 * right away on SIGTERM. We know that we're at a safe point to do that.
	 * Check if we had already received the signal, so that we don't miss a
	 * shutdown request received just before this.
	 */
	in_restore_command = true;
	if (shutdown_requested)
		proc_exit(1);
}

void
PostRestoreCommand(void)
{
	in_restore_command = false;
}

bool
IsPromoteTriggered(void)
{
	return promote_triggered;
}

void
ResetPromoteTriggered(void)
{
	promote_triggered = false;
}
