/*-------------------------------------------------------------------------
 *
 * walreceiver.c
 *
 * The WAL receiver process (walreceiver) is new as of Postgres 8.5. It
 * is the process in the standby server that takes charge of receiving
 * XLOG records from a primary server during streaming replication.
 *
 * When the startup process determines that it's time to start streaming,
 * it instructs postmaster to start walreceiver. Walreceiver first connects
 * connects to the primary server (it will be served by a walsender process
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
 * Walreceiver is a postmaster child process like others, but it's compiled
 * as a dynamic module to avoid linking libpq with the main server binary.
 *
 * Portions Copyright (c) 2010-2010, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/replication/walreceiver/walreceiver.c,v 1.2 2010/01/16 01:55:28 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <sys/time.h>

#include "access/xlog_internal.h"
#include "libpq-fe.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "replication/walreceiver.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/resowner.h"

#ifdef HAVE_POLL_H
#include <poll.h>
#endif
#ifdef HAVE_SYS_POLL_H
#include <sys/poll.h>
#endif
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(WalReceiverMain);
Datum WalReceiverMain(PG_FUNCTION_ARGS);

/* streamConn is a PGconn object of a connection to walsender from walreceiver */
static PGconn *streamConn = NULL;

#define NAPTIME_PER_CYCLE 100	/* max sleep time between cycles (100ms) */

/*
 * These variables are used similarly to openLogFile/Id/Seg/Off,
 * but for walreceiver to write the XLOG.
 */
static int	recvFile = -1;
static uint32 recvId = 0;
static uint32 recvSeg = 0;
static uint32 recvOff = 0;

/* Buffer for currently read records */
static char *recvBuf = NULL;

/* Flags set by interrupt handlers of walreceiver for later service in the main loop */
static volatile sig_atomic_t got_SIGHUP = false;
static volatile sig_atomic_t got_SIGTERM = false;

static void ProcessWalRcvInterrupts(void);
static void EnableImmediateExit(void);
static void DisableImmediateExit(void);

/*
 * About SIGTERM handling:
 *
 * We can't just exit(1) within SIGTERM signal handler, because the signal
 * might arrive in the middle of some critical operation, like while we're
 * holding a spinlock. We also can't just set a flag in signal handler and
 * check it in the main loop, because we perform some blocking libpq
 * operations like PQexec(), which can take a long time to finish.
 *
 * We use a combined approach: When WalRcvImmediateInterruptOK is true, it's
 * safe for the signal handler to elog(FATAL) immediately. Otherwise it just
 * sets got_SIGTERM flag, which is checked in the main loop when convenient.
 *
 * This is very much like what regular backends do with ImmediateInterruptOK,
 * ProcessInterrupts() etc.
 */
static volatile bool WalRcvImmediateInterruptOK = false;

