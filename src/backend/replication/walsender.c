/*-------------------------------------------------------------------------
 *
 * walsender.c
 *
 * The WAL sender process (walsender) is new as of Postgres 9.0. It takes
 * care of sending XLOG from the primary server to a single recipient.
 * (Note that there can be more than one walsender process concurrently.)
 * It is started by the postmaster when the walreceiver of a standby server
 * connects to the primary server and requests XLOG streaming replication.
 * It attempts to keep reading XLOG records from the disk and sending them
 * to the standby server, as long as the connection is alive (i.e., like
 * any backend, there is a one-to-one relationship between a connection
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
 *
 * Portions Copyright (c) 2010-2012, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/walsender.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <unistd.h>

#include "access/transam.h"
#include "access/xlog_internal.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "nodes/replnodes.h"
#include "replication/basebackup.h"
#include "replication/syncrep.h"
#include "replication/walreceiver.h"
#include "replication/walsender.h"
#include "replication/walsender_private.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/resowner.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"

/*
 * Maximum data payload in a WAL data message.	Must be >= XLOG_BLCKSZ.
 *
 * We don't have a good idea of what a good value would be; there's some
 * overhead per message in both walsender and walreceiver, but on the other
 * hand sending large batches makes walsender less responsive to signals
 * because signals are checked only between messages.  128kB (with
 * default 8k blocks) seems like a reasonable guess for now.
 */
#define MAX_SEND_SIZE (XLOG_BLCKSZ * 16)

/* Array of WalSnds in shared memory */
WalSndCtlData *WalSndCtl = NULL;

/* My slot in the shared memory array */
WalSnd	   *MyWalSnd = NULL;

/* Global state */
bool		am_walsender = false;		/* Am I a walsender process ? */
bool		am_cascading_walsender = false;		/* Am I cascading WAL to
												 * another standby ? */

static bool	replication_started = false; /* Started streaming yet? */

/* User-settable parameters for walsender */
int			max_wal_senders = 0;	/* the maximum number of concurrent walsenders */
int			wal_sender_timeout = 60 * 1000;	/* maximum time to send one
												 * WAL data message */
/*
 * State for WalSndWakeupRequest
 */
bool wake_wal_senders = false;

/*
 * These variables are used similarly to openLogFile/Id/Seg/Off,
 * but for walsender to read the XLOG.
 */
static int	sendFile = -1;
static XLogSegNo sendSegNo = 0;
static uint32 sendOff = 0;

/*
 * How far have we sent WAL already? This is also advertised in
 * MyWalSnd->sentPtr.  (Actually, this is the next WAL location to send.)
 */
static XLogRecPtr sentPtr = 0;

/* Buffers for constructing outgoing messages and processing reply messages. */
static StringInfoData output_message;
static StringInfoData reply_message;
static StringInfoData tmpbuf;

/*
 * Timestamp of the last receipt of the reply from the standby.
 */
static TimestampTz last_reply_timestamp;
/* Have we sent a heartbeat message asking for reply, since last reply? */
static bool	ping_sent = false;

/* Flags set by signal handlers for later service in main loop */
static volatile sig_atomic_t got_SIGHUP = false;
volatile sig_atomic_t walsender_ready_to_stop = false;

/* Signal handlers */
static void WalSndSigHupHandler(SIGNAL_ARGS);
static void WalSndXLogSendHandler(SIGNAL_ARGS);
static void WalSndLastCycleHandler(SIGNAL_ARGS);

/* Prototypes for private functions */
static void WalSndLoop(void) __attribute__((noreturn));
static void InitWalSenderSlot(void);
static void WalSndKill(int code, Datum arg);
static void XLogSend(bool *caughtup);
static void IdentifySystem(void);
static void StartReplication(StartReplicationCmd *cmd);
static void ProcessStandbyMessage(void);
static void ProcessStandbyReplyMessage(void);
static void ProcessStandbyHSFeedbackMessage(void);
static void ProcessRepliesIfAny(void);
static void WalSndKeepalive(bool requestReply);


/* Initialize walsender process before entering the main command loop */
void
InitWalSender(void)
{
	am_cascading_walsender = RecoveryInProgress();

	/* Create a per-walsender data structure in shared memory */
	InitWalSenderSlot();

	/* Set up resource owner */
	CurrentResourceOwner = ResourceOwnerCreate(NULL, "walsender top-level resource owner");

	/*
	 * Use the recovery target timeline ID during recovery
	 */
	if (am_cascading_walsender)
		ThisTimeLineID = GetRecoveryTargetTLI();
}

/*
 * Clean up after an error.
 *
 * WAL sender processes don't use transactions like regular backends do.
 * This function does any cleanup requited after an error in a WAL sender
 * process, similar to what transaction abort does in a regular backend.
 */
void
WalSndErrorCleanup()
{
	if (sendFile >= 0)
	{
		close(sendFile);
		sendFile = -1;
	}

	/*
	 * Don't return back to the command loop after we've started replicating.
	 * We've already marked us as an actively streaming WAL sender in the
	 * PMSignal slot, and there's currently no way to undo that.
	 */
	if (replication_started)
		proc_exit(0);
}

/*
 * IDENTIFY_SYSTEM
 */
