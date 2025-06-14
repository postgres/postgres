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
 * COPY protocol, until either side ends the replication by exiting COPY
 * mode (or until the connection is closed).
 *
 * Normal termination is by SIGTERM, which instructs the walsender to
 * close the connection and exit(0) at the next convenient moment. Emergency
 * termination is by SIGQUIT; like any backend, the walsender will simply
 * abort and exit on SIGQUIT. A close of the connection and a FATAL error
 * are treated as not a crash but approximately normal termination;
 * the walsender will exit quickly without sending any more XLOG records.
 *
 * If the server is shut down, checkpointer sends us
 * PROCSIG_WALSND_INIT_STOPPING after all regular backends have exited.  If
 * the backend is idle or runs an SQL query this causes the backend to
 * shutdown, if logical replication is in progress all existing WAL records
 * are processed followed by a shutdown.  Otherwise this causes the walsender
 * to switch to the "stopping" state. In this state, the walsender will reject
 * any further replication commands. The checkpointer begins the shutdown
 * checkpoint once all walsenders are confirmed as stopping. When the shutdown
 * checkpoint finishes, the postmaster sends us SIGUSR2. This instructs
 * walsender to send any outstanding WAL, including the shutdown checkpoint
 * record, wait for it to be replicated to the standby, and then exit.
 *
 *
 * Portions Copyright (c) 2010-2024, PostgreSQL Global Development Group
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
#include "access/xlogreader.h"
#include "access/xlogrecovery.h"
#include "access/xlogutils.h"
#include "backup/basebackup.h"
#include "backup/basebackup_incremental.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "funcapi.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "nodes/replnodes.h"
#include "pgstat.h"
#include "postmaster/interrupt.h"
#include "replication/decode.h"
#include "replication/logical.h"
#include "replication/slotsync.h"
#include "replication/slot.h"
#include "replication/snapbuild.h"
#include "replication/syncrep.h"
#include "replication/walreceiver.h"
#include "replication/walsender.h"
#include "replication/walsender_private.h"
#include "storage/condition_variable.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "tcop/dest.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/pg_lsn.h"
#include "utils/pgstat_internal.h"
#include "utils/ps_status.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"

/* Minimum interval used by walsender for stats flushes, in ms */
#define WALSENDER_STATS_FLUSH_INTERVAL         1000

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
bool		am_walsender = false;	/* Am I a walsender process? */
bool		am_cascading_walsender = false; /* Am I cascading WAL to another
											 * standby? */
bool		am_db_walsender = false;	/* Connected to a database? */

/* GUC variables */
int			max_wal_senders = 10;	/* the maximum number of concurrent
									 * walsenders */
int			wal_sender_timeout = 60 * 1000; /* maximum time to send one WAL
											 * data message */
bool		log_replication_commands = false;

/*
 * State for WalSndWakeupRequest
 */
bool		wake_wal_senders = false;

/*
 * xlogreader used for replication.  Note that a WAL sender doing physical
 * replication does not need xlogreader to read WAL, but it needs one to
 * keep a state of its work.
 */
static XLogReaderState *xlogreader = NULL;

/*
 * If the UPLOAD_MANIFEST command is used to provide a backup manifest in
 * preparation for an incremental backup, uploaded_manifest will be point
 * to an object containing information about its contexts, and
 * uploaded_manifest_mcxt will point to the memory context that contains
 * that object and all of its subordinate data. Otherwise, both values will
 * be NULL.
 */
static IncrementalBackupInfo *uploaded_manifest = NULL;
static MemoryContext uploaded_manifest_mcxt = NULL;

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
static XLogRecPtr sentPtr = InvalidXLogRecPtr;

/* Buffers for constructing outgoing messages and processing reply messages. */
static StringInfoData output_message;
static StringInfoData reply_message;
static StringInfoData tmpbuf;

/* Timestamp of last ProcessRepliesIfAny(). */
static TimestampTz last_processing = 0;

/*
 * Timestamp of last ProcessRepliesIfAny() that saw a reply from the
 * standby. Set to 0 if wal_sender_timeout doesn't need to be active.
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
static volatile sig_atomic_t got_SIGUSR2 = false;
static volatile sig_atomic_t got_STOPPING = false;

/*
 * This is set while we are streaming. When not set
 * PROCSIG_WALSND_INIT_STOPPING signal will be handled like SIGTERM. When set,
 * the main loop is responsible for checking got_STOPPING and terminating when
 * it's set (after streaming any remaining WAL).
 */
static volatile sig_atomic_t replication_active = false;

static LogicalDecodingContext *logical_decoding_ctx = NULL;

/* A sample associating a WAL location with the time it was written. */
typedef struct
{
	XLogRecPtr	lsn;
	TimestampTz time;
} WalTimeSample;

/* The size of our buffer of time samples. */
#define LAG_TRACKER_BUFFER_SIZE 8192

/* A mechanism for tracking replication lag. */
typedef struct
{
	XLogRecPtr	last_lsn;
	WalTimeSample buffer[LAG_TRACKER_BUFFER_SIZE];
	int			write_head;
	int			read_heads[NUM_SYNC_REP_WAIT_MODE];
	WalTimeSample last_read[NUM_SYNC_REP_WAIT_MODE];
} LagTracker;

static LagTracker *lag_tracker;

/* Signal handlers */
static void WalSndLastCycleHandler(SIGNAL_ARGS);

/* Prototypes for private functions */
typedef void (*WalSndSendDataCallback) (void);
static void WalSndLoop(WalSndSendDataCallback send_data);
static void InitWalSenderSlot(void);
static void WalSndKill(int code, Datum arg);
static void WalSndShutdown(void) pg_attribute_noreturn();
static void XLogSendPhysical(void);
static void XLogSendLogical(void);
static void WalSndDone(WalSndSendDataCallback send_data);
static void IdentifySystem(void);
static void UploadManifest(void);
static bool HandleUploadManifestPacket(StringInfo buf, off_t *offset,
									   IncrementalBackupInfo *ib);
static void ReadReplicationSlot(ReadReplicationSlotCmd *cmd);
static void CreateReplicationSlot(CreateReplicationSlotCmd *cmd);
static void DropReplicationSlot(DropReplicationSlotCmd *cmd);
static void StartReplication(StartReplicationCmd *cmd);
static void StartLogicalReplication(StartReplicationCmd *cmd);
static void ProcessStandbyMessage(void);
static void ProcessStandbyReplyMessage(void);
static void ProcessStandbyHSFeedbackMessage(void);
static void ProcessRepliesIfAny(void);
static void ProcessPendingWrites(void);
static void WalSndKeepalive(bool requestReply, XLogRecPtr writePtr);
static void WalSndKeepaliveIfNecessary(void);
static void WalSndCheckTimeOut(void);
static long WalSndComputeSleeptime(TimestampTz now);
static void WalSndWait(uint32 socket_events, long timeout, uint32 wait_event);
static void WalSndPrepareWrite(LogicalDecodingContext *ctx, XLogRecPtr lsn, TransactionId xid, bool last_write);
static void WalSndWriteData(LogicalDecodingContext *ctx, XLogRecPtr lsn, TransactionId xid, bool last_write);
static void WalSndUpdateProgress(LogicalDecodingContext *ctx, XLogRecPtr lsn, TransactionId xid,
								 bool skipped_xact);
static XLogRecPtr WalSndWaitForWal(XLogRecPtr loc);
static void LagTrackerWrite(XLogRecPtr lsn, TimestampTz local_flush_time);
static TimeOffset LagTrackerRead(int head, XLogRecPtr lsn, TimestampTz now);
static bool TransactionIdInRecentPast(TransactionId xid, uint32 epoch);

static void WalSndSegmentOpen(XLogReaderState *state, XLogSegNo nextSegNo,
							  TimeLineID *tli_p);


/* Initialize walsender process before entering the main command loop */
void
InitWalSender(void)
{
	am_cascading_walsender = RecoveryInProgress();

	/* Create a per-walsender data structure in shared memory */
	InitWalSenderSlot();

	/*
	 * We don't currently need any ResourceOwner in a walsender process, but
	 * if we did, we could call CreateAuxProcessResourceOwner here.
	 */

	/*
	 * Let postmaster know that we're a WAL sender. Once we've declared us as
	 * a WAL sender process, postmaster will let us outlive the bgwriter and
	 * kill us last in the shutdown sequence, so we get a chance to stream all
	 * remaining WAL at shutdown, including the shutdown checkpoint. Note that
	 * there's no going back, and we mustn't write any WAL records after this.
	 */
	MarkPostmasterChildWalSender();
	SendPostmasterSignal(PMSIGNAL_ADVANCE_STATE_MACHINE);

	/*
	 * If the client didn't specify a database to connect to, show in PGPROC
	 * that our advertised xmin should affect vacuum horizons in all
	 * databases.  This allows physical replication clients to send hot
	 * standby feedback that will delay vacuum cleanup in all databases.
	 */
	if (MyDatabaseId == InvalidOid)
	{
		Assert(MyProc->xmin == InvalidTransactionId);
		LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
		MyProc->statusFlags |= PROC_AFFECTS_ALL_HORIZONS;
		ProcGlobal->statusFlags[MyProc->pgxactoff] = MyProc->statusFlags;
		LWLockRelease(ProcArrayLock);
	}

	/* Initialize empty timestamp buffer for lag tracking. */
	lag_tracker = MemoryContextAllocZero(TopMemoryContext, sizeof(LagTracker));
}

/*
 * Clean up after an error.
 *
 * WAL sender processes don't use transactions like regular backends do.
 * This function does any cleanup required after an error in a WAL sender
 * process, similar to what transaction abort does in a regular backend.
 */
void
WalSndErrorCleanup(void)
{
	LWLockReleaseAll();
	ConditionVariableCancelSleep();
	pgstat_report_wait_end();

	if (xlogreader != NULL && xlogreader->seg.ws_file >= 0)
		wal_segment_close(xlogreader);

	if (MyReplicationSlot != NULL)
		ReplicationSlotRelease();

	ReplicationSlotCleanup(false);

	replication_active = false;

	/*
	 * If there is a transaction in progress, it will clean up our
	 * ResourceOwner, but if a replication command set up a resource owner
	 * without a transaction, we've got to clean that up now.
	 */
	if (!IsTransactionOrTransactionBlock())
		WalSndResourceCleanup(false);

	if (got_STOPPING || got_SIGUSR2)
		proc_exit(0);

	/* Revert back to startup state */
	WalSndSetState(WALSNDSTATE_STARTUP);
}

/*
 * Clean up any ResourceOwner we created.
 */
void
WalSndResourceCleanup(bool isCommit)
{
	ResourceOwner resowner;

	if (CurrentResourceOwner == NULL)
		return;

	/*
	 * Deleting CurrentResourceOwner is not allowed, so we must save a pointer
	 * in a local variable and clear it first.
	 */
	resowner = CurrentResourceOwner;
	CurrentResourceOwner = NULL;

	/* Now we can release resources and delete it. */
	ResourceOwnerRelease(resowner,
						 RESOURCE_RELEASE_BEFORE_LOCKS, isCommit, true);
	ResourceOwnerRelease(resowner,
						 RESOURCE_RELEASE_LOCKS, isCommit, true);
	ResourceOwnerRelease(resowner,
						 RESOURCE_RELEASE_AFTER_LOCKS, isCommit, true);
	ResourceOwnerDelete(resowner);
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
	char		sysid[32];
	char		xloc[MAXFNAMELEN];
	XLogRecPtr	logptr;
	char	   *dbname = NULL;
	DestReceiver *dest;
	TupOutputState *tstate;
	TupleDesc	tupdesc;
	Datum		values[4];
	bool		nulls[4] = {0};
	TimeLineID	currTLI;

	/*
	 * Reply with a result set with one row, four columns. First col is system
	 * ID, second is timeline ID, third is current xlog location and the
	 * fourth contains the database name if we are connected to one.
	 */

	snprintf(sysid, sizeof(sysid), UINT64_FORMAT,
			 GetSystemIdentifier());

	am_cascading_walsender = RecoveryInProgress();
	if (am_cascading_walsender)
		logptr = GetStandbyFlushRecPtr(&currTLI);
	else
		logptr = GetFlushRecPtr(&currTLI);

	snprintf(xloc, sizeof(xloc), "%X/%X", LSN_FORMAT_ARGS(logptr));

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

	dest = CreateDestReceiver(DestRemoteSimple);

	/* need a tuple descriptor representing four columns */
	tupdesc = CreateTemplateTupleDesc(4);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 1, "systemid",
							  TEXTOID, -1, 0);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 2, "timeline",
							  INT8OID, -1, 0);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 3, "xlogpos",
							  TEXTOID, -1, 0);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 4, "dbname",
							  TEXTOID, -1, 0);

	/* prepare for projection of tuples */
	tstate = begin_tup_output_tupdesc(dest, tupdesc, &TTSOpsVirtual);

	/* column 1: system identifier */
	values[0] = CStringGetTextDatum(sysid);

	/* column 2: timeline */
	values[1] = Int64GetDatum(currTLI);

	/* column 3: wal location */
	values[2] = CStringGetTextDatum(xloc);

	/* column 4: database name, or NULL if none */
	if (dbname)
		values[3] = CStringGetTextDatum(dbname);
	else
		nulls[3] = true;

	/* send it to dest */
	do_tup_output(tstate, values, nulls);

	end_tup_output(tstate);
}

