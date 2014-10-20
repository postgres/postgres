/*-------------------------------------------------------------------------
 *
 * walsender.c
 *
 * The WAL sender process (walsender) is new as of Postgres 9.0. It takes
 * care of sending XLOG from the primary server to a single recipient.
 * (Note that there can be more than one walsender process concurrently.)
 * It is started by the postmaster when the walreceiver of a standby server
 * connects to the primary server and requests XLOG streaming replication.
 *
 * A walsender is similar to a regular backend, ie. there is a one-to-one
 * relationship between a connection and a walsender process, but instead
 * of processing SQL queries, it understands a small set of special
 * replication-mode commands. The START_REPLICATION command begins streaming
 * WAL to the client. While streaming, the walsender keeps reading XLOG
 * records from the disk and sends them to the standby server over the
 * COPY protocol, until the either side ends the replication by exiting COPY
 * mode (or until the connection is closed).
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
 * This instructs walsender to send any outstanding WAL, including the
 * shutdown checkpoint record, wait for it to be replicated to the standby,
 * and then exit.
 *
 *
 * Portions Copyright (c) 2010-2014, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/walsender.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <unistd.h>

#include "access/timeline.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog_internal.h"

#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "funcapi.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "nodes/replnodes.h"
#include "replication/basebackup.h"
#include "replication/decode.h"
#include "replication/logical.h"
#include "replication/logicalfuncs.h"
#include "replication/slot.h"
#include "replication/snapbuild.h"
#include "replication/syncrep.h"
#include "replication/slot.h"
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
#include "utils/pg_lsn.h"
#include "utils/ps_status.h"
#include "utils/resowner.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"

/*
 * Maximum data payload in a WAL data message.  Must be >= XLOG_BLCKSZ.
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
bool		am_walsender = false;		/* Am I a walsender process? */
bool		am_cascading_walsender = false;		/* Am I cascading WAL to
												 * another standby? */
bool		am_db_walsender = false;	/* Connected to a database? */

/* User-settable parameters for walsender */
int			max_wal_senders = 0;	/* the maximum number of concurrent walsenders */
int			wal_sender_timeout = 60 * 1000;		/* maximum time to send one
												 * WAL data message */
bool		log_replication_commands = false;

/*
 * State for WalSndWakeupRequest
 */
bool		wake_wal_senders = false;

/*
 * These variables are used similarly to openLogFile/SegNo/Off,
 * but for walsender to read the XLOG.
 */
static int	sendFile = -1;
static XLogSegNo sendSegNo = 0;
static uint32 sendOff = 0;

/* Timeline ID of the currently open file */
static TimeLineID curFileTimeLine = 0;

/*
 * These variables keep track of the state of the timeline we're currently
 * sending. sendTimeLine identifies the timeline. If sendTimeLineIsHistoric,
 * the timeline is not the latest timeline on this server, and the server's
 * history forked off from that timeline at sendTimeLineValidUpto.
 */
static TimeLineID sendTimeLine = 0;
static TimeLineID sendTimeLineNextTLI = 0;
static bool sendTimeLineIsHistoric = false;
static XLogRecPtr sendTimeLineValidUpto = InvalidXLogRecPtr;

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
 * Timestamp of the last receipt of the reply from the standby. Set to 0 if
 * wal_sender_timeout doesn't need to be active.
 */
static TimestampTz last_reply_timestamp = 0;

/* Have we sent a heartbeat message asking for reply, since last reply? */
static bool waiting_for_ping_response = false;

/*
 * While streaming WAL in Copy mode, streamingDoneSending is set to true
 * after we have sent CopyDone. We should not send any more CopyData messages
 * after that. streamingDoneReceiving is set to true when we receive CopyDone
 * from the other end. When both become true, it's time to exit Copy mode.
 */
static bool streamingDoneSending;
static bool streamingDoneReceiving;

/* Are we there yet? */
static bool WalSndCaughtUp = false;

/* Flags set by signal handlers for later service in main loop */
static volatile sig_atomic_t got_SIGHUP = false;
static volatile sig_atomic_t walsender_ready_to_stop = false;

/*
 * This is set while we are streaming. When not set, SIGUSR2 signal will be
 * handled like SIGTERM. When set, the main loop is responsible for checking
 * walsender_ready_to_stop and terminating when it's set (after streaming any
 * remaining WAL).
 */
static volatile sig_atomic_t replication_active = false;

static LogicalDecodingContext *logical_decoding_ctx = NULL;
static XLogRecPtr logical_startptr = InvalidXLogRecPtr;

/* Signal handlers */
static void WalSndSigHupHandler(SIGNAL_ARGS);
static void WalSndXLogSendHandler(SIGNAL_ARGS);
static void WalSndLastCycleHandler(SIGNAL_ARGS);

/* Prototypes for private functions */
typedef void (*WalSndSendDataCallback) (void);
static void WalSndLoop(WalSndSendDataCallback send_data);
static void InitWalSenderSlot(void);
static void WalSndKill(int code, Datum arg);
static void WalSndShutdown(void) __attribute__((noreturn));
static void XLogSendPhysical(void);
static void XLogSendLogical(void);
static void WalSndDone(WalSndSendDataCallback send_data);
static XLogRecPtr GetStandbyFlushRecPtr(void);
static void IdentifySystem(void);
static void CreateReplicationSlot(CreateReplicationSlotCmd *cmd);
static void DropReplicationSlot(DropReplicationSlotCmd *cmd);
static void StartReplication(StartReplicationCmd *cmd);
static void StartLogicalReplication(StartReplicationCmd *cmd);
static void ProcessStandbyMessage(void);
static void ProcessStandbyReplyMessage(void);
static void ProcessStandbyHSFeedbackMessage(void);
static void ProcessRepliesIfAny(void);
static void WalSndKeepalive(bool requestReply);
static void WalSndKeepaliveIfNecessary(TimestampTz now);
static void WalSndCheckTimeOut(TimestampTz now);
static long WalSndComputeSleeptime(TimestampTz now);
static void WalSndPrepareWrite(LogicalDecodingContext *ctx, XLogRecPtr lsn, TransactionId xid, bool last_write);
static void WalSndWriteData(LogicalDecodingContext *ctx, XLogRecPtr lsn, TransactionId xid, bool last_write);
static XLogRecPtr WalSndWaitForWal(XLogRecPtr loc);

static void XLogRead(char *buf, XLogRecPtr startptr, Size count);


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
	 * Let postmaster know that we're a WAL sender. Once we've declared us as
	 * a WAL sender process, postmaster will let us outlive the bgwriter and
	 * kill us last in the shutdown sequence, so we get a chance to stream all
	 * remaining WAL at shutdown, including the shutdown checkpoint. Note that
	 * there's no going back, and we mustn't write any WAL records after this.
	 */
	MarkPostmasterChildWalSender();
	SendPostmasterSignal(PMSIGNAL_ADVANCE_STATE_MACHINE);
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
	LWLockReleaseAll();

	if (sendFile >= 0)
	{
		close(sendFile);
		sendFile = -1;
	}

	if (MyReplicationSlot != NULL)
		ReplicationSlotRelease();

	replication_active = false;
	if (walsender_ready_to_stop)
		proc_exit(0);

	/* Revert back to startup state */
	WalSndSetState(WALSNDSTATE_STARTUP);
}

/*
 * Handle a client's connection abort in an orderly manner.
 */
static void
WalSndShutdown(void)
{
	/*
	 * Reset whereToSendOutput to prevent ereport from attempting to send any
	 * more messages to the standby.
	 */
	if (whereToSendOutput == DestRemote)
		whereToSendOutput = DestNone;

	proc_exit(0);
	abort();					/* keep the compiler quiet */
}

/*
 * Handle the IDENTIFY_SYSTEM command.
 */