static void
IdentifySystem(void)
{
	StringInfoData buf;
	char		sysid[32];
	char		tli[11];
	char		xpos[MAXFNAMELEN];
	XLogRecPtr	logptr;

	/*
	 * Reply with a result set with one row, three columns. First col is
	 * system ID, second is timeline ID, and third is current xlog location.
	 */

	snprintf(sysid, sizeof(sysid), UINT64_FORMAT,
			 GetSystemIdentifier());
	snprintf(tli, sizeof(tli), "%u", ThisTimeLineID);

	logptr = am_cascading_walsender ? GetStandbyFlushRecPtr(NULL) : GetInsertRecPtr();

	snprintf(xpos, sizeof(xpos), "%X/%X", (uint32) (logptr >> 32), (uint32) logptr);

	/* Send a RowDescription message */
	pq_beginmessage(&buf, 'T');
	pq_sendint(&buf, 3, 2);		/* 3 fields */

	/* first field */
	pq_sendstring(&buf, "systemid");	/* col name */
	pq_sendint(&buf, 0, 4);		/* table oid */
	pq_sendint(&buf, 0, 2);		/* attnum */
	pq_sendint(&buf, TEXTOID, 4);		/* type oid */
	pq_sendint(&buf, -1, 2);	/* typlen */
	pq_sendint(&buf, 0, 4);		/* typmod */
	pq_sendint(&buf, 0, 2);		/* format code */

	/* second field */
	pq_sendstring(&buf, "timeline");	/* col name */
	pq_sendint(&buf, 0, 4);		/* table oid */
	pq_sendint(&buf, 0, 2);		/* attnum */
	pq_sendint(&buf, INT4OID, 4);		/* type oid */
	pq_sendint(&buf, 4, 2);		/* typlen */
	pq_sendint(&buf, 0, 4);		/* typmod */
	pq_sendint(&buf, 0, 2);		/* format code */

	/* third field */
	pq_sendstring(&buf, "xlogpos");
	pq_sendint(&buf, 0, 4);
	pq_sendint(&buf, 0, 2);
	pq_sendint(&buf, TEXTOID, 4);
	pq_sendint(&buf, -1, 2);
	pq_sendint(&buf, 0, 4);
	pq_sendint(&buf, 0, 2);
	pq_endmessage(&buf);

	/* Send a DataRow message */
	pq_beginmessage(&buf, 'D');
	pq_sendint(&buf, 3, 2);		/* # of columns */
	pq_sendint(&buf, strlen(sysid), 4); /* col1 len */
	pq_sendbytes(&buf, (char *) &sysid, strlen(sysid));
	pq_sendint(&buf, strlen(tli), 4);	/* col2 len */
	pq_sendbytes(&buf, (char *) tli, strlen(tli));
	pq_sendint(&buf, strlen(xpos), 4);	/* col3 len */
	pq_sendbytes(&buf, (char *) xpos, strlen(xpos));

	pq_endmessage(&buf);
}

/*
 * Handle START_REPLICATION command.
 *
 * At the moment, this never returns, but an ereport(ERROR) will take us back
 * to the main loop.
 */
static void
StartReplication(StartReplicationCmd *cmd)
{
	StringInfoData buf;

	/*
	 * Let postmaster know that we're streaming. Once we've declared us as a
	 * WAL sender process, postmaster will let us outlive the bgwriter and
	 * kill us last in the shutdown sequence, so we get a chance to stream all
	 * remaining WAL at shutdown, including the shutdown checkpoint. Note that
	 * there's no going back, and we mustn't write any WAL records after this.
	 */
	MarkPostmasterChildWalSender();
	SendPostmasterSignal(PMSIGNAL_ADVANCE_STATE_MACHINE);
	replication_started = true;

	/*
	 * When promoting a cascading standby, postmaster sends SIGUSR2 to any
	 * cascading walsenders to kill them. But there is a corner-case where
	 * such walsender fails to receive SIGUSR2 and survives a standby
	 * promotion unexpectedly. This happens when postmaster sends SIGUSR2
	 * before the walsender marks itself as a WAL sender, because postmaster
	 * sends SIGUSR2 to only the processes marked as a WAL sender.
	 *
	 * To avoid this corner-case, if recovery is NOT in progress even though
	 * the walsender is cascading one, we do the same thing as SIGUSR2 signal
	 * handler does, i.e., set walsender_ready_to_stop to true. Which causes
	 * the walsender to end later.
	 *
	 * When terminating cascading walsenders, usually postmaster writes the
	 * log message announcing the terminations. But there is a race condition
	 * here. If there is no walsender except this process before reaching
	 * here, postmaster thinks that there is no walsender and suppresses that
	 * log message. To handle this case, we always emit that log message here.
	 * This might cause duplicate log messages, but which is less likely to
	 * happen, so it's not worth writing some code to suppress them.
	 */
	if (am_cascading_walsender && !RecoveryInProgress())
	{
		ereport(LOG,
		   (errmsg("terminating walsender process to force cascaded standby "
				   "to update timeline and reconnect")));
		walsender_ready_to_stop = true;
	}

	/*
	 * We assume here that we're logging enough information in the WAL for
	 * log-shipping, since this is checked in PostmasterMain().
	 *
	 * NOTE: wal_level can only change at shutdown, so in most cases it is
	 * difficult for there to be WAL data that we can still see that was
	 * written at wal_level='minimal'.
	 */

	/*
	 * When we first start replication the standby will be behind the primary.
	 * For some applications, for example, synchronous replication, it is
	 * important to have a clear state for this initial catchup mode, so we
	 * can trigger actions when we change streaming state later. We may stay
	 * in this state for a long time, which is exactly why we want to be able
	 * to monitor whether or not we are still here.
	 */
	WalSndSetState(WALSNDSTATE_CATCHUP);

	/* Send a CopyBothResponse message, and start streaming */
	pq_beginmessage(&buf, 'W');
	pq_sendbyte(&buf, 0);
	pq_sendint(&buf, 0, 2);
	pq_endmessage(&buf);
	pq_flush();

	/*
	 * Initialize position to the received one, then the xlog records begin to
	 * be shipped from that position
	 */
	sentPtr = cmd->startpoint;

	/* Also update the start position status in shared memory */
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile WalSnd *walsnd = MyWalSnd;

		SpinLockAcquire(&walsnd->mutex);
		walsnd->sentPtr = sentPtr;
		SpinLockRelease(&walsnd->mutex);
	}

	SyncRepInitConfig();

	/* Main loop of walsender */
	WalSndLoop();
}