/* Handle READ_REPLICATION_SLOT command */
static void
ReadReplicationSlot(ReadReplicationSlotCmd *cmd)
{
#define READ_REPLICATION_SLOT_COLS 3
	ReplicationSlot *slot;
	DestReceiver *dest;
	TupOutputState *tstate;
	TupleDesc	tupdesc;
	Datum		values[READ_REPLICATION_SLOT_COLS] = {0};
	bool		nulls[READ_REPLICATION_SLOT_COLS];

	tupdesc = CreateTemplateTupleDesc(READ_REPLICATION_SLOT_COLS);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 1, "slot_type",
							  TEXTOID, -1, 0);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 2, "restart_lsn",
							  TEXTOID, -1, 0);
	/* TimeLineID is unsigned, so int4 is not wide enough. */
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 3, "restart_tli",
							  INT8OID, -1, 0);

	memset(nulls, true, READ_REPLICATION_SLOT_COLS * sizeof(bool));

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	slot = SearchNamedReplicationSlot(cmd->slotname, false);
	if (slot == NULL || !slot->in_use)
	{
		LWLockRelease(ReplicationSlotControlLock);
	}
	else
	{
		ReplicationSlot slot_contents;
		int			i = 0;

		/* Copy slot contents while holding spinlock */
		SpinLockAcquire(&slot->mutex);
		slot_contents = *slot;
		SpinLockRelease(&slot->mutex);
		LWLockRelease(ReplicationSlotControlLock);

		if (OidIsValid(slot_contents.data.database))
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("cannot use %s with a logical replication slot",
						   "READ_REPLICATION_SLOT"));

		/* slot type */
		values[i] = CStringGetTextDatum("physical");
		nulls[i] = false;
		i++;

		/* start LSN */
		if (!XLogRecPtrIsInvalid(slot_contents.data.restart_lsn))
		{
			char		xloc[64];

			snprintf(xloc, sizeof(xloc), "%X/%X",
					 LSN_FORMAT_ARGS(slot_contents.data.restart_lsn));
			values[i] = CStringGetTextDatum(xloc);
			nulls[i] = false;
		}
		i++;

		/* timeline this WAL was produced on */
		if (!XLogRecPtrIsInvalid(slot_contents.data.restart_lsn))
		{
			TimeLineID	slots_position_timeline;
			TimeLineID	current_timeline;
			List	   *timeline_history = NIL;

			/*
			 * While in recovery, use as timeline the currently-replaying one
			 * to get the LSN position's history.
			 */
			if (RecoveryInProgress())
				(void) GetXLogReplayRecPtr(&current_timeline);
			else
				current_timeline = GetWALInsertionTimeLine();

			timeline_history = readTimeLineHistory(current_timeline);
			slots_position_timeline = tliOfPointInHistory(slot_contents.data.restart_lsn,
														  timeline_history);
			values[i] = Int64GetDatum((int64) slots_position_timeline);
			nulls[i] = false;
		}
		i++;

		Assert(i == READ_REPLICATION_SLOT_COLS);
	}

	dest = CreateDestReceiver(DestRemoteSimple);
	tstate = begin_tup_output_tupdesc(dest, tupdesc, &TTSOpsVirtual);
	do_tup_output(tstate, values, nulls);
	end_tup_output(tstate);
}


/*
 * Handle TIMELINE_HISTORY command.
 */
static void
SendTimeLineHistory(TimeLineHistoryCmd *cmd)
{
	DestReceiver *dest;
	TupleDesc	tupdesc;
	StringInfoData buf;
	char		histfname[MAXFNAMELEN];
	char		path[MAXPGPATH];
	int			fd;
	off_t		histfilelen;
	off_t		bytesleft;
	Size		len;

	dest = CreateDestReceiver(DestRemoteSimple);

	/*
	 * Reply with a result set with one row, and two columns. The first col is
	 * the name of the history file, 2nd is the contents.
	 */
	tupdesc = CreateTemplateTupleDesc(2);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 1, "filename", TEXTOID, -1, 0);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 2, "content", TEXTOID, -1, 0);

	TLHistoryFileName(histfname, cmd->timeline);
	TLHistoryFilePath(path, cmd->timeline);

	/* Send a RowDescription message */
	dest->rStartup(dest, CMD_SELECT, tupdesc);

	/* Send a DataRow message */
	pq_beginmessage(&buf, PqMsg_DataRow);
	pq_sendint16(&buf, 2);		/* # of columns */
	len = strlen(histfname);
	pq_sendint32(&buf, len);	/* col1 len */
	pq_sendbytes(&buf, histfname, len);

	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
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

	pq_sendint32(&buf, histfilelen);	/* col2 len */

	bytesleft = histfilelen;
	while (bytesleft > 0)
	{
		PGAlignedBlock rbuf;
		int			nread;

		pgstat_report_wait_start(WAIT_EVENT_WALSENDER_TIMELINE_HISTORY_READ);
		nread = read(fd, rbuf.data, sizeof(rbuf));
		pgstat_report_wait_end();
		if (nread < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m",
							path)));
		else if (nread == 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("could not read file \"%s\": read %d of %zu",
							path, nread, (Size) bytesleft)));

		pq_sendbytes(&buf, rbuf.data, nread);
		bytesleft -= nread;
	}

	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));

	pq_endmessage(&buf);
}

/*
 * Handle UPLOAD_MANIFEST command.
 */
static void
UploadManifest(void)
{
	MemoryContext mcxt;
	IncrementalBackupInfo *ib;
	off_t		offset = 0;
	StringInfoData buf;

	/*
	 * parsing the manifest will use the cryptohash stuff, which requires a
	 * resource owner
	 */
	Assert(CurrentResourceOwner == NULL);
	CurrentResourceOwner = ResourceOwnerCreate(NULL, "base backup");

	/* Prepare to read manifest data into a temporary context. */
	mcxt = AllocSetContextCreate(CurrentMemoryContext,
								 "incremental backup information",
								 ALLOCSET_DEFAULT_SIZES);
	ib = CreateIncrementalBackupInfo(mcxt);

	/* Send a CopyInResponse message */
	pq_beginmessage(&buf, PqMsg_CopyInResponse);
	pq_sendbyte(&buf, 0);
	pq_sendint16(&buf, 0);
	pq_endmessage_reuse(&buf);
	pq_flush();

	/* Receive packets from client until done. */
	while (HandleUploadManifestPacket(&buf, &offset, ib))
		;

	/* Finish up manifest processing. */
	FinalizeIncrementalManifest(ib);

	/*
	 * Discard any old manifest information and arrange to preserve the new
	 * information we just got.
	 *
	 * We assume that MemoryContextDelete and MemoryContextSetParent won't
	 * fail, and thus we shouldn't end up bailing out of here in such a way as
	 * to leave dangling pointers.
	 */
	if (uploaded_manifest_mcxt != NULL)
		MemoryContextDelete(uploaded_manifest_mcxt);
	MemoryContextSetParent(mcxt, CacheMemoryContext);
	uploaded_manifest = ib;
	uploaded_manifest_mcxt = mcxt;

	/* clean up the resource owner we created */
	WalSndResourceCleanup(true);
}

/*
 * Process one packet received during the handling of an UPLOAD_MANIFEST
 * operation.
 *
 * 'buf' is scratch space. This function expects it to be initialized, doesn't
 * care what the current contents are, and may override them with completely
 * new contents.
 *
 * The return value is true if the caller should continue processing
 * additional packets and false if the UPLOAD_MANIFEST operation is complete.
 */
static bool
HandleUploadManifestPacket(StringInfo buf, off_t *offset,
						   IncrementalBackupInfo *ib)
{
	int			mtype;
	int			maxmsglen;

	HOLD_CANCEL_INTERRUPTS();

	pq_startmsgread();
	mtype = pq_getbyte();
	if (mtype == EOF)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("unexpected EOF on client connection with an open transaction")));

	switch (mtype)
	{
		case 'd':				/* CopyData */
			maxmsglen = PQ_LARGE_MESSAGE_LIMIT;
			break;
		case 'c':				/* CopyDone */
		case 'f':				/* CopyFail */
		case 'H':				/* Flush */
		case 'S':				/* Sync */
			maxmsglen = PQ_SMALL_MESSAGE_LIMIT;
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("unexpected message type 0x%02X during COPY from stdin",
							mtype)));
			maxmsglen = 0;		/* keep compiler quiet */
			break;
	}

	/* Now collect the message body */
	if (pq_getmessage(buf, maxmsglen))
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("unexpected EOF on client connection with an open transaction")));
	RESUME_CANCEL_INTERRUPTS();

	/* Process the message */
	switch (mtype)
	{
		case 'd':				/* CopyData */
			AppendIncrementalManifestData(ib, buf->data, buf->len);
			return true;

		case 'c':				/* CopyDone */
			return false;

		case 'H':				/* Sync */
		case 'S':				/* Flush */
			/* Ignore these while in CopyOut mode as we do elsewhere. */
			return true;

		case 'f':
			ereport(ERROR,
					(errcode(ERRCODE_QUERY_CANCELED),
					 errmsg("COPY from stdin failed: %s",
							pq_getmsgstring(buf))));
	}

	/* Not reached. */
	Assert(false);
	return false;
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
	TimeLineID	FlushTLI;

	/* create xlogreader for physical replication */
	xlogreader =
		XLogReaderAllocate(wal_segment_size, NULL,
						   XL_ROUTINE(.segment_open = WalSndSegmentOpen,
									  .segment_close = wal_segment_close),
						   NULL);

	if (!xlogreader)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed while allocating a WAL reading processor.")));

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
		ReplicationSlotAcquire(cmd->slotname, true);
		if (SlotIsLogical(MyReplicationSlot))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot use a logical replication slot for physical replication")));

		/*
		 * We don't need to verify the slot's restart_lsn here; instead we
		 * rely on the caller requesting the starting point to use.  If the
		 * WAL segment doesn't exist, we'll fail later.
		 */
	}

	/*
	 * Select the timeline. If it was given explicitly by the client, use
	 * that. Otherwise use the timeline of the last replayed record.
	 */
	am_cascading_walsender = RecoveryInProgress();
	if (am_cascading_walsender)
		FlushPtr = GetStandbyFlushRecPtr(&FlushTLI);
	else
		FlushPtr = GetFlushRecPtr(&FlushTLI);

	if (cmd->timeline != 0)
	{
		XLogRecPtr	switchpoint;

		sendTimeLine = cmd->timeline;
		if (sendTimeLine == FlushTLI)
		{
			sendTimeLineIsHistoric = false;
			sendTimeLineValidUpto = InvalidXLogRecPtr;
		}
		else
		{
			List	   *timeLineHistory;

			sendTimeLineIsHistoric = true;

			/*
			 * Check that the timeline the client requested exists, and the
			 * requested start location is on that timeline.
			 */
			timeLineHistory = readTimeLineHistory(FlushTLI);
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
			 * that it doesn't end up with a partial segment. If you ask for
			 * too old a starting point, you'll get an error later when we
			 * fail to find the requested WAL segment in pg_wal.
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
								LSN_FORMAT_ARGS(cmd->startpoint),
								cmd->timeline),
						 errdetail("This server's history forked from timeline %u at %X/%X.",
								   cmd->timeline,
								   LSN_FORMAT_ARGS(switchpoint))));
			}
			sendTimeLineValidUpto = switchpoint;
		}
	}
	else
	{
		sendTimeLine = FlushTLI;
		sendTimeLineValidUpto = InvalidXLogRecPtr;
		sendTimeLineIsHistoric = false;
	}

	streamingDoneSending = streamingDoneReceiving = false;

	/* If there is nothing to stream, don't even enter COPY mode */
	if (!sendTimeLineIsHistoric || cmd->startpoint < sendTimeLineValidUpto)
	{
		/*
		 * When we first start replication the standby will be behind the
		 * primary. For some applications, for example synchronous
		 * replication, it is important to have a clear state for this initial
		 * catchup mode, so we can trigger actions when we change streaming
		 * state later. We may stay in this state for a long time, which is
		 * exactly why we want to be able to monitor whether or not we are
		 * still here.
		 */
		WalSndSetState(WALSNDSTATE_CATCHUP);

		/* Send a CopyBothResponse message, and start streaming */
		pq_beginmessage(&buf, PqMsg_CopyBothResponse);
		pq_sendbyte(&buf, 0);
		pq_sendint16(&buf, 0);
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
							LSN_FORMAT_ARGS(cmd->startpoint),
							LSN_FORMAT_ARGS(FlushPtr))));
		}

		/* Start streaming from the requested point */
		sentPtr = cmd->startpoint;

		/* Initialize shared memory status, too */
		SpinLockAcquire(&MyWalSnd->mutex);
		MyWalSnd->sentPtr = sentPtr;
		SpinLockRelease(&MyWalSnd->mutex);

		SyncRepInitConfig();

		/* Main loop of walsender */
		replication_active = true;

		WalSndLoop(XLogSendPhysical);

		replication_active = false;
		if (got_STOPPING)
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
		char		startpos_str[8 + 1 + 8 + 1];
		DestReceiver *dest;
		TupOutputState *tstate;
		TupleDesc	tupdesc;
		Datum		values[2];
		bool		nulls[2] = {0};

		snprintf(startpos_str, sizeof(startpos_str), "%X/%X",
				 LSN_FORMAT_ARGS(sendTimeLineValidUpto));

		dest = CreateDestReceiver(DestRemoteSimple);

		/*
		 * Need a tuple descriptor representing two columns. int8 may seem
		 * like a surprising data type for this, but in theory int4 would not
		 * be wide enough for this, as TimeLineID is unsigned.
		 */
		tupdesc = CreateTemplateTupleDesc(2);
		TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 1, "next_tli",
								  INT8OID, -1, 0);
		TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 2, "next_tli_startpos",
								  TEXTOID, -1, 0);

		/* prepare for projection of tuple */
		tstate = begin_tup_output_tupdesc(dest, tupdesc, &TTSOpsVirtual);

		values[0] = Int64GetDatum((int64) sendTimeLineNextTLI);
		values[1] = CStringGetTextDatum(startpos_str);

		/* send it to dest */
		do_tup_output(tstate, values, nulls);

		end_tup_output(tstate);
	}

	/* Send CommandComplete message */
	EndReplicationCommand("START_STREAMING");
}

/*
 * XLogReaderRoutine->page_read callback for logical decoding contexts, as a
 * walsender process.
 *
 * Inside the walsender we can do better than read_local_xlog_page,
 * which has to do a plain sleep/busy loop, because the walsender's latch gets
 * set every time WAL is flushed.
 */