static void
IdentifySystem(void)
{
	StringInfoData buf;
	char		sysid[32];
	char		tli[11];
	char		xpos[MAXFNAMELEN];
	XLogRecPtr	logptr;
	char	   *dbname = NULL;

	/*
	 * Reply with a result set with one row, four columns. First col is system
	 * ID, second is timeline ID, third is current xlog location and the
	 * fourth contains the database name if we are connected to one.
	 */

	snprintf(sysid, sizeof(sysid), UINT64_FORMAT,
			 GetSystemIdentifier());

	am_cascading_walsender = RecoveryInProgress();
	if (am_cascading_walsender)
	{
		/* this also updates ThisTimeLineID */
		logptr = GetStandbyFlushRecPtr();
	}
	else
		logptr = GetInsertRecPtr();

	snprintf(tli, sizeof(tli), "%u", ThisTimeLineID);

	snprintf(xpos, sizeof(xpos), "%X/%X", (uint32) (logptr >> 32), (uint32) logptr);

	if (MyDatabaseId != InvalidOid)
	{
		MemoryContext cur = CurrentMemoryContext;

		/* syscache access needs a transaction env. */
		StartTransactionCommand();
		/* make dbname live outside TX context */
		MemoryContextSwitchTo(cur);
		dbname = get_database_name(MyDatabaseId);
		CommitTransactionCommand();
		/* CommitTransactionCommand switches to TopMemoryContext */
		MemoryContextSwitchTo(cur);
	}

	/* Send a RowDescription message */
	pq_beginmessage(&buf, 'T');
	pq_sendint(&buf, 4, 2);		/* 4 fields */

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
	pq_sendstring(&buf, "xlogpos");		/* col name */
	pq_sendint(&buf, 0, 4);		/* table oid */
	pq_sendint(&buf, 0, 2);		/* attnum */
	pq_sendint(&buf, TEXTOID, 4);		/* type oid */
	pq_sendint(&buf, -1, 2);	/* typlen */
	pq_sendint(&buf, 0, 4);		/* typmod */
	pq_sendint(&buf, 0, 2);		/* format code */

	/* fourth field */
	pq_sendstring(&buf, "dbname");		/* col name */
	pq_sendint(&buf, 0, 4);		/* table oid */
	pq_sendint(&buf, 0, 2);		/* attnum */
	pq_sendint(&buf, TEXTOID, 4);		/* type oid */
	pq_sendint(&buf, -1, 2);	/* typlen */
	pq_sendint(&buf, 0, 4);		/* typmod */
	pq_sendint(&buf, 0, 2);		/* format code */
	pq_endmessage(&buf);

	/* Send a DataRow message */
	pq_beginmessage(&buf, 'D');
	pq_sendint(&buf, 4, 2);		/* # of columns */
	pq_sendint(&buf, strlen(sysid), 4); /* col1 len */
	pq_sendbytes(&buf, (char *) &sysid, strlen(sysid));
	pq_sendint(&buf, strlen(tli), 4);	/* col2 len */
	pq_sendbytes(&buf, (char *) tli, strlen(tli));
	pq_sendint(&buf, strlen(xpos), 4);	/* col3 len */
	pq_sendbytes(&buf, (char *) xpos, strlen(xpos));
	/* send NULL if not connected to a database */
	if (dbname)
	{
		pq_sendint(&buf, strlen(dbname), 4);	/* col4 len */
		pq_sendbytes(&buf, (char *) dbname, strlen(dbname));
	}
	else
	{
		pq_sendint(&buf, -1, 4);	/* col4 len, NULL */
	}

	pq_endmessage(&buf);
}


/*
 * Handle TIMELINE_HISTORY command.
 */