static void
ProcessWalRcvInterrupts(void)
{
	/*
	 * Although walreceiver interrupt handling doesn't use the same scheme
	 * as regular backends, call CHECK_FOR_INTERRUPTS() to make sure we
	 * receive any incoming signals on Win32.
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
EnableImmediateExit()
{
	WalRcvImmediateInterruptOK = true;
	ProcessWalRcvInterrupts();
}

static void
DisableImmediateExit()
{
	WalRcvImmediateInterruptOK = false;
	ProcessWalRcvInterrupts();
}

/* Signal handlers */
static void WalRcvSigHupHandler(SIGNAL_ARGS);
static void WalRcvShutdownHandler(SIGNAL_ARGS);
static void WalRcvQuickDieHandler(SIGNAL_ARGS);

/* Prototypes for private functions */
static void WalRcvLoop(void);
static void InitWalRcv(void);
static void WalRcvConnect(void);
static bool WalRcvWait(int timeout_ms);
static void WalRcvKill(int code, Datum arg);
static void XLogRecv(void);
static void XLogWalRcvWrite(char *buf, Size nbytes, XLogRecPtr recptr);
static void XLogWalRcvFlush(void);

/*
 * LogstreamResult indicates the byte positions that we have already
 * written/fsynced.
 */
static struct
{
	XLogRecPtr	Write;	/* last byte + 1 written out in the standby */
	XLogRecPtr	Flush;	/* last byte + 1 flushed in the standby */
} LogstreamResult;

/* Main entry point for walreceiver process */
Datum
WalReceiverMain(PG_FUNCTION_ARGS)
{
	sigjmp_buf	local_sigjmp_buf;
	MemoryContext walrcv_context;

	/* Mark walreceiver in progress */
	InitWalRcv();

	/*
	 * If possible, make this process a group leader, so that the postmaster
	 * can signal any child processes too.	(walreceiver probably never has
	 * any child processes, but for consistency we make all postmaster child
	 * processes do this.)
	 */
#ifdef HAVE_SETSID
	if (setsid() < 0)
		elog(FATAL, "setsid() failed: %m");
#endif

	/* Properly accept or ignore signals the postmaster might send us */
	pqsignal(SIGHUP, WalRcvSigHupHandler);		/* set flag to read config file */
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

	/*
	 * Create a resource owner to keep track of our resources (not clear that
	 * we need this, but may as well have one).
	 */
	CurrentResourceOwner = ResourceOwnerCreate(NULL, "Wal Receiver");

	/*
	 * Create a memory context that we will do all our work in.  We do this so
	 * that we can reset the context during error recovery and thereby avoid
	 * possible memory leaks.
	 */
	walrcv_context = AllocSetContextCreate(TopMemoryContext,
											  "Wal Receiver",
											  ALLOCSET_DEFAULT_MINSIZE,
											  ALLOCSET_DEFAULT_INITSIZE,
											  ALLOCSET_DEFAULT_MAXSIZE);
	MemoryContextSwitchTo(walrcv_context);

	/*
	 * If an exception is encountered, processing resumes here.
	 *
	 * This code is heavily based on bgwriter.c, q.v.
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* Since not using PG_TRY, must reset error stack by hand */
		error_context_stack = NULL;

		/* Reset WalRcvImmediateInterruptOK */
		DisableImmediateExit();

		/* Prevent interrupts while cleaning up */
		HOLD_INTERRUPTS();

		/* Report the error to the server log */
		EmitErrorReport();

		/* Free the data structure related to a connection */
		PQfinish(streamConn);
		streamConn = NULL;
		if (recvBuf != NULL)
			PQfreemem(recvBuf);
		recvBuf = NULL;

		/*
		 * Now return to normal top-level context and clear ErrorContext for
		 * next time.
		 */
		MemoryContextSwitchTo(walrcv_context);
		FlushErrorState();

		/* Flush any leaked data in the top-level context */
		MemoryContextResetAndDeleteChildren(walrcv_context);

		/* Now we can allow interrupts again */
		RESUME_INTERRUPTS();

		/*
		 * Sleep at least 1 second after any error.  A write error is likely
		 * to be repeated, and we don't want to be filling the error logs as
		 * fast as we can.
		 */
		pg_usleep(1000000L);
	}

	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	/* Unblock signals (they were blocked when the postmaster forked us) */
	PG_SETMASK(&UnBlockSig);

	/* Establish the connection to the primary for XLOG streaming */
	WalRcvConnect();

	/* Main loop of walreceiver */
	WalRcvLoop();

	PG_RETURN_VOID(); /* WalRcvLoop() never returns, but keep compiler quiet */
}

/* Main loop of walreceiver process */
static void
WalRcvLoop(void)
{
	/* Loop until end-of-streaming or error */
	for (;;)
	{
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
					(errmsg("cannot continue XLOG streaming, recovery has already ended")));

		/* Process any requests or signals received recently */
		ProcessWalRcvInterrupts();

		if (got_SIGHUP)
		{
			got_SIGHUP = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		/* Wait a while for data to arrive */
		if (WalRcvWait(NAPTIME_PER_CYCLE))
		{
			/* data has arrived. Process it */
			if (PQconsumeInput(streamConn) == 0)
				ereport(ERROR,
						(errmsg("could not read xlog records: %s",
								PQerrorMessage(streamConn))));
			XLogRecv();
		}
	}
}

