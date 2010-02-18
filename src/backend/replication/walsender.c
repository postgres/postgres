/*-------------------------------------------------------------------------
 *
 * walsender.c
 *
 * The WAL sender process (walsender) is new as of Postgres 9.0. It takes
 * charge of XLOG streaming sender in the primary server. At first, it is
 * started by the postmaster when the walreceiver in the standby server
 * connects to the primary server and requests XLOG streaming replication,
 * i.e., unlike any auxiliary process, it is not an always-running process.
 * It attempts to keep reading XLOG records from the disk and sending them
 * to the standby server, as long as the connection is alive (i.e., like
 * any backend, there is an one to one relationship between a connection
 * and a walsender process).
 *
 * Normal termination is by SIGTERM, which instructs the walsender to
 * close the connection and exit(0) at next convenient moment. Emergency
 * termination is by SIGQUIT; like any backend, the walsender will simply
 * abort and exit on SIGQUIT. A close of the connection and a FATAL error
 * are treated as not a crash but approximately normal termination;
 * the walsender will exit quickly without sending any more XLOG records.
 *
 * If the server is shut down, postmaster sends us SIGUSR2 after all
 * regular backends have exited and the shutdown checkpoint has been written.
 * This instruct walsender to send any outstanding WAL, including the
 * shutdown checkpoint record, and then exit.
 *
 * Note that there can be more than one walsender process concurrently.
 *
 * Portions Copyright (c) 2010-2010, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/replication/walsender.c,v 1.7 2010/02/18 11:13:46 heikki Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/xlog_internal.h"
#include "catalog/pg_type.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "replication/walsender.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lock.h"
#include "storage/pmsignal.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"

/* Array of WalSnds in shared memory */
WalSndCtlData *WalSndCtl = NULL;

/* My slot in the shared memory array */
static WalSnd *MyWalSnd = NULL;

/* Global state */
bool	am_walsender	= false;	/* Am I a walsender process ? */

/* User-settable parameters for walsender */
int	MaxWalSenders = 0;		/* the maximum number of concurrent walsenders */
int	WalSndDelay	= 200;		/* max sleep time between some actions */

#define NAPTIME_PER_CYCLE 100	/* max sleep time between cycles (100ms) */

/*
 * These variables are used similarly to openLogFile/Id/Seg/Off,
 * but for walsender to read the XLOG.
 */
static int	sendFile = -1;
static uint32 sendId = 0;
static uint32 sendSeg = 0;
static uint32 sendOff = 0;

/*
 * How far have we sent WAL already? This is also advertised in
 * MyWalSnd->sentPtr.
 */
static XLogRecPtr sentPtr = {0, 0};

/* Flags set by signal handlers for later service in main loop */
static volatile sig_atomic_t got_SIGHUP = false;
static volatile sig_atomic_t shutdown_requested = false;
static volatile sig_atomic_t ready_to_stop = false;

/* Signal handlers */
static void WalSndSigHupHandler(SIGNAL_ARGS);
static void WalSndShutdownHandler(SIGNAL_ARGS);
static void WalSndQuickDieHandler(SIGNAL_ARGS);

/* Prototypes for private functions */
static int	WalSndLoop(void);
static void	InitWalSnd(void);
static void	WalSndHandshake(void);
static void	WalSndKill(int code, Datum arg);
static void XLogRead(char *buf, XLogRecPtr recptr, Size nbytes);
static bool XLogSend(StringInfo outMsg);
static void CheckClosedConnection(void);

/*
 * How much WAL to send in one message? Must be >= XLOG_BLCKSZ.
 */
#define MAX_SEND_SIZE (XLOG_SEG_SIZE / 2)