static void
SendTimeLineHistory(TimeLineHistoryCmd *cmd)
{
	StringInfoData buf;
	char		histfname[MAXFNAMELEN];
	char		path[MAXPGPATH];
	int			fd;
	off_t		histfilelen;
	off_t		bytesleft;

	/*
	 * Reply with a result set with one row, and two columns. The first col is
	 * the name of the history file, 2nd is the contents.
	 */

	TLHistoryFileName(histfname, cmd->timeline);
	TLHistoryFilePath(path, cmd->timeline);

	/* Send a RowDescription message */
	pq_beginmessage(&buf, 'T');
	pq_sendint(&buf, 2, 2);		/* 2 fields */

	/* first field */
	pq_sendstring(&buf, "filename");	/* col name */
	pq_sendint(&buf, 0, 4);		/* table oid */
	pq_sendint(&buf, 0, 2);		/* attnum */
	pq_sendint(&buf, TEXTOID, 4);		/* type oid */
	pq_sendint(&buf, -1, 2);	/* typlen */
	pq_sendint(&buf, 0, 4);		/* typmod */
	pq_sendint(&buf, 0, 2);		/* format code */

	/* second field */
	pq_sendstring(&buf, "content");		/* col name */
	pq_sendint(&buf, 0, 4);		/* table oid */
	pq_sendint(&buf, 0, 2);		/* attnum */
	pq_sendint(&buf, BYTEAOID, 4);		/* type oid */
	pq_sendint(&buf, -1, 2);	/* typlen */
	pq_sendint(&buf, 0, 4);		/* typmod */
	pq_sendint(&buf, 0, 2);		/* format code */
	pq_endmessage(&buf);

	/* Send a DataRow message */
	pq_beginmessage(&buf, 'D');
	pq_sendint(&buf, 2, 2);		/* # of columns */
	pq_sendint(&buf, strlen(histfname), 4);		/* col1 len */
	pq_sendbytes(&buf, histfname, strlen(histfname));

	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY, 0666);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));

	/* Determine file length and send it to client */
	histfilelen = lseek(fd, 0, SEEK_END);
	if (histfilelen < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not seek to end of file \"%s\": %m", path)));
	if (lseek(fd, 0, SEEK_SET) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
			errmsg("could not seek to beginning of file \"%s\": %m", path)));

	pq_sendint(&buf, histfilelen, 4);	/* col2 len */

	bytesleft = histfilelen;
	while (bytesleft > 0)
	{
		char		rbuf[BLCKSZ];
		int			nread;

		nread = read(fd, rbuf, sizeof(rbuf));
		if (nread <= 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m",
							path)));
		pq_sendbytes(&buf, rbuf, nread);
		bytesleft -= nread;
	}
	CloseTransientFile(fd);

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
	XLogRecPtr	FlushPtr;

	/*
	 * We assume here that we're logging enough information in the WAL for
	 * log-shipping, since this is checked in PostmasterMain().
	 *
	 * NOTE: wal_level can only change at shutdown, so in most cases it is
	 * difficult for there to be WAL data that we can still see that was
	 * written at wal_level='minimal'.
	 */

	if (cmd->slotname)
	{
		ReplicationSlotAcquire(cmd->slotname);
		if (MyReplicationSlot->data.database != InvalidOid)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 (errmsg("cannot use a logical replication slot for physical replication"))));
	}

	/*
	 * Select the timeline. If it was given explicitly by the client, use
	 * that. Otherwise use the timeline of the last replayed record, which is
	 * kept in ThisTimeLineID.
	 */
	if (am_cascading_walsender)
	{
		/* this also updates ThisTimeLineID */
		FlushPtr = GetStandbyFlushRecPtr();
	}
	else
		FlushPtr = GetFlushRecPtr();

	if (cmd->timeline != 0)
	{
		XLogRecPtr	switchpoint;

		sendTimeLine = cmd->timeline;
		if (sendTimeLine == ThisTimeLineID)
		{
			sendTimeLineIsHistoric = false;
			sendTimeLineValidUpto = InvalidXLogRecPtr;
		}
		else
		{
			List	   *timeLineHistory;

			sendTimeLineIsHistoric = true;

			/*
			 * Check that the timeline the client requested for exists, and
			 * the requested start location is on that timeline.
			 */
			timeLineHistory = readTimeLineHistory(ThisTimeLineID);
			switchpoint = tliSwitchPoint(cmd->timeline, timeLineHistory,
										 &sendTimeLineNextTLI);
			list_free_deep(timeLineHistory);

			/*
			 * Found the requested timeline in the history. Check that
			 * requested startpoint is on that timeline in our history.
			 *
			 * This is quite loose on purpose. We only check that we didn't
			 * fork off the requested timeline before the switchpoint. We
			 * don't check that we switched *to* it before the requested
			 * starting point. This is because the client can legitimately
			 * request to start replication from the beginning of the WAL
			 * segment that contains switchpoint, but on the new timeline, so
			 * that it doesn't end up with a partial segment. If you ask for a
			 * too old starting point, you'll get an error later when we fail
			 * to find the requested WAL segment in pg_xlog.
			 *
			 * XXX: we could be more strict here and only allow a startpoint
			 * that's older than the switchpoint, if it's still in the same
			 * WAL segment.
			 */
			if (!XLogRecPtrIsInvalid(switchpoint) &&
				switchpoint < cmd->startpoint)
			{
				ereport(ERROR,
						(errmsg("requested starting point %X/%X on timeline %u is not in this server's history",
								(uint32) (cmd->startpoint >> 32),
								(uint32) (cmd->startpoint),
								cmd->timeline),
						 errdetail("This server's history forked from timeline %u at %X/%X.",
								   cmd->timeline,
								   (uint32) (switchpoint >> 32),
								   (uint32) (switchpoint))));
			}
			sendTimeLineValidUpto = switchpoint;
		}
	}
	else
	{
		sendTimeLine = ThisTimeLineID;
		sendTimeLineValidUpto = InvalidXLogRecPtr;
		sendTimeLineIsHistoric = false;
	}

	streamingDoneSending = streamingDoneReceiving = false;

	/* If there is nothing to stream, don't even enter COPY mode */
	if (!sendTimeLineIsHistoric || cmd->startpoint < sendTimeLineValidUpto)
	{
		/*
		 * When we first start replication the standby will be behind the
		 * primary. For some applications, for example, synchronous
		 * replication, it is important to have a clear state for this initial
		 * catchup mode, so we can trigger actions when we change streaming
		 * state later. We may stay in this state for a long time, which is
		 * exactly why we want to be able to monitor whether or not we are
		 * still here.
		 */
		WalSndSetState(WALSNDSTATE_CATCHUP);

		/* Send a CopyBothResponse message, and start streaming */
		pq_beginmessage(&buf, 'W');
		pq_sendbyte(&buf, 0);
		pq_sendint(&buf, 0, 2);
		pq_endmessage(&buf);
		pq_flush();

		/*
		 * Don't allow a request to stream from a future point in WAL that
		 * hasn't been flushed to disk in this server yet.
		 */
		if (FlushPtr < cmd->startpoint)
		{
			ereport(ERROR,
					(errmsg("requested starting point %X/%X is ahead of the WAL flush position of this server %X/%X",
							(uint32) (cmd->startpoint >> 32),
							(uint32) (cmd->startpoint),
							(uint32) (FlushPtr >> 32),
							(uint32) (FlushPtr))));
		}

		/* Start streaming from the requested point */
		sentPtr = cmd->startpoint;

		/* Initialize shared memory status, too */
		{
			/* use volatile pointer to prevent code rearrangement */
			volatile WalSnd *walsnd = MyWalSnd;

			SpinLockAcquire(&walsnd->mutex);
			walsnd->sentPtr = sentPtr;
			SpinLockRelease(&walsnd->mutex);
		}

		SyncRepInitConfig();

		/* Main loop of walsender */
		replication_active = true;

		WalSndLoop(XLogSendPhysical);

		replication_active = false;
		if (walsender_ready_to_stop)
			proc_exit(0);
		WalSndSetState(WALSNDSTATE_STARTUP);

		Assert(streamingDoneSending && streamingDoneReceiving);
	}

	if (cmd->slotname)
		ReplicationSlotRelease();

	/*
	 * Copy is finished now. Send a single-row result set indicating the next
	 * timeline.
	 */
	if (sendTimeLineIsHistoric)
	{
		char		tli_str[11];
		char		startpos_str[8 + 1 + 8 + 1];

		snprintf(tli_str, sizeof(tli_str), "%u", sendTimeLineNextTLI);
		snprintf(startpos_str, sizeof(startpos_str), "%X/%X",
				 (uint32) (sendTimeLineValidUpto >> 32),
				 (uint32) sendTimeLineValidUpto);

		pq_beginmessage(&buf, 'T');		/* RowDescription */
		pq_sendint(&buf, 2, 2); /* 2 fields */

		/* Field header */
		pq_sendstring(&buf, "next_tli");
		pq_sendint(&buf, 0, 4); /* table oid */
		pq_sendint(&buf, 0, 2); /* attnum */

		/*
		 * int8 may seem like a surprising data type for this, but in theory
		 * int4 would not be wide enough for this, as TimeLineID is unsigned.
		 */
		pq_sendint(&buf, INT8OID, 4);	/* type oid */
		pq_sendint(&buf, -1, 2);
		pq_sendint(&buf, 0, 4);
		pq_sendint(&buf, 0, 2);

		pq_sendstring(&buf, "next_tli_startpos");
		pq_sendint(&buf, 0, 4); /* table oid */
		pq_sendint(&buf, 0, 2); /* attnum */
		pq_sendint(&buf, TEXTOID, 4);	/* type oid */
		pq_sendint(&buf, -1, 2);
		pq_sendint(&buf, 0, 4);
		pq_sendint(&buf, 0, 2);
		pq_endmessage(&buf);

		/* Data row */
		pq_beginmessage(&buf, 'D');
		pq_sendint(&buf, 2, 2); /* number of columns */

		pq_sendint(&buf, strlen(tli_str), 4);	/* length */
		pq_sendbytes(&buf, tli_str, strlen(tli_str));

		pq_sendint(&buf, strlen(startpos_str), 4);		/* length */
		pq_sendbytes(&buf, startpos_str, strlen(startpos_str));

		pq_endmessage(&buf);
	}

	/* Send CommandComplete message */
	pq_puttextmessage('C', "START_STREAMING");
}

/*
 * read_page callback for logical decoding contexts, as a walsender process.
 *
 * Inside the walsender we can do better than logical_read_local_xlog_page,
 * which has to do a plain sleep/busy loop, because the walsender's latch gets
 * set everytime WAL is flushed.
 */
static int
logical_read_xlog_page(XLogReaderState *state, XLogRecPtr targetPagePtr, int reqLen,
				XLogRecPtr targetRecPtr, char *cur_page, TimeLineID *pageTLI)
{
	XLogRecPtr	flushptr;
	int			count;

	/* make sure we have enough WAL available */
	flushptr = WalSndWaitForWal(targetPagePtr + reqLen);

	/* more than one block available */
	if (targetPagePtr + XLOG_BLCKSZ <= flushptr)
		count = XLOG_BLCKSZ;
	/* not enough WAL synced, that can happen during shutdown */
	else if (targetPagePtr + reqLen > flushptr)
		return -1;
	/* part of the page available */
	else
		count = flushptr - targetPagePtr;

	/* now actually read the data, we know it's there */
	XLogRead(cur_page, targetPagePtr, XLOG_BLCKSZ);

	return count;
}

/*
 * Create a new replication slot.
 */
