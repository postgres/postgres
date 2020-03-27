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
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/startup.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xlog.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/interrupt.h"
#include "postmaster/startup.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/pmsignal.h"
#include "storage/procsignal.h"
#include "storage/standby.h"
#include "utils/guc.h"
#include "utils/timeout.h"


/*
 * Flags set by interrupt handlers for later service in the redo loop.
 */
static volatile sig_atomic_t got_SIGHUP = false;
static volatile sig_atomic_t shutdown_requested = false;
static volatile sig_atomic_t promote_signaled = false;

/*
 * Flag set when executing a restore command, to tell SIGTERM signal handler
 * that it's safe to just proc_exit.
 */
static volatile sig_atomic_t in_restore_command = false;

/* Signal handlers */
static void StartupProcTriggerHandler(SIGNAL_ARGS);
static void StartupProcSigHupHandler(SIGNAL_ARGS);


/* --------------------------------
 *		signal handler routines
 * --------------------------------
 */

/* SIGUSR2: set flag to finish recovery */
static void
StartupProcTriggerHandler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	promote_signaled = true;
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

/*
 * Re-read the config file.
 *
 * If one of the critical walreceiver options has changed, flag xlog.c
 * to restart it.
 */
static void
StartupRereadConfig(void)
{
	char	   *conninfo = pstrdup(PrimaryConnInfo);
	char	   *slotname = pstrdup(PrimarySlotName);
	bool		tempSlot = wal_receiver_create_temp_slot;
	bool		conninfoChanged;
	bool		slotnameChanged;
	bool		tempSlotChanged = false;

	ProcessConfigFile(PGC_SIGHUP);

	conninfoChanged = strcmp(conninfo, PrimaryConnInfo) != 0;
	slotnameChanged = strcmp(slotname, PrimarySlotName) != 0;

	/*
	 * wal_receiver_create_temp_slot is used only when we have no slot
	 * configured.  We do not need to track this change if it has no effect.
	 */
	if (!slotnameChanged && strcmp(PrimarySlotName, "") == 0)
		tempSlotChanged = tempSlot != wal_receiver_create_temp_slot;
	pfree(conninfo);
	pfree(slotname);

	if (conninfoChanged || slotnameChanged || tempSlotChanged)
		StartupRequestWalReceiverRestart();
}

/* Handle various signals that might be sent to the startup process */
void
HandleStartupProcInterrupts(void)
{
	/*
	 * Process any requests or signals received recently.
	 */
	if (got_SIGHUP)
	{
		got_SIGHUP = false;
		StartupRereadConfig();
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

	/* Process barrier events */
	if (ProcSignalBarrierPending)
		ProcessProcSignalBarrier();
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
	pqsignal(SIGTERM, StartupProcShutdownHandler);	/* request shutdown */
	pqsignal(SIGQUIT, SignalHandlerForCrashExit);
	InitializeTimeouts();		/* establishes SIGALRM handler */
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, StartupProcTriggerHandler);

	/*
	 * Reset some signals that are accepted by postmaster but not here
	 */
	pqsignal(SIGCHLD, SIG_DFL);

	/*
	 * Register timeouts needed for standby mode
	 */
	RegisterTimeout(STANDBY_DEADLOCK_TIMEOUT, StandbyDeadLockHandler);
	RegisterTimeout(STANDBY_TIMEOUT, StandbyTimeoutHandler);
	RegisterTimeout(STANDBY_LOCK_TIMEOUT, StandbyLockTimeoutHandler);

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
IsPromoteSignaled(void)
{
	return promote_signaled;
}

void
ResetPromoteSignaled(void)
{
	promote_signaled = false;
}