/* Main entry point for walsender process */
int
WalSenderMain(void)
{
	MemoryContext walsnd_context;

	if (!superuser())
		ereport(FATAL,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to start walsender")));

	/* Create a per-walsender data structure in shared memory */
	InitWalSnd();

	/*
	 * Create a memory context that we will do all our work in.  We do this so
	 * that we can reset the context during error recovery and thereby avoid
	 * possible memory leaks.  Formerly this code just ran in
	 * TopMemoryContext, but resetting that would be a really bad idea.
	 *
	 * XXX: we don't actually attempt error recovery in walsender, we just
	 * close the connection and exit.
	 */
	walsnd_context = AllocSetContextCreate(TopMemoryContext,
										   "Wal Sender",
										   ALLOCSET_DEFAULT_MINSIZE,
										   ALLOCSET_DEFAULT_INITSIZE,
										   ALLOCSET_DEFAULT_MAXSIZE);
	MemoryContextSwitchTo(walsnd_context);

	/* Unblock signals (they were blocked when the postmaster forked us) */
	PG_SETMASK(&UnBlockSig);

	/* Tell the standby that walsender is ready for receiving commands */
	ReadyForQuery(DestRemote);

	/* Handle handshake messages before streaming */
	WalSndHandshake();

	/* Main loop of walsender */
	return WalSndLoop();
}

static void
WalSndHandshake(void)
{
	StringInfoData input_message;
	bool replication_started = false;

	initStringInfo(&input_message);

	while (!replication_started)
	{
		int firstchar;

		/* Wait for a command to arrive */
		firstchar = pq_getbyte();

		/*
		 * Check for any other interesting events that happened while we
		 * slept.
		 */
		if (got_SIGHUP)
		{
			got_SIGHUP = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		if (firstchar != EOF)
		{
			/*
			 * Read the message contents. This is expected to be done without
			 * blocking because we've been able to get message type code.
			 */
			if (pq_getmessage(&input_message, 0))
				firstchar = EOF;		/* suitable message already logged */
		}

		/* Handle the very limited subset of commands expected in this phase */
		switch (firstchar)
		{
			case 'Q':	/* Query message */
			{
				const char *query_string;
				XLogRecPtr	recptr;

				query_string = pq_getmsgstring(&input_message);
				pq_getmsgend(&input_message);

				if (strcmp(query_string, "IDENTIFY_SYSTEM") == 0)
				{
					StringInfoData	buf;
					char	sysid[32];
					char	tli[11];

					/*
					 * Reply with a result set with one row, two columns.
					 * First col is system ID, and second if timeline ID
					 */

					snprintf(sysid, sizeof(sysid), UINT64_FORMAT,
							 GetSystemIdentifier());
					snprintf(tli, sizeof(tli), "%u", ThisTimeLineID);

					/* Send a RowDescription message */
					pq_beginmessage(&buf, 'T');
					pq_sendint(&buf, 2, 2); /* 2 fields */

					/* first field */
					pq_sendstring(&buf, "systemid"); /* col name */
					pq_sendint(&buf, 0, 4);			/* table oid */
					pq_sendint(&buf, 0, 2);			/* attnum */
					pq_sendint(&buf, TEXTOID, 4);	/* type oid */
					pq_sendint(&buf, -1, 2);		/* typlen */
					pq_sendint(&buf, 0, 4);			/* typmod */
					pq_sendint(&buf, 0, 2);			/* format code */

					/* second field */
					pq_sendstring(&buf, "timeline"); /* col name */
					pq_sendint(&buf, 0, 4);			/* table oid */
					pq_sendint(&buf, 0, 2);			/* attnum */
					pq_sendint(&buf, INT4OID, 4);	/* type oid */
					pq_sendint(&buf, 4, 2);			/* typlen */
					pq_sendint(&buf, 0, 4);			/* typmod */
					pq_sendint(&buf, 0, 2);			/* format code */
					pq_endmessage(&buf);

					/* Send a DataRow message */
					pq_beginmessage(&buf, 'D');
					pq_sendint(&buf, 2, 2);			/* # of columns */
					pq_sendint(&buf, strlen(sysid), 4); /* col1 len */
					pq_sendbytes(&buf, (char *) &sysid, strlen(sysid));
					pq_sendint(&buf, strlen(tli), 4); /* col2 len */
					pq_sendbytes(&buf, (char *) tli, strlen(tli));
					pq_endmessage(&buf);

					/* Send CommandComplete and ReadyForQuery messages */
					EndCommand("SELECT", DestRemote);
					ReadyForQuery(DestRemote);
				}
				else if (sscanf(query_string, "START_REPLICATION %X/%X",
								&recptr.xlogid, &recptr.xrecoff) == 2)
				{
					StringInfoData	buf;

					/* Send a CopyOutResponse message, and start streaming */
					pq_beginmessage(&buf, 'H');
					pq_sendbyte(&buf, 0);
					pq_sendint(&buf, 0, 2);
					pq_endmessage(&buf);

					/*
					 * Initialize position to the received one, then
					 * the xlog records begin to be shipped from that position
					 */
					sentPtr	= recptr;

					/* break out of the loop */
					replication_started = true;
				}
				else
				{
					ereport(FATAL,
							(errcode(ERRCODE_PROTOCOL_VIOLATION),
							 errmsg("invalid standby query string: %s", query_string)));
				}
				break;
			}

			case 'X':
				/* standby is closing the connection */
				proc_exit(0);

			case EOF:
				/* standby disconnected unexpectedly */
				ereport(COMMERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("unexpected EOF on standby connection")));
				proc_exit(0);

			default:
				ereport(FATAL,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("invalid standby handshake message type %d", firstchar)));
		}
	}
}

