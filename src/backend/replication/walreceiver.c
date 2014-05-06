/*-------------------------------------------------------------------------
 *
 * walreceiver.c
 *
 * The WAL receiver process (walreceiver) is new as of Postgres 9.0. It
 * is the process in the standby server that takes charge of receiving
 * XLOG records from a primary server during streaming replication.
 *
 * When the startup process determines that it's time to start streaming,
 * it instructs postmaster to start walreceiver. Walreceiver first connects
 * to the primary server (it will be served by a walsender process
 * in the primary server), and then keeps receiving XLOG records and
 * writing them to the disk as long as the connection is alive. As XLOG
 * records are received and flushed to disk, it updates the
 * WalRcv->receivedUpTo variable in shared memory, to inform the startup
 * process of how far it can proceed with XLOG replay.
 *
 * Normal termination is by SIGTERM, which instructs the walreceiver to
 * exit(0). Emergency termination is by SIGQUIT; like any postmaster child
 * process, the walreceiver will simply abort and exit on SIGQUIT. A close
 * of the connection and a FATAL error are treated not as a crash but as
 * normal operation.
 *
 * This file contains the server-facing parts of walreceiver. The libpq-
 * specific parts are in the libpqwalreceiver module. It's loaded
 * dynamically to avoid linking the server with libpq.
 *
 * Portions Copyright (c) 2010-2010, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/replication/walreceiver.c,v 1.16 2010/07/06 19:18:57 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <unistd.h>

#include "access/xlog_internal.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "replication/walprotocol.h"
#include "replication/walreceiver.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/resowner.h"

/* Global variable to indicate if this process is a walreceiver process */
bool		am_walreceiver;

/* libpqreceiver hooks to these when loaded */
walrcv_connect_type walrcv_connect = NULL;
walrcv_receive_type walrcv_receive = NULL;
walrcv_disconnect_type walrcv_disconnect = NULL;

#define NAPTIME_PER_CYCLE 100	/* max sleep time between cycles (100ms) */

/*
 * These variables are used similarly to openLogFile/Id/Seg/Off,
 * but for walreceiver to write the XLOG.
 */
static int	recvFile = -1;
static uint32 recvId = 0;
static uint32 recvSeg = 0;
static uint32 recvOff = 0;

/*
 * Flags set by interrupt handlers of walreceiver for later service in the
 * main loop.
 */
static volatile sig_atomic_t got_SIGHUP = false;
static volatile sig_atomic_t got_SIGTERM = false;

/*
 * LogstreamResult indicates the byte positions that we have already
 * written/fsynced.
 */
static struct
{
	XLogRecPtr	Write;			/* last byte + 1 written out in the standby */
	XLogRecPtr	Flush;			/* last byte + 1 flushed in the standby */
}	LogstreamResult;

/*
 * About SIGTERM handling:
 *
 * We can't just exit(1) within SIGTERM signal handler, because the signal
 * might arrive in the middle of some critical operation, like while we're
 * holding a spinlock. We also can't just set a flag in signal handler and
 * check it in the main loop, because we perform some blocking operations
 * like libpqrcv_PQexec(), which can take a long time to finish.
 *
 * We use a combined approach: When WalRcvImmediateInterruptOK is true, it's
 * safe for the signal handler to elog(FATAL) immediately. Otherwise it just
 * sets got_SIGTERM flag, which is checked in the main loop when convenient.
 *
 * This is very much like what regular backends do with ImmediateInterruptOK,
 * ProcessInterrupts() etc.
 */
static volatile bool WalRcvImmediateInterruptOK = false;

/* Prototypes for private functions */
static void ProcessWalRcvInterrupts(void);
static void EnableWalRcvImmediateExit(void);
static void DisableWalRcvImmediateExit(void);
static void WalRcvDie(int code, Datum arg);
static void XLogWalRcvProcessMsg(unsigned char type, char *buf, Size len);
static void XLogWalRcvWrite(char *buf, Size nbytes, XLogRecPtr recptr);
static void XLogWalRcvFlush(void);

/* Signal handlers */
static void WalRcvSigHupHandler(SIGNAL_ARGS);
static void WalRcvShutdownHandler(SIGNAL_ARGS);
static void WalRcvQuickDieHandler(SIGNAL_ARGS);