static int
logical_read_xlog_page(XLogReaderState *state, XLogRecPtr targetPagePtr, int reqLen,
					   XLogRecPtr targetRecPtr, char *cur_page)
{
	XLogRecPtr	flushptr;
	int			count;
	WALReadError errinfo;
	XLogSegNo	segno;
	TimeLineID	currTLI;

	/*
	 * Make sure we have enough WAL available before retrieving the current
	 * timeline. This is needed to determine am_cascading_walsender accurately
	 * which is needed to determine the current timeline.
	 */
	flushptr = WalSndWaitForWal(targetPagePtr + reqLen);

	/*
	 * Since logical decoding is also permitted on a standby server, we need
	 * to check if the server is in recovery to decide how to get the current
	 * timeline ID (so that it also cover the promotion or timeline change
	 * cases).
	 */
	am_cascading_walsender = RecoveryInProgress();

	if (am_cascading_walsender)
		GetXLogReplayRecPtr(&currTLI);
	else
		currTLI = GetWALInsertionTimeLine();

	XLogReadDetermineTimeline(state, targetPagePtr, reqLen, currTLI);
	sendTimeLineIsHistoric = (state->currTLI != currTLI);
	sendTimeLine = state->currTLI;
	sendTimeLineValidUpto = state->currTLIValidUntil;
	sendTimeLineNextTLI = state->nextTLI;

	/* fail if not (implies we are going to shut down) */
	if (flushptr < targetPagePtr + reqLen)
		return -1;

	if (targetPagePtr + XLOG_BLCKSZ <= flushptr)
		count = XLOG_BLCKSZ;	/* more than one block available */
	else
		count = flushptr - targetPagePtr;	/* part of the page available */

	/* now actually read the data, we know it's there */
	if (!WALRead(state,
				 cur_page,
				 targetPagePtr,
				 count,
				 currTLI,		/* Pass the current TLI because only
								 * WalSndSegmentOpen controls whether new TLI
								 * is needed. */
				 &errinfo))
		WALReadRaiseError(&errinfo);

	/*
	 * After reading into the buffer, check that what we read was valid. We do
	 * this after reading, because even though the segment was present when we
	 * opened it, it might get recycled or removed while we read it. The
	 * read() succeeds in that case, but the data we tried to read might
	 * already have been overwritten with new WAL records.
	 */
	XLByteToSeg(targetPagePtr, segno, state->segcxt.ws_segsize);
	CheckXLogRemoved(segno, state->seg.ws_tli);

	return count;
}

/*
 * Process extra options given to CREATE_REPLICATION_SLOT.
 */
static void
parseCreateReplSlotOptions(CreateReplicationSlotCmd *cmd,
						   bool *reserve_wal,
						   CRSSnapshotAction *snapshot_action,
						   bool *two_phase, bool *failover)
{
	ListCell   *lc;
	bool		snapshot_action_given = false;
	bool		reserve_wal_given = false;
	bool		two_phase_given = false;
	bool		failover_given = false;

	/* Parse options */
	foreach(lc, cmd->options)
	{
		DefElem    *defel = (DefElem *) lfirst(lc);

		if (strcmp(defel->defname, "snapshot") == 0)
		{
			char	   *action;

			if (snapshot_action_given || cmd->kind != REPLICATION_KIND_LOGICAL)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));

			action = defGetString(defel);
			snapshot_action_given = true;

			if (strcmp(action, "export") == 0)
				*snapshot_action = CRS_EXPORT_SNAPSHOT;
			else if (strcmp(action, "nothing") == 0)
				*snapshot_action = CRS_NOEXPORT_SNAPSHOT;
			else if (strcmp(action, "use") == 0)
				*snapshot_action = CRS_USE_SNAPSHOT;
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("unrecognized value for CREATE_REPLICATION_SLOT option \"%s\": \"%s\"",
								defel->defname, action)));
		}
		else if (strcmp(defel->defname, "reserve_wal") == 0)
		{
			if (reserve_wal_given || cmd->kind != REPLICATION_KIND_PHYSICAL)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));

			reserve_wal_given = true;
			*reserve_wal = defGetBoolean(defel);
		}
		else if (strcmp(defel->defname, "two_phase") == 0)
		{
			if (two_phase_given || cmd->kind != REPLICATION_KIND_LOGICAL)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			two_phase_given = true;
			*two_phase = defGetBoolean(defel);
		}
		else if (strcmp(defel->defname, "failover") == 0)
		{
			if (failover_given || cmd->kind != REPLICATION_KIND_LOGICAL)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			failover_given = true;
			*failover = defGetBoolean(defel);
		}
		else
			elog(ERROR, "unrecognized option: %s", defel->defname);
	}
}

/*
 * Create a new replication slot.
 */
static void
CreateReplicationSlot(CreateReplicationSlotCmd *cmd)
{
	const char *snapshot_name = NULL;
	char		xloc[MAXFNAMELEN];
	char	   *slot_name;
	bool		reserve_wal = false;
	bool		two_phase = false;
	bool		failover = false;
	CRSSnapshotAction snapshot_action = CRS_EXPORT_SNAPSHOT;
	DestReceiver *dest;
	TupOutputState *tstate;
	TupleDesc	tupdesc;
	Datum		values[4];
	bool		nulls[4] = {0};

	Assert(!MyReplicationSlot);

	parseCreateReplSlotOptions(cmd, &reserve_wal, &snapshot_action, &two_phase,
							   &failover);

	if (cmd->kind == REPLICATION_KIND_PHYSICAL)
	{
		ReplicationSlotCreate(cmd->slotname, false,
							  cmd->temporary ? RS_TEMPORARY : RS_PERSISTENT,
							  false, false, false);

		if (reserve_wal)
		{
			ReplicationSlotReserveWal();

			ReplicationSlotMarkDirty();

			/* Write this slot to disk if it's a permanent one. */
			if (!cmd->temporary)
				ReplicationSlotSave();
		}
	}
	else
	{
		LogicalDecodingContext *ctx;
		bool		need_full_snapshot = false;

		Assert(cmd->kind == REPLICATION_KIND_LOGICAL);

		CheckLogicalDecodingRequirements();

		/*
		 * Initially create persistent slot as ephemeral - that allows us to
		 * nicely handle errors during initialization because it'll get
		 * dropped if this transaction fails. We'll make it persistent at the
		 * end. Temporary slots can be created as temporary from beginning as
		 * they get dropped on error as well.
		 */
		ReplicationSlotCreate(cmd->slotname, true,
							  cmd->temporary ? RS_TEMPORARY : RS_EPHEMERAL,
							  two_phase, failover, false);

		/*
		 * Do options check early so that we can bail before calling the
		 * DecodingContextFindStartpoint which can take long time.
		 */
		if (snapshot_action == CRS_EXPORT_SNAPSHOT)
		{
			if (IsTransactionBlock())
				ereport(ERROR,
				/*- translator: %s is a CREATE_REPLICATION_SLOT statement */
						(errmsg("%s must not be called inside a transaction",
								"CREATE_REPLICATION_SLOT ... (SNAPSHOT 'export')")));

			need_full_snapshot = true;
		}
		else if (snapshot_action == CRS_USE_SNAPSHOT)
		{
			if (!IsTransactionBlock())
				ereport(ERROR,
				/*- translator: %s is a CREATE_REPLICATION_SLOT statement */
						(errmsg("%s must be called inside a transaction",
								"CREATE_REPLICATION_SLOT ... (SNAPSHOT 'use')")));

			if (XactIsoLevel != XACT_REPEATABLE_READ)
				ereport(ERROR,
				/*- translator: %s is a CREATE_REPLICATION_SLOT statement */
						(errmsg("%s must be called in REPEATABLE READ isolation mode transaction",
								"CREATE_REPLICATION_SLOT ... (SNAPSHOT 'use')")));
			if (!XactReadOnly)
				ereport(ERROR,
				/*- translator: %s is a CREATE_REPLICATION_SLOT statement */
						(errmsg("%s must be called in a read-only transaction",
								"CREATE_REPLICATION_SLOT ... (SNAPSHOT 'use')")));

			if (FirstSnapshotSet)
				ereport(ERROR,
				/*- translator: %s is a CREATE_REPLICATION_SLOT statement */
						(errmsg("%s must be called before any query",
								"CREATE_REPLICATION_SLOT ... (SNAPSHOT 'use')")));

			if (IsSubTransaction())
				ereport(ERROR,
				/*- translator: %s is a CREATE_REPLICATION_SLOT statement */
						(errmsg("%s must not be called in a subtransaction",
								"CREATE_REPLICATION_SLOT ... (SNAPSHOT 'use')")));

			need_full_snapshot = true;
		}

		ctx = CreateInitDecodingContext(cmd->plugin, NIL, need_full_snapshot,
										InvalidXLogRecPtr,
										XL_ROUTINE(.page_read = logical_read_xlog_page,
												   .segment_open = WalSndSegmentOpen,
												   .segment_close = wal_segment_close),
										WalSndPrepareWrite, WalSndWriteData,
										WalSndUpdateProgress);

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
		 * Export or use the snapshot if we've been asked to do so.
		 *
		 * NB. We will convert the snapbuild.c kind of snapshot to normal
		 * snapshot when doing this.
		 */
		if (snapshot_action == CRS_EXPORT_SNAPSHOT)
		{
			snapshot_name = SnapBuildExportSnapshot(ctx->snapshot_builder);
		}
		else if (snapshot_action == CRS_USE_SNAPSHOT)
		{
			Snapshot	snap;

			snap = SnapBuildInitialSnapshot(ctx->snapshot_builder);
			RestoreTransactionSnapshot(snap, MyProc);
		}

		/* don't need the decoding context anymore */
		FreeDecodingContext(ctx);

		if (!cmd->temporary)
			ReplicationSlotPersist();
	}

	snprintf(xloc, sizeof(xloc), "%X/%X",
			 LSN_FORMAT_ARGS(MyReplicationSlot->data.confirmed_flush));

	dest = CreateDestReceiver(DestRemoteSimple);

	/*----------
	 * Need a tuple descriptor representing four columns:
	 * - first field: the slot name
	 * - second field: LSN at which we became consistent
	 * - third field: exported snapshot's name
	 * - fourth field: output plugin
	 */
	tupdesc = CreateTemplateTupleDesc(4);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 1, "slot_name",
							  TEXTOID, -1, 0);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 2, "consistent_point",
							  TEXTOID, -1, 0);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 3, "snapshot_name",
							  TEXTOID, -1, 0);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 4, "output_plugin",
							  TEXTOID, -1, 0);

	/* prepare for projection of tuples */
	tstate = begin_tup_output_tupdesc(dest, tupdesc, &TTSOpsVirtual);

	/* slot_name */
	slot_name = NameStr(MyReplicationSlot->data.name);
	values[0] = CStringGetTextDatum(slot_name);

	/* consistent wal location */
	values[1] = CStringGetTextDatum(xloc);

	/* snapshot name, or NULL if none */
	if (snapshot_name != NULL)
		values[2] = CStringGetTextDatum(snapshot_name);
	else
		nulls[2] = true;

	/* plugin, or NULL if none */
	if (cmd->plugin != NULL)
		values[3] = CStringGetTextDatum(cmd->plugin);
	else
		nulls[3] = true;

	/* send it to dest */
	do_tup_output(tstate, values, nulls);
	end_tup_output(tstate);

	ReplicationSlotRelease();
}

/*
 * Get rid of a replication slot that is no longer wanted.
 */
static void
DropReplicationSlot(DropReplicationSlotCmd *cmd)
{
	ReplicationSlotDrop(cmd->slotname, !cmd->wait);
}

/*
 * Process extra options given to ALTER_REPLICATION_SLOT.
 */
static void
ParseAlterReplSlotOptions(AlterReplicationSlotCmd *cmd, bool *failover)
{
	bool		failover_given = false;

	/* Parse options */
	foreach_ptr(DefElem, defel, cmd->options)
	{
		if (strcmp(defel->defname, "failover") == 0)
		{
			if (failover_given)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			failover_given = true;
			*failover = defGetBoolean(defel);
		}
		else
			elog(ERROR, "unrecognized option: %s", defel->defname);
	}
}

/*
 * Change the definition of a replication slot.
 */
static void
AlterReplicationSlot(AlterReplicationSlotCmd *cmd)
{
	bool		failover = false;

	ParseAlterReplSlotOptions(cmd, &failover);
	ReplicationSlotAlter(cmd->slotname, failover);
}

/*
 * Load previously initiated logical slot and prepare for sending data (via
 * WalSndLoop).
 */
static void
StartLogicalReplication(StartReplicationCmd *cmd)
{
	StringInfoData buf;
	QueryCompletion qc;

	/* make sure that our requirements are still fulfilled */
	CheckLogicalDecodingRequirements();

	Assert(!MyReplicationSlot);

	ReplicationSlotAcquire(cmd->slotname, true);

	/*
	 * Force a disconnect, so that the decoding code doesn't need to care
	 * about an eventual switch from running in recovery, to running in a
	 * normal environment. Client code is expected to handle reconnects.
	 */
	if (am_cascading_walsender && !RecoveryInProgress())
	{
		ereport(LOG,
				(errmsg("terminating walsender process after promotion")));
		got_STOPPING = true;
	}

	/*
	 * Create our decoding context, making it start at the previously ack'ed
	 * position.
	 *
	 * Do this before sending a CopyBothResponse message, so that any errors
	 * are reported early.
	 */
	logical_decoding_ctx =
		CreateDecodingContext(cmd->startpoint, cmd->options, false,
							  XL_ROUTINE(.page_read = logical_read_xlog_page,
										 .segment_open = WalSndSegmentOpen,
										 .segment_close = wal_segment_close),
							  WalSndPrepareWrite, WalSndWriteData,
							  WalSndUpdateProgress);
	xlogreader = logical_decoding_ctx->reader;

	WalSndSetState(WALSNDSTATE_CATCHUP);

	/* Send a CopyBothResponse message, and start streaming */
	pq_beginmessage(&buf, PqMsg_CopyBothResponse);
	pq_sendbyte(&buf, 0);
	pq_sendint16(&buf, 0);
	pq_endmessage(&buf);
	pq_flush();

	/* Start reading WAL from the oldest required WAL. */
	XLogBeginRead(logical_decoding_ctx->reader,
				  MyReplicationSlot->data.restart_lsn);

	/*
	 * Report the location after which we'll send out further commits as the
	 * current sentPtr.
	 */
	sentPtr = MyReplicationSlot->data.confirmed_flush;

	/* Also update the sent position status in shared memory */
	SpinLockAcquire(&MyWalSnd->mutex);
	MyWalSnd->sentPtr = MyReplicationSlot->data.restart_lsn;
	SpinLockRelease(&MyWalSnd->mutex);

	replication_active = true;

	SyncRepInitConfig();

	/* Main loop of walsender */
	WalSndLoop(XLogSendLogical);

	FreeDecodingContext(logical_decoding_ctx);
	ReplicationSlotRelease();

	replication_active = false;
	if (got_STOPPING)
		proc_exit(0);
	WalSndSetState(WALSNDSTATE_STARTUP);

	/* Get out of COPY mode (CommandComplete). */
	SetQueryCompletion(&qc, CMDTAG_COPY, 0);
	EndCommand(&qc, DestRemote, false);
}