static void
CreateReplicationSlot(CreateReplicationSlotCmd *cmd)
{
	const char *slot_name;
	const char *snapshot_name = NULL;
	char		xpos[MAXFNAMELEN];
	StringInfoData buf;

	Assert(!MyReplicationSlot);

	/* setup state for XLogReadPage */
	sendTimeLineIsHistoric = false;
	sendTimeLine = ThisTimeLineID;

	if (cmd->kind == REPLICATION_KIND_PHYSICAL)
	{
		ReplicationSlotCreate(cmd->slotname, false, RS_PERSISTENT);
	}
	else
	{
		CheckLogicalDecodingRequirements();
		/*
		 * Initially create the slot as ephemeral - that allows us to nicely
		 * handle errors during initialization because it'll get dropped if
		 * this transaction fails. We'll make it persistent at the end.
		 */
		ReplicationSlotCreate(cmd->slotname, true, RS_EPHEMERAL);
	}

	initStringInfo(&output_message);

	slot_name = NameStr(MyReplicationSlot->data.name);

	if (cmd->kind == REPLICATION_KIND_LOGICAL)
	{
		LogicalDecodingContext *ctx;

		ctx = CreateInitDecodingContext(
										cmd->plugin, NIL,
										logical_read_xlog_page,
										WalSndPrepareWrite, WalSndWriteData);

		/*
		 * Signal that we don't need the timeout mechanism. We're just
		 * creating the replication slot and don't yet accept feedback
		 * messages or send keepalives. As we possibly need to wait for
		 * further WAL the walsender would otherwise possibly be killed too
		 * soon.
		 */
		last_reply_timestamp = 0;

		/* build initial snapshot, might take a while */
		DecodingContextFindStartpoint(ctx);

		/*
		 * Export a plain (not of the snapbuild.c type) snapshot to the user
		 * that can be imported into another session.
		 */
		snapshot_name = SnapBuildExportSnapshot(ctx->snapshot_builder);

		/* don't need the decoding context anymore */
		FreeDecodingContext(ctx);

		ReplicationSlotPersist();
	}

	slot_name = NameStr(MyReplicationSlot->data.name);
	snprintf(xpos, sizeof(xpos), "%X/%X",
			 (uint32) (MyReplicationSlot->data.confirmed_flush >> 32),
			 (uint32) MyReplicationSlot->data.confirmed_flush);

	pq_beginmessage(&buf, 'T');
	pq_sendint(&buf, 4, 2);		/* 4 fields */

	/* first field: slot name */
	pq_sendstring(&buf, "slot_name");	/* col name */
	pq_sendint(&buf, 0, 4);		/* table oid */
	pq_sendint(&buf, 0, 2);		/* attnum */
	pq_sendint(&buf, TEXTOID, 4);		/* type oid */
	pq_sendint(&buf, -1, 2);	/* typlen */
	pq_sendint(&buf, 0, 4);		/* typmod */
	pq_sendint(&buf, 0, 2);		/* format code */

	/* second field: LSN at which we became consistent */
	pq_sendstring(&buf, "consistent_point");	/* col name */
	pq_sendint(&buf, 0, 4);		/* table oid */
	pq_sendint(&buf, 0, 2);		/* attnum */
	pq_sendint(&buf, TEXTOID, 4);		/* type oid */
	pq_sendint(&buf, -1, 2);	/* typlen */
	pq_sendint(&buf, 0, 4);		/* typmod */
	pq_sendint(&buf, 0, 2);		/* format code */

	/* third field: exported snapshot's name */
	pq_sendstring(&buf, "snapshot_name");		/* col name */
	pq_sendint(&buf, 0, 4);		/* table oid */
	pq_sendint(&buf, 0, 2);		/* attnum */
	pq_sendint(&buf, TEXTOID, 4);		/* type oid */
	pq_sendint(&buf, -1, 2);	/* typlen */
	pq_sendint(&buf, 0, 4);		/* typmod */
	pq_sendint(&buf, 0, 2);		/* format code */

	/* fourth field: output plugin */
	pq_sendstring(&buf, "output_plugin");		/* col name */
	pq_sendint(&buf, 0, 4);		/* table oid */
	pq_sendint(&buf, 0, 2);		/* attnum */
	pq_sendint(&buf, TEXTOID, 4);		/* type oid */
	pq_sendint(&buf, -1, 2);	/* typlen */
	pq_sendint(&buf, 0, 4);		/* typmod */
	pq_sendint(&buf, 0, 2);		/* format code */

	pq_endmessage(&buf);

	/* Send a DataRow message */
	pq_beginmessage(&buf, 'D');
	pq_sendint(&buf, 4, 2);		/* # of columns */

	/* slot_name */
	pq_sendint(&buf, strlen(slot_name), 4);		/* col1 len */
	pq_sendbytes(&buf, slot_name, strlen(slot_name));

	/* consistent wal location */
	pq_sendint(&buf, strlen(xpos), 4);	/* col2 len */
	pq_sendbytes(&buf, xpos, strlen(xpos));

	/* snapshot name */
	if (snapshot_name != NULL)
	{
		pq_sendint(&buf, strlen(snapshot_name), 4);		/* col3 len */
		pq_sendbytes(&buf, snapshot_name, strlen(snapshot_name));
	}
	else
		pq_sendint(&buf, -1, 4);	/* col3 len, NULL */

	/* plugin */
	if (cmd->plugin != NULL)
	{
		pq_sendint(&buf, strlen(cmd->plugin), 4);		/* col4 len */
		pq_sendbytes(&buf, cmd->plugin, strlen(cmd->plugin));
	}
	else
		pq_sendint(&buf, -1, 4);	/* col4 len, NULL */

	pq_endmessage(&buf);

	/*
	 * release active status again, START_REPLICATION will reacquire it
	 */
	ReplicationSlotRelease();
}

/*
 * Get rid of a replication slot that is no longer wanted.
 */
static void
DropReplicationSlot(DropReplicationSlotCmd *cmd)
{
	ReplicationSlotDrop(cmd->slotname);
	EndCommand("DROP_REPLICATION_SLOT", DestRemote);
}

/*
 * Load previously initiated logical slot and prepare for sending data (via
 * WalSndLoop).
 */
static void
StartLogicalReplication(StartReplicationCmd *cmd)
{
	StringInfoData buf;

	/* make sure that our requirements are still fulfilled */
	CheckLogicalDecodingRequirements();

	Assert(!MyReplicationSlot);

	ReplicationSlotAcquire(cmd->slotname);

	/*
	 * Force a disconnect, so that the decoding code doesn't need to care
	 * about a eventual switch from running in recovery, to running in a
	 * normal environment. Client code is expected to handle reconnects.
	 */
	if (am_cascading_walsender && !RecoveryInProgress())
	{
		ereport(LOG,
				(errmsg("terminating walsender process after promotion")));
		walsender_ready_to_stop = true;
	}

	WalSndSetState(WALSNDSTATE_CATCHUP);

	/* Send a CopyBothResponse message, and start streaming */
	pq_beginmessage(&buf, 'W');
	pq_sendbyte(&buf, 0);
	pq_sendint(&buf, 0, 2);
	pq_endmessage(&buf);
	pq_flush();

	/* setup state for XLogReadPage */
	sendTimeLineIsHistoric = false;
	sendTimeLine = ThisTimeLineID;

	/*
	 * Initialize position to the last ack'ed one, then the xlog records begin
	 * to be shipped from that position.
	 */
	logical_decoding_ctx = CreateDecodingContext(
											   cmd->startpoint, cmd->options,
												 logical_read_xlog_page,
										WalSndPrepareWrite, WalSndWriteData);

	/* Start reading WAL from the oldest required WAL. */
	logical_startptr = MyReplicationSlot->data.restart_lsn;

	/*
	 * Report the location after which we'll send out further commits as the
	 * current sentPtr.
	 */
	sentPtr = MyReplicationSlot->data.confirmed_flush;

	/* Also update the sent position status in shared memory */
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile WalSnd *walsnd = MyWalSnd;

		SpinLockAcquire(&walsnd->mutex);
		walsnd->sentPtr = MyReplicationSlot->data.restart_lsn;
		SpinLockRelease(&walsnd->mutex);
	}

	replication_active = true;

	SyncRepInitConfig();

	/* Main loop of walsender */
	WalSndLoop(XLogSendLogical);

	FreeDecodingContext(logical_decoding_ctx);
	ReplicationSlotRelease();

	replication_active = false;
	if (walsender_ready_to_stop)
		proc_exit(0);
	WalSndSetState(WALSNDSTATE_STARTUP);

	/* Get out of COPY mode (CommandComplete). */
	EndCommand("COPY 0", DestRemote);
}

/*
 * LogicalDecodingContext 'prepare_write' callback.
 *
 * Prepare a write into a StringInfo.
 *
 * Don't do anything lasting in here, it's quite possible that nothing will done
 * with the data.
 */
static void
WalSndPrepareWrite(LogicalDecodingContext *ctx, XLogRecPtr lsn, TransactionId xid, bool last_write)
{
	/* can't have sync rep confused by sending the same LSN several times */
	if (!last_write)
		lsn = InvalidXLogRecPtr;

	resetStringInfo(ctx->out);

	pq_sendbyte(ctx->out, 'w');
	pq_sendint64(ctx->out, lsn);	/* dataStart */
	pq_sendint64(ctx->out, lsn);	/* walEnd */

	/*
	 * Fill out the sendtime later, just as it's done in XLogSendPhysical, but
	 * reserve space here.
	 */
	pq_sendint64(ctx->out, 0);	/* sendtime */
}

/*
 * LogicalDecodingContext 'write' callback.
 *
 * Actually write out data previously prepared by WalSndPrepareWrite out to
 * the network. Take as long as needed, but process replies from the other
 * side and check timeouts during that.
 */