static void
ProcessWalRcvInterrupts(void)
{
	/*
	 * Although walreceiver interrupt handling doesn't use the same scheme as
	 * regular backends, call CHECK_FOR_INTERRUPTS() to make sure we receive
	 * any incoming signals on Win32.
	 */
	CHECK_FOR_INTERRUPTS();

	if (got_SIGTERM)
	{
		WalRcvImmediateInterruptOK = false;
		ereport(FATAL,
				(errcode(ERRCODE_ADMIN_SHUTDOWN),
				 errmsg("terminating walreceiver process due to administrator command")));
	}
}

static void
EnableWalRcvImmediateExit(void)
{
	WalRcvImmediateInterruptOK = true;
	ProcessWalRcvInterrupts();
}

static void
DisableWalRcvImmediateExit(void)
{
	WalRcvImmediateInterruptOK = false;
	ProcessWalRcvInterrupts();
}

/* Main entry point for walreceiver process */
void
WalReceiverMain(void)
{
	char		conninfo[MAXCONNINFO];
	XLogRecPtr	startpoint;

	/* use volatile pointer to prevent code rearrangement */
	volatile WalRcvData *walrcv = WalRcv;

	am_walreceiver = true;

	/*
	 * WalRcv should be set up already (if we are a backend, we inherit this
	 * by fork() or EXEC_BACKEND mechanism from the postmaster).
	 */
	Assert(walrcv != NULL);

	/*
	 * Mark walreceiver as running in shared memory.
	 *
	 * Do this as early as possible, so that if we fail later on, we'll set
	 * state to STOPPED. If we die before this, the startup process will keep
	 * waiting for us to start up, until it times out.
	 */
	SpinLockAcquire(&walrcv->mutex);
	Assert(walrcv->pid == 0);
	switch (walrcv->walRcvState)
	{
		case WALRCV_STOPPING:
			/* If we've already been requested to stop, don't start up. */
			walrcv->walRcvState = WALRCV_STOPPED;
			/* fall through */

		case WALRCV_STOPPED:
			SpinLockRelease(&walrcv->mutex);
			proc_exit(1);
			break;

		case WALRCV_STARTING:
			/* The usual case */
			break;

		case WALRCV_RUNNING:
			/* Shouldn't happen */
			elog(PANIC, "walreceiver still running according to shared memory state");
	}
	/* Advertise our PID so that the startup process can kill us */
	walrcv->pid = MyProcPid;
	walrcv->walRcvState = WALRCV_RUNNING;

	/* Fetch information required to start streaming */
	strlcpy(conninfo, (char *) walrcv->conninfo, MAXCONNINFO);
	startpoint = walrcv->receivedUpto;
	SpinLockRelease(&walrcv->mutex);

	/* Arrange to clean up at walreceiver exit */
	on_shmem_exit(WalRcvDie, 0);

	/*
	 * If possible, make this process a group leader, so that the postmaster
	 * can signal any child processes too.  (walreceiver probably never has
	 * any child processes, but for consistency we make all postmaster child
	 * processes do this.)
	 */
#ifdef HAVE_SETSID
	if (setsid() < 0)
		elog(FATAL, "setsid() failed: %m");
#endif

	/* Properly accept or ignore signals the postmaster might send us */
	pqsignal(SIGHUP, WalRcvSigHupHandler);		/* set flag to read config
												 * file */
	pqsignal(SIGINT, SIG_IGN);
	pqsignal(SIGTERM, WalRcvShutdownHandler);	/* request shutdown */
	pqsignal(SIGQUIT, WalRcvQuickDieHandler);	/* hard crash time */
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, SIG_IGN);
	pqsignal(SIGUSR2, SIG_IGN);

	/* Reset some signals that are accepted by postmaster but not here */
	pqsignal(SIGCHLD, SIG_DFL);
	pqsignal(SIGTTIN, SIG_DFL);
	pqsignal(SIGTTOU, SIG_DFL);
	pqsignal(SIGCONT, SIG_DFL);
	pqsignal(SIGWINCH, SIG_DFL);

	/* We allow SIGQUIT (quickdie) at all times */
	sigdelset(&BlockSig, SIGQUIT);

	/* Load the libpq-specific functions */
	load_file("libpqwalreceiver", false);
	if (walrcv_connect == NULL || walrcv_receive == NULL ||
		walrcv_disconnect == NULL)
		elog(ERROR, "libpqwalreceiver didn't initialize correctly");

	/*
	 * Create a resource owner to keep track of our resources (not clear that
	 * we need this, but may as well have one).
	 */
	CurrentResourceOwner = ResourceOwnerCreate(NULL, "Wal Receiver");

	/* Unblock signals (they were blocked when the postmaster forked us) */
	PG_SETMASK(&UnBlockSig);

	/* Establish the connection to the primary for XLOG streaming */
	EnableWalRcvImmediateExit();
	walrcv_connect(conninfo, startpoint);
	DisableWalRcvImmediateExit();

	/* Loop until end-of-streaming or error */
	for (;;)
	{
		unsigned char type;
		char	   *buf;
		int			len;

		/*
		 * Emergency bailout if postmaster has died.  This is to avoid the
		 * necessity for manual cleanup of all postmaster children.
		 */
		if (!PostmasterIsAlive(true))
			exit(1);

		/*
		 * Exit walreceiver if we're not in recovery. This should not happen,
		 * but cross-check the status here.
		 */
		if (!RecoveryInProgress())
			ereport(FATAL,
					(errmsg("cannot continue WAL streaming, recovery has already ended")));

		/* Process any requests or signals received recently */
		ProcessWalRcvInterrupts();

		if (got_SIGHUP)
		{
			got_SIGHUP = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		/* Wait a while for data to arrive */
		if (walrcv_receive(NAPTIME_PER_CYCLE, &type, &buf, &len))
		{
			/* Accept the received data, and process it */
			XLogWalRcvProcessMsg(type, buf, len);

			/* Receive any more data we can without sleeping */
			while (walrcv_receive(0, &type, &buf, &len))
				XLogWalRcvProcessMsg(type, buf, len);

			/*
			 * If we've written some records, flush them to disk and let the
			 * startup process know about them.
			 */
			XLogWalRcvFlush();
		}
	}
}