/*
 * LogicalDecodingContext 'prepare_write' callback.
 *
 * Prepare a write into a StringInfo.
 *
 * Don't do anything lasting in here, it's quite possible that nothing will be done
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
	TimestampTz now;

	/*
	 * Fill the send timestamp last, so that it is taken as late as possible.
	 * This is somewhat ugly, but the protocol is set as it's already used for
	 * several releases by streaming physical replication.
	 */
	resetStringInfo(&tmpbuf);
	now = GetCurrentTimestamp();
	pq_sendint64(&tmpbuf, now);
	memcpy(&ctx->out->data[1 + sizeof(int64) + sizeof(int64)],
		   tmpbuf.data, sizeof(int64));

	/* output previously gathered data in a CopyData packet */
	pq_putmessage_noblock('d', ctx->out->data, ctx->out->len);

	CHECK_FOR_INTERRUPTS();

	/* Try to flush pending output to the client */
	if (pq_flush_if_writable() != 0)
		WalSndShutdown();

	/* Try taking fast path unless we get too close to walsender timeout. */
	if (now < TimestampTzPlusMilliseconds(last_reply_timestamp,
										  wal_sender_timeout / 2) &&
		!pq_is_send_pending())
	{
		return;
	}

	/* If we have pending write here, go to slow path */
	ProcessPendingWrites();
}

/*
 * Wait until there is no pending write. Also process replies from the other
 * side and check timeouts during that.
 */
static void
ProcessPendingWrites(void)
{
	for (;;)
	{
		long		sleeptime;

		/* Check for input from the client */
		ProcessRepliesIfAny();

		/* die if timeout was reached */
		WalSndCheckTimeOut();

		/* Send keepalive if the time has come */
		WalSndKeepaliveIfNecessary();

		if (!pq_is_send_pending())
			break;

		sleeptime = WalSndComputeSleeptime(GetCurrentTimestamp());

		/* Sleep until something happens or we time out */
		WalSndWait(WL_SOCKET_WRITEABLE | WL_SOCKET_READABLE, sleeptime,
				   WAIT_EVENT_WAL_SENDER_WRITE_DATA);

		/* Clear any already-pending wakeups */
		ResetLatch(MyLatch);

		CHECK_FOR_INTERRUPTS();

		/* Process any requests or signals received recently */
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
			SyncRepInitConfig();
		}

		/* Try to flush pending output to the client */
		if (pq_flush_if_writable() != 0)
			WalSndShutdown();
	}

	/* reactivate latch so WalSndLoop knows to continue */
	SetLatch(MyLatch);
}

/*
 * LogicalDecodingContext 'update_progress' callback.
 *
 * Write the current position to the lag tracker (see XLogSendPhysical).
 *
 * When skipping empty transactions, send a keepalive message if necessary.
 */
static void
WalSndUpdateProgress(LogicalDecodingContext *ctx, XLogRecPtr lsn, TransactionId xid,
					 bool skipped_xact)
{
	static TimestampTz sendTime = 0;
	TimestampTz now = GetCurrentTimestamp();
	bool		pending_writes = false;
	bool		end_xact = ctx->end_xact;

	/*
	 * Track lag no more than once per WALSND_LOGICAL_LAG_TRACK_INTERVAL_MS to
	 * avoid flooding the lag tracker when we commit frequently.
	 *
	 * We don't have a mechanism to get the ack for any LSN other than end
	 * xact LSN from the downstream. So, we track lag only for end of
	 * transaction LSN.
	 */
#define WALSND_LOGICAL_LAG_TRACK_INTERVAL_MS	1000
	if (end_xact && TimestampDifferenceExceeds(sendTime, now,
											   WALSND_LOGICAL_LAG_TRACK_INTERVAL_MS))
	{
		LagTrackerWrite(lsn, now);
		sendTime = now;
	}

	/*
	 * When skipping empty transactions in synchronous replication, we send a
	 * keepalive message to avoid delaying such transactions.
	 *
	 * It is okay to check sync_standbys_status without lock here as in the
	 * worst case we will just send an extra keepalive message when it is
	 * really not required.
	 */
	if (skipped_xact &&
		SyncRepRequested() &&
		(((volatile WalSndCtlData *) WalSndCtl)->sync_standbys_status & SYNC_STANDBY_DEFINED))
	{
		WalSndKeepalive(false, lsn);

		/* Try to flush pending output to the client */
		if (pq_flush_if_writable() != 0)
			WalSndShutdown();

		/* If we have pending write here, make sure it's actually flushed */
		if (pq_is_send_pending())
			pending_writes = true;
	}

	/*
	 * Process pending writes if any or try to send a keepalive if required.
	 * We don't need to try sending keep alive messages at the transaction end
	 * as that will be done at a later point in time. This is required only
	 * for large transactions where we don't send any changes to the
	 * downstream and the receiver can timeout due to that.
	 */
	if (pending_writes || (!end_xact &&
						   now >= TimestampTzPlusMilliseconds(last_reply_timestamp,
															  wal_sender_timeout / 2)))
		ProcessPendingWrites();
}

/*
 * Wake up the logical walsender processes with logical failover slots if the
 * currently acquired physical slot is specified in synchronized_standby_slots GUC.
 */
void
PhysicalWakeupLogicalWalSnd(void)
{
	Assert(MyReplicationSlot && SlotIsPhysical(MyReplicationSlot));

	/*
	 * If we are running in a standby, there is no need to wake up walsenders.
	 * This is because we do not support syncing slots to cascading standbys,
	 * so, there are no walsenders waiting for standbys to catch up.
	 */
	if (RecoveryInProgress())
		return;

	if (SlotExistsInSyncStandbySlots(NameStr(MyReplicationSlot->data.name)))
		ConditionVariableBroadcast(&WalSndCtl->wal_confirm_rcv_cv);
}

/*
 * Returns true if not all standbys have caught up to the flushed position
 * (flushed_lsn) when the current acquired slot is a logical failover
 * slot and we are streaming; otherwise, returns false.
 *
 * If returning true, the function sets the appropriate wait event in
 * wait_event; otherwise, wait_event is set to 0.
 */
static bool
NeedToWaitForStandbys(XLogRecPtr flushed_lsn, uint32 *wait_event)
{
	int			elevel = got_STOPPING ? ERROR : WARNING;
	bool		failover_slot;

	failover_slot = (replication_active && MyReplicationSlot->data.failover);

	/*
	 * Note that after receiving the shutdown signal, an ERROR is reported if
	 * any slots are dropped, invalidated, or inactive. This measure is taken
	 * to prevent the walsender from waiting indefinitely.
	 */
	if (failover_slot && !StandbySlotsHaveCaughtup(flushed_lsn, elevel))
	{
		*wait_event = WAIT_EVENT_WAIT_FOR_STANDBY_CONFIRMATION;
		return true;
	}

	*wait_event = 0;
	return false;
}

/*
 * Returns true if we need to wait for WALs to be flushed to disk, or if not
 * all standbys have caught up to the flushed position (flushed_lsn) when the
 * current acquired slot is a logical failover slot and we are
 * streaming; otherwise, returns false.
 *
 * If returning true, the function sets the appropriate wait event in
 * wait_event; otherwise, wait_event is set to 0.
 */
static bool
NeedToWaitForWal(XLogRecPtr target_lsn, XLogRecPtr flushed_lsn,
				 uint32 *wait_event)
{
	/* Check if we need to wait for WALs to be flushed to disk */
	if (target_lsn > flushed_lsn)
	{
		*wait_event = WAIT_EVENT_WAL_SENDER_WAIT_FOR_WAL;
		return true;
	}

	/* Check if the standby slots have caught up to the flushed position */
	return NeedToWaitForStandbys(flushed_lsn, wait_event);
}

/*
 * Wait till WAL < loc is flushed to disk so it can be safely sent to client.
 *
 * If the walsender holds a logical failover slot, we also wait for all the
 * specified streaming replication standby servers to confirm receipt of WAL
 * up to RecentFlushPtr. It is beneficial to wait here for the confirmation
 * up to RecentFlushPtr rather than waiting before transmitting each change
 * to logical subscribers, which is already covered by RecentFlushPtr.
 *
 * Returns end LSN of flushed WAL.  Normally this will be >= loc, but if we
 * detect a shutdown request (either from postmaster or client) we will return
 * early, so caller must always check.
 */
static XLogRecPtr
WalSndWaitForWal(XLogRecPtr loc)
{
	int			wakeEvents;
	uint32		wait_event = 0;
	static XLogRecPtr RecentFlushPtr = InvalidXLogRecPtr;
	TimestampTz last_flush = 0;

	/*
	 * Fast path to avoid acquiring the spinlock in case we already know we
	 * have enough WAL available and all the standby servers have confirmed
	 * receipt of WAL up to RecentFlushPtr. This is particularly interesting
	 * if we're far behind.
	 */
	if (!XLogRecPtrIsInvalid(RecentFlushPtr) &&
		!NeedToWaitForWal(loc, RecentFlushPtr, &wait_event))
		return RecentFlushPtr;

	/*
	 * Within the loop, we wait for the necessary WALs to be flushed to disk
	 * first, followed by waiting for standbys to catch up if there are enough
	 * WALs (see NeedToWaitForWal()) or upon receiving the shutdown signal.
	 */
	for (;;)
	{
		bool		wait_for_standby_at_stop = false;
		long		sleeptime;
		TimestampTz now;

		/* Clear any already-pending wakeups */
		ResetLatch(MyLatch);

		CHECK_FOR_INTERRUPTS();

		/* Process any requests or signals received recently */
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
			SyncRepInitConfig();
		}

		/* Check for input from the client */
		ProcessRepliesIfAny();

		/*
		 * If we're shutting down, trigger pending WAL to be written out,
		 * otherwise we'd possibly end up waiting for WAL that never gets
		 * written, because walwriter has shut down already.
		 */
		if (got_STOPPING)
			XLogBackgroundFlush();

		/*
		 * To avoid the scenario where standbys need to catch up to a newer
		 * WAL location in each iteration, we update our idea of the currently
		 * flushed position only if we are not waiting for standbys to catch
		 * up.
		 */
		if (wait_event != WAIT_EVENT_WAIT_FOR_STANDBY_CONFIRMATION)
		{
			if (!RecoveryInProgress())
				RecentFlushPtr = GetFlushRecPtr(NULL);
			else
				RecentFlushPtr = GetXLogReplayRecPtr(NULL);
		}

		/*
		 * If postmaster asked us to stop and the standby slots have caught up
		 * to the flushed position, don't wait anymore.
		 *
		 * It's important to do this check after the recomputation of
		 * RecentFlushPtr, so we can send all remaining data before shutting
		 * down.
		 */
		if (got_STOPPING)
		{
			if (NeedToWaitForStandbys(RecentFlushPtr, &wait_event))
				wait_for_standby_at_stop = true;
			else
				break;
		}

		/*
		 * We only send regular messages to the client for full decoded
		 * transactions, but a synchronous replication and walsender shutdown
		 * possibly are waiting for a later location. So, before sleeping, we
		 * send a ping containing the flush location. If the receiver is
		 * otherwise idle, this keepalive will trigger a reply. Processing the
		 * reply will update these MyWalSnd locations.
		 */
		if (MyWalSnd->flush < sentPtr &&
			MyWalSnd->write < sentPtr &&
			!waiting_for_ping_response)
			WalSndKeepalive(false, InvalidXLogRecPtr);

		/*
		 * Exit the loop if already caught up and doesn't need to wait for
		 * standby slots.
		 */
		if (!wait_for_standby_at_stop &&
			!NeedToWaitForWal(loc, RecentFlushPtr, &wait_event))
			break;

		/*
		 * Waiting for new WAL or waiting for standbys to catch up. Since we
		 * need to wait, we're now caught up.
		 */
		WalSndCaughtUp = true;

		/*
		 * Try to flush any pending output to the client.
		 */
		if (pq_flush_if_writable() != 0)
			WalSndShutdown();

		/*
		 * If we have received CopyDone from the client, sent CopyDone
		 * ourselves, and the output buffer is empty, it's time to exit
		 * streaming, so fail the current WAL fetch request.
		 */
		if (streamingDoneReceiving && streamingDoneSending &&
			!pq_is_send_pending())
			break;

		/* die if timeout was reached */
		WalSndCheckTimeOut();

		/* Send keepalive if the time has come */
		WalSndKeepaliveIfNecessary();

		/*
		 * Sleep until something happens or we time out.  Also wait for the
		 * socket becoming writable, if there's still pending output.
		 * Otherwise we might sit on sendable output data while waiting for
		 * new WAL to be generated.  (But if we have nothing to send, we don't
		 * want to wake on socket-writable.)
		 */
		now = GetCurrentTimestamp();
		sleeptime = WalSndComputeSleeptime(now);

		wakeEvents = WL_SOCKET_READABLE;

		if (pq_is_send_pending())
			wakeEvents |= WL_SOCKET_WRITEABLE;

		Assert(wait_event != 0);

		/* Report IO statistics, if needed */
		if (TimestampDifferenceExceeds(last_flush, now,
									   WALSENDER_STATS_FLUSH_INTERVAL))
		{
			pgstat_flush_io(false);
			last_flush = now;
		}

		WalSndWait(wakeEvents, sleeptime, wait_event);
	}

	/* reactivate latch so WalSndLoop knows to continue */
	SetLatch(MyLatch);
	return RecentFlushPtr;
}

/*
 * Execute an incoming replication command.
 *
 * Returns true if the cmd_string was recognized as WalSender command, false
 * if not.
 */
