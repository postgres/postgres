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
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/startup.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xlog.h"
#include "access/xlogrecovery.h"
#include "access/xlogutils.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/auxprocess.h"
#include "postmaster/startup.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "storage/procsignal.h"
#include "storage/standby.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/timeout.h"


#ifndef USE_POSTMASTER_DEATH_SIGNAL
/*
 * On systems that need to make a system call to find out if the postmaster has
 * gone away, we'll do so only every Nth call to ProcessStartupProcInterrupts().
 * This only affects how long it takes us to detect the condition while we're
 * busy replaying WAL.  Latch waits and similar which should react immediately
 * through the usual techniques.
 */
#define POSTMASTER_POLL_RATE_LIMIT 1024
#endif

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

/*
 * Time at which the most recent startup operation started.
 */
static TimestampTz startup_progress_phase_start_time;

/*
 * Indicates whether the startup progress interval mentioned by the user is
 * elapsed or not. TRUE if timeout occurred, FALSE otherwise.
 */
static volatile sig_atomic_t startup_progress_timer_expired = false;

/*
 * Time between progress updates for long-running startup operations.
 */
int			log_startup_progress_interval = 10000;	/* 10 sec */

/* Signal handlers */
static void StartupProcTriggerHandler(SIGNAL_ARGS);
static void StartupProcSigHupHandler(SIGNAL_ARGS);

/* Callbacks */
static void StartupProcExit(int code, Datum arg);


/* --------------------------------
 *		signal handler routines
 * --------------------------------
 */

/* SIGUSR2: set flag to finish recovery */
static void
StartupProcTriggerHandler(SIGNAL_ARGS)
{
	promote_signaled = true;
	WakeupRecovery();
}

/* SIGHUP: set flag to re-read config file at next convenient time */
static void
StartupProcSigHupHandler(SIGNAL_ARGS)
{
	got_SIGHUP = true;
	WakeupRecovery();
}

/* SIGTERM: set flag to abort redo and exit */
static void
StartupProcShutdownHandler(SIGNAL_ARGS)
{
	if (in_restore_command)
		proc_exit(1);
	else
		shutdown_requested = true;
	WakeupRecovery();
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

/* Process various signals that might be sent to the startup process */
void
ProcessStartupProcInterrupts(void)
{
#ifdef POSTMASTER_POLL_RATE_LIMIT
	static uint32 postmaster_poll_count = 0;
#endif

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
	 * necessity for manual cleanup of all postmaster children.  Do this less
	 * frequently on systems for which we don't have signals to make that
	 * cheap.
	 */
	if (IsUnderPostmaster &&
#ifdef POSTMASTER_POLL_RATE_LIMIT
		postmaster_poll_count++ % POSTMASTER_POLL_RATE_LIMIT == 0 &&
#endif
		!PostmasterIsAlive())
		exit(1);

	/* Process barrier events */
	if (ProcSignalBarrierPending)
		ProcessProcSignalBarrier();

	/* Perform logging of memory contexts of this process */
	if (LogMemoryContextPending)
		ProcessLogMemoryContextInterrupt();

	/* Publish memory contexts of this process */
	if (PublishMemoryContextPending)
		ProcessGetMemoryContextInterrupt();
}


/* --------------------------------
 *		signal handler routines
 * --------------------------------
 */
static void
StartupProcExit(int code, Datum arg)
{
	/* Shutdown the recovery environment */
	if (standbyState != STANDBY_DISABLED)
		ShutdownRecoveryTransactionEnvironment();
}


/* ----------------------------------
 *	Startup Process main entry point
 * ----------------------------------
 */
void
StartupProcessMain(const void *startup_data, size_t startup_data_len)
{
	Assert(startup_data_len == 0);

	MyBackendType = B_STARTUP;
	AuxiliaryProcessMainCommon();

	/* Arrange to clean up at startup process exit */
	on_shmem_exit(StartupProcExit, 0);

	/*
	 * Properly accept or ignore signals the postmaster might send us.
	 */
	pqsignal(SIGHUP, StartupProcSigHupHandler); /* reload config file */
	pqsignal(SIGINT, SIG_IGN);	/* ignore query cancel */
	pqsignal(SIGTERM, StartupProcShutdownHandler);	/* request shutdown */
	/* SIGQUIT handler was already set up by InitPostmasterChild */
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
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

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

/*
 * Set a flag indicating that it's time to log a progress report.
 */
void
startup_progress_timeout_handler(void)
{
	startup_progress_timer_expired = true;
}

void
disable_startup_progress_timeout(void)
{
	/* Feature is disabled. */
	if (log_startup_progress_interval == 0)
		return;

	disable_timeout(STARTUP_PROGRESS_TIMEOUT, false);
	startup_progress_timer_expired = false;
}

/*
 * Set the start timestamp of the current operation and enable the timeout.
 */
void
enable_startup_progress_timeout(void)
{
	TimestampTz fin_time;

	/* Feature is disabled. */
	if (log_startup_progress_interval == 0)
		return;

	startup_progress_phase_start_time = GetCurrentTimestamp();
	fin_time = TimestampTzPlusMilliseconds(startup_progress_phase_start_time,
										   log_startup_progress_interval);
	enable_timeout_every(STARTUP_PROGRESS_TIMEOUT, fin_time,
						 log_startup_progress_interval);
}

/*
 * A thin wrapper to first disable and then enable the startup progress
 * timeout.
 */
void
begin_startup_progress_phase(void)
{
	/* Feature is disabled. */
	if (log_startup_progress_interval == 0)
		return;

	disable_startup_progress_timeout();
	enable_startup_progress_timeout();
}

/*
 * Report whether startup progress timeout has occurred. Reset the timer flag
 * if it did, set the elapsed time to the out parameters and return true,
 * otherwise return false.
 */
bool
has_startup_progress_timeout_expired(long *secs, int *usecs)
{
	long		seconds;
	int			useconds;
	TimestampTz now;

	/* No timeout has occurred. */
	if (!startup_progress_timer_expired)
		return false;

	/* Calculate the elapsed time. */
	now = GetCurrentTimestamp();
	TimestampDifference(startup_progress_phase_start_time, now, &seconds, &useconds);

	*secs = seconds;
	*usecs = useconds;
	startup_progress_timer_expired = false;

	return true;
}