static void
WalSndWriteData(LogicalDecodingContext *ctx, XLogRecPtr lsn, TransactionId xid,
				bool last_write)
{
	/* output previously gathered data in a CopyData packet */
	pq_putmessage_noblock('d', ctx->out->data, ctx->out->len);

	/*
	 * Fill the send timestamp last, so that it is taken as late as possible.
	 * This is somewhat ugly, but the protocol's set as it's already used for
	 * several releases by streaming physical replication.
	 */
	resetStringInfo(&tmpbuf);
	pq_sendint64(&tmpbuf, GetCurrentIntegerTimestamp());
	memcpy(&ctx->out->data[1 + sizeof(int64) + sizeof(int64)],
		   tmpbuf.data, sizeof(int64));

	/* fast path */
	/* Try to flush pending output to the client */
	if (pq_flush_if_writable() != 0)
		WalSndShutdown();

	if (!pq_is_send_pending())
		return;

	for (;;)
	{
		int			wakeEvents;
		long		sleeptime;
		TimestampTz now;

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

		/* Check for input from the client */
		ProcessRepliesIfAny();

		/* Clear any already-pending wakeups */
		ResetLatch(&MyWalSnd->latch);

		/* Try to flush pending output to the client */
		if (pq_flush_if_writable() != 0)
			WalSndShutdown();

		/* If we finished clearing the buffered data, we're done here. */
		if (!pq_is_send_pending())
			break;

		now = GetCurrentTimestamp();

		/* die if timeout was reached */
		WalSndCheckTimeOut(now);

		/* Send keepalive if the time has come */
		WalSndKeepaliveIfNecessary(now);

		sleeptime = WalSndComputeSleeptime(now);

		wakeEvents = WL_LATCH_SET | WL_POSTMASTER_DEATH |
			WL_SOCKET_WRITEABLE | WL_SOCKET_READABLE | WL_TIMEOUT;

		/* Sleep until something happens or we time out */
		ImmediateInterruptOK = true;
		CHECK_FOR_INTERRUPTS();
		WaitLatchOrSocket(&MyWalSnd->latch, wakeEvents,
						  MyProcPort->sock, sleeptime);
		ImmediateInterruptOK = false;
	}

	/* reactivate latch so WalSndLoop knows to continue */
	SetLatch(&MyWalSnd->latch);
}

/*
 * Wait till WAL < loc is flushed to disk so it can be safely read.
 */
static XLogRecPtr
WalSndWaitForWal(XLogRecPtr loc)
{
	int			wakeEvents;
	static XLogRecPtr RecentFlushPtr = InvalidXLogRecPtr;


	/*
	 * Fast path to avoid acquiring the spinlock in the we already know we
	 * have enough WAL available. This is particularly interesting if we're
	 * far behind.
	 */
	if (RecentFlushPtr != InvalidXLogRecPtr &&
		loc <= RecentFlushPtr)
		return RecentFlushPtr;

	/* Get a more recent flush pointer. */
	if (!RecoveryInProgress())
		RecentFlushPtr = GetFlushRecPtr();
	else
		RecentFlushPtr = GetXLogReplayRecPtr(NULL);

	for (;;)
	{
		long		sleeptime;
		TimestampTz now;

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

		/* Check for input from the client */
		ProcessRepliesIfAny();

		/* Clear any already-pending wakeups */
		ResetLatch(&MyWalSnd->latch);

		/* Update our idea of the currently flushed position. */
		if (!RecoveryInProgress())
			RecentFlushPtr = GetFlushRecPtr();
		else
			RecentFlushPtr = GetXLogReplayRecPtr(NULL);

		/*
		 * If postmaster asked us to stop, don't wait here anymore. This will
		 * cause the xlogreader to return without reading a full record, which
		 * is the fastest way to reach the mainloop which then can quit.
		 *
		 * It's important to do this check after the recomputation of
		 * RecentFlushPtr, so we can send all remaining data before shutting
		 * down.
		 */
		if (walsender_ready_to_stop)
			break;

		/*
		 * We only send regular messages to the client for full decoded
		 * transactions, but a synchronous replication and walsender shutdown
		 * possibly are waiting for a later location. So we send pings
		 * containing the flush location every now and then.
		 */
		if (MyWalSnd->flush < sentPtr &&
			MyWalSnd->write < sentPtr &&
			!waiting_for_ping_response)
		{
			WalSndKeepalive(false);
			waiting_for_ping_response = true;
		}

		/* check whether we're done */
		if (loc <= RecentFlushPtr)
			break;

		/* Waiting for new WAL. Since we need to wait, we're now caught up. */
		WalSndCaughtUp = true;

		/*
		 * Try to flush pending output to the client. Also wait for the socket
		 * becoming writable, if there's still pending output after an attempt
		 * to flush. Otherwise we might just sit on output data while waiting
		 * for new WAL being generated.
		 */
		if (pq_flush_if_writable() != 0)
			WalSndShutdown();

		now = GetCurrentTimestamp();

		/* die if timeout was reached */
		WalSndCheckTimeOut(now);

		/* Send keepalive if the time has come */
		WalSndKeepaliveIfNecessary(now);

		sleeptime = WalSndComputeSleeptime(now);

		wakeEvents = WL_LATCH_SET | WL_POSTMASTER_DEATH |
			WL_SOCKET_READABLE | WL_TIMEOUT;

		if (pq_is_send_pending())
			wakeEvents |= WL_SOCKET_WRITEABLE;

		/* Sleep until something happens or we time out */
		ImmediateInterruptOK = true;
		CHECK_FOR_INTERRUPTS();
		WaitLatchOrSocket(&MyWalSnd->latch, wakeEvents,
						  MyProcPort->sock, sleeptime);
		ImmediateInterruptOK = false;
	}

	/* reactivate latch so WalSndLoop knows to continue */
	SetLatch(&MyWalSnd->latch);
	return RecentFlushPtr;
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

	/*
	 * Log replication command if log_replication_commands is enabled.
	 * Even when it's disabled, log the command with DEBUG1 level for
	 * backward compatibility.
	 */
	ereport(log_replication_commands ? LOG : DEBUG1,
			(errmsg("received replication command: %s", cmd_string)));

	/*
	 * CREATE_REPLICATION_SLOT ... LOGICAL exports a snapshot until the next
	 * command arrives. Clean up the old stuff if there's anything.
	 */
	SnapBuildClearExportedSnapshot();

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

		case T_BaseBackupCmd:
			SendBaseBackup((BaseBackupCmd *) cmd_node);
			break;

		case T_CreateReplicationSlotCmd:
			CreateReplicationSlot((CreateReplicationSlotCmd *) cmd_node);
			break;

		case T_DropReplicationSlotCmd:
			DropReplicationSlot((DropReplicationSlotCmd *) cmd_node);
			break;

		case T_StartReplicationCmd:
			{
				StartReplicationCmd *cmd = (StartReplicationCmd *) cmd_node;

				if (cmd->kind == REPLICATION_KIND_PHYSICAL)
					StartReplication(cmd);
				else
					StartLogicalReplication(cmd);
				break;
			}

		case T_TimeLineHistoryCmd:
			SendTimeLineHistory((TimeLineHistoryCmd *) cmd_node);
			break;

		default:
			elog(ERROR, "unrecognized replication command node tag: %u",
				 cmd_node->type);
	}

	/* done */
	MemoryContextSwitchTo(old_context);
	MemoryContextDelete(cmd_context);

	/* Send CommandComplete message */
	EndCommand("SELECT", DestRemote);
}

/*
 * Process any incoming messages while streaming. Also checks if the remote
 * end has closed the connection.
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

		/*
		 * If we already received a CopyDone from the frontend, the frontend
		 * should not send us anything until we've closed our end of the COPY.
		 * XXX: In theory, the frontend could already send the next command
		 * before receiving the CopyDone, but libpq doesn't currently allow
		 * that.
		 */
		if (streamingDoneReceiving && firstchar != 'X')
			ereport(FATAL,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("unexpected standby message type \"%c\", after receiving CopyDone",
							firstchar)));

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
				 * CopyDone means the standby requested to finish streaming.
				 * Reply with CopyDone, if we had not sent that already.
				 */
			case 'c':
				if (!streamingDoneSending)
				{
					pq_putmessage_noblock('c', NULL, 0);
					streamingDoneSending = true;
				}

				/* consume the CopyData message */
				resetStringInfo(&reply_message);
				if (pq_getmessage(&reply_message, 0))
				{
					ereport(COMMERROR,
							(errcode(ERRCODE_PROTOCOL_VIOLATION),
							 errmsg("unexpected EOF on standby connection")));
					proc_exit(0);
				}

				streamingDoneReceiving = true;
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
		waiting_for_ping_response = false;
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
 * Remember that a walreceiver just confirmed receipt of lsn `lsn`.
 */