bool
exec_replication_command(const char *cmd_string)
{
	int			parse_rc;
	Node	   *cmd_node;
	const char *cmdtag;
	MemoryContext cmd_context;
	MemoryContext old_context;

	/*
	 * If WAL sender has been told that shutdown is getting close, switch its
	 * status accordingly to handle the next replication commands correctly.
	 */
	if (got_STOPPING)
		WalSndSetState(WALSNDSTATE_STOPPING);

	/*
	 * Throw error if in stopping mode.  We need prevent commands that could
	 * generate WAL while the shutdown checkpoint is being written.  To be
	 * safe, we just prohibit all new commands.
	 */
	if (MyWalSnd->state == WALSNDSTATE_STOPPING)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot execute new commands while WAL sender is in stopping mode")));

	/*
	 * CREATE_REPLICATION_SLOT ... LOGICAL exports a snapshot until the next
	 * command arrives. Clean up the old stuff if there's anything.
	 */
	SnapBuildClearExportedSnapshot();

	CHECK_FOR_INTERRUPTS();

	/*
	 * Prepare to parse and execute the command.
	 */
	cmd_context = AllocSetContextCreate(CurrentMemoryContext,
										"Replication command context",
										ALLOCSET_DEFAULT_SIZES);
	old_context = MemoryContextSwitchTo(cmd_context);

	replication_scanner_init(cmd_string);

	/*
	 * Is it a WalSender command?
	 */
	if (!replication_scanner_is_replication_command())
	{
		/* Nope; clean up and get out. */
		replication_scanner_finish();

		MemoryContextSwitchTo(old_context);
		MemoryContextDelete(cmd_context);

		/* XXX this is a pretty random place to make this check */
		if (MyDatabaseId == InvalidOid)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot execute SQL commands in WAL sender for physical replication")));

		/* Tell the caller that this wasn't a WalSender command. */
		return false;
	}

	/*
	 * Looks like a WalSender command, so parse it.
	 */
	parse_rc = replication_yyparse();
	if (parse_rc != 0)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg_internal("replication command parser returned %d",
								 parse_rc)));
	replication_scanner_finish();

	cmd_node = replication_parse_result;

	/*
	 * Report query to various monitoring facilities.  For this purpose, we
	 * report replication commands just like SQL commands.
	 */
	debug_query_string = cmd_string;

	pgstat_report_activity(STATE_RUNNING, cmd_string);

	/*
	 * Log replication command if log_replication_commands is enabled. Even
	 * when it's disabled, log the command with DEBUG1 level for backward
	 * compatibility.
	 */
	ereport(log_replication_commands ? LOG : DEBUG1,
			(errmsg("received replication command: %s", cmd_string)));

	/*
	 * Disallow replication commands in aborted transaction blocks.
	 */
	if (IsAbortedTransactionBlockState())
		ereport(ERROR,
				(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
				 errmsg("current transaction is aborted, "
						"commands ignored until end of transaction block")));

	CHECK_FOR_INTERRUPTS();

	/*
	 * Allocate buffers that will be used for each outgoing and incoming
	 * message.  We do this just once per command to reduce palloc overhead.
	 */
	initStringInfo(&output_message);
	initStringInfo(&reply_message);
	initStringInfo(&tmpbuf);

	switch (cmd_node->type)
	{
		case T_IdentifySystemCmd:
			cmdtag = "IDENTIFY_SYSTEM";
			set_ps_display(cmdtag);
			IdentifySystem();
			EndReplicationCommand(cmdtag);
			break;

		case T_ReadReplicationSlotCmd:
			cmdtag = "READ_REPLICATION_SLOT";
			set_ps_display(cmdtag);
			ReadReplicationSlot((ReadReplicationSlotCmd *) cmd_node);
			EndReplicationCommand(cmdtag);
			break;

		case T_BaseBackupCmd:
			cmdtag = "BASE_BACKUP";
			set_ps_display(cmdtag);
			PreventInTransactionBlock(true, cmdtag);
			SendBaseBackup((BaseBackupCmd *) cmd_node, uploaded_manifest);
			EndReplicationCommand(cmdtag);
			break;

		case T_CreateReplicationSlotCmd:
			cmdtag = "CREATE_REPLICATION_SLOT";
			set_ps_display(cmdtag);
			CreateReplicationSlot((CreateReplicationSlotCmd *) cmd_node);
			EndReplicationCommand(cmdtag);
			break;

		case T_DropReplicationSlotCmd:
			cmdtag = "DROP_REPLICATION_SLOT";
			set_ps_display(cmdtag);
			DropReplicationSlot((DropReplicationSlotCmd *) cmd_node);
			EndReplicationCommand(cmdtag);
			break;

		case T_AlterReplicationSlotCmd:
			cmdtag = "ALTER_REPLICATION_SLOT";
			set_ps_display(cmdtag);
			AlterReplicationSlot((AlterReplicationSlotCmd *) cmd_node);
			EndReplicationCommand(cmdtag);
			break;

		case T_StartReplicationCmd:
			{
				StartReplicationCmd *cmd = (StartReplicationCmd *) cmd_node;

				cmdtag = "START_REPLICATION";
				set_ps_display(cmdtag);
				PreventInTransactionBlock(true, cmdtag);

				if (cmd->kind == REPLICATION_KIND_PHYSICAL)
					StartReplication(cmd);
				else
					StartLogicalReplication(cmd);

				/* dupe, but necessary per libpqrcv_endstreaming */
				EndReplicationCommand(cmdtag);

				Assert(xlogreader != NULL);
				break;
			}

		case T_TimeLineHistoryCmd:
			cmdtag = "TIMELINE_HISTORY";
			set_ps_display(cmdtag);
			PreventInTransactionBlock(true, cmdtag);
			SendTimeLineHistory((TimeLineHistoryCmd *) cmd_node);
			EndReplicationCommand(cmdtag);
			break;

		case T_VariableShowStmt:
			{
				DestReceiver *dest = CreateDestReceiver(DestRemoteSimple);
				VariableShowStmt *n = (VariableShowStmt *) cmd_node;

				cmdtag = "SHOW";
				set_ps_display(cmdtag);

				/* syscache access needs a transaction environment */
				StartTransactionCommand();
				GetPGVariable(n->name, dest);
				CommitTransactionCommand();
				EndReplicationCommand(cmdtag);
			}
			break;

		case T_UploadManifestCmd:
			cmdtag = "UPLOAD_MANIFEST";
			set_ps_display(cmdtag);
			PreventInTransactionBlock(true, cmdtag);
			UploadManifest();
			EndReplicationCommand(cmdtag);
			break;

		default:
			elog(ERROR, "unrecognized replication command node tag: %u",
				 cmd_node->type);
	}

	/* done */
	MemoryContextSwitchTo(old_context);
	MemoryContextDelete(cmd_context);

	/*
	 * We need not update ps display or pg_stat_activity, because PostgresMain
	 * will reset those to "idle".  But we must reset debug_query_string to
	 * ensure it doesn't become a dangling pointer.
	 */
	debug_query_string = NULL;

	return true;
}

/*
 * Process any incoming messages while streaming. Also checks if the remote
 * end has closed the connection.
 */
static void
ProcessRepliesIfAny(void)
{
	unsigned char firstchar;
	int			maxmsglen;
	int			r;
	bool		received = false;

	last_processing = GetCurrentTimestamp();

	/*
	 * If we already received a CopyDone from the frontend, any subsequent
	 * message is the beginning of a new command, and should be processed in
	 * the main processing loop.
	 */
	while (!streamingDoneReceiving)
	{
		pq_startmsgread();
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
			pq_endmsgread();
			break;
		}

		/* Validate message type and set packet size limit */
		switch (firstchar)
		{
			case PqMsg_CopyData:
				maxmsglen = PQ_LARGE_MESSAGE_LIMIT;
				break;
			case PqMsg_CopyDone:
			case PqMsg_Terminate:
				maxmsglen = PQ_SMALL_MESSAGE_LIMIT;
				break;
			default:
				ereport(FATAL,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("invalid standby message type \"%c\"",
								firstchar)));
				maxmsglen = 0;	/* keep compiler quiet */
				break;
		}

		/* Read the message contents */
		resetStringInfo(&reply_message);
		if (pq_getmessage(&reply_message, maxmsglen))
		{
			ereport(COMMERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("unexpected EOF on standby connection")));
			proc_exit(0);
		}

		/* ... and process it */
		switch (firstchar)
		{
				/*
				 * 'd' means a standby reply wrapped in a CopyData packet.
				 */
			case PqMsg_CopyData:
				ProcessStandbyMessage();
				received = true;
				break;

				/*
				 * CopyDone means the standby requested to finish streaming.
				 * Reply with CopyDone, if we had not sent that already.
				 */
			case PqMsg_CopyDone:
				if (!streamingDoneSending)
				{
					pq_putmessage_noblock('c', NULL, 0);
					streamingDoneSending = true;
				}

				streamingDoneReceiving = true;
				received = true;
				break;

				/*
				 * 'X' means that the standby is closing down the socket.
				 */
			case PqMsg_Terminate:
				proc_exit(0);

			default:
				Assert(false);	/* NOT REACHED */
		}
	}

	/*
	 * Save the last reply timestamp if we've received at least one reply.
	 */
	if (received)
	{
		last_reply_timestamp = last_processing;
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
	ReplicationSlot *slot = MyReplicationSlot;

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
		PhysicalWakeupLogicalWalSnd();
	}

	/*
	 * One could argue that the slot should be saved to disk now, but that'd
	 * be energy wasted - the worst thing lost information could cause here is
	 * to give wrong information in a statistics view - we'll just potentially
	 * be more conservative in removing files.
	 *
	 * Checkpointer makes special efforts to keep the WAL segments required by
	 * the restart_lsn written to the disk. See CreateCheckPoint() and
	 * CreateRestartPoint() for details.
	 */
}

/*
 * Regular reply from standby advising of WAL locations on standby server.
 */
static void
ProcessStandbyReplyMessage(void)
{
	XLogRecPtr	writePtr,
				flushPtr,
				applyPtr;
	bool		replyRequested;
	TimeOffset	writeLag,
				flushLag,
				applyLag;
	bool		clearLagTimes;
	TimestampTz now;
	TimestampTz replyTime;

	static bool fullyAppliedLastTime = false;

	/* the caller already consumed the msgtype byte */
	writePtr = pq_getmsgint64(&reply_message);
	flushPtr = pq_getmsgint64(&reply_message);
	applyPtr = pq_getmsgint64(&reply_message);
	replyTime = pq_getmsgint64(&reply_message);
	replyRequested = pq_getmsgbyte(&reply_message);

	if (message_level_is_interesting(DEBUG2))
	{
		char	   *replyTimeStr;

		/* Copy because timestamptz_to_str returns a static buffer */
		replyTimeStr = pstrdup(timestamptz_to_str(replyTime));

		elog(DEBUG2, "write %X/%X flush %X/%X apply %X/%X%s reply_time %s",
			 LSN_FORMAT_ARGS(writePtr),
			 LSN_FORMAT_ARGS(flushPtr),
			 LSN_FORMAT_ARGS(applyPtr),
			 replyRequested ? " (reply requested)" : "",
			 replyTimeStr);

		pfree(replyTimeStr);
	}

	/* See if we can compute the round-trip lag for these positions. */
	now = GetCurrentTimestamp();
	writeLag = LagTrackerRead(SYNC_REP_WAIT_WRITE, writePtr, now);
	flushLag = LagTrackerRead(SYNC_REP_WAIT_FLUSH, flushPtr, now);
	applyLag = LagTrackerRead(SYNC_REP_WAIT_APPLY, applyPtr, now);

	/*
	 * If the standby reports that it has fully replayed the WAL in two
	 * consecutive reply messages, then the second such message must result
	 * from wal_receiver_status_interval expiring on the standby.  This is a
	 * convenient time to forget the lag times measured when it last
	 * wrote/flushed/applied a WAL record, to avoid displaying stale lag data
	 * until more WAL traffic arrives.
	 */
	clearLagTimes = false;
	if (applyPtr == sentPtr)
	{
		if (fullyAppliedLastTime)
			clearLagTimes = true;
		fullyAppliedLastTime = true;
	}
	else
		fullyAppliedLastTime = false;

	/* Send a reply if the standby requested one. */
	if (replyRequested)
		WalSndKeepalive(false, InvalidXLogRecPtr);

	/*
	 * Update shared state for this WalSender process based on reply data from
	 * standby.
	 */
	{
		WalSnd	   *walsnd = MyWalSnd;

		SpinLockAcquire(&walsnd->mutex);
		walsnd->write = writePtr;
		walsnd->flush = flushPtr;
		walsnd->apply = applyPtr;
		if (writeLag != -1 || clearLagTimes)
			walsnd->writeLag = writeLag;
		if (flushLag != -1 || clearLagTimes)
			walsnd->flushLag = flushLag;
		if (applyLag != -1 || clearLagTimes)
			walsnd->applyLag = applyLag;
		walsnd->replyTime = replyTime;
		SpinLockRelease(&walsnd->mutex);
	}

	if (!am_cascading_walsender)
		SyncRepReleaseWaiters();

	/*
	 * Advance our local xmin horizon when the client confirmed a flush.
	 */
	if (MyReplicationSlot && flushPtr != InvalidXLogRecPtr)
	{
		if (SlotIsLogical(MyReplicationSlot))
			LogicalConfirmReceivedLocation(flushPtr);
		else
			PhysicalConfirmReceivedLocation(flushPtr);
	}
}

/* compute new replication slot xmin horizon if needed */
static void
PhysicalReplicationSlotNewXmin(TransactionId feedbackXmin, TransactionId feedbackCatalogXmin)
{
	bool		changed = false;
	ReplicationSlot *slot = MyReplicationSlot;

	SpinLockAcquire(&slot->mutex);
	MyProc->xmin = InvalidTransactionId;

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
	if (!TransactionIdIsNormal(slot->data.catalog_xmin) ||
		!TransactionIdIsNormal(feedbackCatalogXmin) ||
		TransactionIdPrecedes(slot->data.catalog_xmin, feedbackCatalogXmin))
	{
		changed = true;
		slot->data.catalog_xmin = feedbackCatalogXmin;
		slot->effective_catalog_xmin = feedbackCatalogXmin;
	}
	SpinLockRelease(&slot->mutex);

	if (changed)
	{
		ReplicationSlotMarkDirty();
		ReplicationSlotsComputeRequiredXmin(false);
	}
}

/*
 * Check that the provided xmin/epoch are sane, that is, not in the future
 * and not so far back as to be already wrapped around.
 *
 * Epoch of nextXid should be same as standby, or if the counter has
 * wrapped, then one greater than standby.
 *
 * This check doesn't care about whether clog exists for these xids
 * at all.
 */
static bool
TransactionIdInRecentPast(TransactionId xid, uint32 epoch)
{
	FullTransactionId nextFullXid;
	TransactionId nextXid;
	uint32		nextEpoch;

	nextFullXid = ReadNextFullTransactionId();
	nextXid = XidFromFullTransactionId(nextFullXid);
	nextEpoch = EpochFromFullTransactionId(nextFullXid);

	if (xid <= nextXid)
	{
		if (epoch != nextEpoch)
			return false;
	}
	else
	{
		if (epoch + 1 != nextEpoch)
			return false;
	}

	if (!TransactionIdPrecedesOrEquals(xid, nextXid))
		return false;			/* epoch OK, but it's wrapped around */

	return true;
}

/*
 * Hot Standby feedback
 */