/* Advertise our pid in shared memory, so that startup process can kill us. */
static void
InitWalRcv(void)
{
	/* use volatile pointer to prevent code rearrangement */
	volatile WalRcvData *walrcv = WalRcv;

	/*
	 * WalRcv should be set up already (if we are a backend, we inherit
	 * this by fork() or EXEC_BACKEND mechanism from the postmaster).
	 */
	if (walrcv == NULL)
		elog(PANIC, "walreceiver control data uninitialized");

	/* If we've already been requested to stop, don't start up */
	SpinLockAcquire(&walrcv->mutex);
	Assert(walrcv->pid == 0);
	if (walrcv->walRcvState == WALRCV_STOPPED ||
		walrcv->walRcvState == WALRCV_STOPPING)
	{
		walrcv->walRcvState = WALRCV_STOPPED;
		SpinLockRelease(&walrcv->mutex);
		proc_exit(1);
	}
	walrcv->pid = MyProcPid;
	SpinLockRelease(&walrcv->mutex);

	/* Arrange to clean up at walreceiver exit */
	on_shmem_exit(WalRcvKill, 0);
}

/*
 * Establish the connection to the primary server for XLOG streaming
 */
static void
WalRcvConnect(void)
{
	char		conninfo[MAXCONNINFO + 14];
	char	   *primary_sysid;
	char		standby_sysid[32];
	TimeLineID	primary_tli;
	TimeLineID	standby_tli;
	PGresult   *res;
	XLogRecPtr	recptr;
	char		cmd[64];
	/* use volatile pointer to prevent code rearrangement */
	volatile WalRcvData *walrcv = WalRcv;

	/*
	 * Set up a connection for XLOG streaming
	 */
	SpinLockAcquire(&walrcv->mutex);
	snprintf(conninfo, sizeof(conninfo), "%s replication=true", walrcv->conninfo);
	recptr = walrcv->receivedUpto;
	SpinLockRelease(&walrcv->mutex);

	/* initialize local XLOG pointers */
	LogstreamResult.Write = LogstreamResult.Flush = recptr;

	Assert(recptr.xlogid != 0 || recptr.xrecoff != 0);

	EnableImmediateExit();
	streamConn = PQconnectdb(conninfo);
	DisableImmediateExit();
	if (PQstatus(streamConn) != CONNECTION_OK)
		ereport(ERROR,
				(errmsg("could not connect to the primary server : %s",
						PQerrorMessage(streamConn))));

	/*
	 * Get the system identifier and timeline ID as a DataRow message
	 * from the primary server.
	 */
	EnableImmediateExit();
	res = PQexec(streamConn, "IDENTIFY_SYSTEM");
	DisableImmediateExit();
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
		PQclear(res);
		ereport(ERROR,
				(errmsg("could not receive the SYSID and timeline ID from "
						"the primary server: %s",
						PQerrorMessage(streamConn))));
    }
	if (PQnfields(res) != 2 || PQntuples(res) != 1)
	{
		int ntuples = PQntuples(res);
		int nfields = PQnfields(res);
		PQclear(res);
		ereport(ERROR,
				(errmsg("invalid response from primary server"),
				 errdetail("expected 1 tuple with 2 fields, got %d tuples with %d fields",
						   ntuples, nfields)));
	}
	primary_sysid = PQgetvalue(res, 0, 0);
	primary_tli = pg_atoi(PQgetvalue(res, 0, 1), 4, 0);

	/*
	 * Confirm that the system identifier of the primary is the same
	 * as ours.
	 */
	snprintf(standby_sysid, sizeof(standby_sysid), UINT64_FORMAT,
			 GetSystemIdentifier());
	if (strcmp(primary_sysid, standby_sysid) != 0)
	{
		PQclear(res);
		ereport(ERROR,
				(errmsg("system differs between the primary and standby"),
				 errdetail("the primary SYSID is %s, standby SYSID is %s",
						   primary_sysid, standby_sysid)));
	}

	/*
	 * Confirm that the current timeline of the primary is the same
	 * as the recovery target timeline.
	 */
	standby_tli = GetRecoveryTargetTLI();
	PQclear(res);
	if (primary_tli != standby_tli)
		ereport(ERROR,
				(errmsg("timeline %u of the primary does not match recovery target timeline %u",
						primary_tli, standby_tli)));
	ThisTimeLineID = primary_tli;

	/* Start streaming from the point requested by startup process */
	snprintf(cmd, sizeof(cmd), "START_REPLICATION %X/%X", recptr.xlogid, recptr.xrecoff);
	EnableImmediateExit();
	res = PQexec(streamConn, cmd);
	DisableImmediateExit();
	if (PQresultStatus(res) != PGRES_COPY_OUT)
		ereport(ERROR,
				(errmsg("could not start XLOG streaming: %s",
						PQerrorMessage(streamConn))));
	PQclear(res);

	/*
	 * Process the outstanding messages before beginning to wait for
	 * new message to arrive.
	 */
	XLogRecv();
}