/*
 * Execute an incoming replication command.
 */
void
exec_replication_command(const char *cmd_string)
{
	int			parse_rc;
	Node	   *cmd_node;
	MemoryContext cmd_context;
	MemoryContext old_context;

	elog(DEBUG1, "received replication command: %s", cmd_string);

	CHECK_FOR_INTERRUPTS();

	cmd_context = AllocSetContextCreate(CurrentMemoryContext,
										"Replication command context",
										ALLOCSET_DEFAULT_MINSIZE,
										ALLOCSET_DEFAULT_INITSIZE,
										ALLOCSET_DEFAULT_MAXSIZE);
	old_context = MemoryContextSwitchTo(cmd_context);

	replication_scanner_init(cmd_string);
	parse_rc = replication_yyparse();
	if (parse_rc != 0)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 (errmsg_internal("replication command parser returned %d",
								  parse_rc))));

	cmd_node = replication_parse_result;

	switch (cmd_node->type)
	{
		case T_IdentifySystemCmd:
			IdentifySystem();
			break;

		case T_StartReplicationCmd:
			StartReplication((StartReplicationCmd *) cmd_node);
			break;

		case T_BaseBackupCmd:
			SendBaseBackup((BaseBackupCmd *) cmd_node);
			break;

		default:
			ereport(FATAL,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("invalid standby query string: %s", cmd_string)));
	}

	/* done */
	MemoryContextSwitchTo(old_context);
	MemoryContextDelete(cmd_context);

	/* Send CommandComplete message */
	EndCommand("SELECT", DestRemote);
}

/*
 * Check if the remote end has closed the connection.
 */
static void
ProcessRepliesIfAny(void)
{
	unsigned char firstchar;
	int			r;
	bool		received = false;

	for (;;)
	{
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
			break;
		}

		/* Handle the very limited subset of commands expected in this phase */
		switch (firstchar)
		{
				/*
				 * 'd' means a standby reply wrapped in a CopyData packet.
				 */
			case 'd':
				ProcessStandbyMessage();
				received = true;
				break;

				/*
				 * 'X' means that the standby is closing down the socket.
				 */
			case 'X':
				proc_exit(0);

			default:
				ereport(FATAL,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("invalid standby message type \"%c\"",
								firstchar)));
		}
	}

	/*
	 * Save the last reply timestamp if we've received at least one reply.
	 */
	if (received)
	{
		last_reply_timestamp = GetCurrentTimestamp();
		ping_sent = false;
	}
}

/*
 * Process a status update message received from standby.
 */
static void
ProcessStandbyMessage(void)
{
	char		msgtype;

	resetStringInfo(&reply_message);

	/*
	 * Read the message contents.
	 */
	if (pq_getmessage(&reply_message, 0))
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("unexpected EOF on standby connection")));
		proc_exit(0);
	}

	/*
	 * Check message type from the first byte.
	 */
	msgtype = pq_getmsgbyte(&reply_message);

	switch (msgtype)
	{
		case 'r':
			ProcessStandbyReplyMessage();
			break;

		case 'h':
			ProcessStandbyHSFeedbackMessage();
			break;

		default:
			ereport(COMMERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("unexpected message type \"%c\"", msgtype)));
			proc_exit(0);
	}
}

/*
 * Regular reply from standby advising of WAL positions on standby server.
 */
static void
ProcessStandbyReplyMessage(void)
{
	XLogRecPtr	writePtr,
				flushPtr,
				applyPtr;
	bool		replyRequested;

	/* the caller already consumed the msgtype byte */
	writePtr = pq_getmsgint64(&reply_message);
	flushPtr = pq_getmsgint64(&reply_message);
	applyPtr = pq_getmsgint64(&reply_message);
	(void) pq_getmsgint64(&reply_message);	/* sendTime; not used ATM */
	replyRequested = pq_getmsgbyte(&reply_message);

	elog(DEBUG2, "write %X/%X flush %X/%X apply %X/%X%s",
		 (uint32) (writePtr >> 32), (uint32) writePtr,
		 (uint32) (flushPtr >> 32), (uint32) flushPtr,
		 (uint32) (applyPtr >> 32), (uint32) applyPtr,
		 replyRequested ? " (reply requested)" : "");

	/* Send a reply if the standby requested one. */
	if (replyRequested)
		WalSndKeepalive(false);

	/*
	 * Update shared state for this WalSender process based on reply data from
	 * standby.
	 */
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile WalSnd *walsnd = MyWalSnd;

		SpinLockAcquire(&walsnd->mutex);
		walsnd->write = writePtr;
		walsnd->flush = flushPtr;
		walsnd->apply = applyPtr;
		SpinLockRelease(&walsnd->mutex);
	}

	if (!am_cascading_walsender)
		SyncRepReleaseWaiters();
}

/*
 * Hot Standby feedback
 */