static void
ProcessStandbyHSFeedbackMessage(void)
{
	TransactionId feedbackXmin;
	uint32		feedbackEpoch;
	TransactionId feedbackCatalogXmin;
	uint32		feedbackCatalogEpoch;
	TimestampTz replyTime;

	/*
	 * Decipher the reply message. The caller already consumed the msgtype
	 * byte. See XLogWalRcvSendHSFeedback() in walreceiver.c for the creation
	 * of this message.
	 */
	replyTime = pq_getmsgint64(&reply_message);
	feedbackXmin = pq_getmsgint(&reply_message, 4);
	feedbackEpoch = pq_getmsgint(&reply_message, 4);
	feedbackCatalogXmin = pq_getmsgint(&reply_message, 4);
	feedbackCatalogEpoch = pq_getmsgint(&reply_message, 4);

	if (message_level_is_interesting(DEBUG2))
	{
		char	   *replyTimeStr;

		/* Copy because timestamptz_to_str returns a static buffer */
		replyTimeStr = pstrdup(timestamptz_to_str(replyTime));

		elog(DEBUG2, "hot standby feedback xmin %u epoch %u, catalog_xmin %u epoch %u reply_time %s",
			 feedbackXmin,
			 feedbackEpoch,
			 feedbackCatalogXmin,
			 feedbackCatalogEpoch,
			 replyTimeStr);

		pfree(replyTimeStr);
	}

	/*
	 * Update shared state for this WalSender process based on reply data from
	 * standby.
	 */
	{
		WalSnd	   *walsnd = MyWalSnd;

		SpinLockAcquire(&walsnd->mutex);
		walsnd->replyTime = replyTime;
		SpinLockRelease(&walsnd->mutex);
	}

	/*
	 * Unset WalSender's xmins if the feedback message values are invalid.
	 * This happens when the downstream turned hot_standby_feedback off.
	 */
	if (!TransactionIdIsNormal(feedbackXmin)
		&& !TransactionIdIsNormal(feedbackCatalogXmin))
	{
		MyProc->xmin = InvalidTransactionId;
		if (MyReplicationSlot != NULL)
			PhysicalReplicationSlotNewXmin(feedbackXmin, feedbackCatalogXmin);
		return;
	}

	/*
	 * Check that the provided xmin/epoch are sane, that is, not in the future
	 * and not so far back as to be already wrapped around.  Ignore if not.
	 */
	if (TransactionIdIsNormal(feedbackXmin) &&
		!TransactionIdInRecentPast(feedbackXmin, feedbackEpoch))
		return;

	if (TransactionIdIsNormal(feedbackCatalogXmin) &&
		!TransactionIdInRecentPast(feedbackCatalogXmin, feedbackCatalogEpoch))
		return;

	/*
	 * Set the WalSender's xmin equal to the standby's requested xmin, so that
	 * the xmin will be taken into account by GetSnapshotData() /
	 * ComputeXidHorizons().  This will hold back the removal of dead rows and
	 * thereby prevent the generation of cleanup conflicts on the standby
	 * server.
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
	 * is assumed atomic, and there's no real need to prevent concurrent
	 * horizon determinations.  (If we're moving our xmin forward, this is
	 * obviously safe, and if we're moving it backwards, well, the data is at
	 * risk already since a VACUUM could already have determined the horizon.)
	 *
	 * If we're using a replication slot we reserve the xmin via that,
	 * otherwise via the walsender's PGPROC entry. We can only track the
	 * catalog xmin separately when using a slot, so we store the least of the
	 * two provided when not using a slot.
	 *
	 * XXX: It might make sense to generalize the ephemeral slot concept and
	 * always use the slot mechanism to handle the feedback xmin.
	 */
	if (MyReplicationSlot != NULL)	/* XXX: persistency configurable? */
		PhysicalReplicationSlotNewXmin(feedbackXmin, feedbackCatalogXmin);
	else
	{
		if (TransactionIdIsNormal(feedbackCatalogXmin)
			&& TransactionIdPrecedes(feedbackCatalogXmin, feedbackXmin))
			MyProc->xmin = feedbackCatalogXmin;
		else
			MyProc->xmin = feedbackXmin;
	}
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
	long		sleeptime = 10000;	/* 10 s */

	if (wal_sender_timeout > 0 && last_reply_timestamp > 0)
	{
		TimestampTz wakeup_time;

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
		sleeptime = TimestampDifferenceMilliseconds(now, wakeup_time);
	}

	return sleeptime;
}

/*
 * Check whether there have been responses by the client within
 * wal_sender_timeout and shutdown if not.  Using last_processing as the
 * reference point avoids counting server-side stalls against the client.
 * However, a long server-side stall can make WalSndKeepaliveIfNecessary()
 * postdate last_processing by more than wal_sender_timeout.  If that happens,
 * the client must reply almost immediately to avoid a timeout.  This rarely
 * affects the default configuration, under which clients spontaneously send a
 * message every standby_message_timeout = wal_sender_timeout/6 = 10s.  We
 * could eliminate that problem by recognizing timeout expiration at
 * wal_sender_timeout/2 after the keepalive.
 */
static void
WalSndCheckTimeOut(void)
{
	TimestampTz timeout;

	/* don't bail out if we're doing something that doesn't require timeouts */
	if (last_reply_timestamp <= 0)
		return;

	timeout = TimestampTzPlusMilliseconds(last_reply_timestamp,
										  wal_sender_timeout);

	if (wal_sender_timeout > 0 && last_processing >= timeout)
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
	TimestampTz last_flush = 0;

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
		/* Clear any already-pending wakeups */
		ResetLatch(MyLatch);

		CHECK_FOR_INTERRUPTS();

		/* Process any requests or signals received recently */
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
			SyncRepInitConfig();
		}

		/* Check for input from the client */
		ProcessRepliesIfAny();

		/*
		 * If we have received CopyDone from the client, sent CopyDone
		 * ourselves, and the output buffer is empty, it's time to exit
		 * streaming.
		 */
		if (streamingDoneReceiving && streamingDoneSending &&
			!pq_is_send_pending())
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
						(errmsg_internal("\"%s\" has now caught up with upstream server",
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
			if (got_SIGUSR2)
				WalSndDone(send_data);
		}

		/* Check for replication timeout. */
		WalSndCheckTimeOut();

		/* Send keepalive if the time has come */
		WalSndKeepaliveIfNecessary();

		/*
		 * Block if we have unsent data.  XXX For logical replication, let
		 * WalSndWaitForWal() handle any other blocking; idle receivers need
		 * its additional actions.  For physical replication, also block if
		 * caught up; its send_data does not block.
		 *
		 * The IO statistics are reported in WalSndWaitForWal() for the
		 * logical WAL senders.
		 */
		if ((WalSndCaughtUp && send_data != XLogSendLogical &&
			 !streamingDoneSending) ||
			pq_is_send_pending())
		{
			long		sleeptime;
			int			wakeEvents;
			TimestampTz now;

			if (!streamingDoneReceiving)
				wakeEvents = WL_SOCKET_READABLE;
			else
				wakeEvents = 0;

			/*
			 * Use fresh timestamp, not last_processing, to reduce the chance
			 * of reaching wal_sender_timeout before sending a keepalive.
			 */
			now = GetCurrentTimestamp();
			sleeptime = WalSndComputeSleeptime(now);

			if (pq_is_send_pending())
				wakeEvents |= WL_SOCKET_WRITEABLE;

			/* Report IO statistics, if needed */
			if (TimestampDifferenceExceeds(last_flush, now,
										   WALSENDER_STATS_FLUSH_INTERVAL))
			{
				pgstat_flush_io(false);
				last_flush = now;
			}

			/* Sleep until something happens or we time out */
			WalSndWait(wakeEvents, sleeptime, WAIT_EVENT_WAL_SENDER_MAIN);
		}
	}
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
	 * Find a free walsender slot and reserve it. This must not fail due to
	 * the prior check for free WAL senders in InitProcess().
	 */
	for (i = 0; i < max_wal_senders; i++)
	{
		WalSnd	   *walsnd = &WalSndCtl->walsnds[i];

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
			walsnd->state = WALSNDSTATE_STARTUP;
			walsnd->sentPtr = InvalidXLogRecPtr;
			walsnd->needreload = false;
			walsnd->write = InvalidXLogRecPtr;
			walsnd->flush = InvalidXLogRecPtr;
			walsnd->apply = InvalidXLogRecPtr;
			walsnd->writeLag = -1;
			walsnd->flushLag = -1;
			walsnd->applyLag = -1;
			walsnd->sync_standby_priority = 0;
			walsnd->latch = &MyProc->procLatch;
			walsnd->replyTime = 0;

			/*
			 * The kind assignment is done here and not in StartReplication()
			 * and StartLogicalReplication(). Indeed, the logical walsender
			 * needs to read WAL records (like snapshot of running
			 * transactions) during the slot creation. So it needs to be woken
			 * up based on its kind.
			 *
			 * The kind assignment could also be done in StartReplication(),
			 * StartLogicalReplication() and CREATE_REPLICATION_SLOT but it
			 * seems better to set it on one place.
			 */
			if (MyDatabaseId == InvalidOid)
				walsnd->kind = REPLICATION_KIND_PHYSICAL;
			else
				walsnd->kind = REPLICATION_KIND_LOGICAL;

			SpinLockRelease(&walsnd->mutex);
			/* don't need the lock anymore */
			MyWalSnd = (WalSnd *) walsnd;

			break;
		}
	}

	Assert(MyWalSnd != NULL);

	/* Arrange to clean up at walsender exit */
	on_shmem_exit(WalSndKill, 0);
}

/* Destroy the per-walsender data structure for this walsender process */
static void
WalSndKill(int code, Datum arg)
{
	WalSnd	   *walsnd = MyWalSnd;

	Assert(walsnd != NULL);

	MyWalSnd = NULL;

	SpinLockAcquire(&walsnd->mutex);
	/* clear latch while holding the spinlock, so it can safely be read */
	walsnd->latch = NULL;
	/* Mark WalSnd struct as no longer being in use. */
	walsnd->pid = 0;
	SpinLockRelease(&walsnd->mutex);
}