/*
 * Wait until we can read WAL stream, or timeout.
 *
 * Returns true if data has become available for reading, false if timed out
 * or interrupted by signal.
 *
 * This is based on pqSocketCheck.
 */
static bool
WalRcvWait(int timeout_ms)
{
	int	ret;

	Assert(streamConn != NULL);
	if (PQsocket(streamConn) < 0)
		ereport(ERROR,
				(errcode_for_socket_access(),
				 errmsg("socket not open")));

	/* We use poll(2) if available, otherwise select(2) */
	{
#ifdef HAVE_POLL
		struct pollfd input_fd;

		input_fd.fd = PQsocket(streamConn);
		input_fd.events = POLLIN | POLLERR;
		input_fd.revents = 0;

		ret = poll(&input_fd, 1, timeout_ms);
#else							/* !HAVE_POLL */

		fd_set		input_mask;
		struct timeval timeout;
		struct timeval *ptr_timeout;

		FD_ZERO(&input_mask);
		FD_SET(PQsocket(streamConn), &input_mask);

		if (timeout_ms < 0)
			ptr_timeout = NULL;
		else
		{
			timeout.tv_sec	= timeout_ms / 1000;
			timeout.tv_usec	= (timeout_ms % 1000) * 1000;
			ptr_timeout		= &timeout;
		}

		ret = select(PQsocket(streamConn) + 1, &input_mask,
					 NULL, NULL, ptr_timeout);
#endif   /* HAVE_POLL */
	}

	if (ret == 0 || (ret < 0 && errno == EINTR))
		return false;
	if (ret < 0)
		ereport(ERROR,
				(errcode_for_socket_access(),
				 errmsg("select() failed: %m")));
	return true;
}

/*
 * Clear our pid from shared memory at exit.
 */
static void
WalRcvKill(int code, Datum arg)
{
	/* use volatile pointer to prevent code rearrangement */
	volatile WalRcvData *walrcv = WalRcv;
	bool stopped = false;

	SpinLockAcquire(&walrcv->mutex);
	if (walrcv->walRcvState == WALRCV_STOPPING ||
		walrcv->walRcvState == WALRCV_STOPPED)
	{
		walrcv->walRcvState = WALRCV_STOPPED;
		stopped = true;
		elog(LOG, "walreceiver stopped");
	}
	walrcv->pid = 0;
	SpinLockRelease(&walrcv->mutex);

	PQfinish(streamConn);

	/* If requested to stop, tell postmaster to not restart us. */
	if (stopped)
		SendPostmasterSignal(PMSIGNAL_SHUTDOWN_WALRECEIVER);
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
	 * Note we do exit(2) not exit(0).	This is to force the postmaster into a
	 * system reset cycle if some idiot DBA sends a manual SIGQUIT to a random
	 * backend.  This is necessary precisely because we don't clean up our
	 * shared memory state.  (The "dead man switch" mechanism in pmsignal.c
	 * should ensure the postmaster sees this as a crash, too, but no harm
	 * in being doubly sure.)
	 */
	exit(2);
}