/*
 * Mark us as STOPPED in shared memory at exit.
 */
static void
WalRcvDie(int code, Datum arg)
{
	/* use volatile pointer to prevent code rearrangement */
	volatile WalRcvData *walrcv = WalRcv;

	/* Ensure that all WAL records received are flushed to disk */
	XLogWalRcvFlush();

	SpinLockAcquire(&walrcv->mutex);
	Assert(walrcv->walRcvState == WALRCV_RUNNING ||
		   walrcv->walRcvState == WALRCV_STOPPING);
	walrcv->walRcvState = WALRCV_STOPPED;
	walrcv->pid = 0;
	SpinLockRelease(&walrcv->mutex);

	/* Terminate the connection gracefully. */
	if (walrcv_disconnect != NULL)
		walrcv_disconnect();
}

/* SIGHUP: set flag to re-read config file at next convenient time */
static void
WalRcvSigHupHandler(SIGNAL_ARGS)
{
	got_SIGHUP = true;
}

/* SIGTERM: set flag for main loop, or shutdown immediately if safe */
static void
WalRcvShutdownHandler(SIGNAL_ARGS)
{
	got_SIGTERM = true;

	/* Don't joggle the elbow of proc_exit */
	if (!proc_exit_inprogress && WalRcvImmediateInterruptOK)
		ProcessWalRcvInterrupts();
}

/*
 * WalRcvQuickDieHandler() occurs when signalled SIGQUIT by the postmaster.
 *
 * Some backend has bought the farm, so we need to stop what we're doing and
 * exit.
 */
static void
WalRcvQuickDieHandler(SIGNAL_ARGS)
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

/*
 * Accept the message from XLOG stream, and process it.
 */