/* XLogReaderRoutine->segment_open callback */
static void
WalSndSegmentOpen(XLogReaderState *state, XLogSegNo nextSegNo,
				  TimeLineID *tli_p)
{
	char		path[MAXPGPATH];

	/*-------
	 * When reading from a historic timeline, and there is a timeline switch
	 * within this segment, read from the WAL segment belonging to the new
	 * timeline.
	 *
	 * For example, imagine that this server is currently on timeline 5, and
	 * we're streaming timeline 4. The switch from timeline 4 to 5 happened at
	 * 0/13002088. In pg_wal, we have these files:
	 *
	 * ...
	 * 000000040000000000000012
	 * 000000040000000000000013
	 * 000000050000000000000013
	 * 000000050000000000000014
	 * ...
	 *
	 * In this situation, when requested to send the WAL from segment 0x13, on
	 * timeline 4, we read the WAL from file 000000050000000000000013. Archive
	 * recovery prefers files from newer timelines, so if the segment was
	 * restored from the archive on this server, the file belonging to the old
	 * timeline, 000000040000000000000013, might not exist. Their contents are
	 * equal up to the switchpoint, because at a timeline switch, the used
	 * portion of the old segment is copied to the new file.
	 */
	*tli_p = sendTimeLine;
	if (sendTimeLineIsHistoric)
	{
		XLogSegNo	endSegNo;

		XLByteToSeg(sendTimeLineValidUpto, endSegNo, state->segcxt.ws_segsize);
		if (nextSegNo == endSegNo)
			*tli_p = sendTimeLineNextTLI;
	}

	XLogFilePath(path, *tli_p, nextSegNo, state->segcxt.ws_segsize);
	state->seg.ws_file = BasicOpenFile(path, O_RDONLY | PG_BINARY);
	if (state->seg.ws_file >= 0)
		return;

	/*
	 * If the file is not found, assume it's because the standby asked for a
	 * too old WAL segment that has already been removed or recycled.
	 */
	if (errno == ENOENT)
	{
		char		xlogfname[MAXFNAMELEN];
		int			save_errno = errno;

		XLogFileName(xlogfname, *tli_p, nextSegNo, wal_segment_size);
		errno = save_errno;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("requested WAL segment %s has already been removed",
						xlogfname)));
	}
	else
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m",
						path)));
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
	XLogSegNo	segno;
	WALReadError errinfo;
	Size		rbytes;

	/* If requested switch the WAL sender to the stopping state. */
	if (got_STOPPING)
		WalSndSetState(WALSNDSTATE_STOPPING);

	if (streamingDoneSending)
	{
		WalSndCaughtUp = true;
		return;
	}

	/* Figure out how far we can safely send the WAL. */
	if (sendTimeLineIsHistoric)
	{
		/*
		 * Streaming an old timeline that's in this server's history, but is
		 * not the one we're currently inserting or replaying. It can be
		 * streamed up to the point where we switched off that timeline.
		 */
		SendRqstPtr = sendTimeLineValidUpto;
	}
	else if (am_cascading_walsender)
	{
		TimeLineID	SendRqstTLI;

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

		SendRqstPtr = GetStandbyFlushRecPtr(&SendRqstTLI);

		if (!RecoveryInProgress())
		{
			/* We have been promoted. */
			SendRqstTLI = GetWALInsertionTimeLine();
			am_cascading_walsender = false;
			becameHistoric = true;
		}
		else
		{
			/*
			 * Still a cascading standby. But is the timeline we're sending
			 * still the one recovery is recovering from?
			 */
			if (sendTimeLine != SendRqstTLI)
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

			history = readTimeLineHistory(SendRqstTLI);
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
		 * Streaming the current timeline on a primary.
		 *
		 * Attempt to send all data that's already been written out and
		 * fsync'd to disk.  We cannot go further than what's been written out
		 * given the current implementation of WALRead().  And in any case
		 * it's unsafe to send WAL that is not securely down to disk on the
		 * primary: if the primary subsequently crashes and restarts, standbys
		 * must not have applied any WAL that got lost on the primary.
		 */
		SendRqstPtr = GetFlushRecPtr(NULL);
	}

	/*
	 * Record the current system time as an approximation of the time at which
	 * this WAL location was written for the purposes of lag tracking.
	 *
	 * In theory we could make XLogFlush() record a time in shmem whenever WAL
	 * is flushed and we could get that time as well as the LSN when we call
	 * GetFlushRecPtr() above (and likewise for the cascading standby
	 * equivalent), but rather than putting any new code into the hot WAL path
	 * it seems good enough to capture the time here.  We should reach this
	 * after XLogFlush() runs WalSndWakeupProcessRequests(), and although that
	 * may take some time, we read the WAL flush pointer and take the time
	 * very close to together here so that we'll get a later position if it is
	 * still moving.
	 *
	 * Because LagTrackerWrite ignores samples when the LSN hasn't advanced,
	 * this gives us a cheap approximation for the WAL flush time for this
	 * LSN.
	 *
	 * Note that the LSN is not necessarily the LSN for the data contained in
	 * the present message; it's the end of the WAL, which might be further
	 * ahead.  All the lag tracking machinery cares about is finding out when
	 * that arbitrary LSN is eventually reported as written, flushed and
	 * applied, so that it can measure the elapsed time.
	 */
	LagTrackerWrite(SendRqstPtr, GetCurrentTimestamp());

	/*
	 * If this is a historic timeline and we've reached the point where we
	 * forked to the next timeline, stop streaming.
	 *
	 * Note: We might already have sent WAL > sendTimeLineValidUpto. The
	 * startup process will normally replay all WAL that has been received
	 * from the primary, before promoting, but if the WAL streaming is
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
		if (xlogreader->seg.ws_file >= 0)
			wal_segment_close(xlogreader);

		/* Send CopyDone */
		pq_putmessage_noblock('c', NULL, 0);
		streamingDoneSending = true;

		WalSndCaughtUp = true;

		elog(DEBUG1, "walsender reached end of timeline at %X/%X (sent up to %X/%X)",
			 LSN_FORMAT_ARGS(sendTimeLineValidUpto),
			 LSN_FORMAT_ARGS(sentPtr));
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

retry:
	/* attempt to read WAL from WAL buffers first */
	rbytes = WALReadFromBuffers(&output_message.data[output_message.len],
								startptr, nbytes, xlogreader->seg.ws_tli);
	output_message.len += rbytes;
	startptr += rbytes;
	nbytes -= rbytes;

	/* now read the remaining WAL from WAL file */
	if (nbytes > 0 &&
		!WALRead(xlogreader,
				 &output_message.data[output_message.len],
				 startptr,
				 nbytes,
				 xlogreader->seg.ws_tli,	/* Pass the current TLI because
											 * only WalSndSegmentOpen controls
											 * whether new TLI is needed. */
				 &errinfo))
		WALReadRaiseError(&errinfo);

	/* See logical_read_xlog_page(). */
	XLByteToSeg(startptr, segno, xlogreader->segcxt.ws_segsize);
	CheckXLogRemoved(segno, xlogreader->seg.ws_tli);

	/*
	 * During recovery, the currently-open WAL file might be replaced with the
	 * file of the same name retrieved from archive. So we always need to
	 * check what we read was valid after reading into the buffer. If it's
	 * invalid, we try to open and read the file again.
	 */
	if (am_cascading_walsender)
	{
		WalSnd	   *walsnd = MyWalSnd;
		bool		reload;

		SpinLockAcquire(&walsnd->mutex);
		reload = walsnd->needreload;
		walsnd->needreload = false;
		SpinLockRelease(&walsnd->mutex);

		if (reload && xlogreader->seg.ws_file >= 0)
		{
			wal_segment_close(xlogreader);

			goto retry;
		}
	}

	output_message.len += nbytes;
	output_message.data[output_message.len] = '\0';

	/*
	 * Fill the send timestamp last, so that it is taken as late as possible.
	 */
	resetStringInfo(&tmpbuf);
	pq_sendint64(&tmpbuf, GetCurrentTimestamp());
	memcpy(&output_message.data[1 + sizeof(int64) + sizeof(int64)],
		   tmpbuf.data, sizeof(int64));

	pq_putmessage_noblock('d', output_message.data, output_message.len);

	sentPtr = endptr;

	/* Update shared memory status */
	{
		WalSnd	   *walsnd = MyWalSnd;

		SpinLockAcquire(&walsnd->mutex);
		walsnd->sentPtr = sentPtr;
		SpinLockRelease(&walsnd->mutex);
	}

	/* Report progress of XLOG streaming in PS display */
	if (update_process_title)
	{
		char		activitymsg[50];

		snprintf(activitymsg, sizeof(activitymsg), "streaming %X/%X",
				 LSN_FORMAT_ARGS(sentPtr));
		set_ps_display(activitymsg);
	}
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
	 * We'll use the current flush point to determine whether we've caught up.
	 * This variable is static in order to cache it across calls.  Caching is
	 * helpful because GetFlushRecPtr() needs to acquire a heavily-contended
	 * spinlock.
	 */
	static XLogRecPtr flushPtr = InvalidXLogRecPtr;

	/*
	 * Don't know whether we've caught up yet. We'll set WalSndCaughtUp to
	 * true in WalSndWaitForWal, if we're actually waiting. We also set to
	 * true if XLogReadRecord() had to stop reading but WalSndWaitForWal
	 * didn't wait - i.e. when we're shutting down.
	 */
	WalSndCaughtUp = false;

	record = XLogReadRecord(logical_decoding_ctx->reader, &errm);

	/* xlog record was invalid */
	if (errm != NULL)
		elog(ERROR, "could not find record while sending logically-decoded data: %s",
			 errm);

	if (record != NULL)
	{
		/*
		 * Note the lack of any call to LagTrackerWrite() which is handled by
		 * WalSndUpdateProgress which is called by output plugin through
		 * logical decoding write api.
		 */
		LogicalDecodingProcessRecord(logical_decoding_ctx, logical_decoding_ctx->reader);

		sentPtr = logical_decoding_ctx->reader->EndRecPtr;
	}

	/*
	 * If first time through in this session, initialize flushPtr.  Otherwise,
	 * we only need to update flushPtr if EndRecPtr is past it.
	 */
	if (flushPtr == InvalidXLogRecPtr ||
		logical_decoding_ctx->reader->EndRecPtr >= flushPtr)
	{
		/*
		 * For cascading logical WAL senders, we use the replay LSN instead of
		 * the flush LSN, since logical decoding on a standby only processes
		 * WAL that has been replayed.  This distinction becomes particularly
		 * important during shutdown, as new WAL is no longer replayed and the
		 * last replayed LSN marks the furthest point up to which decoding can
		 * proceed.
		 */
		if (am_cascading_walsender)
			flushPtr = GetXLogReplayRecPtr(NULL);
		else
			flushPtr = GetFlushRecPtr(NULL);
	}

	/* If EndRecPtr is still past our flushPtr, it means we caught up. */
	if (logical_decoding_ctx->reader->EndRecPtr >= flushPtr)
		WalSndCaughtUp = true;

	/*
	 * If we're caught up and have been requested to stop, have WalSndLoop()
	 * terminate the connection in an orderly manner, after writing out all
	 * the pending data.
	 */
	if (WalSndCaughtUp && got_STOPPING)
		got_SIGUSR2 = true;

	/* Update shared memory status */
	{
		WalSnd	   *walsnd = MyWalSnd;

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
	 * To figure out whether all WAL has successfully been replicated, check
	 * flush location if valid, write otherwise. Tools like pg_receivewal will
	 * usually (unless in synchronous mode) return an invalid flush location.
	 */
	replicatedPtr = XLogRecPtrIsInvalid(MyWalSnd->flush) ?
		MyWalSnd->write : MyWalSnd->flush;

	if (WalSndCaughtUp && sentPtr == replicatedPtr &&
		!pq_is_send_pending())
	{
		QueryCompletion qc;

		/* Inform the standby that XLOG streaming is done */
		SetQueryCompletion(&qc, CMDTAG_COPY, 0);
		EndCommand(&qc, DestRemote, false);
		pq_flush();

		proc_exit(0);
	}
	if (!waiting_for_ping_response)
		WalSndKeepalive(true, InvalidXLogRecPtr);
}

/*
 * Returns the latest point in WAL that has been safely flushed to disk.
 * This should only be called when in recovery.
 *
 * This is called either by cascading walsender to find WAL position to be sent
 * to a cascaded standby or by slot synchronization operation to validate remote
 * slot's lsn before syncing it locally.
 *
 * As a side-effect, *tli is updated to the TLI of the last
 * replayed WAL record.
 */
XLogRecPtr
GetStandbyFlushRecPtr(TimeLineID *tli)
{
	XLogRecPtr	replayPtr;
	TimeLineID	replayTLI;
	XLogRecPtr	receivePtr;
	TimeLineID	receiveTLI;
	XLogRecPtr	result;

	Assert(am_cascading_walsender || IsSyncingReplicationSlots());

	/*
	 * We can safely send what's already been replayed. Also, if walreceiver
	 * is streaming WAL from the same timeline, we can send anything that it
	 * has streamed, but hasn't been replayed yet.
	 */

	receivePtr = GetWalRcvFlushRecPtr(NULL, &receiveTLI);
	replayPtr = GetXLogReplayRecPtr(&replayTLI);

	if (tli)
		*tli = replayTLI;

	result = replayPtr;
	if (receiveTLI == replayTLI && receivePtr > replayPtr)
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
		WalSnd	   *walsnd = &WalSndCtl->walsnds[i];

		SpinLockAcquire(&walsnd->mutex);
		if (walsnd->pid == 0)
		{
			SpinLockRelease(&walsnd->mutex);
			continue;
		}
		walsnd->needreload = true;
		SpinLockRelease(&walsnd->mutex);
	}
}

/*
 * Handle PROCSIG_WALSND_INIT_STOPPING signal.
 */
void
HandleWalSndInitStopping(void)
{
	Assert(am_walsender);

	/*
	 * If replication has not yet started, die like with SIGTERM. If
	 * replication is active, only set a flag and wake up the main loop. It
	 * will send any outstanding WAL, wait for it to be replicated to the
	 * standby, and then exit gracefully.
	 */
	if (!replication_active)
		kill(MyProcPid, SIGTERM);
	else
		got_STOPPING = true;
}

/*
 * SIGUSR2: set flag to do a last cycle and shut down afterwards. The WAL
 * sender should already have been switched to WALSNDSTATE_STOPPING at
 * this point.
 */
static void
WalSndLastCycleHandler(SIGNAL_ARGS)
{
	got_SIGUSR2 = true;
	SetLatch(MyLatch);
}

/* Set up signal handlers */
void
WalSndSignals(void)
{
	/* Set up signal handlers */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGINT, StatementCancelHandler);	/* query cancel */
	pqsignal(SIGTERM, die);		/* request shutdown */
	/* SIGQUIT handler was already set up by InitPostmasterChild */
	InitializeTimeouts();		/* establishes SIGALRM handler */
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, WalSndLastCycleHandler);	/* request a last cycle and
												 * shutdown */

	/* Reset some signals that are accepted by postmaster but not here */
	pqsignal(SIGCHLD, SIG_DFL);
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
			dlist_init(&(WalSndCtl->SyncRepQueue[i]));

		for (i = 0; i < max_wal_senders; i++)
		{
			WalSnd	   *walsnd = &WalSndCtl->walsnds[i];

			SpinLockInit(&walsnd->mutex);
		}

		ConditionVariableInit(&WalSndCtl->wal_flush_cv);
		ConditionVariableInit(&WalSndCtl->wal_replay_cv);
		ConditionVariableInit(&WalSndCtl->wal_confirm_rcv_cv);
	}
}

/*
 * Wake up physical, logical or both kinds of walsenders
 *
 * The distinction between physical and logical walsenders is done, because:
 * - physical walsenders can't send data until it's been flushed
 * - logical walsenders on standby can't decode and send data until it's been
 *   applied
 *
 * For cascading replication we need to wake up physical walsenders separately
 * from logical walsenders (see the comment before calling WalSndWakeup() in
 * ApplyWalRecord() for more details).
 *
 * This will be called inside critical sections, so throwing an error is not
 * advisable.
 */
void
WalSndWakeup(bool physical, bool logical)
{
	/*
	 * Wake up all the walsenders waiting on WAL being flushed or replayed
	 * respectively.  Note that waiting walsender would have prepared to sleep
	 * on the CV (i.e., added itself to the CV's waitlist) in WalSndWait()
	 * before actually waiting.
	 */
	if (physical)
		ConditionVariableBroadcast(&WalSndCtl->wal_flush_cv);

	if (logical)
		ConditionVariableBroadcast(&WalSndCtl->wal_replay_cv);
}

/*
 * Wait for readiness on the FeBe socket, or a timeout.  The mask should be
 * composed of optional WL_SOCKET_WRITEABLE and WL_SOCKET_READABLE flags.  Exit
 * on postmaster death.
 */
static void
WalSndWait(uint32 socket_events, long timeout, uint32 wait_event)
{
	WaitEvent	event;

	ModifyWaitEvent(FeBeWaitSet, FeBeWaitSetSocketPos, socket_events, NULL);

	/*
	 * We use a condition variable to efficiently wake up walsenders in
	 * WalSndWakeup().
	 *
	 * Every walsender prepares to sleep on a shared memory CV. Note that it
	 * just prepares to sleep on the CV (i.e., adds itself to the CV's
	 * waitlist), but does not actually wait on the CV (IOW, it never calls
	 * ConditionVariableSleep()). It still uses WaitEventSetWait() for
	 * waiting, because we also need to wait for socket events. The processes
	 * (startup process, walreceiver etc.) wanting to wake up walsenders use
	 * ConditionVariableBroadcast(), which in turn calls SetLatch(), helping
	 * walsenders come out of WaitEventSetWait().
	 *
	 * This approach is simple and efficient because, one doesn't have to loop
	 * through all the walsenders slots, with a spinlock acquisition and
	 * release for every iteration, just to wake up only the waiting
	 * walsenders. It makes WalSndWakeup() callers' life easy.
	 *
	 * XXX: A desirable future improvement would be to add support for CVs
	 * into WaitEventSetWait().
	 *
	 * And, we use separate shared memory CVs for physical and logical
	 * walsenders for selective wake ups, see WalSndWakeup() for more details.
	 *
	 * If the wait event is WAIT_FOR_STANDBY_CONFIRMATION, wait on another CV
	 * until awakened by physical walsenders after the walreceiver confirms
	 * the receipt of the LSN.
	 */
	if (wait_event == WAIT_EVENT_WAIT_FOR_STANDBY_CONFIRMATION)
		ConditionVariablePrepareToSleep(&WalSndCtl->wal_confirm_rcv_cv);
	else if (MyWalSnd->kind == REPLICATION_KIND_PHYSICAL)
		ConditionVariablePrepareToSleep(&WalSndCtl->wal_flush_cv);
	else if (MyWalSnd->kind == REPLICATION_KIND_LOGICAL)
		ConditionVariablePrepareToSleep(&WalSndCtl->wal_replay_cv);

	if (WaitEventSetWait(FeBeWaitSet, timeout, &event, 1, wait_event) == 1 &&
		(event.events & WL_POSTMASTER_DEATH))
	{
		ConditionVariableCancelSleep();
		proc_exit(1);
	}

	ConditionVariableCancelSleep();
}