static void
PhysicalConfirmReceivedLocation(XLogRecPtr lsn)
{
	bool		changed = false;

	/* use volatile pointer to prevent code rearrangement */
	volatile ReplicationSlot *slot = MyReplicationSlot;

	Assert(lsn != InvalidXLogRecPtr);
	SpinLockAcquire(&slot->mutex);
	if (slot->data.restart_lsn != lsn)
	{
		changed = true;
		slot->data.restart_lsn = lsn;
	}
	SpinLockRelease(&slot->mutex);

	if (changed)
	{
		ReplicationSlotMarkDirty();
		ReplicationSlotsComputeRequiredLSN();
	}

	/*
	 * One could argue that the slot should be saved to disk now, but that'd
	 * be energy wasted - the worst lost information can do here is give us
	 * wrong information in a statistics view - we'll just potentially be more
	 * conservative in removing files.
	 */
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
	(void) pq_getmsgint64(&reply_message);		/* sendTime; not used ATM */
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

	/*
	 * Advance our local xmin horizon when the client confirmed a flush.
	 */
	if (MyReplicationSlot && flushPtr != InvalidXLogRecPtr)
	{
		if (MyReplicationSlot->data.database != InvalidOid)
			LogicalConfirmReceivedLocation(flushPtr);
		else
			PhysicalConfirmReceivedLocation(flushPtr);
	}
}

/* compute new replication slot xmin horizon if needed */
static void
PhysicalReplicationSlotNewXmin(TransactionId feedbackXmin)
{
	bool		changed = false;
	volatile ReplicationSlot *slot = MyReplicationSlot;

	SpinLockAcquire(&slot->mutex);
	MyPgXact->xmin = InvalidTransactionId;

	/*
	 * For physical replication we don't need the interlock provided by xmin
	 * and effective_xmin since the consequences of a missed increase are
	 * limited to query cancellations, so set both at once.
	 */
	if (!TransactionIdIsNormal(slot->data.xmin) ||
		!TransactionIdIsNormal(feedbackXmin) ||
		TransactionIdPrecedes(slot->data.xmin, feedbackXmin))
	{
		changed = true;
		slot->data.xmin = feedbackXmin;
		slot->effective_xmin = feedbackXmin;
	}
	SpinLockRelease(&slot->mutex);

	if (changed)
	{
		ReplicationSlotMarkDirty();
		ReplicationSlotsComputeRequiredXmin(false);
	}
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
	(void) pq_getmsgint64(&reply_message);		/* sendTime; not used ATM */
	feedbackXmin = pq_getmsgint(&reply_message, 4);
	feedbackEpoch = pq_getmsgint(&reply_message, 4);

	elog(DEBUG2, "hot standby feedback xmin %u epoch %u",
		 feedbackXmin,
		 feedbackEpoch);

	/* Unset WalSender's xmin if the feedback message value is invalid */
	if (!TransactionIdIsNormal(feedbackXmin))
	{
		MyPgXact->xmin = InvalidTransactionId;
		if (MyReplicationSlot != NULL)
			PhysicalReplicationSlotNewXmin(feedbackXmin);
		return;
	}

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
	 * checked that feedbackXmin precedes nextXid, the nextXid could have
	 * gotten advanced between our fetching it and applying the xmin below,
	 * perhaps far enough to make feedbackXmin wrap around.  In that case the
	 * xmin we set here would be "in the future" and have no effect.  No point
	 * in worrying about this since it's too late to save the desired data
	 * anyway.  Assuming that the standby sends us an increasing sequence of
	 * xmins, this could only happen during the first reply cycle, else our
	 * own xmin would prevent nextXid from advancing so far.
	 *
	 * We don't bother taking the ProcArrayLock here.  Setting the xmin field
	 * is assumed atomic, and there's no real need to prevent a concurrent
	 * GetOldestXmin.  (If we're moving our xmin forward, this is obviously
	 * safe, and if we're moving it backwards, well, the data is at risk
	 * already since a VACUUM could have just finished calling GetOldestXmin.)
	 *
	 * If we're using a replication slot we reserve the xmin via that,
	 * otherwise via the walsender's PGXACT entry.
	 *
	 * XXX: It might make sense to generalize the ephemeral slot concept and
	 * always use the slot mechanism to handle the feedback xmin.
	 */
	if (MyReplicationSlot != NULL)		/* XXX: persistency configurable? */
		PhysicalReplicationSlotNewXmin(feedbackXmin);
	else
		MyPgXact->xmin = feedbackXmin;
}

/*
 * Compute how long send/receive loops should sleep.
 *
 * If wal_sender_timeout is enabled we want to wake up in time to send
 * keepalives and to abort the connection if wal_sender_timeout has been
 * reached.
 */
static long
WalSndComputeSleeptime(TimestampTz now)
{
	long		sleeptime = 10000;		/* 10 s */

	if (wal_sender_timeout > 0 && last_reply_timestamp > 0)
	{
		TimestampTz wakeup_time;
		long		sec_to_timeout;
		int			microsec_to_timeout;

		/*
		 * At the latest stop sleeping once wal_sender_timeout has been
		 * reached.
		 */
		wakeup_time = TimestampTzPlusMilliseconds(last_reply_timestamp,
												  wal_sender_timeout);

		/*
		 * If no ping has been sent yet, wakeup when it's time to do so.
		 * WalSndKeepaliveIfNecessary() wants to send a keepalive once half of
		 * the timeout passed without a response.
		 */
		if (!waiting_for_ping_response)
			wakeup_time = TimestampTzPlusMilliseconds(last_reply_timestamp,
													  wal_sender_timeout / 2);

		/* Compute relative time until wakeup. */
		TimestampDifference(now, wakeup_time,
							&sec_to_timeout, &microsec_to_timeout);

		sleeptime = sec_to_timeout * 1000 +
			microsec_to_timeout / 1000;
	}

	return sleeptime;
}

/*
 * Check whether there have been responses by the client within
 * wal_sender_timeout and shutdown if not.
 */
static void
WalSndCheckTimeOut(TimestampTz now)
{
	TimestampTz timeout;

	/* don't bail out if we're doing something that doesn't require timeouts */
	if (last_reply_timestamp <= 0)
		return;

	timeout = TimestampTzPlusMilliseconds(last_reply_timestamp,
										  wal_sender_timeout);

	if (wal_sender_timeout > 0 && now >= timeout)
	{
		/*
		 * Since typically expiration of replication timeout means
		 * communication problem, we don't send the error message to the
		 * standby.
		 */
		ereport(COMMERROR,
		(errmsg("terminating walsender process due to replication timeout")));

		WalSndShutdown();
	}
}