static void
ProcessStandbyHSFeedbackMessage(void)
{
	TransactionId nextXid;
	uint32		nextEpoch;
	TransactionId feedbackXmin;
	uint32		feedbackEpoch;

	/*
	 * Decipher the reply message. The caller already consumed the msgtype
	 * byte.
	 */
	(void) pq_getmsgint64(&reply_message);	/* sendTime; not used ATM */
	feedbackXmin = pq_getmsgint(&reply_message, 4);
	feedbackEpoch = pq_getmsgint(&reply_message, 4);

	elog(DEBUG2, "hot standby feedback xmin %u epoch %u",
		 feedbackXmin,
		 feedbackEpoch);

	/* Ignore invalid xmin (can't actually happen with current walreceiver) */
	if (!TransactionIdIsNormal(feedbackXmin))
		return;

	/*
	 * Check that the provided xmin/epoch are sane, that is, not in the future
	 * and not so far back as to be already wrapped around.  Ignore if not.
	 *
	 * Epoch of nextXid should be same as standby, or if the counter has
	 * wrapped, then one greater than standby.
	 */
	GetNextXidAndEpoch(&nextXid, &nextEpoch);

	if (feedbackXmin <= nextXid)
	{
		if (feedbackEpoch != nextEpoch)
			return;
	}
	else
	{
		if (feedbackEpoch + 1 != nextEpoch)
			return;
	}

	if (!TransactionIdPrecedesOrEquals(feedbackXmin, nextXid))
		return;					/* epoch OK, but it's wrapped around */

	/*
	 * Set the WalSender's xmin equal to the standby's requested xmin, so that
	 * the xmin will be taken into account by GetOldestXmin.  This will hold
	 * back the removal of dead rows and thereby prevent the generation of
	 * cleanup conflicts on the standby server.
	 *
	 * There is a small window for a race condition here: although we just
	 * checked that feedbackXmin precedes nextXid, the nextXid could have gotten
	 * advanced between our fetching it and applying the xmin below, perhaps
	 * far enough to make feedbackXmin wrap around.  In that case the xmin we
	 * set here would be "in the future" and have no effect.  No point in
	 * worrying about this since it's too late to save the desired data
	 * anyway.	Assuming that the standby sends us an increasing sequence of
	 * xmins, this could only happen during the first reply cycle, else our
	 * own xmin would prevent nextXid from advancing so far.
	 *
	 * We don't bother taking the ProcArrayLock here.  Setting the xmin field
	 * is assumed atomic, and there's no real need to prevent a concurrent
	 * GetOldestXmin.  (If we're moving our xmin forward, this is obviously
	 * safe, and if we're moving it backwards, well, the data is at risk
	 * already since a VACUUM could have just finished calling GetOldestXmin.)
	 */
	MyPgXact->xmin = feedbackXmin;
}

/* Main loop of walsender process that streams the WAL over Copy messages. */
static void
WalSndLoop(void)
{
	bool		caughtup = false;

	/*
	 * Allocate buffers that will be used for each outgoing and incoming
	 * message.  We do this just once to reduce palloc overhead.
	 */
	initStringInfo(&output_message);
	initStringInfo(&reply_message);
	initStringInfo(&tmpbuf);

	/* Initialize the last reply timestamp */
	last_reply_timestamp = GetCurrentTimestamp();
	ping_sent = false;

	/* Loop forever, unless we get an error */
	for (;;)
	{
		/* Clear any already-pending wakeups */
		ResetLatch(&MyWalSnd->latch);

		/*
		 * Emergency bailout if postmaster has died.  This is to avoid the
		 * necessity for manual cleanup of all postmaster children.
		 */
		if (!PostmasterIsAlive())
			exit(1);

		/* Process any requests or signals received recently */
		if (got_SIGHUP)
		{
			got_SIGHUP = false;
			ProcessConfigFile(PGC_SIGHUP);
			SyncRepInitConfig();
		}

		CHECK_FOR_INTERRUPTS();

		/* Check for input from the client */
		ProcessRepliesIfAny();

		/*
		 * If we don't have any pending data in the output buffer, try to send
		 * some more.  If there is some, we don't bother to call XLogSend
		 * again until we've flushed it ... but we'd better assume we are not
		 * caught up.
		 */
		if (!pq_is_send_pending())
			XLogSend(&caughtup);
		else
			caughtup = false;

		/* Try to flush pending output to the client */
		if (pq_flush_if_writable() != 0)
			break;

		/* If nothing remains to be sent right now ... */
		if (caughtup && !pq_is_send_pending())
		{
			/*
			 * If we're in catchup state, move to streaming.  This is an
			 * important state change for users to know about, since before
			 * this point data loss might occur if the primary dies and we
			 * need to failover to the standby. The state change is also
			 * important for synchronous replication, since commits that
			 * started to wait at that point might wait for some time.
			 */
			if (MyWalSnd->state == WALSNDSTATE_CATCHUP)
			{
				ereport(DEBUG1,
					 (errmsg("standby \"%s\" has now caught up with primary",
							 application_name)));
				WalSndSetState(WALSNDSTATE_STREAMING);
			}

			/*
			 * When SIGUSR2 arrives, we send any outstanding logs up to the
			 * shutdown checkpoint record (i.e., the latest record) and exit.
			 * This may be a normal termination at shutdown, or a promotion,
			 * the walsender is not sure which.
			 */
			if (walsender_ready_to_stop)
			{
				/* ... let's just be real sure we're caught up ... */
				XLogSend(&caughtup);
				if (caughtup && !pq_is_send_pending())
				{
					/* Inform the standby that XLOG streaming is done */
					pq_puttextmessage('C', "COPY 0");
					pq_flush();

					proc_exit(0);
				}
			}
		}

		/*
		 * We don't block if not caught up, unless there is unsent data
		 * pending in which case we'd better block until the socket is
		 * write-ready.  This test is only needed for the case where XLogSend
		 * loaded a subset of the available data but then pq_flush_if_writable
		 * flushed it all --- we should immediately try to send more.
		 */
		if (caughtup || pq_is_send_pending())
		{
			TimestampTz timeout = 0;
			long		sleeptime = 10000;		/* 10 s */
			int			wakeEvents;

			wakeEvents = WL_LATCH_SET | WL_POSTMASTER_DEATH |
				WL_SOCKET_READABLE | WL_TIMEOUT;

			if (pq_is_send_pending())
				wakeEvents |= WL_SOCKET_WRITEABLE;
			else if (wal_sender_timeout > 0 && !ping_sent)
			{
				/*
				 * If half of wal_sender_timeout has lapsed without receiving
				 * any reply from standby, send a keep-alive message to standby
				 * requesting an immediate reply.
				 */
				timeout = TimestampTzPlusMilliseconds(last_reply_timestamp,
													  wal_sender_timeout / 2);
				if (GetCurrentTimestamp() >= timeout)
				{
					WalSndKeepalive(true);
					ping_sent = true;
					/* Try to flush pending output to the client */
					if (pq_flush_if_writable() != 0)
						break;
				}
			}

			/* Determine time until replication timeout */
			if (wal_sender_timeout > 0)
			{
				timeout = TimestampTzPlusMilliseconds(last_reply_timestamp,
													  wal_sender_timeout);
				sleeptime = 1 + (wal_sender_timeout / 10);
			}

			/* Sleep until something happens or we time out */
			ImmediateInterruptOK = true;
			CHECK_FOR_INTERRUPTS();
			WaitLatchOrSocket(&MyWalSnd->latch, wakeEvents,
							  MyProcPort->sock, sleeptime);
			ImmediateInterruptOK = false;

			/*
			 * Check for replication timeout.  Note we ignore the corner case
			 * possibility that the client replied just as we reached the
			 * timeout ... he's supposed to reply *before* that.
			 */
			if (wal_sender_timeout > 0 && GetCurrentTimestamp() >= timeout)
			{
				/*
				 * Since typically expiration of replication timeout means
				 * communication problem, we don't send the error message to
				 * the standby.
				 */
				ereport(COMMERROR,
						(errmsg("terminating walsender process due to replication timeout")));
				break;
			}
		}
	}

	/*
	 * Get here on send failure.  Clean up and exit.
	 *
	 * Reset whereToSendOutput to prevent ereport from attempting to send any
	 * more messages to the standby.
	 */
	if (whereToSendOutput == DestRemote)
		whereToSendOutput = DestNone;

	proc_exit(0);
	abort();					/* keep the compiler quiet */
}