/*
 * Check if the remote end has closed the connection.
 */
static void
CheckClosedConnection(void)
{
	unsigned char firstchar;
	int r;

	r = pq_getbyte_if_available(&firstchar);
	if (r < 0)
	{
		/* unexpected error or EOF */
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("unexpected EOF on standby connection")));
		proc_exit(0);
	}
	if (r == 0)
	{
		/* no data available without blocking */
		return;
	}

	/* Handle the very limited subset of commands expected in this phase */
	switch (firstchar)
	{
		/*
		 * 'X' means that the standby is closing down the socket.
		 */
		case 'X':
			proc_exit(0);

		default:
			ereport(FATAL,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("invalid standby closing message type %d",
							firstchar)));
	}
}

/* Main loop of walsender process */
static int
WalSndLoop(void)
{
	StringInfoData output_message;

	initStringInfo(&output_message);

	/* Loop forever */
	for (;;)
	{
		int remain;	/* remaining time (ms) */

		/*
		 * Emergency bailout if postmaster has died.  This is to avoid the
		 * necessity for manual cleanup of all postmaster children.
		 */
		if (!PostmasterIsAlive(true))
			exit(1);
		/* Process any requests or signals received recently */
		if (got_SIGHUP)
		{
			got_SIGHUP = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		/*
		 * When SIGUSR2 arrives, we send all outstanding logs up to the
		 * shutdown checkpoint record (i.e., the latest record) and exit.
		 */
		if (ready_to_stop)
		{
			XLogSend(&output_message);
			shutdown_requested = true;
		}

		/* Normal exit from the walsender is here */
		if (shutdown_requested)
		{
			/* Inform the standby that XLOG streaming was done */
			pq_puttextmessage('C', "COPY 0");
			pq_flush();

			proc_exit(0);
		}

		/*
		 * Nap for the configured time or until a message arrives.
		 *
		 * On some platforms, signals won't interrupt the sleep.  To ensure we
		 * respond reasonably promptly when someone signals us, break down the
		 * sleep into NAPTIME_PER_CYCLE (ms) increments, and check for
		 * interrupts after each nap.
		 */
		remain = WalSndDelay;
		while (remain > 0)
		{
			if (got_SIGHUP || shutdown_requested || ready_to_stop)
				break;

			/*
			 * Check to see whether a message from the standby or an interrupt
			 * from other processes has arrived.
			 */
			pg_usleep(remain > NAPTIME_PER_CYCLE ? NAPTIME_PER_CYCLE : remain);
			CheckClosedConnection();

			remain -= NAPTIME_PER_CYCLE;
		}

		/* Attempt to send the log once every loop */
		if (!XLogSend(&output_message))
			goto eof;
	}

	/* can't get here because the above loop never exits */
	return 1;

eof:
	/*
	 * Reset whereToSendOutput to prevent ereport from attempting
	 * to send any more messages to the standby.
	 */
	if (whereToSendOutput == DestRemote)
		whereToSendOutput = DestNone;

	proc_exit(0);
	return 1;		/* keep the compiler quiet */
}

/* Initialize a per-walsender data structure for this walsender process */
static void
InitWalSnd(void)
{
	/* use volatile pointer to prevent code rearrangement */
	int		i;

	/*
	 * WalSndCtl should be set up already (we inherit this by fork() or
	 * EXEC_BACKEND mechanism from the postmaster).
	 */
	Assert(WalSndCtl != NULL);
	Assert(MyWalSnd == NULL);

	/*
	 * Find a free walsender slot and reserve it. If this fails, we must be
	 * out of WalSnd structures.
	 */
	for (i = 0; i < MaxWalSenders; i++)
	{
		volatile WalSnd *walsnd = &WalSndCtl->walsnds[i];

		SpinLockAcquire(&walsnd->mutex);

		if (walsnd->pid != 0)
		{
			SpinLockRelease(&walsnd->mutex);
			continue;
		}
		else
		{
			/* found */
			MyWalSnd = (WalSnd *) walsnd;
			walsnd->pid = MyProcPid;
			MemSet(&MyWalSnd->sentPtr, 0, sizeof(XLogRecPtr));
			SpinLockRelease(&walsnd->mutex);
			break;
		}
	}
	if (MyWalSnd == NULL)
		ereport(FATAL,
				(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
				 errmsg("sorry, too many standbys already")));

	/* Arrange to clean up at walsender exit */
	on_shmem_exit(WalSndKill, 0);
}

/* Destroy the per-walsender data structure for this walsender process */
static void
WalSndKill(int code, Datum arg)
{
	Assert(MyWalSnd != NULL);

	/*
	 * Mark WalSnd struct no longer in use. Assume that no lock is required
	 * for this.
	 */
	MyWalSnd->pid = 0;

	/* WalSnd struct isn't mine anymore */
	MyWalSnd = NULL;
}

/*
 * Read 'nbytes' bytes from WAL into 'buf', starting at location 'recptr'
 */
void
XLogRead(char *buf, XLogRecPtr recptr, Size nbytes)
{
	char path[MAXPGPATH];
	uint32 startoff;

	while (nbytes > 0)
	{
		int segbytes;
		int readbytes;

		startoff = recptr.xrecoff % XLogSegSize;

		if (sendFile < 0 || !XLByteInSeg(recptr, sendId, sendSeg))
		{
			/* Switch to another logfile segment */
			if (sendFile >= 0)
				close(sendFile);

			XLByteToSeg(recptr, sendId, sendSeg);
			XLogFilePath(path, ThisTimeLineID, sendId, sendSeg);

			sendFile = BasicOpenFile(path, O_RDONLY | PG_BINARY, 0);
			if (sendFile < 0)
				ereport(FATAL, /* XXX: Why FATAL? */
						(errcode_for_file_access(),
						 errmsg("could not open file \"%s\" (log file %u, segment %u): %m",
								path, sendId, sendSeg)));
			sendOff = 0;
		}

		/* Need to seek in the file? */
		if (sendOff != startoff)
		{
			if (lseek(sendFile, (off_t) startoff, SEEK_SET) < 0)
				ereport(FATAL,
						(errcode_for_file_access(),
						 errmsg("could not seek in log file %u, segment %u to offset %u: %m",
								sendId, sendSeg, startoff)));
			sendOff = startoff;
		}

		/* How many bytes are within this segment? */
		if (nbytes > (XLogSegSize - startoff))
			segbytes = XLogSegSize - startoff;
		else
			segbytes = nbytes;

		readbytes = read(sendFile, buf, segbytes);
		if (readbytes <= 0)
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not read from log file %u, segment %u, offset %u, "
							"length %lu: %m",
							sendId, sendSeg, sendOff, (unsigned long) segbytes)));

		/* Update state for read */
		XLByteAdvance(recptr, readbytes);

		sendOff += readbytes;
		nbytes -= readbytes;
		buf += readbytes;
	}
}