/* Main loop of walsender process that streams the WAL over Copy messages. */
static void
WalSndLoop(WalSndSendDataCallback send_data)
{
	/*
	 * Allocate buffers that will be used for each outgoing and incoming
	 * message.  We do this just once to reduce palloc overhead.
	 */
	initStringInfo(&output_message);
	initStringInfo(&reply_message);
	initStringInfo(&tmpbuf);

	/*
	 * Initialize the last reply timestamp. That enables timeout processing
	 * from hereon.
	 */
	last_reply_timestamp = GetCurrentTimestamp();
	waiting_for_ping_response = false;

	/*
	 * Loop until we reach the end of this timeline or the client requests to
	 * stop streaming.
	 */
	for (;;)
	{
		TimestampTz now;

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

		/* Clear any already-pending wakeups */
		ResetLatch(&MyWalSnd->latch);

		/*
		 * If we have received CopyDone from the client, sent CopyDone
		 * ourselves, and the output buffer is empty, it's time to exit
		 * streaming.
		 */
		if (!pq_is_send_pending() && streamingDoneSending && streamingDoneReceiving)
			break;

		/*
		 * If we don't have any pending data in the output buffer, try to send
		 * some more.  If there is some, we don't bother to call send_data
		 * again until we've flushed it ... but we'd better assume we are not
		 * caught up.
		 */
		if (!pq_is_send_pending())
			send_data();
		else
			WalSndCaughtUp = false;

		/* Try to flush pending output to the client */
		if (pq_flush_if_writable() != 0)
			WalSndShutdown();

		/* If nothing remains to be sent right now ... */
		if (WalSndCaughtUp && !pq_is_send_pending())
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
			 * shutdown checkpoint record (i.e., the latest record), wait for
			 * them to be replicated to the standby, and exit. This may be a
			 * normal termination at shutdown, or a promotion, the walsender
			 * is not sure which.
			 */
			if (walsender_ready_to_stop)
				WalSndDone(send_data);
		}

		now = GetCurrentTimestamp();

		/* Check for replication timeout. */
		WalSndCheckTimeOut(now);

		/* Send keepalive if the time has come */
		WalSndKeepaliveIfNecessary(now);

		/*
		 * We don't block if not caught up, unless there is unsent data
		 * pending in which case we'd better block until the socket is
		 * write-ready.  This test is only needed for the case where the
		 * send_data callback handled a subset of the available data but then
		 * pq_flush_if_writable flushed it all --- we should immediately try
		 * to send more.
		 */
		if ((WalSndCaughtUp && !streamingDoneSending) || pq_is_send_pending())
		{
			long		sleeptime;
			int			wakeEvents;

			wakeEvents = WL_LATCH_SET | WL_POSTMASTER_DEATH | WL_TIMEOUT |
				WL_SOCKET_READABLE;

			sleeptime = WalSndComputeSleeptime(now);

			if (pq_is_send_pending())
				wakeEvents |= WL_SOCKET_WRITEABLE;

			/* Sleep until something happens or we time out */
			ImmediateInterruptOK = true;
			CHECK_FOR_INTERRUPTS();
			WaitLatchOrSocket(&MyWalSnd->latch, wakeEvents,
							  MyProcPort->sock, sleeptime);
			ImmediateInterruptOK = false;
		}
	}
	return;
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
			walsnd->sentPtr = InvalidXLogRecPtr;
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
	WalSnd	   *walsnd = MyWalSnd;

	Assert(walsnd != NULL);

	/*
	 * Clear MyWalSnd first; then disown the latch.  This is so that signal
	 * handlers won't try to touch the latch after it's no longer ours.
	 */
	MyWalSnd = NULL;

	DisownLatch(&walsnd->latch);

	/*
	 * Mark WalSnd struct no longer in use. Assume that no lock is required
	 * for this.
	 */
	walsnd->pid = 0;
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
static void
XLogRead(char *buf, XLogRecPtr startptr, Size count)
{
	char	   *p;
	XLogRecPtr	recptr;
	Size		nbytes;
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

			/*-------
			 * When reading from a historic timeline, and there is a timeline
			 * switch within this segment, read from the WAL segment belonging
			 * to the new timeline.
			 *
			 * For example, imagine that this server is currently on timeline
			 * 5, and we're streaming timeline 4. The switch from timeline 4
			 * to 5 happened at 0/13002088. In pg_xlog, we have these files:
			 *
			 * ...
			 * 000000040000000000000012
			 * 000000040000000000000013
			 * 000000050000000000000013
			 * 000000050000000000000014
			 * ...
			 *
			 * In this situation, when requested to send the WAL from
			 * segment 0x13, on timeline 4, we read the WAL from file
			 * 000000050000000000000013. Archive recovery prefers files from
			 * newer timelines, so if the segment was restored from the
			 * archive on this server, the file belonging to the old timeline,
			 * 000000040000000000000013, might not exist. Their contents are
			 * equal up to the switchpoint, because at a timeline switch, the
			 * used portion of the old segment is copied to the new file.
			 *-------
			 */
			curFileTimeLine = sendTimeLine;
			if (sendTimeLineIsHistoric)
			{
				XLogSegNo	endSegNo;

				XLByteToSeg(sendTimeLineValidUpto, endSegNo);
				if (sendSegNo == endSegNo)
					curFileTimeLine = sendTimeLineNextTLI;
			}

			XLogFilePath(path, curFileTimeLine, sendSegNo);

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
								XLogFileNameP(curFileTimeLine, sendSegNo))));
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
						 XLogFileNameP(curFileTimeLine, sendSegNo),
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
							XLogFileNameP(curFileTimeLine, sendSegNo),
							sendOff, (unsigned long) segbytes)));
		}

		/* Update state for read */
		recptr += readbytes;

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
	XLByteToSeg(startptr, segno);
	CheckXLogRemoved(segno, ThisTimeLineID);

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
 * Send out the WAL in its normal physical/stored form.
 *
 * Read up to MAX_SEND_SIZE bytes of WAL that's been flushed to disk,
 * but not yet sent to the client, and buffer it in the libpq output
 * buffer.
 *
 * If there is no unsent WAL remaining, WalSndCaughtUp is set to true,
 * otherwise WalSndCaughtUp is set to false.
 */