/* Initialize a per-walsender data structure for this walsender process */
static void
InitWalSenderSlot(void)
{
	int			i;

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
	for (i = 0; i < max_wal_senders; i++)
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile WalSnd *walsnd = &WalSndCtl->walsnds[i];

		SpinLockAcquire(&walsnd->mutex);

		if (walsnd->pid != 0)
		{
			SpinLockRelease(&walsnd->mutex);
			continue;
		}
		else
		{
			/*
			 * Found a free slot. Reserve it for us.
			 */
			walsnd->pid = MyProcPid;
			MemSet(&walsnd->sentPtr, 0, sizeof(XLogRecPtr));
			walsnd->state = WALSNDSTATE_STARTUP;
			SpinLockRelease(&walsnd->mutex);
			/* don't need the lock anymore */
			OwnLatch((Latch *) &walsnd->latch);
			MyWalSnd = (WalSnd *) walsnd;

			break;
		}
	}
	if (MyWalSnd == NULL)
		ereport(FATAL,
				(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
				 errmsg("number of requested standby connections "
						"exceeds max_wal_senders (currently %d)",
						max_wal_senders)));

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
	DisownLatch(&MyWalSnd->latch);

	/* WalSnd struct isn't mine anymore */
	MyWalSnd = NULL;
}

/*
 * Read 'count' bytes from WAL into 'buf', starting at location 'startptr'
 *
 * XXX probably this should be improved to suck data directly from the
 * WAL buffers when possible.
 *
 * Will open, and keep open, one WAL segment stored in the global file
 * descriptor sendFile. This means if XLogRead is used once, there will
 * always be one descriptor left open until the process ends, but never
 * more than one.
 */