/*
 * Receive any WAL records available without blocking from XLOG stream and
 * write it to the disk.
 */
static void
XLogRecv(void)
{
	XLogRecPtr *recptr;
	int			len;

	for (;;)
	{
		/* Receive CopyData message */
		len = PQgetCopyData(streamConn, &recvBuf, 1);
		if (len == 0)	/* no records available yet, then return */
			break;
		if (len == -1)	/* end-of-streaming or error */
		{
			PGresult	*res;

			res = PQgetResult(streamConn);
			if (PQresultStatus(res) == PGRES_COMMAND_OK)
			{
				PQclear(res);
				ereport(ERROR,
						(errmsg("replication terminated by primary server")));
			}
			PQclear(res);
			ereport(ERROR,
					(errmsg("could not read xlog records: %s",
							PQerrorMessage(streamConn))));
		}
		if (len < -1)
			ereport(ERROR,
					(errmsg("could not read xlog records: %s",
							PQerrorMessage(streamConn))));

		if (len < sizeof(XLogRecPtr))
			ereport(ERROR,
					(errmsg("invalid WAL message received from primary")));

		/* Write received WAL records to disk */
		recptr = (XLogRecPtr *) recvBuf;
		XLogWalRcvWrite(recvBuf + sizeof(XLogRecPtr),
						len - sizeof(XLogRecPtr), *recptr);

		if (recvBuf != NULL)
			PQfreemem(recvBuf);
		recvBuf = NULL;
	}

	/*
	 * Now that we've written some records, flush them to disk and let the
	 * startup process know about them.
	 */
	XLogWalRcvFlush();
}

/*
 * Write XLOG data to disk.
 */
static void
XLogWalRcvWrite(char *buf, Size nbytes, XLogRecPtr recptr)
{
	int		startoff;
	int		byteswritten;

	while (nbytes > 0)
	{
		int		segbytes;

		if (recvFile < 0 || !XLByteInSeg(recptr, recvId, recvSeg))
		{
			bool	use_existent;

			/*
			 * XLOG segment files will be re-read in recovery operation soon,
			 * so we don't need to advise the OS to release any cache page.
			 */
			if (recvFile >= 0)
			{
				/*
				 * fsync() before we switch to next file. We would otherwise
				 * have to reopen this file to fsync it later
				 */
				XLogWalRcvFlush();
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
			recvFile = XLogFileInit(recvId, recvSeg,
									&use_existent, true);
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

		LogstreamResult.Write	= recptr;

		/*
		 * XXX: Should we signal bgwriter to start a restartpoint
		 * if we've consumed too much xlog since the last one, like
		 * in normal processing? But this is not worth doing unless
		 * a restartpoint can be created independently from a
		 * checkpoint record.
		 */
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
		char	activitymsg[50];

		issue_xlog_fsync(recvFile, recvId, recvSeg);

		LogstreamResult.Flush = LogstreamResult.Write;

		/* Update shared-memory status */
		SpinLockAcquire(&walrcv->mutex);
		walrcv->receivedUpto = LogstreamResult.Flush;
		SpinLockRelease(&walrcv->mutex);

		/* Report XLOG streaming progress in PS display */
		snprintf(activitymsg, sizeof(activitymsg), "streaming %X/%X",
				 LogstreamResult.Write.xlogid, LogstreamResult.Write.xrecoff);
		set_ps_display(activitymsg, false);
	}
}