/*
 * Signal all walsenders to move to stopping state.
 *
 * This will trigger walsenders to move to a state where no further WAL can be
 * generated. See this file's header for details.
 */
void
WalSndInitStopping(void)
{
	int			i;

	for (i = 0; i < max_wal_senders; i++)
	{
		WalSnd	   *walsnd = &WalSndCtl->walsnds[i];
		pid_t		pid;

		SpinLockAcquire(&walsnd->mutex);
		pid = walsnd->pid;
		SpinLockRelease(&walsnd->mutex);

		if (pid == 0)
			continue;

		SendProcSignal(pid, PROCSIG_WALSND_INIT_STOPPING, INVALID_PROC_NUMBER);
	}
}

/*
 * Wait that all the WAL senders have quit or reached the stopping state. This
 * is used by the checkpointer to control when the shutdown checkpoint can
 * safely be performed.
 */
void
WalSndWaitStopping(void)
{
	for (;;)
	{
		int			i;
		bool		all_stopped = true;

		for (i = 0; i < max_wal_senders; i++)
		{
			WalSnd	   *walsnd = &WalSndCtl->walsnds[i];

			SpinLockAcquire(&walsnd->mutex);

			if (walsnd->pid == 0)
			{
				SpinLockRelease(&walsnd->mutex);
				continue;
			}

			if (walsnd->state != WALSNDSTATE_STOPPING)
			{
				all_stopped = false;
				SpinLockRelease(&walsnd->mutex);
				break;
			}
			SpinLockRelease(&walsnd->mutex);
		}

		/* safe to leave if confirmation is done for all WAL senders */
		if (all_stopped)
			return;

		pg_usleep(10000L);		/* wait for 10 msec */
	}
}

/* Set state for current walsender (only called in walsender) */
void
WalSndSetState(WalSndState state)
{
	WalSnd	   *walsnd = MyWalSnd;

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
		case WALSNDSTATE_STOPPING:
			return "stopping";
	}
	return "UNKNOWN";
}

static Interval *
offset_to_interval(TimeOffset offset)
{
	Interval   *result = palloc(sizeof(Interval));

	result->month = 0;
	result->day = 0;
	result->time = offset;

	return result;
}

/*
 * Returns activity of walsenders, including pids and xlog locations sent to
 * standby servers.
 */
Datum
pg_stat_get_wal_senders(PG_FUNCTION_ARGS)
{
#define PG_STAT_GET_WAL_SENDERS_COLS	12
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	SyncRepStandbyData *sync_standbys;
	int			num_standbys;
	int			i;

	InitMaterializedSRF(fcinfo, 0);

	/*
	 * Get the currently active synchronous standbys.  This could be out of
	 * date before we're done, but we'll use the data anyway.
	 */
	num_standbys = SyncRepGetCandidateStandbys(&sync_standbys);

	for (i = 0; i < max_wal_senders; i++)
	{
		WalSnd	   *walsnd = &WalSndCtl->walsnds[i];
		XLogRecPtr	sent_ptr;
		XLogRecPtr	write;
		XLogRecPtr	flush;
		XLogRecPtr	apply;
		TimeOffset	writeLag;
		TimeOffset	flushLag;
		TimeOffset	applyLag;
		int			priority;
		int			pid;
		WalSndState state;
		TimestampTz replyTime;
		bool		is_sync_standby;
		Datum		values[PG_STAT_GET_WAL_SENDERS_COLS];
		bool		nulls[PG_STAT_GET_WAL_SENDERS_COLS] = {0};
		int			j;

		/* Collect data from shared memory */
		SpinLockAcquire(&walsnd->mutex);
		if (walsnd->pid == 0)
		{
			SpinLockRelease(&walsnd->mutex);
			continue;
		}
		pid = walsnd->pid;
		sent_ptr = walsnd->sentPtr;
		state = walsnd->state;
		write = walsnd->write;
		flush = walsnd->flush;
		apply = walsnd->apply;
		writeLag = walsnd->writeLag;
		flushLag = walsnd->flushLag;
		applyLag = walsnd->applyLag;
		priority = walsnd->sync_standby_priority;
		replyTime = walsnd->replyTime;
		SpinLockRelease(&walsnd->mutex);

		/*
		 * Detect whether walsender is/was considered synchronous.  We can
		 * provide some protection against stale data by checking the PID
		 * along with walsnd_index.
		 */
		is_sync_standby = false;
		for (j = 0; j < num_standbys; j++)
		{
			if (sync_standbys[j].walsnd_index == i &&
				sync_standbys[j].pid == pid)
			{
				is_sync_standby = true;
				break;
			}
		}

		values[0] = Int32GetDatum(pid);

		if (!has_privs_of_role(GetUserId(), ROLE_PG_READ_ALL_STATS))
		{
			/*
			 * Only superusers and roles with privileges of pg_read_all_stats
			 * can see details. Other users only get the pid value to know
			 * it's a walsender, but no details.
			 */
			MemSet(&nulls[1], true, PG_STAT_GET_WAL_SENDERS_COLS - 1);
		}
		else
		{
			values[1] = CStringGetTextDatum(WalSndGetStateString(state));

			if (XLogRecPtrIsInvalid(sent_ptr))
				nulls[2] = true;
			values[2] = LSNGetDatum(sent_ptr);

			if (XLogRecPtrIsInvalid(write))
				nulls[3] = true;
			values[3] = LSNGetDatum(write);

			if (XLogRecPtrIsInvalid(flush))
				nulls[4] = true;
			values[4] = LSNGetDatum(flush);

			if (XLogRecPtrIsInvalid(apply))
				nulls[5] = true;
			values[5] = LSNGetDatum(apply);

			/*
			 * Treat a standby such as a pg_basebackup background process
			 * which always returns an invalid flush location, as an
			 * asynchronous standby.
			 */
			priority = XLogRecPtrIsInvalid(flush) ? 0 : priority;

			if (writeLag < 0)
				nulls[6] = true;
			else
				values[6] = IntervalPGetDatum(offset_to_interval(writeLag));

			if (flushLag < 0)
				nulls[7] = true;
			else
				values[7] = IntervalPGetDatum(offset_to_interval(flushLag));

			if (applyLag < 0)
				nulls[8] = true;
			else
				values[8] = IntervalPGetDatum(offset_to_interval(applyLag));

			values[9] = Int32GetDatum(priority);

			/*
			 * More easily understood version of standby state. This is purely
			 * informational.
			 *
			 * In quorum-based sync replication, the role of each standby
			 * listed in synchronous_standby_names can be changing very
			 * frequently. Any standbys considered as "sync" at one moment can
			 * be switched to "potential" ones at the next moment. So, it's
			 * basically useless to report "sync" or "potential" as their sync
			 * states. We report just "quorum" for them.
			 */
			if (priority == 0)
				values[10] = CStringGetTextDatum("async");
			else if (is_sync_standby)
				values[10] = SyncRepConfig->syncrep_method == SYNC_REP_PRIORITY ?
					CStringGetTextDatum("sync") : CStringGetTextDatum("quorum");
			else
				values[10] = CStringGetTextDatum("potential");

			if (replyTime == 0)
				nulls[11] = true;
			else
				values[11] = TimestampTzGetDatum(replyTime);
		}

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
							 values, nulls);
	}

	return (Datum) 0;
}

/*
 * Send a keepalive message to standby.
 *
 * If requestReply is set, the message requests the other party to send
 * a message back to us, for heartbeat purposes.  We also set a flag to
 * let nearby code know that we're waiting for that response, to avoid
 * repeated requests.
 *
 * writePtr is the location up to which the WAL is sent. It is essentially
 * the same as sentPtr but in some cases, we need to send keep alive before
 * sentPtr is updated like when skipping empty transactions.
 */
static void
WalSndKeepalive(bool requestReply, XLogRecPtr writePtr)
{
	elog(DEBUG2, "sending replication keepalive");

	/* construct the message... */
	resetStringInfo(&output_message);
	pq_sendbyte(&output_message, 'k');
	pq_sendint64(&output_message, XLogRecPtrIsInvalid(writePtr) ? sentPtr : writePtr);
	pq_sendint64(&output_message, GetCurrentTimestamp());
	pq_sendbyte(&output_message, requestReply ? 1 : 0);

	/* ... and send it wrapped in CopyData */
	pq_putmessage_noblock('d', output_message.data, output_message.len);

	/* Set local flag */
	if (requestReply)
		waiting_for_ping_response = true;
}

/*
 * Send keepalive message if too much time has elapsed.
 */
static void
WalSndKeepaliveIfNecessary(void)
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
	if (last_processing >= ping_time)
	{
		WalSndKeepalive(true, InvalidXLogRecPtr);

		/* Try to flush pending output to the client */
		if (pq_flush_if_writable() != 0)
			WalSndShutdown();
	}
}

/*
 * Record the end of the WAL and the time it was flushed locally, so that
 * LagTrackerRead can compute the elapsed time (lag) when this WAL location is
 * eventually reported to have been written, flushed and applied by the
 * standby in a reply message.
 */
static void
LagTrackerWrite(XLogRecPtr lsn, TimestampTz local_flush_time)
{
	bool		buffer_full;
	int			new_write_head;
	int			i;

	if (!am_walsender)
		return;

	/*
	 * If the lsn hasn't advanced since last time, then do nothing.  This way
	 * we only record a new sample when new WAL has been written.
	 */
	if (lag_tracker->last_lsn == lsn)
		return;
	lag_tracker->last_lsn = lsn;

	/*
	 * If advancing the write head of the circular buffer would crash into any
	 * of the read heads, then the buffer is full.  In other words, the
	 * slowest reader (presumably apply) is the one that controls the release
	 * of space.
	 */
	new_write_head = (lag_tracker->write_head + 1) % LAG_TRACKER_BUFFER_SIZE;
	buffer_full = false;
	for (i = 0; i < NUM_SYNC_REP_WAIT_MODE; ++i)
	{
		if (new_write_head == lag_tracker->read_heads[i])
			buffer_full = true;
	}

	/*
	 * If the buffer is full, for now we just rewind by one slot and overwrite
	 * the last sample, as a simple (if somewhat uneven) way to lower the
	 * sampling rate.  There may be better adaptive compaction algorithms.
	 */
	if (buffer_full)
	{
		new_write_head = lag_tracker->write_head;
		if (lag_tracker->write_head > 0)
			lag_tracker->write_head--;
		else
			lag_tracker->write_head = LAG_TRACKER_BUFFER_SIZE - 1;
	}

	/* Store a sample at the current write head position. */
	lag_tracker->buffer[lag_tracker->write_head].lsn = lsn;
	lag_tracker->buffer[lag_tracker->write_head].time = local_flush_time;
	lag_tracker->write_head = new_write_head;
}

/*
 * Find out how much time has elapsed between the moment WAL location 'lsn'
 * (or the highest known earlier LSN) was flushed locally and the time 'now'.
 * We have a separate read head for each of the reported LSN locations we
 * receive in replies from standby; 'head' controls which read head is
 * used.  Whenever a read head crosses an LSN which was written into the
 * lag buffer with LagTrackerWrite, we can use the associated timestamp to
 * find out the time this LSN (or an earlier one) was flushed locally, and
 * therefore compute the lag.
 *
 * Return -1 if no new sample data is available, and otherwise the elapsed
 * time in microseconds.
 */
static TimeOffset
LagTrackerRead(int head, XLogRecPtr lsn, TimestampTz now)
{
	TimestampTz time = 0;

	/* Read all unread samples up to this LSN or end of buffer. */
	while (lag_tracker->read_heads[head] != lag_tracker->write_head &&
		   lag_tracker->buffer[lag_tracker->read_heads[head]].lsn <= lsn)
	{
		time = lag_tracker->buffer[lag_tracker->read_heads[head]].time;
		lag_tracker->last_read[head] =
			lag_tracker->buffer[lag_tracker->read_heads[head]];
		lag_tracker->read_heads[head] =
			(lag_tracker->read_heads[head] + 1) % LAG_TRACKER_BUFFER_SIZE;
	}

	/*
	 * If the lag tracker is empty, that means the standby has processed
	 * everything we've ever sent so we should now clear 'last_read'.  If we
	 * didn't do that, we'd risk using a stale and irrelevant sample for
	 * interpolation at the beginning of the next burst of WAL after a period
	 * of idleness.
	 */
	if (lag_tracker->read_heads[head] == lag_tracker->write_head)
		lag_tracker->last_read[head].time = 0;

	if (time > now)
	{
		/* If the clock somehow went backwards, treat as not found. */
		return -1;
	}
	else if (time == 0)
	{
		/*
		 * We didn't cross a time.  If there is a future sample that we
		 * haven't reached yet, and we've already reached at least one sample,
		 * let's interpolate the local flushed time.  This is mainly useful
		 * for reporting a completely stuck apply position as having
		 * increasing lag, since otherwise we'd have to wait for it to
		 * eventually start moving again and cross one of our samples before
		 * we can show the lag increasing.
		 */
		if (lag_tracker->read_heads[head] == lag_tracker->write_head)
		{
			/* There are no future samples, so we can't interpolate. */
			return -1;
		}
		else if (lag_tracker->last_read[head].time != 0)
		{
			/* We can interpolate between last_read and the next sample. */
			double		fraction;
			WalTimeSample prev = lag_tracker->last_read[head];
			WalTimeSample next = lag_tracker->buffer[lag_tracker->read_heads[head]];

			if (lsn < prev.lsn)
			{
				/*
				 * Reported LSNs shouldn't normally go backwards, but it's
				 * possible when there is a timeline change.  Treat as not
				 * found.
				 */
				return -1;
			}

			Assert(prev.lsn < next.lsn);

			if (prev.time > next.time)
			{
				/* If the clock somehow went backwards, treat as not found. */
				return -1;
			}

			/* See how far we are between the previous and next samples. */
			fraction =
				(double) (lsn - prev.lsn) / (double) (next.lsn - prev.lsn);

			/* Scale the local flush time proportionally. */
			time = (TimestampTz)
				((double) prev.time + (next.time - prev.time) * fraction);
		}
		else
		{
			/*
			 * We have only a future sample, implying that we were entirely
			 * caught up but and now there is a new burst of WAL and the
			 * standby hasn't processed the first sample yet.  Until the
			 * standby reaches the future sample the best we can do is report
			 * the hypothetical lag if that sample were to be replayed now.
			 */
			time = lag_tracker->buffer[lag_tracker->read_heads[head]].time;
		}
	}

	/* Return the elapsed time since local flush time in microseconds. */
	Assert(time != 0);
	return now - time;
}