void
XLogRead(char *buf, XLogRecPtr startptr, Size count)
{
	char	   *p;
	XLogRecPtr	recptr;
	Size		nbytes;
	XLogSegNo	lastRemovedSegNo;
	XLogSegNo	segno;

retry:
	p = buf;
	recptr = startptr;
	nbytes = count;

	while (nbytes > 0)
	{
		uint32		startoff;
		int			segbytes;
		int			readbytes;

		startoff = recptr % XLogSegSize;

		if (sendFile < 0 || !XLByteInSeg(recptr, sendSegNo))
		{
			char		path[MAXPGPATH];

			/* Switch to another logfile segment */
			if (sendFile >= 0)
				close(sendFile);

			XLByteToSeg(recptr, sendSegNo);
			XLogFilePath(path, ThisTimeLineID, sendSegNo);

			sendFile = BasicOpenFile(path, O_RDONLY | PG_BINARY, 0);
			if (sendFile < 0)
			{
				/*
				 * If the file is not found, assume it's because the standby
				 * asked for a too old WAL segment that has already been
				 * removed or recycled.
				 */
				if (errno == ENOENT)
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("requested WAL segment %s has already been removed",
									XLogFileNameP(ThisTimeLineID, sendSegNo))));
				else
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not open file \"%s\": %m",
									path)));
			}
			sendOff = 0;
		}

		/* Need to seek in the file? */
		if (sendOff != startoff)
		{
			if (lseek(sendFile, (off_t) startoff, SEEK_SET) < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not seek in log segment %s to offset %u: %m",
								XLogFileNameP(ThisTimeLineID, sendSegNo),
								startoff)));
			sendOff = startoff;
		}

		/* How many bytes are within this segment? */
		if (nbytes > (XLogSegSize - startoff))
			segbytes = XLogSegSize - startoff;
		else
			segbytes = nbytes;

		readbytes = read(sendFile, p, segbytes);
		if (readbytes <= 0)
		{
			ereport(ERROR,
					(errcode_for_file_access(),
			errmsg("could not read from log segment %s, offset %u, length %lu: %m",
				   XLogFileNameP(ThisTimeLineID, sendSegNo),
				   sendOff, (unsigned long) segbytes)));
		}

		/* Update state for read */
		XLByteAdvance(recptr, readbytes);

		sendOff += readbytes;
		nbytes -= readbytes;
		p += readbytes;
	}

	/*
	 * After reading into the buffer, check that what we read was valid. We do
	 * this after reading, because even though the segment was present when we
	 * opened it, it might get recycled or removed while we read it. The
	 * read() succeeds in that case, but the data we tried to read might
	 * already have been overwritten with new WAL records.
	 */
	XLogGetLastRemoved(&lastRemovedSegNo);
	XLByteToSeg(startptr, segno);
	if (segno <= lastRemovedSegNo)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("requested WAL segment %s has already been removed",
						XLogFileNameP(ThisTimeLineID, segno))));

	/*
	 * During recovery, the currently-open WAL file might be replaced with the
	 * file of the same name retrieved from archive. So we always need to
	 * check what we read was valid after reading into the buffer. If it's
	 * invalid, we try to open and read the file again.
	 */
	if (am_cascading_walsender)
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile WalSnd *walsnd = MyWalSnd;
		bool		reload;

		SpinLockAcquire(&walsnd->mutex);
		reload = walsnd->needreload;
		walsnd->needreload = false;
		SpinLockRelease(&walsnd->mutex);

		if (reload && sendFile >= 0)
		{
			close(sendFile);
			sendFile = -1;

			goto retry;
		}
	}
}

/*
 * Read up to MAX_SEND_SIZE bytes of WAL that's been flushed to disk,
 * but not yet sent to the client, and buffer it in the libpq output
 * buffer.
 *
 * If there is no unsent WAL remaining, *caughtup is set to true, otherwise
 * *caughtup is set to false.
 */
static void
XLogSend(bool *caughtup)
{
	XLogRecPtr	SendRqstPtr;
	XLogRecPtr	startptr;
	XLogRecPtr	endptr;
	Size		nbytes;

	/*
	 * Attempt to send all data that's already been written out and fsync'd to
	 * disk.  We cannot go further than what's been written out given the
	 * current implementation of XLogRead().  And in any case it's unsafe to
	 * send WAL that is not securely down to disk on the master: if the master
	 * subsequently crashes and restarts, slaves must not have applied any WAL
	 * that gets lost on the master.
	 */
	if (am_cascading_walsender)
	{
		TimeLineID	currentTargetTLI;
		SendRqstPtr = GetStandbyFlushRecPtr(&currentTargetTLI);

		/*
		 * If the recovery target timeline changed, bail out. It's a bit
		 * unfortunate that we have to just disconnect, but there is no way
		 * to tell the client that the timeline changed. We also don't know
		 * exactly where the switch happened, so we cannot safely try to send
		 * up to the switchover point before disconnecting.
		 */
		if (currentTargetTLI != ThisTimeLineID)
		{
			if (!walsender_ready_to_stop)
				ereport(LOG,
						(errmsg("terminating walsender process to force cascaded standby "
								"to update timeline and reconnect")));
			walsender_ready_to_stop = true;
			*caughtup = true;
			return;
		}
	}
	else
		SendRqstPtr = GetFlushRecPtr();

	/* Quick exit if nothing to do */
	if (XLByteLE(SendRqstPtr, sentPtr))
	{
		*caughtup = true;
		return;
	}

	/*
	 * Figure out how much to send in one message. If there's no more than
	 * MAX_SEND_SIZE bytes to send, send everything. Otherwise send
	 * MAX_SEND_SIZE bytes, but round back to logfile or page boundary.
	 *
	 * The rounding is not only for performance reasons. Walreceiver relies on
	 * the fact that we never split a WAL record across two messages. Since a
	 * long WAL record is split at page boundary into continuation records,
	 * page boundary is always a safe cut-off point. We also assume that
	 * SendRqstPtr never points to the middle of a WAL record.
	 */
	startptr = sentPtr;
	endptr = startptr;
	XLByteAdvance(endptr, MAX_SEND_SIZE);

	/* if we went beyond SendRqstPtr, back off */
	if (XLByteLE(SendRqstPtr, endptr))
	{
		endptr = SendRqstPtr;
		*caughtup = true;
	}
	else
	{
		/* round down to page boundary. */
		endptr -= (endptr % XLOG_BLCKSZ);
		*caughtup = false;
	}

	nbytes = endptr - startptr;
	Assert(nbytes <= MAX_SEND_SIZE);

	/*
	 * OK to read and send the slice.
	 */
	resetStringInfo(&output_message);
	pq_sendbyte(&output_message, 'w');

	pq_sendint64(&output_message, startptr);	/* dataStart */
	pq_sendint64(&output_message, SendRqstPtr);	/* walEnd */
	pq_sendint64(&output_message, 0);			/* sendtime, filled in last */

	/*
	 * Read the log directly into the output buffer to avoid extra memcpy
	 * calls.
	 */
	enlargeStringInfo(&output_message, nbytes);
	XLogRead(&output_message.data[output_message.len], startptr, nbytes);
	output_message.len += nbytes;
	output_message.data[output_message.len] = '\0';

	/*
	 * Fill the send timestamp last, so that it is taken as late as possible.
	 */
	resetStringInfo(&tmpbuf);
	pq_sendint64(&tmpbuf, GetCurrentIntegerTimestamp());
	memcpy(&output_message.data[1 + sizeof(int64) + sizeof(int64)],
		   tmpbuf.data, sizeof(int64));

	pq_putmessage_noblock('d', output_message.data, output_message.len);

	sentPtr = endptr;

	/* Update shared memory status */
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile WalSnd *walsnd = MyWalSnd;

		SpinLockAcquire(&walsnd->mutex);
		walsnd->sentPtr = sentPtr;
		SpinLockRelease(&walsnd->mutex);
	}

	/* Report progress of XLOG streaming in PS display */
	if (update_process_title)
	{
		char		activitymsg[50];

		snprintf(activitymsg, sizeof(activitymsg), "streaming %X/%X",
				 (uint32) (sentPtr >> 32), (uint32) sentPtr);
		set_ps_display(activitymsg, false);
	}

	return;
}