/*
 * Read all WAL that's been written (and flushed) since last cycle, and send
 * it to client.
 *
 * Returns true if OK, false if trouble.
 */
static bool
XLogSend(StringInfo outMsg)
{
	XLogRecPtr	SendRqstPtr;
	char	activitymsg[50];
	/* use volatile pointer to prevent code rearrangement */
	volatile WalSnd *walsnd = MyWalSnd;

	/*
	 * Invalid position means that we have not yet received the initial
	 * CopyData message from the slave that indicates where to start the
	 * streaming.
	 */
	if (sentPtr.xlogid == 0 &&
		sentPtr.xrecoff == 0)
		return true;

	/* Attempt to send all records flushed to the disk already */
	SendRqstPtr = GetWriteRecPtr();

	/* Quick exit if nothing to do */
	if (!XLByteLT(sentPtr, SendRqstPtr))
		return true;

	/*
	 * We gather multiple records together by issuing just one XLogRead()
	 * of a suitable size, and send them as one CopyData message. Repeat
	 * until we've sent everything we can.
	 */
	while (XLByteLT(sentPtr, SendRqstPtr))
	{
		XLogRecPtr startptr;
		XLogRecPtr endptr;
		Size	nbytes;

		/*
		 * Figure out how much to send in one message. If there's less than
		 * MAX_SEND_SIZE bytes to send, send everything. Otherwise send
		 * MAX_SEND_SIZE bytes, but round to page boundary.
		 *
		 * The rounding is not only for performance reasons. Walreceiver
		 * relies on the fact that we never split a WAL record across two
		 * messages. Since a long WAL record is split at page boundary into
		 * continuation records, page boundary is always a safe cut-off point.
		 * We also assume that SendRqstPtr never points in the middle of a
		 * WAL record.
		 */
		startptr = sentPtr;
		if (startptr.xrecoff >= XLogFileSize)
		{
			/*
			 * crossing a logid boundary, skip the non-existent last log
			 * segment in previous logical log file.
			 */
			startptr.xlogid += 1;
			startptr.xrecoff = 0;
		}

		endptr = startptr;
		XLByteAdvance(endptr, MAX_SEND_SIZE);
		/* round down to page boundary. */
		endptr.xrecoff -= (endptr.xrecoff % XLOG_BLCKSZ);
		/* if we went beyond SendRqstPtr, back off */
		if (XLByteLT(SendRqstPtr, endptr))
			endptr = SendRqstPtr;

		/*
		 * OK to read and send the slice.
		 *
		 * We don't need to convert the xlogid/xrecoff from host byte order
		 * to network byte order because the both server can be expected to
		 * have the same byte order. If they have different byte order, we
		 * don't reach here.
		 */
		pq_sendbyte(outMsg, 'w');
		pq_sendbytes(outMsg, (char *) &startptr, sizeof(startptr));

		if (endptr.xlogid != startptr.xlogid)
		{
			Assert(endptr.xlogid == startptr.xlogid + 1);
			nbytes = endptr.xrecoff + XLogFileSize - startptr.xrecoff;
		}
		else
			nbytes = endptr.xrecoff - startptr.xrecoff;

		sentPtr = endptr;

		/*
		 * Read the log directly into the output buffer to prevent
		 * extra memcpy calls.
		 */
		enlargeStringInfo(outMsg, nbytes);

		XLogRead(&outMsg->data[outMsg->len], startptr, nbytes);
		outMsg->len += nbytes;
		outMsg->data[outMsg->len] = '\0';

		pq_putmessage('d', outMsg->data, outMsg->len);
		resetStringInfo(outMsg);
	}

	/* Update shared memory status */
	SpinLockAcquire(&walsnd->mutex);
	walsnd->sentPtr = sentPtr;
	SpinLockRelease(&walsnd->mutex);

	/* Flush pending output */
	if (pq_flush())
		return false;

	/* Report progress of XLOG streaming in PS display */
	snprintf(activitymsg, sizeof(activitymsg), "streaming %X/%X",
			 sentPtr.xlogid, sentPtr.xrecoff);
	set_ps_display(activitymsg, false);

	return true;
}