static void
XLogSendPhysical(void)
{
	XLogRecPtr	SendRqstPtr;
	XLogRecPtr	startptr;
	XLogRecPtr	endptr;
	Size		nbytes;

	if (streamingDoneSending)
	{
		WalSndCaughtUp = true;
		return;
	}

	/* Figure out how far we can safely send the WAL. */
	if (sendTimeLineIsHistoric)
	{
		/*
		 * Streaming an old timeline timeline that's in this server's history,
		 * but is not the one we're currently inserting or replaying. It can
		 * be streamed up to the point where we switched off that timeline.
		 */
		SendRqstPtr = sendTimeLineValidUpto;
	}
	else if (am_cascading_walsender)
	{
		/*
		 * Streaming the latest timeline on a standby.
		 *
		 * Attempt to send all WAL that has already been replayed, so that we
		 * know it's valid. If we're receiving WAL through streaming
		 * replication, it's also OK to send any WAL that has been received
		 * but not replayed.
		 *
		 * The timeline we're recovering from can change, or we can be
		 * promoted. In either case, the current timeline becomes historic. We
		 * need to detect that so that we don't try to stream past the point
		 * where we switched to another timeline. We check for promotion or
		 * timeline switch after calculating FlushPtr, to avoid a race
		 * condition: if the timeline becomes historic just after we checked
		 * that it was still current, it's still be OK to stream it up to the
		 * FlushPtr that was calculated before it became historic.
		 */
		bool		becameHistoric = false;

		SendRqstPtr = GetStandbyFlushRecPtr();

		if (!RecoveryInProgress())
		{
			/*
			 * We have been promoted. RecoveryInProgress() updated
			 * ThisTimeLineID to the new current timeline.
			 */
			am_cascading_walsender = false;
			becameHistoric = true;
		}
		else
		{
			/*
			 * Still a cascading standby. But is the timeline we're sending
			 * still the one recovery is recovering from? ThisTimeLineID was
			 * updated by the GetStandbyFlushRecPtr() call above.
			 */
			if (sendTimeLine != ThisTimeLineID)
				becameHistoric = true;
		}

		if (becameHistoric)
		{
			/*
			 * The timeline we were sending has become historic. Read the
			 * timeline history file of the new timeline to see where exactly
			 * we forked off from the timeline we were sending.
			 */
			List	   *history;

			history = readTimeLineHistory(ThisTimeLineID);
			sendTimeLineValidUpto = tliSwitchPoint(sendTimeLine, history, &sendTimeLineNextTLI);

			Assert(sendTimeLine < sendTimeLineNextTLI);
			list_free_deep(history);

			sendTimeLineIsHistoric = true;

			SendRqstPtr = sendTimeLineValidUpto;
		}
	}
	else
	{
		/*
		 * Streaming the current timeline on a master.
		 *
		 * Attempt to send all data that's already been written out and
		 * fsync'd to disk.  We cannot go further than what's been written out
		 * given the current implementation of XLogRead().  And in any case
		 * it's unsafe to send WAL that is not securely down to disk on the
		 * master: if the master subsequently crashes and restarts, slaves
		 * must not have applied any WAL that gets lost on the master.
		 */
		SendRqstPtr = GetFlushRecPtr();
	}

	/*
	 * If this is a historic timeline and we've reached the point where we
	 * forked to the next timeline, stop streaming.
	 *
	 * Note: We might already have sent WAL > sendTimeLineValidUpto. The
	 * startup process will normally replay all WAL that has been received
	 * from the master, before promoting, but if the WAL streaming is
	 * terminated at a WAL page boundary, the valid portion of the timeline
	 * might end in the middle of a WAL record. We might've already sent the
	 * first half of that partial WAL record to the cascading standby, so that
	 * sentPtr > sendTimeLineValidUpto. That's OK; the cascading standby can't
	 * replay the partial WAL record either, so it can still follow our
	 * timeline switch.
	 */
	if (sendTimeLineIsHistoric && sendTimeLineValidUpto <= sentPtr)
	{
		/* close the current file. */
		if (sendFile >= 0)
			close(sendFile);
		sendFile = -1;

		/* Send CopyDone */
		pq_putmessage_noblock('c', NULL, 0);
		streamingDoneSending = true;

		WalSndCaughtUp = true;

		elog(DEBUG1, "walsender reached end of timeline at %X/%X (sent up to %X/%X)",
			 (uint32) (sendTimeLineValidUpto >> 32), (uint32) sendTimeLineValidUpto,
			 (uint32) (sentPtr >> 32), (uint32) sentPtr);
		return;
	}

	/* Do we have any work to do? */
	Assert(sentPtr <= SendRqstPtr);
	if (SendRqstPtr <= sentPtr)
	{
		WalSndCaughtUp = true;
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
	endptr += MAX_SEND_SIZE;

	/* if we went beyond SendRqstPtr, back off */
	if (SendRqstPtr <= endptr)
	{
		endptr = SendRqstPtr;
		if (sendTimeLineIsHistoric)
			WalSndCaughtUp = false;
		else
			WalSndCaughtUp = true;
	}
	else
	{
		/* round down to page boundary. */
		endptr -= (endptr % XLOG_BLCKSZ);
		WalSndCaughtUp = false;
	}

	nbytes = endptr - startptr;
	Assert(nbytes <= MAX_SEND_SIZE);

	/*
	 * OK to read and send the slice.
	 */
	resetStringInfo(&output_message);
	pq_sendbyte(&output_message, 'w');

	pq_sendint64(&output_message, startptr);	/* dataStart */
	pq_sendint64(&output_message, SendRqstPtr); /* walEnd */
	pq_sendint64(&output_message, 0);	/* sendtime, filled in last */

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
 * Stream out logically decoded data.
 */
static void
XLogSendLogical(void)
{
	XLogRecord *record;
	char	   *errm;

	/*
	 * Don't know whether we've caught up yet. We'll set it to true in
	 * WalSndWaitForWal, if we're actually waiting. We also set to true if
	 * XLogReadRecord() had to stop reading but WalSndWaitForWal didn't wait -
	 * i.e. when we're shutting down.
	 */
	WalSndCaughtUp = false;

	record = XLogReadRecord(logical_decoding_ctx->reader, logical_startptr, &errm);
	logical_startptr = InvalidXLogRecPtr;

	/* xlog record was invalid */
	if (errm != NULL)
		elog(ERROR, "%s", errm);

	if (record != NULL)
	{
		LogicalDecodingProcessRecord(logical_decoding_ctx, record);

		sentPtr = logical_decoding_ctx->reader->EndRecPtr;
	}
	else
	{
		/*
		 * If the record we just wanted read is at or beyond the flushed
		 * point, then we're caught up.
		 */
		if (logical_decoding_ctx->reader->EndRecPtr >= GetFlushRecPtr())
			WalSndCaughtUp = true;
	}

	/* Update shared memory status */
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile WalSnd *walsnd = MyWalSnd;

		SpinLockAcquire(&walsnd->mutex);
		walsnd->sentPtr = sentPtr;
		SpinLockRelease(&walsnd->mutex);
	}
}

/*
 * Shutdown if the sender is caught up.
 *
 * NB: This should only be called when the shutdown signal has been received
 * from postmaster.
 *
 * Note that if we determine that there's still more data to send, this
 * function will return control to the caller.
 */
static void
WalSndDone(WalSndSendDataCallback send_data)
{
	XLogRecPtr	replicatedPtr;

	/* ... let's just be real sure we're caught up ... */
	send_data();

	/*
	 * Check a write location to see whether all the WAL have successfully
	 * been replicated if this walsender is connecting to a standby such as
	 * pg_receivexlog which always returns an invalid flush location.
	 * Otherwise, check a flush location.
	 */
	replicatedPtr = XLogRecPtrIsInvalid(MyWalSnd->flush) ?
		MyWalSnd->write : MyWalSnd->flush;
	if (WalSndCaughtUp && sentPtr == replicatedPtr &&
		!pq_is_send_pending())
	{
		/* Inform the standby that XLOG streaming is done */
		EndCommand("COPY 0", DestRemote);
		pq_flush();

		proc_exit(0);
	}
	if (!waiting_for_ping_response)
		WalSndKeepalive(true);
}

/*
 * Returns the latest point in WAL that has been safely flushed to disk, and
 * can be sent to the standby. This should only be called when in recovery,
 * ie. we're streaming to a cascaded standby.
 *
 * As a side-effect, ThisTimeLineID is updated to the TLI of the last
 * replayed WAL record.
 */
static XLogRecPtr
GetStandbyFlushRecPtr(void)
{
	XLogRecPtr	replayPtr;
	TimeLineID	replayTLI;
	XLogRecPtr	receivePtr;
	TimeLineID	receiveTLI;
	XLogRecPtr	result;

	/*
	 * We can safely send what's already been replayed. Also, if walreceiver
	 * is streaming WAL from the same timeline, we can send anything that it
	 * has streamed, but hasn't been replayed yet.
	 */

	receivePtr = GetWalRcvWriteRecPtr(NULL, &receiveTLI);
	replayPtr = GetXLogReplayRecPtr(&replayTLI);

	ThisTimeLineID = replayTLI;

	result = replayPtr;
	if (receiveTLI == ThisTimeLineID && receivePtr > replayPtr)
		result = receivePtr;

	return result;
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

	/*
	 * If replication has not yet started, die like with SIGTERM. If
	 * replication is active, only set a flag and wake up the main loop. It
	 * will send any outstanding WAL, wait for it to be replicated to the
	 * standby, and then exit gracefully.
	 */
	if (!replication_active)
		kill(MyProcPid, SIGTERM);

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
	pqsignal(SIGTERM, die);		/* request shutdown */
	pqsignal(SIGQUIT, quickdie);	/* hard crash time */
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
			values[2] = LSNGetDatum(sentPtr);

			if (write == 0)
				nulls[3] = true;
			values[3] = LSNGetDatum(write);

			if (flush == 0)
				nulls[4] = true;
			values[4] = LSNGetDatum(flush);

			if (apply == 0)
				nulls[5] = true;
			values[5] = LSNGetDatum(apply);

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
 * Send keepalive message if too much time has elapsed.
 */
static void
WalSndKeepaliveIfNecessary(TimestampTz now)
{
	TimestampTz ping_time;

	/*
	 * Don't send keepalive messages if timeouts are globally disabled or
	 * we're doing something not partaking in timeouts.
	 */
	if (wal_sender_timeout <= 0 || last_reply_timestamp <= 0)
		return;

	if (waiting_for_ping_response)
		return;

	/*
	 * If half of wal_sender_timeout has lapsed without receiving any reply
	 * from the standby, send a keep-alive message to the standby requesting
	 * an immediate reply.
	 */
	ping_time = TimestampTzPlusMilliseconds(last_reply_timestamp,
											wal_sender_timeout / 2);
	if (now >= ping_time)
	{
		WalSndKeepalive(true);
		waiting_for_ping_response = true;

		/* Try to flush pending output to the client */
		if (pq_flush_if_writable() != 0)
			WalSndShutdown();
	}
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

		if (!found || recptr < oldest)
			oldest = recptr;
		found = true;
	}
	return oldest;
}

#endif