/*
 * Request walsenders to reload the currently-open WAL file
 */
void
WalSndRqstFileReload(void)
{
	int			i;

	for (i = 0; i < max_wal_senders; i++)
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile WalSnd *walsnd = &WalSndCtl->walsnds[i];

		if (walsnd->pid == 0)
			continue;

		SpinLockAcquire(&walsnd->mutex);
		walsnd->needreload = true;
		SpinLockRelease(&walsnd->mutex);
	}
}

/* SIGHUP: set flag to re-read config file at next convenient time */
static void
WalSndSigHupHandler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	got_SIGHUP = true;
	if (MyWalSnd)
		SetLatch(&MyWalSnd->latch);

	errno = save_errno;
}

/* SIGUSR1: set flag to send WAL records */
static void
WalSndXLogSendHandler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	latch_sigusr1_handler();

	errno = save_errno;
}

/* SIGUSR2: set flag to do a last cycle and shut down afterwards */
static void
WalSndLastCycleHandler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	walsender_ready_to_stop = true;
	if (MyWalSnd)
		SetLatch(&MyWalSnd->latch);

	errno = save_errno;
}

/* Set up signal handlers */
void
WalSndSignals(void)
{
	/* Set up signal handlers */
	pqsignal(SIGHUP, WalSndSigHupHandler);		/* set flag to read config
												 * file */
	pqsignal(SIGINT, SIG_IGN);	/* not used */
	pqsignal(SIGTERM, die);						/* request shutdown */
	pqsignal(SIGQUIT, quickdie);				/* hard crash time */
	InitializeTimeouts();		/* establishes SIGALRM handler */
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, WalSndXLogSendHandler);	/* request WAL sending */
	pqsignal(SIGUSR2, WalSndLastCycleHandler);	/* request a last cycle and
												 * shutdown */

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
	Size		size = 0;

	size = offsetof(WalSndCtlData, walsnds);
	size = add_size(size, mul_size(max_wal_senders, sizeof(WalSnd)));

	return size;
}

/* Allocate and initialize walsender-related shared memory */
void
WalSndShmemInit(void)
{
	bool		found;
	int			i;

	WalSndCtl = (WalSndCtlData *)
		ShmemInitStruct("Wal Sender Ctl", WalSndShmemSize(), &found);

	if (!found)
	{
		/* First time through, so initialize */
		MemSet(WalSndCtl, 0, WalSndShmemSize());

		for (i = 0; i < NUM_SYNC_REP_WAIT_MODE; i++)
			SHMQueueInit(&(WalSndCtl->SyncRepQueue[i]));

		for (i = 0; i < max_wal_senders; i++)
		{
			WalSnd	   *walsnd = &WalSndCtl->walsnds[i];

			SpinLockInit(&walsnd->mutex);
			InitSharedLatch(&walsnd->latch);
		}
	}
}

/*
 * Wake up all walsenders
 *
 * This will be called inside critical sections, so throwing an error is not
 * adviseable.
 */
void
WalSndWakeup(void)
{
	int			i;

	for (i = 0; i < max_wal_senders; i++)
		SetLatch(&WalSndCtl->walsnds[i].latch);
}

/* Set state for current walsender (only called in walsender) */
void
WalSndSetState(WalSndState state)
{
	/* use volatile pointer to prevent code rearrangement */
	volatile WalSnd *walsnd = MyWalSnd;

	Assert(am_walsender);

	if (walsnd->state == state)
		return;

	SpinLockAcquire(&walsnd->mutex);
	walsnd->state = state;
	SpinLockRelease(&walsnd->mutex);
}

/*
 * Return a string constant representing the state. This is used
 * in system views, and should *not* be translated.
 */
static const char *
WalSndGetStateString(WalSndState state)
{
	switch (state)
	{
		case WALSNDSTATE_STARTUP:
			return "startup";
		case WALSNDSTATE_BACKUP:
			return "backup";
		case WALSNDSTATE_CATCHUP:
			return "catchup";
		case WALSNDSTATE_STREAMING:
			return "streaming";
	}
	return "UNKNOWN";
}