/* SIGHUP: set flag to re-read config file at next convenient time */
static void
WalSndSigHupHandler(SIGNAL_ARGS)
{
	got_SIGHUP = true;
}

/* SIGTERM: set flag to shut down */
static void
WalSndShutdownHandler(SIGNAL_ARGS)
{
	shutdown_requested = true;
}

/*
 * WalSndQuickDieHandler() occurs when signalled SIGQUIT by the postmaster.
 *
 * Some backend has bought the farm,
 * so we need to stop what we're doing and exit.
 */
static void
WalSndQuickDieHandler(SIGNAL_ARGS)
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

/* SIGUSR2: set flag to do a last cycle and shut down afterwards */
static void
WalSndLastCycleHandler(SIGNAL_ARGS)
{
	ready_to_stop = true;
}

/* Set up signal handlers */
void
WalSndSignals(void)
{
	/* Set up signal handlers */
	pqsignal(SIGHUP, WalSndSigHupHandler);	/* set flag to read config file */
	pqsignal(SIGINT, SIG_IGN);	/* not used */
	pqsignal(SIGTERM, WalSndShutdownHandler);	/* request shutdown */
	pqsignal(SIGQUIT, WalSndQuickDieHandler);	/* hard crash time */
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, SIG_IGN);	/* not used */
	pqsignal(SIGUSR2, WalSndLastCycleHandler);	/* request a last cycle and shutdown */

	/* Reset some signals that are accepted by postmaster but not here */
	pqsignal(SIGCHLD, SIG_DFL);
	pqsignal(SIGTTIN, SIG_DFL);
	pqsignal(SIGTTOU, SIG_DFL);
	pqsignal(SIGCONT, SIG_DFL);
	pqsignal(SIGWINCH, SIG_DFL);
}