static void
XLogWalRcvProcessMsg(unsigned char type, char *buf, Size len)
{
	switch (type)
	{
		case 'w':				/* WAL records */
			{
				WalDataMessageHeader msghdr;

				if (len < sizeof(WalDataMessageHeader))
					ereport(ERROR,
							(errcode(ERRCODE_PROTOCOL_VIOLATION),
							 errmsg_internal("invalid WAL message received from primary")));
				/* memcpy is required here for alignment reasons */
				memcpy(&msghdr, buf, sizeof(WalDataMessageHeader));
				buf += sizeof(WalDataMessageHeader);
				len -= sizeof(WalDataMessageHeader);

				XLogWalRcvWrite(buf, len, msghdr.dataStart);
				break;
			}
		default:
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg_internal("invalid replication message type %d",
									 type)));
	}
}

/*
 * Write XLOG data to disk.
 */
static void
XLogWalRcvWrite(char *buf, Size nbytes, XLogRecPtr recptr)
{
	int			startoff;
	int			byteswritten;

	while (nbytes > 0)
	{
		int			segbytes;

		if (recvFile < 0 || !XLByteInSeg(recptr, recvId, recvSeg))
		{
			bool		use_existent;

			/*
			 * fsync() and close current file before we switch to next one. We
			 * would otherwise have to reopen this file to fsync it later
			 */
			if (recvFile >= 0)
			{
				XLogWalRcvFlush();

				/*
				 * XLOG segment files will be re-read by recovery in startup
				 * process soon, so we don't advise the OS to release cache
				 * pages associated with the file like XLogFileClose() does.
				 */
				if (close(recvFile) != 0)
					ereport(PANIC,
							(errcode_for_file_access(),
						errmsg("could not close log file %u, segment %u: %m",
							   recvId, recvSeg)));
			}
			recvFile = -1;

			/* Create/use new log file */
			XLByteToSeg(recptr, recvId, recvSeg);
			use_existent = true;
			recvFile = XLogFileInit(recvId, recvSeg, &use_existent, true);
			recvOff = 0;
		}

		/* Calculate the start offset of the received logs */
		startoff = recptr.xrecoff % XLogSegSize;

		if (startoff + nbytes > XLogSegSize)
			segbytes = XLogSegSize - startoff;
		else
			segbytes = nbytes;

		/* Need to seek in the file? */
		if (recvOff != startoff)
		{
			if (lseek(recvFile, (off_t) startoff, SEEK_SET) < 0)
				ereport(PANIC,
						(errcode_for_file_access(),
						 errmsg("could not seek in log file %u, "
								"segment %u to offset %u: %m",
								recvId, recvSeg, startoff)));
			recvOff = startoff;
		}

		/* OK to write the logs */
		errno = 0;

		byteswritten = write(recvFile, buf, segbytes);
		if (byteswritten <= 0)
		{
			/* if write didn't set errno, assume no disk space */
			if (errno == 0)
				errno = ENOSPC;
			ereport(PANIC,
					(errcode_for_file_access(),
					 errmsg("could not write to log file %u, segment %u "
							"at offset %u, length %lu: %m",
							recvId, recvSeg,
							recvOff, (unsigned long) segbytes)));
		}

		/* Update state for write */
		XLByteAdvance(recptr, byteswritten);

		recvOff += byteswritten;
		nbytes -= byteswritten;
		buf += byteswritten;

		LogstreamResult.Write = recptr;
	}
}

/* Flush the log to disk */
static void
XLogWalRcvFlush(void)
{
	if (XLByteLT(LogstreamResult.Flush, LogstreamResult.Write))
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile WalRcvData *walrcv = WalRcv;

		issue_xlog_fsync(recvFile, recvId, recvSeg);

		LogstreamResult.Flush = LogstreamResult.Write;

		/* Update shared-memory status */
		SpinLockAcquire(&walrcv->mutex);
		walrcv->latestChunkStart = walrcv->receivedUpto;
		walrcv->receivedUpto = LogstreamResult.Flush;
		SpinLockRelease(&walrcv->mutex);

		/* Report XLOG streaming progress in PS display */
		if (update_process_title)
		{
			char		activitymsg[50];

			snprintf(activitymsg, sizeof(activitymsg), "streaming %X/%X",
					 LogstreamResult.Write.xlogid,
					 LogstreamResult.Write.xrecoff);
			set_ps_display(activitymsg, false);
		}
	}
}