/*
 * Returns activity of walsenders, including pids and xlog locations sent to
 * standby servers.
 */
Datum
pg_stat_get_wal_senders(PG_FUNCTION_ARGS)
{
#define PG_STAT_GET_WAL_SENDERS_COLS	8
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	int		   *sync_priority;
	int			priority = 0;
	int			sync_standby = -1;
	int			i;

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
						"allowed in this context")));

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	/*
	 * Get the priorities of sync standbys all in one go, to minimise lock
	 * acquisitions and to allow us to evaluate who is the current sync
	 * standby. This code must match the code in SyncRepReleaseWaiters().
	 */
	sync_priority = palloc(sizeof(int) * max_wal_senders);
	LWLockAcquire(SyncRepLock, LW_SHARED);
	for (i = 0; i < max_wal_senders; i++)
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile WalSnd *walsnd = &WalSndCtl->walsnds[i];

		if (walsnd->pid != 0)
		{
			/*
			 * Treat a standby such as a pg_basebackup background process
			 * which always returns an invalid flush location, as an
			 * asynchronous standby.
			 */
			sync_priority[i] = XLogRecPtrIsInvalid(walsnd->flush) ?
				0 : walsnd->sync_standby_priority;

			if (walsnd->state == WALSNDSTATE_STREAMING &&
				walsnd->sync_standby_priority > 0 &&
				(priority == 0 ||
				 priority > walsnd->sync_standby_priority) &&
				!XLogRecPtrIsInvalid(walsnd->flush))
			{
				priority = walsnd->sync_standby_priority;
				sync_standby = i;
			}
		}
	}
	LWLockRelease(SyncRepLock);

	for (i = 0; i < max_wal_senders; i++)
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile WalSnd *walsnd = &WalSndCtl->walsnds[i];
		char		location[MAXFNAMELEN];
		XLogRecPtr	sentPtr;
		XLogRecPtr	write;
		XLogRecPtr	flush;
		XLogRecPtr	apply;
		WalSndState state;
		Datum		values[PG_STAT_GET_WAL_SENDERS_COLS];
		bool		nulls[PG_STAT_GET_WAL_SENDERS_COLS];

		if (walsnd->pid == 0)
			continue;

		SpinLockAcquire(&walsnd->mutex);
		sentPtr = walsnd->sentPtr;
		state = walsnd->state;
		write = walsnd->write;
		flush = walsnd->flush;
		apply = walsnd->apply;
		SpinLockRelease(&walsnd->mutex);

		memset(nulls, 0, sizeof(nulls));
		values[0] = Int32GetDatum(walsnd->pid);

		if (!superuser())
		{
			/*
			 * Only superusers can see details. Other users only get the pid
			 * value to know it's a walsender, but no details.
			 */
			MemSet(&nulls[1], true, PG_STAT_GET_WAL_SENDERS_COLS - 1);
		}
		else
		{
			values[1] = CStringGetTextDatum(WalSndGetStateString(state));

			snprintf(location, sizeof(location), "%X/%X",
					 (uint32) (sentPtr >> 32), (uint32) sentPtr);
			values[2] = CStringGetTextDatum(location);

			if (write == 0)
				nulls[3] = true;
			snprintf(location, sizeof(location), "%X/%X",
					 (uint32) (write >> 32), (uint32) write);
			values[3] = CStringGetTextDatum(location);

			if (flush == 0)
				nulls[4] = true;
			snprintf(location, sizeof(location), "%X/%X",
					 (uint32) (flush >> 32), (uint32) flush);
			values[4] = CStringGetTextDatum(location);

			if (apply == 0)
				nulls[5] = true;
			snprintf(location, sizeof(location), "%X/%X",
					 (uint32) (apply >> 32), (uint32) apply);
			values[5] = CStringGetTextDatum(location);

			values[6] = Int32GetDatum(sync_priority[i]);

			/*
			 * More easily understood version of standby state. This is purely
			 * informational, not different from priority.
			 */
			if (sync_priority[i] == 0)
				values[7] = CStringGetTextDatum("async");
			else if (i == sync_standby)
				values[7] = CStringGetTextDatum("sync");
			else
				values[7] = CStringGetTextDatum("potential");
		}

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}
	pfree(sync_priority);

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}

/*
  * This function is used to send keepalive message to standby.
  * If requestReply is set, sets a flag in the message requesting the standby
  * to send a message back to us, for heartbeat purposes.
  */
static void
WalSndKeepalive(bool requestReply)
{
	elog(DEBUG2, "sending replication keepalive");

	/* construct the message... */
	resetStringInfo(&output_message);
	pq_sendbyte(&output_message, 'k');
	pq_sendint64(&output_message, sentPtr);
	pq_sendint64(&output_message, GetCurrentIntegerTimestamp());
	pq_sendbyte(&output_message, requestReply ? 1 : 0);

	/* ... and send it wrapped in CopyData */
	pq_putmessage_noblock('d', output_message.data, output_message.len);
}

/*
 * This isn't currently used for anything. Monitoring tools might be
 * interested in the future, and we'll need something like this in the
 * future for synchronous replication.
 */
#ifdef NOT_USED
/*
 * Returns the oldest Send position among walsenders. Or InvalidXLogRecPtr
 * if none.
 */
XLogRecPtr
GetOldestWALSendPointer(void)
{
	XLogRecPtr	oldest = {0, 0};
	int			i;
	bool		found = false;

	for (i = 0; i < max_wal_senders; i++)
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile WalSnd *walsnd = &WalSndCtl->walsnds[i];
		XLogRecPtr	recptr;

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

#endif