/* Report shared-memory space needed by WalSndShmemInit */
Size
WalSndShmemSize(void)
{
	Size size = 0;

	size = offsetof(WalSndCtlData, walsnds);
	size = add_size(size, mul_size(MaxWalSenders, sizeof(WalSnd)));

	return size;
}

/* Allocate and initialize walsender-related shared memory */
void
WalSndShmemInit(void)
{
	bool	found;
	int		i;

	WalSndCtl = (WalSndCtlData *)
		ShmemInitStruct("Wal Sender Ctl", WalSndShmemSize(), &found);

	if (WalSndCtl == NULL)
		ereport(FATAL,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("not enough shared memory for walsender")));
	if (found)
		return;					/* already initialized */

	/* Initialize the data structures */
	MemSet(WalSndCtl, 0, WalSndShmemSize());

	for (i = 0; i < MaxWalSenders; i++)
	{
		WalSnd	*walsnd = &WalSndCtl->walsnds[i];
		SpinLockInit(&walsnd->mutex);
	}
}

/*
 * Returns the oldest Send position among walsenders. Or InvalidXLogRecPtr
 * if none.
 */
XLogRecPtr
GetOldestWALSendPointer(void)
{
	XLogRecPtr oldest = {0, 0};
	int		i;
	bool	found = false;

	for (i = 0; i < MaxWalSenders; i++)
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile WalSnd	*walsnd = &WalSndCtl->walsnds[i];
		XLogRecPtr recptr;

		if (walsnd->pid == 0)
			continue;

		SpinLockAcquire(&walsnd->mutex);
		recptr = walsnd->sentPtr;
		SpinLockRelease(&walsnd->mutex);

		if (recptr.xlogid == 0 && recptr.xrecoff == 0)
			continue;

		if (!found || XLByteLT(recptr, oldest))
			oldest = recptr;
		found = true;
	}
	return oldest;
}
