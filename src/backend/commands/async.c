/*-------------------------------------------------------------------------
 *
 * async.c
 *	  Asynchronous notification: NOTIFY, LISTEN, UNLISTEN
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/async.c
 *
 *-------------------------------------------------------------------------
 */

/*-------------------------------------------------------------------------
 * Async Notification Model as of 9.0:
 *
 * 1. Multiple backends on same machine. Multiple backends listening on
 *	  several channels. (Channels are also called "conditions" in other
 *	  parts of the code.)
 *
 * 2. There is one central queue in disk-based storage (directory pg_notify/),
 *	  with actively-used pages mapped into shared memory by the slru.c module.
 *	  All notification messages are placed in the queue and later read out
 *	  by listening backends.
 *
 *	  There is no central knowledge of which backend listens on which channel;
 *	  every backend has its own list of interesting channels.
 *
 *	  Although there is only one queue, notifications are treated as being
 *	  database-local; this is done by including the sender's database OID
 *	  in each notification message.  Listening backends ignore messages
 *	  that don't match their database OID.  This is important because it
 *	  ensures senders and receivers have the same database encoding and won't
 *	  misinterpret non-ASCII text in the channel name or payload string.
 *
 *	  Since notifications are not expected to survive database crashes,
 *	  we can simply clean out the pg_notify data at any reboot, and there
 *	  is no need for WAL support or fsync'ing.
 *
 * 3. Every backend that is listening on at least one channel registers by
 *	  entering its PID into the array in AsyncQueueControl. It then scans all
 *	  incoming notifications in the central queue and first compares the
 *	  database OID of the notification with its own database OID and then
 *	  compares the notified channel with the list of channels that it listens
 *	  to. In case there is a match it delivers the notification event to its
 *	  frontend.  Non-matching events are simply skipped.
 *
 * 4. The NOTIFY statement (routine Async_Notify) stores the notification in
 *	  a backend-local list which will not be processed until transaction end.
 *
 *	  Duplicate notifications from the same transaction are sent out as one
 *	  notification only. This is done to save work when for example a trigger
 *	  on a 2 million row table fires a notification for each row that has been
 *	  changed. If the application needs to receive every single notification
 *	  that has been sent, it can easily add some unique string into the extra
 *	  payload parameter.
 *
 *	  When the transaction is ready to commit, PreCommit_Notify() adds the
 *	  pending notifications to the head of the queue. The head pointer of the
 *	  queue always points to the next free position and a position is just a
 *	  page number and the offset in that page. This is done before marking the
 *	  transaction as committed in clog. If we run into problems writing the
 *	  notifications, we can still call elog(ERROR, ...) and the transaction
 *	  will roll back.
 *
 *	  Once we have put all of the notifications into the queue, we return to
 *	  CommitTransaction() which will then do the actual transaction commit.
 *
 *	  After commit we are called another time (AtCommit_Notify()). Here we
 *	  make the actual updates to the effective listen state (listenChannels).
 *
 *	  Finally, after we are out of the transaction altogether, we check if
 *	  we need to signal listening backends.  In SignalBackends() we scan the
 *	  list of listening backends and send a PROCSIG_NOTIFY_INTERRUPT signal
 *	  to every listening backend (we don't know which backend is listening on
 *	  which channel so we must signal them all). We can exclude backends that
 *	  are already up to date, though.  We don't bother with a self-signal
 *	  either, but just process the queue directly.
 *
 * 5. Upon receipt of a PROCSIG_NOTIFY_INTERRUPT signal, the signal handler
 *	  sets the process's latch, which triggers the event to be processed
 *	  immediately if this backend is idle (i.e., it is waiting for a frontend
 *	  command and is not within a transaction block. C.f.
 *	  ProcessClientReadInterrupt()).  Otherwise the handler may only set a
 *	  flag, which will cause the processing to occur just before we next go
 *	  idle.
 *
 *	  Inbound-notify processing consists of reading all of the notifications
 *	  that have arrived since scanning last time. We read every notification
 *	  until we reach either a notification from an uncommitted transaction or
 *	  the head pointer's position. Then we check if we were the laziest
 *	  backend: if our pointer is set to the same position as the global tail
 *	  pointer is set, then we move the global tail pointer ahead to where the
 *	  second-laziest backend is (in general, we take the MIN of the current
 *	  head position and all active backends' new tail pointers). Whenever we
 *	  move the global tail pointer we also truncate now-unused pages (i.e.,
 *	  delete files in pg_notify/ that are no longer used).
 *
 * An application that listens on the same channel it notifies will get
 * NOTIFY messages for its own NOTIFYs.  These can be ignored, if not useful,
 * by comparing be_pid in the NOTIFY message to the application's own backend's
 * PID.  (As of FE/BE protocol 2.0, the backend's PID is provided to the
 * frontend during startup.)  The above design guarantees that notifies from
 * other backends will never be missed by ignoring self-notifies.
 *
 * The amount of shared memory used for notify management (NUM_ASYNC_BUFFERS)
 * can be varied without affecting anything but performance.  The maximum
 * amount of notification data that can be queued at one time is determined
 * by slru.c's wraparound limit; see QUEUE_MAX_PAGE below.
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>
#include <unistd.h>
#include <signal.h>

#include "access/parallel.h"
#include "access/slru.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/pg_database.h"
#include "commands/async.h"
#include "funcapi.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/procsignal.h"
#include "storage/sinval.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/timestamp.h"


/*
 * Maximum size of a NOTIFY payload, including terminating NULL.  This
 * must be kept small enough so that a notification message fits on one
 * SLRU page.  The magic fudge factor here is noncritical as long as it's
 * more than AsyncQueueEntryEmptySize --- we make it significantly bigger
 * than that, so changes in that data structure won't affect user-visible
 * restrictions.
 */
#define NOTIFY_PAYLOAD_MAX_LENGTH	(BLCKSZ - NAMEDATALEN - 128)

/*
 * Struct representing an entry in the global notify queue
 *
 * This struct declaration has the maximal length, but in a real queue entry
 * the data area is only big enough for the actual channel and payload strings
 * (each null-terminated).  AsyncQueueEntryEmptySize is the minimum possible
 * entry size, if both channel and payload strings are empty (but note it
 * doesn't include alignment padding).
 *
 * The "length" field should always be rounded up to the next QUEUEALIGN
 * multiple so that all fields are properly aligned.
 */
typedef struct AsyncQueueEntry
{
	int			length;			/* total allocated length of entry */
	Oid			dboid;			/* sender's database OID */
	TransactionId xid;			/* sender's XID */
	int32		srcPid;			/* sender's PID */
	char		data[NAMEDATALEN + NOTIFY_PAYLOAD_MAX_LENGTH];
} AsyncQueueEntry;

/* Currently, no field of AsyncQueueEntry requires more than int alignment */
#define QUEUEALIGN(len)		INTALIGN(len)

#define AsyncQueueEntryEmptySize	(offsetof(AsyncQueueEntry, data) + 2)

/*
 * Struct describing a queue position, and assorted macros for working with it
 */
typedef struct QueuePosition
{
	int			page;			/* SLRU page number */
	int			offset;			/* byte offset within page */
} QueuePosition;

#define QUEUE_POS_PAGE(x)		((x).page)
#define QUEUE_POS_OFFSET(x)		((x).offset)

#define SET_QUEUE_POS(x,y,z) \
	do { \
		(x).page = (y); \
		(x).offset = (z); \
	} while (0)

#define QUEUE_POS_EQUAL(x,y) \
	 ((x).page == (y).page && (x).offset == (y).offset)

/* choose logically smaller QueuePosition */
#define QUEUE_POS_MIN(x,y) \
	(asyncQueuePagePrecedes((x).page, (y).page) ? (x) : \
	 (x).page != (y).page ? (y) : \
	 (x).offset < (y).offset ? (x) : (y))

/* choose logically larger QueuePosition */
#define QUEUE_POS_MAX(x,y) \
	(asyncQueuePagePrecedes((x).page, (y).page) ? (y) : \
	 (x).page != (y).page ? (x) : \
	 (x).offset > (y).offset ? (x) : (y))

/*
 * Struct describing a listening backend's status
 */
typedef struct QueueBackendStatus
{
	int32		pid;			/* either a PID or InvalidPid */
	Oid			dboid;			/* backend's database OID, or InvalidOid */
	QueuePosition pos;			/* backend has read queue up to here */
} QueueBackendStatus;

/*
 * Shared memory state for LISTEN/NOTIFY (excluding its SLRU stuff)
 *
 * The AsyncQueueControl structure is protected by the AsyncQueueLock.
 *
 * When holding the lock in SHARED mode, backends may only inspect their own
 * entries as well as the head and tail pointers. Consequently we can allow a
 * backend to update its own record while holding only SHARED lock (since no
 * other backend will inspect it).
 *
 * When holding the lock in EXCLUSIVE mode, backends can inspect the entries
 * of other backends and also change the head and tail pointers.
 *
 * AsyncCtlLock is used as the control lock for the pg_notify SLRU buffers.
 * In order to avoid deadlocks, whenever we need both locks, we always first
 * get AsyncQueueLock and then AsyncCtlLock.
 *
 * Each backend uses the backend[] array entry with index equal to its
 * BackendId (which can range from 1 to MaxBackends).  We rely on this to make
 * SendProcSignal fast.
 */
typedef struct AsyncQueueControl
{
	QueuePosition head;			/* head points to the next free location */
	QueuePosition tail;			/* the global tail is equivalent to the pos of
								 * the "slowest" backend */
	TimestampTz lastQueueFillWarn;		/* time of last queue-full msg */
	QueueBackendStatus backend[FLEXIBLE_ARRAY_MEMBER];
	/* backend[0] is not used; used entries are from [1] to [MaxBackends] */
} AsyncQueueControl;

static AsyncQueueControl *asyncQueueControl;

#define QUEUE_HEAD					(asyncQueueControl->head)
#define QUEUE_TAIL					(asyncQueueControl->tail)
#define QUEUE_BACKEND_PID(i)		(asyncQueueControl->backend[i].pid)
#define QUEUE_BACKEND_DBOID(i)		(asyncQueueControl->backend[i].dboid)
#define QUEUE_BACKEND_POS(i)		(asyncQueueControl->backend[i].pos)

/*
 * The SLRU buffer area through which we access the notification queue
 */
static SlruCtlData AsyncCtlData;

#define AsyncCtl					(&AsyncCtlData)
#define QUEUE_PAGESIZE				BLCKSZ
#define QUEUE_FULL_WARN_INTERVAL	5000		/* warn at most once every 5s */

/*
 * slru.c currently assumes that all filenames are four characters of hex
 * digits. That means that we can use segments 0000 through FFFF.
 * Each segment contains SLRU_PAGES_PER_SEGMENT pages which gives us
 * the pages from 0 to SLRU_PAGES_PER_SEGMENT * 0x10000 - 1.
 *
 * It's of course possible to enhance slru.c, but this gives us so much
 * space already that it doesn't seem worth the trouble.
 *
 * The most data we can have in the queue at a time is QUEUE_MAX_PAGE/2
 * pages, because more than that would confuse slru.c into thinking there
 * was a wraparound condition.  With the default BLCKSZ this means there
 * can be up to 8GB of queued-and-not-read data.
 *
 * Note: it's possible to redefine QUEUE_MAX_PAGE with a smaller multiple of
 * SLRU_PAGES_PER_SEGMENT, for easier testing of queue-full behaviour.
 */
#define QUEUE_MAX_PAGE			(SLRU_PAGES_PER_SEGMENT * 0x10000 - 1)

/*
 * listenChannels identifies the channels we are actually listening to
 * (ie, have committed a LISTEN on).  It is a simple list of channel names,
 * allocated in TopMemoryContext.
 */
static List *listenChannels = NIL;		/* list of C strings */

/*
 * State for pending LISTEN/UNLISTEN actions consists of an ordered list of
 * all actions requested in the current transaction.  As explained above,
 * we don't actually change listenChannels until we reach transaction commit.
 *
 * The list is kept in CurTransactionContext.  In subtransactions, each
 * subtransaction has its own list in its own CurTransactionContext, but
 * successful subtransactions attach their lists to their parent's list.
 * Failed subtransactions simply discard their lists.
 */
typedef enum
{
	LISTEN_LISTEN,
	LISTEN_UNLISTEN,
	LISTEN_UNLISTEN_ALL
} ListenActionKind;

typedef struct
{
	ListenActionKind action;
	char		channel[FLEXIBLE_ARRAY_MEMBER]; /* nul-terminated string */
} ListenAction;

static List *pendingActions = NIL;		/* list of ListenAction */

static List *upperPendingActions = NIL; /* list of upper-xact lists */

/*
 * State for outbound notifies consists of a list of all channels+payloads
 * NOTIFYed in the current transaction. We do not actually perform a NOTIFY
 * until and unless the transaction commits.  pendingNotifies is NIL if no
 * NOTIFYs have been done in the current transaction.
 *
 * The list is kept in CurTransactionContext.  In subtransactions, each
 * subtransaction has its own list in its own CurTransactionContext, but
 * successful subtransactions attach their lists to their parent's list.
 * Failed subtransactions simply discard their lists.
 *
 * Note: the action and notify lists do not interact within a transaction.
 * In particular, if a transaction does NOTIFY and then LISTEN on the same
 * condition name, it will get a self-notify at commit.  This is a bit odd
 * but is consistent with our historical behavior.
 */
typedef struct Notification
{
	char	   *channel;		/* channel name */
	char	   *payload;		/* payload string (can be empty) */
} Notification;

static List *pendingNotifies = NIL;		/* list of Notifications */

static List *upperPendingNotifies = NIL;		/* list of upper-xact lists */

/*
 * Inbound notifications are initially processed by HandleNotifyInterrupt(),
 * called from inside a signal handler. That just sets the
 * notifyInterruptPending flag and sets the process
 * latch. ProcessNotifyInterrupt() will then be called whenever it's safe to
 * actually deal with the interrupt.
 */
volatile sig_atomic_t notifyInterruptPending = false;

/* True if we've registered an on_shmem_exit cleanup */
static bool unlistenExitRegistered = false;

/* True if we're currently registered as a listener in asyncQueueControl */
static bool amRegisteredListener = false;

/* has this backend sent notifications in the current transaction? */
static bool backendHasSentNotifications = false;

/* GUC parameter */
bool		Trace_notify = false;

/* local function prototypes */
static bool asyncQueuePagePrecedes(int p, int q);
static void queue_listen(ListenActionKind action, const char *channel);
static void Async_UnlistenOnExit(int code, Datum arg);
static void Exec_ListenPreCommit(void);
static void Exec_ListenCommit(const char *channel);
static void Exec_UnlistenCommit(const char *channel);
static void Exec_UnlistenAllCommit(void);
static bool IsListeningOn(const char *channel);
static void asyncQueueUnregister(void);
static bool asyncQueueIsFull(void);
static bool asyncQueueAdvance(volatile QueuePosition *position, int entryLength);
static void asyncQueueNotificationToEntry(Notification *n, AsyncQueueEntry *qe);
static ListCell *asyncQueueAddEntries(ListCell *nextNotify);
static double asyncQueueUsage(void);
static void asyncQueueFillWarning(void);
static bool SignalBackends(void);
static void asyncQueueReadAllNotifications(void);
static bool asyncQueueProcessPageEntries(volatile QueuePosition *current,
							 QueuePosition stop,
							 char *page_buffer);
static void asyncQueueAdvanceTail(void);
static void ProcessIncomingNotify(void);
static void NotifyMyFrontEnd(const char *channel,
				 const char *payload,
				 int32 srcPid);
static bool AsyncExistsPendingNotify(const char *channel, const char *payload);
static void ClearPendingActionsAndNotifies(void);

/*
 * We will work on the page range of 0..QUEUE_MAX_PAGE.
 */
static bool
asyncQueuePagePrecedes(int p, int q)
{
	int			diff;

	/*
	 * We have to compare modulo (QUEUE_MAX_PAGE+1)/2.  Both inputs should be
	 * in the range 0..QUEUE_MAX_PAGE.
	 */
	Assert(p >= 0 && p <= QUEUE_MAX_PAGE);
	Assert(q >= 0 && q <= QUEUE_MAX_PAGE);

	diff = p - q;
	if (diff >= ((QUEUE_MAX_PAGE + 1) / 2))
		diff -= QUEUE_MAX_PAGE + 1;
	else if (diff < -((QUEUE_MAX_PAGE + 1) / 2))
		diff += QUEUE_MAX_PAGE + 1;
	return diff < 0;
}

/*
 * Report space needed for our shared memory area
 */
Size
AsyncShmemSize(void)
{
	Size		size;

	/* This had better match AsyncShmemInit */
	size = mul_size(MaxBackends + 1, sizeof(QueueBackendStatus));
	size = add_size(size, offsetof(AsyncQueueControl, backend));

	size = add_size(size, SimpleLruShmemSize(NUM_ASYNC_BUFFERS, 0));

	return size;
}

/*
 * Initialize our shared memory area
 */
void
AsyncShmemInit(void)
{
	bool		found;
	int			slotno;
	Size		size;

	/*
	 * Create or attach to the AsyncQueueControl structure.
	 *
	 * The used entries in the backend[] array run from 1 to MaxBackends; the
	 * zero'th entry is unused but must be allocated.
	 */
	size = mul_size(MaxBackends + 1, sizeof(QueueBackendStatus));
	size = add_size(size, offsetof(AsyncQueueControl, backend));

	asyncQueueControl = (AsyncQueueControl *)
		ShmemInitStruct("Async Queue Control", size, &found);

	if (!found)
	{
		/* First time through, so initialize it */
		int			i;

		SET_QUEUE_POS(QUEUE_HEAD, 0, 0);
		SET_QUEUE_POS(QUEUE_TAIL, 0, 0);
		asyncQueueControl->lastQueueFillWarn = 0;
		/* zero'th entry won't be used, but let's initialize it anyway */
		for (i = 0; i <= MaxBackends; i++)
		{
			QUEUE_BACKEND_PID(i) = InvalidPid;
			QUEUE_BACKEND_DBOID(i) = InvalidOid;
			SET_QUEUE_POS(QUEUE_BACKEND_POS(i), 0, 0);
		}
	}

	/*
	 * Set up SLRU management of the pg_notify data.
	 */
	AsyncCtl->PagePrecedes = asyncQueuePagePrecedes;
	SimpleLruInit(AsyncCtl, "async", NUM_ASYNC_BUFFERS, 0,
				  AsyncCtlLock, "pg_notify");
	/* Override default assumption that writes should be fsync'd */
	AsyncCtl->do_fsync = false;

	if (!found)
	{
		/*
		 * During start or reboot, clean out the pg_notify directory.
		 */
		(void) SlruScanDirectory(AsyncCtl, SlruScanDirCbDeleteAll, NULL);

		/* Now initialize page zero to empty */
		LWLockAcquire(AsyncCtlLock, LW_EXCLUSIVE);
		slotno = SimpleLruZeroPage(AsyncCtl, QUEUE_POS_PAGE(QUEUE_HEAD));
		/* This write is just to verify that pg_notify/ is writable */
		SimpleLruWritePage(AsyncCtl, slotno);
		LWLockRelease(AsyncCtlLock);
	}
}


/*
 * pg_notify -
 *	  SQL function to send a notification event
 */
Datum
pg_notify(PG_FUNCTION_ARGS)
{
	const char *channel;
	const char *payload;

	if (PG_ARGISNULL(0))
		channel = "";
	else
		channel = text_to_cstring(PG_GETARG_TEXT_PP(0));

	if (PG_ARGISNULL(1))
		payload = "";
	else
		payload = text_to_cstring(PG_GETARG_TEXT_PP(1));

	/* For NOTIFY as a statement, this is checked in ProcessUtility */
	PreventCommandDuringRecovery("NOTIFY");

	Async_Notify(channel, payload);

	PG_RETURN_VOID();
}


/*
 * Async_Notify
 *
 *		This is executed by the SQL notify command.
 *
 *		Adds the message to the list of pending notifies.
 *		Actual notification happens during transaction commit.
 *		^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
 */
void
Async_Notify(const char *channel, const char *payload)
{
	Notification *n;
	MemoryContext oldcontext;

	if (IsParallelWorker())
		elog(ERROR, "cannot send notifications from a parallel worker");

	if (Trace_notify)
		elog(DEBUG1, "Async_Notify(%s)", channel);

	/* a channel name must be specified */
	if (!channel || !strlen(channel))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("channel name cannot be empty")));

	if (strlen(channel) >= NAMEDATALEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("channel name too long")));

	if (payload)
	{
		if (strlen(payload) >= NOTIFY_PAYLOAD_MAX_LENGTH)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("payload string too long")));
	}

	/* no point in making duplicate entries in the list ... */
	if (AsyncExistsPendingNotify(channel, payload))
		return;

	/*
	 * The notification list needs to live until end of transaction, so store
	 * it in the transaction context.
	 */
	oldcontext = MemoryContextSwitchTo(CurTransactionContext);

	n = (Notification *) palloc(sizeof(Notification));
	n->channel = pstrdup(channel);
	if (payload)
		n->payload = pstrdup(payload);
	else
		n->payload = "";

	/*
	 * We want to preserve the order so we need to append every notification.
	 * See comments at AsyncExistsPendingNotify().
	 */
	pendingNotifies = lappend(pendingNotifies, n);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * queue_listen
 *		Common code for listen, unlisten, unlisten all commands.
 *
 *		Adds the request to the list of pending actions.
 *		Actual update of the listenChannels list happens during transaction
 *		commit.
 */
static void
queue_listen(ListenActionKind action, const char *channel)
{
	MemoryContext oldcontext;
	ListenAction *actrec;

	/*
	 * Unlike Async_Notify, we don't try to collapse out duplicates. It would
	 * be too complicated to ensure we get the right interactions of
	 * conflicting LISTEN/UNLISTEN/UNLISTEN_ALL, and it's unlikely that there
	 * would be any performance benefit anyway in sane applications.
	 */
	oldcontext = MemoryContextSwitchTo(CurTransactionContext);

	/* space for terminating null is included in sizeof(ListenAction) */
	actrec = (ListenAction *) palloc(offsetof(ListenAction, channel) +
									 strlen(channel) + 1);
	actrec->action = action;
	strcpy(actrec->channel, channel);

	pendingActions = lappend(pendingActions, actrec);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * Async_Listen
 *
 *		This is executed by the SQL listen command.
 */
void
Async_Listen(const char *channel)
{
	if (Trace_notify)
		elog(DEBUG1, "Async_Listen(%s,%d)", channel, MyProcPid);

	queue_listen(LISTEN_LISTEN, channel);
}

/*
 * Async_Unlisten
 *
 *		This is executed by the SQL unlisten command.
 */
void
Async_Unlisten(const char *channel)
{
	if (Trace_notify)
		elog(DEBUG1, "Async_Unlisten(%s,%d)", channel, MyProcPid);

	/* If we couldn't possibly be listening, no need to queue anything */
	if (pendingActions == NIL && !unlistenExitRegistered)
		return;

	queue_listen(LISTEN_UNLISTEN, channel);
}

/*
 * Async_UnlistenAll
 *
 *		This is invoked by UNLISTEN * command, and also at backend exit.
 */
void
Async_UnlistenAll(void)
{
	if (Trace_notify)
		elog(DEBUG1, "Async_UnlistenAll(%d)", MyProcPid);

	/* If we couldn't possibly be listening, no need to queue anything */
	if (pendingActions == NIL && !unlistenExitRegistered)
		return;

	queue_listen(LISTEN_UNLISTEN_ALL, "");
}

/*
 * SQL function: return a set of the channel names this backend is actively
 * listening to.
 *
 * Note: this coding relies on the fact that the listenChannels list cannot
 * change within a transaction.
 */
Datum
pg_listening_channels(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	ListCell  **lcp;

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/* switch to memory context appropriate for multiple function calls */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* allocate memory for user context */
		lcp = (ListCell **) palloc(sizeof(ListCell *));
		*lcp = list_head(listenChannels);
		funcctx->user_fctx = (void *) lcp;

		MemoryContextSwitchTo(oldcontext);
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();
	lcp = (ListCell **) funcctx->user_fctx;

	while (*lcp != NULL)
	{
		char	   *channel = (char *) lfirst(*lcp);

		*lcp = lnext(*lcp);
		SRF_RETURN_NEXT(funcctx, CStringGetTextDatum(channel));
	}

	SRF_RETURN_DONE(funcctx);
}

/*
 * Async_UnlistenOnExit
 *
 * This is executed at backend exit if we have done any LISTENs in this
 * backend.  It might not be necessary anymore, if the user UNLISTENed
 * everything, but we don't try to detect that case.
 */
static void
Async_UnlistenOnExit(int code, Datum arg)
{
	Exec_UnlistenAllCommit();
	asyncQueueUnregister();
}

/*
 * AtPrepare_Notify
 *
 *		This is called at the prepare phase of a two-phase
 *		transaction.  Save the state for possible commit later.
 */
void
AtPrepare_Notify(void)
{
	/* It's not allowed to have any pending LISTEN/UNLISTEN/NOTIFY actions */
	if (pendingActions || pendingNotifies)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot PREPARE a transaction that has executed LISTEN, UNLISTEN, or NOTIFY")));
}

/*
 * PreCommit_Notify
 *
 *		This is called at transaction commit, before actually committing to
 *		clog.
 *
 *		If there are pending LISTEN actions, make sure we are listed in the
 *		shared-memory listener array.  This must happen before commit to
 *		ensure we don't miss any notifies from transactions that commit
 *		just after ours.
 *
 *		If there are outbound notify requests in the pendingNotifies list,
 *		add them to the global queue.  We do that before commit so that
 *		we can still throw error if we run out of queue space.
 */
void
PreCommit_Notify(void)
{
	ListCell   *p;

	if (pendingActions == NIL && pendingNotifies == NIL)
		return;					/* no relevant statements in this xact */

	if (Trace_notify)
		elog(DEBUG1, "PreCommit_Notify");

	/* Preflight for any pending listen/unlisten actions */
	foreach(p, pendingActions)
	{
		ListenAction *actrec = (ListenAction *) lfirst(p);

		switch (actrec->action)
		{
			case LISTEN_LISTEN:
				Exec_ListenPreCommit();
				break;
			case LISTEN_UNLISTEN:
				/* there is no Exec_UnlistenPreCommit() */
				break;
			case LISTEN_UNLISTEN_ALL:
				/* there is no Exec_UnlistenAllPreCommit() */
				break;
		}
	}

	/* Queue any pending notifies */
	if (pendingNotifies)
	{
		ListCell   *nextNotify;

		/*
		 * Make sure that we have an XID assigned to the current transaction.
		 * GetCurrentTransactionId is cheap if we already have an XID, but not
		 * so cheap if we don't, and we'd prefer not to do that work while
		 * holding AsyncQueueLock.
		 */
		(void) GetCurrentTransactionId();

		/*
		 * Serialize writers by acquiring a special lock that we hold till
		 * after commit.  This ensures that queue entries appear in commit
		 * order, and in particular that there are never uncommitted queue
		 * entries ahead of committed ones, so an uncommitted transaction
		 * can't block delivery of deliverable notifications.
		 *
		 * We use a heavyweight lock so that it'll automatically be released
		 * after either commit or abort.  This also allows deadlocks to be
		 * detected, though really a deadlock shouldn't be possible here.
		 *
		 * The lock is on "database 0", which is pretty ugly but it doesn't
		 * seem worth inventing a special locktag category just for this.
		 * (Historical note: before PG 9.0, a similar lock on "database 0" was
		 * used by the flatfiles mechanism.)
		 */
		LockSharedObject(DatabaseRelationId, InvalidOid, 0,
						 AccessExclusiveLock);

		/* Now push the notifications into the queue */
		backendHasSentNotifications = true;

		nextNotify = list_head(pendingNotifies);
		while (nextNotify != NULL)
		{
			/*
			 * Add the pending notifications to the queue.  We acquire and
			 * release AsyncQueueLock once per page, which might be overkill
			 * but it does allow readers to get in while we're doing this.
			 *
			 * A full queue is very uncommon and should really not happen,
			 * given that we have so much space available in the SLRU pages.
			 * Nevertheless we need to deal with this possibility. Note that
			 * when we get here we are in the process of committing our
			 * transaction, but we have not yet committed to clog, so at this
			 * point in time we can still roll the transaction back.
			 */
			LWLockAcquire(AsyncQueueLock, LW_EXCLUSIVE);
			asyncQueueFillWarning();
			if (asyncQueueIsFull())
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					  errmsg("too many notifications in the NOTIFY queue")));
			nextNotify = asyncQueueAddEntries(nextNotify);
			LWLockRelease(AsyncQueueLock);
		}
	}
}

/*
 * AtCommit_Notify
 *
 *		This is called at transaction commit, after committing to clog.
 *
 *		Update listenChannels and clear transaction-local state.
 */
void
AtCommit_Notify(void)
{
	ListCell   *p;

	/*
	 * Allow transactions that have not executed LISTEN/UNLISTEN/NOTIFY to
	 * return as soon as possible
	 */
	if (!pendingActions && !pendingNotifies)
		return;

	if (Trace_notify)
		elog(DEBUG1, "AtCommit_Notify");

	/* Perform any pending listen/unlisten actions */
	foreach(p, pendingActions)
	{
		ListenAction *actrec = (ListenAction *) lfirst(p);

		switch (actrec->action)
		{
			case LISTEN_LISTEN:
				Exec_ListenCommit(actrec->channel);
				break;
			case LISTEN_UNLISTEN:
				Exec_UnlistenCommit(actrec->channel);
				break;
			case LISTEN_UNLISTEN_ALL:
				Exec_UnlistenAllCommit();
				break;
		}
	}

	/* If no longer listening to anything, get out of listener array */
	if (amRegisteredListener && listenChannels == NIL)
		asyncQueueUnregister();

	/* And clean up */
	ClearPendingActionsAndNotifies();
}

/*
 * Exec_ListenPreCommit --- subroutine for PreCommit_Notify
 *
 * This function must make sure we are ready to catch any incoming messages.
 */
static void
Exec_ListenPreCommit(void)
{
	QueuePosition head;
	QueuePosition max;
	int			i;

	/*
	 * Nothing to do if we are already listening to something, nor if we
	 * already ran this routine in this transaction.
	 */
	if (amRegisteredListener)
		return;

	if (Trace_notify)
		elog(DEBUG1, "Exec_ListenPreCommit(%d)", MyProcPid);

	/*
	 * Before registering, make sure we will unlisten before dying. (Note:
	 * this action does not get undone if we abort later.)
	 */
	if (!unlistenExitRegistered)
	{
		before_shmem_exit(Async_UnlistenOnExit, 0);
		unlistenExitRegistered = true;
	}

	/*
	 * This is our first LISTEN, so establish our pointer.
	 *
	 * We set our pointer to the global tail pointer and then move it forward
	 * over already-committed notifications.  This ensures we cannot miss any
	 * not-yet-committed notifications.  We might get a few more but that
	 * doesn't hurt.
	 *
	 * In some scenarios there might be a lot of committed notifications that
	 * have not yet been pruned away (because some backend is being lazy about
	 * reading them).  To reduce our startup time, we can look at other
	 * backends and adopt the maximum "pos" pointer of any backend that's in
	 * our database; any notifications it's already advanced over are surely
	 * committed and need not be re-examined by us.  (We must consider only
	 * backends connected to our DB, because others will not have bothered to
	 * check committed-ness of notifications in our DB.)  But we only bother
	 * with that if there's more than a page worth of notifications
	 * outstanding, otherwise scanning all the other backends isn't worth it.
	 *
	 * We need exclusive lock here so we can look at other backends' entries.
	 */
	LWLockAcquire(AsyncQueueLock, LW_EXCLUSIVE);
	head = QUEUE_HEAD;
	max = QUEUE_TAIL;
	if (QUEUE_POS_PAGE(max) != QUEUE_POS_PAGE(head))
	{
		for (i = 1; i <= MaxBackends; i++)
		{
			if (QUEUE_BACKEND_DBOID(i) == MyDatabaseId)
				max = QUEUE_POS_MAX(max, QUEUE_BACKEND_POS(i));
		}
	}
	QUEUE_BACKEND_POS(MyBackendId) = max;
	QUEUE_BACKEND_PID(MyBackendId) = MyProcPid;
	QUEUE_BACKEND_DBOID(MyBackendId) = MyDatabaseId;
	LWLockRelease(AsyncQueueLock);

	/* Now we are listed in the global array, so remember we're listening */
	amRegisteredListener = true;

	/*
	 * Try to move our pointer forward as far as possible. This will skip over
	 * already-committed notifications. Still, we could get notifications that
	 * have already committed before we started to LISTEN.
	 *
	 * Note that we are not yet listening on anything, so we won't deliver any
	 * notification to the frontend.
	 *
	 * This will also advance the global tail pointer if possible.
	 */
	if (!QUEUE_POS_EQUAL(max, head))
		asyncQueueReadAllNotifications();
}

/*
 * Exec_ListenCommit --- subroutine for AtCommit_Notify
 *
 * Add the channel to the list of channels we are listening on.
 */
static void
Exec_ListenCommit(const char *channel)
{
	MemoryContext oldcontext;

	/* Do nothing if we are already listening on this channel */
	if (IsListeningOn(channel))
		return;

	/*
	 * Add the new channel name to listenChannels.
	 *
	 * XXX It is theoretically possible to get an out-of-memory failure here,
	 * which would be bad because we already committed.  For the moment it
	 * doesn't seem worth trying to guard against that, but maybe improve this
	 * later.
	 */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
	listenChannels = lappend(listenChannels, pstrdup(channel));
	MemoryContextSwitchTo(oldcontext);
}

/*
 * Exec_UnlistenCommit --- subroutine for AtCommit_Notify
 *
 * Remove the specified channel name from listenChannels.
 */
static void
Exec_UnlistenCommit(const char *channel)
{
	ListCell   *q;
	ListCell   *prev;

	if (Trace_notify)
		elog(DEBUG1, "Exec_UnlistenCommit(%s,%d)", channel, MyProcPid);

	prev = NULL;
	foreach(q, listenChannels)
	{
		char	   *lchan = (char *) lfirst(q);

		if (strcmp(lchan, channel) == 0)
		{
			listenChannels = list_delete_cell(listenChannels, q, prev);
			pfree(lchan);
			break;
		}
		prev = q;
	}

	/*
	 * We do not complain about unlistening something not being listened;
	 * should we?
	 */
}

/*
 * Exec_UnlistenAllCommit --- subroutine for AtCommit_Notify
 *
 *		Unlisten on all channels for this backend.
 */
static void
Exec_UnlistenAllCommit(void)
{
	if (Trace_notify)
		elog(DEBUG1, "Exec_UnlistenAllCommit(%d)", MyProcPid);

	list_free_deep(listenChannels);
	listenChannels = NIL;
}

/*
 * ProcessCompletedNotifies --- send out signals and self-notifies
 *
 * This is called from postgres.c just before going idle at the completion
 * of a transaction.  If we issued any notifications in the just-completed
 * transaction, send signals to other backends to process them, and also
 * process the queue ourselves to send messages to our own frontend.
 *
 * The reason that this is not done in AtCommit_Notify is that there is
 * a nonzero chance of errors here (for example, encoding conversion errors
 * while trying to format messages to our frontend).  An error during
 * AtCommit_Notify would be a PANIC condition.  The timing is also arranged
 * to ensure that a transaction's self-notifies are delivered to the frontend
 * before it gets the terminating ReadyForQuery message.
 *
 * Note that we send signals and process the queue even if the transaction
 * eventually aborted.  This is because we need to clean out whatever got
 * added to the queue.
 *
 * NOTE: we are outside of any transaction here.
 */
void
ProcessCompletedNotifies(void)
{
	MemoryContext caller_context;
	bool		signalled;

	/* Nothing to do if we didn't send any notifications */
	if (!backendHasSentNotifications)
		return;

	/*
	 * We reset the flag immediately; otherwise, if any sort of error occurs
	 * below, we'd be locked up in an infinite loop, because control will come
	 * right back here after error cleanup.
	 */
	backendHasSentNotifications = false;

	/*
	 * We must preserve the caller's memory context (probably MessageContext)
	 * across the transaction we do here.
	 */
	caller_context = CurrentMemoryContext;

	if (Trace_notify)
		elog(DEBUG1, "ProcessCompletedNotifies");

	/*
	 * We must run asyncQueueReadAllNotifications inside a transaction, else
	 * bad things happen if it gets an error.
	 */
	StartTransactionCommand();

	/* Send signals to other backends */
	signalled = SignalBackends();

	if (listenChannels != NIL)
	{
		/* Read the queue ourselves, and send relevant stuff to the frontend */
		asyncQueueReadAllNotifications();
	}
	else if (!signalled)
	{
		/*
		 * If we found no other listening backends, and we aren't listening
		 * ourselves, then we must execute asyncQueueAdvanceTail to flush the
		 * queue, because ain't nobody else gonna do it.  This prevents queue
		 * overflow when we're sending useless notifies to nobody. (A new
		 * listener could have joined since we looked, but if so this is
		 * harmless.)
		 */
		asyncQueueAdvanceTail();
	}

	CommitTransactionCommand();

	MemoryContextSwitchTo(caller_context);

	/* We don't need pq_flush() here since postgres.c will do one shortly */
}

/*
 * Test whether we are actively listening on the given channel name.
 *
 * Note: this function is executed for every notification found in the queue.
 * Perhaps it is worth further optimization, eg convert the list to a sorted
 * array so we can binary-search it.  In practice the list is likely to be
 * fairly short, though.
 */
static bool
IsListeningOn(const char *channel)
{
	ListCell   *p;

	foreach(p, listenChannels)
	{
		char	   *lchan = (char *) lfirst(p);

		if (strcmp(lchan, channel) == 0)
			return true;
	}
	return false;
}

/*
 * Remove our entry from the listeners array when we are no longer listening
 * on any channel.  NB: must not fail if we're already not listening.
 */
static void
asyncQueueUnregister(void)
{
	bool		advanceTail;

	Assert(listenChannels == NIL);		/* else caller error */

	if (!amRegisteredListener)	/* nothing to do */
		return;

	LWLockAcquire(AsyncQueueLock, LW_SHARED);
	/* check if entry is valid and oldest ... */
	advanceTail = (MyProcPid == QUEUE_BACKEND_PID(MyBackendId)) &&
		QUEUE_POS_EQUAL(QUEUE_BACKEND_POS(MyBackendId), QUEUE_TAIL);
	/* ... then mark it invalid */
	QUEUE_BACKEND_PID(MyBackendId) = InvalidPid;
	QUEUE_BACKEND_DBOID(MyBackendId) = InvalidOid;
	LWLockRelease(AsyncQueueLock);

	/* mark ourselves as no longer listed in the global array */
	amRegisteredListener = false;

	/* If we were the laziest backend, try to advance the tail pointer */
	if (advanceTail)
		asyncQueueAdvanceTail();
}

/*
 * Test whether there is room to insert more notification messages.
 *
 * Caller must hold at least shared AsyncQueueLock.
 */
static bool
asyncQueueIsFull(void)
{
	int			nexthead;
	int			boundary;

	/*
	 * The queue is full if creating a new head page would create a page that
	 * logically precedes the current global tail pointer, ie, the head
	 * pointer would wrap around compared to the tail.  We cannot create such
	 * a head page for fear of confusing slru.c.  For safety we round the tail
	 * pointer back to a segment boundary (compare the truncation logic in
	 * asyncQueueAdvanceTail).
	 *
	 * Note that this test is *not* dependent on how much space there is on
	 * the current head page.  This is necessary because asyncQueueAddEntries
	 * might try to create the next head page in any case.
	 */
	nexthead = QUEUE_POS_PAGE(QUEUE_HEAD) + 1;
	if (nexthead > QUEUE_MAX_PAGE)
		nexthead = 0;			/* wrap around */
	boundary = QUEUE_POS_PAGE(QUEUE_TAIL);
	boundary -= boundary % SLRU_PAGES_PER_SEGMENT;
	return asyncQueuePagePrecedes(nexthead, boundary);
}

/*
 * Advance the QueuePosition to the next entry, assuming that the current
 * entry is of length entryLength.  If we jump to a new page the function
 * returns true, else false.
 */
static bool
asyncQueueAdvance(volatile QueuePosition *position, int entryLength)
{
	int			pageno = QUEUE_POS_PAGE(*position);
	int			offset = QUEUE_POS_OFFSET(*position);
	bool		pageJump = false;

	/*
	 * Move to the next writing position: First jump over what we have just
	 * written or read.
	 */
	offset += entryLength;
	Assert(offset <= QUEUE_PAGESIZE);

	/*
	 * In a second step check if another entry can possibly be written to the
	 * page. If so, stay here, we have reached the next position. If not, then
	 * we need to move on to the next page.
	 */
	if (offset + QUEUEALIGN(AsyncQueueEntryEmptySize) > QUEUE_PAGESIZE)
	{
		pageno++;
		if (pageno > QUEUE_MAX_PAGE)
			pageno = 0;			/* wrap around */
		offset = 0;
		pageJump = true;
	}

	SET_QUEUE_POS(*position, pageno, offset);
	return pageJump;
}

/*
 * Fill the AsyncQueueEntry at *qe with an outbound notification message.
 */
static void
asyncQueueNotificationToEntry(Notification *n, AsyncQueueEntry *qe)
{
	size_t		channellen = strlen(n->channel);
	size_t		payloadlen = strlen(n->payload);
	int			entryLength;

	Assert(channellen < NAMEDATALEN);
	Assert(payloadlen < NOTIFY_PAYLOAD_MAX_LENGTH);

	/* The terminators are already included in AsyncQueueEntryEmptySize */
	entryLength = AsyncQueueEntryEmptySize + payloadlen + channellen;
	entryLength = QUEUEALIGN(entryLength);
	qe->length = entryLength;
	qe->dboid = MyDatabaseId;
	qe->xid = GetCurrentTransactionId();
	qe->srcPid = MyProcPid;
	memcpy(qe->data, n->channel, channellen + 1);
	memcpy(qe->data + channellen + 1, n->payload, payloadlen + 1);
}

/*
 * Add pending notifications to the queue.
 *
 * We go page by page here, i.e. we stop once we have to go to a new page but
 * we will be called again and then fill that next page. If an entry does not
 * fit into the current page, we write a dummy entry with an InvalidOid as the
 * database OID in order to fill the page. So every page is always used up to
 * the last byte which simplifies reading the page later.
 *
 * We are passed the list cell containing the next notification to write
 * and return the first still-unwritten cell back.  Eventually we will return
 * NULL indicating all is done.
 *
 * We are holding AsyncQueueLock already from the caller and grab AsyncCtlLock
 * locally in this function.
 */
static ListCell *
asyncQueueAddEntries(ListCell *nextNotify)
{
	AsyncQueueEntry qe;
	QueuePosition queue_head;
	int			pageno;
	int			offset;
	int			slotno;

	/* We hold both AsyncQueueLock and AsyncCtlLock during this operation */
	LWLockAcquire(AsyncCtlLock, LW_EXCLUSIVE);

	/*
	 * We work with a local copy of QUEUE_HEAD, which we write back to shared
	 * memory upon exiting.  The reason for this is that if we have to advance
	 * to a new page, SimpleLruZeroPage might fail (out of disk space, for
	 * instance), and we must not advance QUEUE_HEAD if it does.  (Otherwise,
	 * subsequent insertions would try to put entries into a page that slru.c
	 * thinks doesn't exist yet.)  So, use a local position variable.  Note
	 * that if we do fail, any already-inserted queue entries are forgotten;
	 * this is okay, since they'd be useless anyway after our transaction
	 * rolls back.
	 */
	queue_head = QUEUE_HEAD;

	/* Fetch the current page */
	pageno = QUEUE_POS_PAGE(queue_head);
	slotno = SimpleLruReadPage(AsyncCtl, pageno, true, InvalidTransactionId);
	/* Note we mark the page dirty before writing in it */
	AsyncCtl->shared->page_dirty[slotno] = true;

	while (nextNotify != NULL)
	{
		Notification *n = (Notification *) lfirst(nextNotify);

		/* Construct a valid queue entry in local variable qe */
		asyncQueueNotificationToEntry(n, &qe);

		offset = QUEUE_POS_OFFSET(queue_head);

		/* Check whether the entry really fits on the current page */
		if (offset + qe.length <= QUEUE_PAGESIZE)
		{
			/* OK, so advance nextNotify past this item */
			nextNotify = lnext(nextNotify);
		}
		else
		{
			/*
			 * Write a dummy entry to fill up the page. Actually readers will
			 * only check dboid and since it won't match any reader's database
			 * OID, they will ignore this entry and move on.
			 */
			qe.length = QUEUE_PAGESIZE - offset;
			qe.dboid = InvalidOid;
			qe.data[0] = '\0';	/* empty channel */
			qe.data[1] = '\0';	/* empty payload */
		}

		/* Now copy qe into the shared buffer page */
		memcpy(AsyncCtl->shared->page_buffer[slotno] + offset,
			   &qe,
			   qe.length);

		/* Advance queue_head appropriately, and detect if page is full */
		if (asyncQueueAdvance(&(queue_head), qe.length))
		{
			/*
			 * Page is full, so we're done here, but first fill the next page
			 * with zeroes.  The reason to do this is to ensure that slru.c's
			 * idea of the head page is always the same as ours, which avoids
			 * boundary problems in SimpleLruTruncate.  The test in
			 * asyncQueueIsFull() ensured that there is room to create this
			 * page without overrunning the queue.
			 */
			slotno = SimpleLruZeroPage(AsyncCtl, QUEUE_POS_PAGE(queue_head));
			/* And exit the loop */
			break;
		}
	}

	/* Success, so update the global QUEUE_HEAD */
	QUEUE_HEAD = queue_head;

	LWLockRelease(AsyncCtlLock);

	return nextNotify;
}

/*
 * SQL function to return the fraction of the notification queue currently
 * occupied.
 */
Datum
pg_notification_queue_usage(PG_FUNCTION_ARGS)
{
	double		usage;

	LWLockAcquire(AsyncQueueLock, LW_SHARED);
	usage = asyncQueueUsage();
	LWLockRelease(AsyncQueueLock);

	PG_RETURN_FLOAT8(usage);
}

/*
 * Return the fraction of the queue that is currently occupied.
 *
 * The caller must hold AsyncQueueLock in (at least) shared mode.
 */
static double
asyncQueueUsage(void)
{
	int			headPage = QUEUE_POS_PAGE(QUEUE_HEAD);
	int			tailPage = QUEUE_POS_PAGE(QUEUE_TAIL);
	int			occupied;

	occupied = headPage - tailPage;

	if (occupied == 0)
		return (double) 0;		/* fast exit for common case */

	if (occupied < 0)
	{
		/* head has wrapped around, tail not yet */
		occupied += QUEUE_MAX_PAGE + 1;
	}

	return (double) occupied / (double) ((QUEUE_MAX_PAGE + 1) / 2);
}

/*
 * Check whether the queue is at least half full, and emit a warning if so.
 *
 * This is unlikely given the size of the queue, but possible.
 * The warnings show up at most once every QUEUE_FULL_WARN_INTERVAL.
 *
 * Caller must hold exclusive AsyncQueueLock.
 */
static void
asyncQueueFillWarning(void)
{
	double		fillDegree;
	TimestampTz t;

	fillDegree = asyncQueueUsage();
	if (fillDegree < 0.5)
		return;

	t = GetCurrentTimestamp();

	if (TimestampDifferenceExceeds(asyncQueueControl->lastQueueFillWarn,
								   t, QUEUE_FULL_WARN_INTERVAL))
	{
		QueuePosition min = QUEUE_HEAD;
		int32		minPid = InvalidPid;
		int			i;

		for (i = 1; i <= MaxBackends; i++)
		{
			if (QUEUE_BACKEND_PID(i) != InvalidPid)
			{
				min = QUEUE_POS_MIN(min, QUEUE_BACKEND_POS(i));
				if (QUEUE_POS_EQUAL(min, QUEUE_BACKEND_POS(i)))
					minPid = QUEUE_BACKEND_PID(i);
			}
		}

		ereport(WARNING,
				(errmsg("NOTIFY queue is %.0f%% full", fillDegree * 100),
				 (minPid != InvalidPid ?
				  errdetail("The server process with PID %d is among those with the oldest transactions.", minPid)
				  : 0),
				 (minPid != InvalidPid ?
				  errhint("The NOTIFY queue cannot be emptied until that process ends its current transaction.")
				  : 0)));

		asyncQueueControl->lastQueueFillWarn = t;
	}
}

/*
 * Send signals to all listening backends (except our own).
 *
 * Returns true if we sent at least one signal.
 *
 * Since we need EXCLUSIVE lock anyway we also check the position of the other
 * backends and in case one is already up-to-date we don't signal it.
 * This can happen if concurrent notifying transactions have sent a signal and
 * the signaled backend has read the other notifications and ours in the same
 * step.
 *
 * Since we know the BackendId and the Pid the signalling is quite cheap.
 */
static bool
SignalBackends(void)
{
	bool		signalled = false;
	int32	   *pids;
	BackendId  *ids;
	int			count;
	int			i;
	int32		pid;

	/*
	 * Identify all backends that are listening and not already up-to-date. We
	 * don't want to send signals while holding the AsyncQueueLock, so we just
	 * build a list of target PIDs.
	 *
	 * XXX in principle these pallocs could fail, which would be bad. Maybe
	 * preallocate the arrays?	But in practice this is only run in trivial
	 * transactions, so there should surely be space available.
	 */
	pids = (int32 *) palloc(MaxBackends * sizeof(int32));
	ids = (BackendId *) palloc(MaxBackends * sizeof(BackendId));
	count = 0;

	LWLockAcquire(AsyncQueueLock, LW_EXCLUSIVE);
	for (i = 1; i <= MaxBackends; i++)
	{
		pid = QUEUE_BACKEND_PID(i);
		if (pid != InvalidPid && pid != MyProcPid)
		{
			QueuePosition pos = QUEUE_BACKEND_POS(i);

			if (!QUEUE_POS_EQUAL(pos, QUEUE_HEAD))
			{
				pids[count] = pid;
				ids[count] = i;
				count++;
			}
		}
	}
	LWLockRelease(AsyncQueueLock);

	/* Now send signals */
	for (i = 0; i < count; i++)
	{
		pid = pids[i];

		/*
		 * Note: assuming things aren't broken, a signal failure here could
		 * only occur if the target backend exited since we released
		 * AsyncQueueLock; which is unlikely but certainly possible. So we
		 * just log a low-level debug message if it happens.
		 */
		if (SendProcSignal(pid, PROCSIG_NOTIFY_INTERRUPT, ids[i]) < 0)
			elog(DEBUG3, "could not signal backend with PID %d: %m", pid);
		else
			signalled = true;
	}

	pfree(pids);
	pfree(ids);

	return signalled;
}

/*
 * AtAbort_Notify
 *
 *	This is called at transaction abort.
 *
 *	Gets rid of pending actions and outbound notifies that we would have
 *	executed if the transaction got committed.
 */
void
AtAbort_Notify(void)
{
	/*
	 * If we LISTEN but then roll back the transaction after PreCommit_Notify,
	 * we have registered as a listener but have not made any entry in
	 * listenChannels.  In that case, deregister again.
	 */
	if (amRegisteredListener && listenChannels == NIL)
		asyncQueueUnregister();

	/* And clean up */
	ClearPendingActionsAndNotifies();
}

/*
 * AtSubStart_Notify() --- Take care of subtransaction start.
 *
 * Push empty state for the new subtransaction.
 */
void
AtSubStart_Notify(void)
{
	MemoryContext old_cxt;

	/* Keep the list-of-lists in TopTransactionContext for simplicity */
	old_cxt = MemoryContextSwitchTo(TopTransactionContext);

	upperPendingActions = lcons(pendingActions, upperPendingActions);

	Assert(list_length(upperPendingActions) ==
		   GetCurrentTransactionNestLevel() - 1);

	pendingActions = NIL;

	upperPendingNotifies = lcons(pendingNotifies, upperPendingNotifies);

	Assert(list_length(upperPendingNotifies) ==
		   GetCurrentTransactionNestLevel() - 1);

	pendingNotifies = NIL;

	MemoryContextSwitchTo(old_cxt);
}

/*
 * AtSubCommit_Notify() --- Take care of subtransaction commit.
 *
 * Reassign all items in the pending lists to the parent transaction.
 */
void
AtSubCommit_Notify(void)
{
	List	   *parentPendingActions;
	List	   *parentPendingNotifies;

	parentPendingActions = (List *) linitial(upperPendingActions);
	upperPendingActions = list_delete_first(upperPendingActions);

	Assert(list_length(upperPendingActions) ==
		   GetCurrentTransactionNestLevel() - 2);

	/*
	 * Mustn't try to eliminate duplicates here --- see queue_listen()
	 */
	pendingActions = list_concat(parentPendingActions, pendingActions);

	parentPendingNotifies = (List *) linitial(upperPendingNotifies);
	upperPendingNotifies = list_delete_first(upperPendingNotifies);

	Assert(list_length(upperPendingNotifies) ==
		   GetCurrentTransactionNestLevel() - 2);

	/*
	 * We could try to eliminate duplicates here, but it seems not worthwhile.
	 */
	pendingNotifies = list_concat(parentPendingNotifies, pendingNotifies);
}

/*
 * AtSubAbort_Notify() --- Take care of subtransaction abort.
 */
void
AtSubAbort_Notify(void)
{
	int			my_level = GetCurrentTransactionNestLevel();

	/*
	 * All we have to do is pop the stack --- the actions/notifies made in
	 * this subxact are no longer interesting, and the space will be freed
	 * when CurTransactionContext is recycled.
	 *
	 * This routine could be called more than once at a given nesting level if
	 * there is trouble during subxact abort.  Avoid dumping core by using
	 * GetCurrentTransactionNestLevel as the indicator of how far we need to
	 * prune the list.
	 */
	while (list_length(upperPendingActions) > my_level - 2)
	{
		pendingActions = (List *) linitial(upperPendingActions);
		upperPendingActions = list_delete_first(upperPendingActions);
	}

	while (list_length(upperPendingNotifies) > my_level - 2)
	{
		pendingNotifies = (List *) linitial(upperPendingNotifies);
		upperPendingNotifies = list_delete_first(upperPendingNotifies);
	}
}

/*
 * HandleNotifyInterrupt
 *
 *		Signal handler portion of interrupt handling. Let the backend know
 *		that there's a pending notify interrupt. If we're currently reading
 *		from the client, this will interrupt the read and
 *		ProcessClientReadInterrupt() will call ProcessNotifyInterrupt().
 */
void
HandleNotifyInterrupt(void)
{
	/*
	 * Note: this is called by a SIGNAL HANDLER. You must be very wary what
	 * you do here.
	 */

	/* signal that work needs to be done */
	notifyInterruptPending = true;

	/* make sure the event is processed in due course */
	SetLatch(MyLatch);
}

/*
 * ProcessNotifyInterrupt
 *
 *		This is called just after waiting for a frontend command.  If a
 *		interrupt arrives (via HandleNotifyInterrupt()) while reading, the
 *		read will be interrupted via the process's latch, and this routine
 *		will get called.  If we are truly idle (ie, *not* inside a transaction
 *		block), process the incoming notifies.
 */
void
ProcessNotifyInterrupt(void)
{
	if (IsTransactionOrTransactionBlock())
		return;					/* not really idle */

	while (notifyInterruptPending)
		ProcessIncomingNotify();
}


/*
 * Read all pending notifications from the queue, and deliver appropriate
 * ones to my frontend.  Stop when we reach queue head or an uncommitted
 * notification.
 */
static void
asyncQueueReadAllNotifications(void)
{
	volatile QueuePosition pos;
	QueuePosition oldpos;
	QueuePosition head;
	bool		advanceTail;

	/* page_buffer must be adequately aligned, so use a union */
	union
	{
		char		buf[QUEUE_PAGESIZE];
		AsyncQueueEntry align;
	}			page_buffer;

	/* Fetch current state */
	LWLockAcquire(AsyncQueueLock, LW_SHARED);
	/* Assert checks that we have a valid state entry */
	Assert(MyProcPid == QUEUE_BACKEND_PID(MyBackendId));
	pos = oldpos = QUEUE_BACKEND_POS(MyBackendId);
	head = QUEUE_HEAD;
	LWLockRelease(AsyncQueueLock);

	if (QUEUE_POS_EQUAL(pos, head))
	{
		/* Nothing to do, we have read all notifications already. */
		return;
	}

	/*----------
	 * Note that we deliver everything that we see in the queue and that
	 * matches our _current_ listening state.
	 * Especially we do not take into account different commit times.
	 * Consider the following example:
	 *
	 * Backend 1:					 Backend 2:
	 *
	 * transaction starts
	 * NOTIFY foo;
	 * commit starts
	 *								 transaction starts
	 *								 LISTEN foo;
	 *								 commit starts
	 * commit to clog
	 *								 commit to clog
	 *
	 * It could happen that backend 2 sees the notification from backend 1 in
	 * the queue.  Even though the notifying transaction committed before
	 * the listening transaction, we still deliver the notification.
	 *
	 * The idea is that an additional notification does not do any harm, we
	 * just need to make sure that we do not miss a notification.
	 *
	 * It is possible that we fail while trying to send a message to our
	 * frontend (for example, because of encoding conversion failure).
	 * If that happens it is critical that we not try to send the same
	 * message over and over again.  Therefore, we place a PG_TRY block
	 * here that will forcibly advance our backend position before we lose
	 * control to an error.  (We could alternatively retake AsyncQueueLock
	 * and move the position before handling each individual message, but
	 * that seems like too much lock traffic.)
	 *----------
	 */
	PG_TRY();
	{
		bool		reachedStop;

		do
		{
			int			curpage = QUEUE_POS_PAGE(pos);
			int			curoffset = QUEUE_POS_OFFSET(pos);
			int			slotno;
			int			copysize;

			/*
			 * We copy the data from SLRU into a local buffer, so as to avoid
			 * holding the AsyncCtlLock while we are examining the entries and
			 * possibly transmitting them to our frontend.  Copy only the part
			 * of the page we will actually inspect.
			 */
			slotno = SimpleLruReadPage_ReadOnly(AsyncCtl, curpage,
												InvalidTransactionId);
			if (curpage == QUEUE_POS_PAGE(head))
			{
				/* we only want to read as far as head */
				copysize = QUEUE_POS_OFFSET(head) - curoffset;
				if (copysize < 0)
					copysize = 0;		/* just for safety */
			}
			else
			{
				/* fetch all the rest of the page */
				copysize = QUEUE_PAGESIZE - curoffset;
			}
			memcpy(page_buffer.buf + curoffset,
				   AsyncCtl->shared->page_buffer[slotno] + curoffset,
				   copysize);
			/* Release lock that we got from SimpleLruReadPage_ReadOnly() */
			LWLockRelease(AsyncCtlLock);

			/*
			 * Process messages up to the stop position, end of page, or an
			 * uncommitted message.
			 *
			 * Our stop position is what we found to be the head's position
			 * when we entered this function. It might have changed already.
			 * But if it has, we will receive (or have already received and
			 * queued) another signal and come here again.
			 *
			 * We are not holding AsyncQueueLock here! The queue can only
			 * extend beyond the head pointer (see above) and we leave our
			 * backend's pointer where it is so nobody will truncate or
			 * rewrite pages under us. Especially we don't want to hold a lock
			 * while sending the notifications to the frontend.
			 */
			reachedStop = asyncQueueProcessPageEntries(&pos, head,
													   page_buffer.buf);
		} while (!reachedStop);
	}
	PG_CATCH();
	{
		/* Update shared state */
		LWLockAcquire(AsyncQueueLock, LW_SHARED);
		QUEUE_BACKEND_POS(MyBackendId) = pos;
		advanceTail = QUEUE_POS_EQUAL(oldpos, QUEUE_TAIL);
		LWLockRelease(AsyncQueueLock);

		/* If we were the laziest backend, try to advance the tail pointer */
		if (advanceTail)
			asyncQueueAdvanceTail();

		PG_RE_THROW();
	}
	PG_END_TRY();

	/* Update shared state */
	LWLockAcquire(AsyncQueueLock, LW_SHARED);
	QUEUE_BACKEND_POS(MyBackendId) = pos;
	advanceTail = QUEUE_POS_EQUAL(oldpos, QUEUE_TAIL);
	LWLockRelease(AsyncQueueLock);

	/* If we were the laziest backend, try to advance the tail pointer */
	if (advanceTail)
		asyncQueueAdvanceTail();
}

/*
 * Fetch notifications from the shared queue, beginning at position current,
 * and deliver relevant ones to my frontend.
 *
 * The current page must have been fetched into page_buffer from shared
 * memory.  (We could access the page right in shared memory, but that
 * would imply holding the AsyncCtlLock throughout this routine.)
 *
 * We stop if we reach the "stop" position, or reach a notification from an
 * uncommitted transaction, or reach the end of the page.
 *
 * The function returns true once we have reached the stop position or an
 * uncommitted notification, and false if we have finished with the page.
 * In other words: once it returns true there is no need to look further.
 * The QueuePosition *current is advanced past all processed messages.
 */
static bool
asyncQueueProcessPageEntries(volatile QueuePosition *current,
							 QueuePosition stop,
							 char *page_buffer)
{
	bool		reachedStop = false;
	bool		reachedEndOfPage;
	AsyncQueueEntry *qe;

	do
	{
		QueuePosition thisentry = *current;

		if (QUEUE_POS_EQUAL(thisentry, stop))
			break;

		qe = (AsyncQueueEntry *) (page_buffer + QUEUE_POS_OFFSET(thisentry));

		/*
		 * Advance *current over this message, possibly to the next page. As
		 * noted in the comments for asyncQueueReadAllNotifications, we must
		 * do this before possibly failing while processing the message.
		 */
		reachedEndOfPage = asyncQueueAdvance(current, qe->length);

		/* Ignore messages destined for other databases */
		if (qe->dboid == MyDatabaseId)
		{
			if (TransactionIdIsInProgress(qe->xid))
			{
				/*
				 * The source transaction is still in progress, so we can't
				 * process this message yet.  Break out of the loop, but first
				 * back up *current so we will reprocess the message next
				 * time.  (Note: it is unlikely but not impossible for
				 * TransactionIdDidCommit to fail, so we can't really avoid
				 * this advance-then-back-up behavior when dealing with an
				 * uncommitted message.)
				 *
				 * Note that we must test TransactionIdIsInProgress before we
				 * test TransactionIdDidCommit, else we might return a message
				 * from a transaction that is not yet visible to snapshots;
				 * compare the comments at the head of tqual.c.
				 */
				*current = thisentry;
				reachedStop = true;
				break;
			}
			else if (TransactionIdDidCommit(qe->xid))
			{
				/* qe->data is the null-terminated channel name */
				char	   *channel = qe->data;

				if (IsListeningOn(channel))
				{
					/* payload follows channel name */
					char	   *payload = qe->data + strlen(channel) + 1;

					NotifyMyFrontEnd(channel, payload, qe->srcPid);
				}
			}
			else
			{
				/*
				 * The source transaction aborted or crashed, so we just
				 * ignore its notifications.
				 */
			}
		}

		/* Loop back if we're not at end of page */
	} while (!reachedEndOfPage);

	if (QUEUE_POS_EQUAL(*current, stop))
		reachedStop = true;

	return reachedStop;
}

/*
 * Advance the shared queue tail variable to the minimum of all the
 * per-backend tail pointers.  Truncate pg_notify space if possible.
 */
static void
asyncQueueAdvanceTail(void)
{
	QueuePosition min;
	int			i;
	int			oldtailpage;
	int			newtailpage;
	int			boundary;

	LWLockAcquire(AsyncQueueLock, LW_EXCLUSIVE);
	min = QUEUE_HEAD;
	for (i = 1; i <= MaxBackends; i++)
	{
		if (QUEUE_BACKEND_PID(i) != InvalidPid)
			min = QUEUE_POS_MIN(min, QUEUE_BACKEND_POS(i));
	}
	oldtailpage = QUEUE_POS_PAGE(QUEUE_TAIL);
	QUEUE_TAIL = min;
	LWLockRelease(AsyncQueueLock);

	/*
	 * We can truncate something if the global tail advanced across an SLRU
	 * segment boundary.
	 *
	 * XXX it might be better to truncate only once every several segments, to
	 * reduce the number of directory scans.
	 */
	newtailpage = QUEUE_POS_PAGE(min);
	boundary = newtailpage - (newtailpage % SLRU_PAGES_PER_SEGMENT);
	if (asyncQueuePagePrecedes(oldtailpage, boundary))
	{
		/*
		 * SimpleLruTruncate() will ask for AsyncCtlLock but will also release
		 * the lock again.
		 */
		SimpleLruTruncate(AsyncCtl, newtailpage);
	}
}

/*
 * ProcessIncomingNotify
 *
 *		Deal with arriving NOTIFYs from other backends as soon as it's safe to
 *		do so. This used to be called from the PROCSIG_NOTIFY_INTERRUPT
 *		signal handler, but isn't anymore.
 *
 *		Scan the queue for arriving notifications and report them to my front
 *		end.
 *
 *		NOTE: since we are outside any transaction, we must create our own.
 */
static void
ProcessIncomingNotify(void)
{
	/* We *must* reset the flag */
	notifyInterruptPending = false;

	/* Do nothing else if we aren't actively listening */
	if (listenChannels == NIL)
		return;

	if (Trace_notify)
		elog(DEBUG1, "ProcessIncomingNotify");

	set_ps_display("notify interrupt", false);

	/*
	 * We must run asyncQueueReadAllNotifications inside a transaction, else
	 * bad things happen if it gets an error.
	 */
	StartTransactionCommand();

	asyncQueueReadAllNotifications();

	CommitTransactionCommand();

	/*
	 * Must flush the notify messages to ensure frontend gets them promptly.
	 */
	pq_flush();

	set_ps_display("idle", false);

	if (Trace_notify)
		elog(DEBUG1, "ProcessIncomingNotify: done");
}

/*
 * Send NOTIFY message to my front end.
 */
static void
NotifyMyFrontEnd(const char *channel, const char *payload, int32 srcPid)
{
	if (whereToSendOutput == DestRemote)
	{
		StringInfoData buf;

		pq_beginmessage(&buf, 'A');
		pq_sendint(&buf, srcPid, sizeof(int32));
		pq_sendstring(&buf, channel);
		if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 3)
			pq_sendstring(&buf, payload);
		pq_endmessage(&buf);

		/*
		 * NOTE: we do not do pq_flush() here.  For a self-notify, it will
		 * happen at the end of the transaction, and for incoming notifies
		 * ProcessIncomingNotify will do it after finding all the notifies.
		 */
	}
	else
		elog(INFO, "NOTIFY for \"%s\" payload \"%s\"", channel, payload);
}

/* Does pendingNotifies include the given channel/payload? */
static bool
AsyncExistsPendingNotify(const char *channel, const char *payload)
{
	ListCell   *p;
	Notification *n;

	if (pendingNotifies == NIL)
		return false;

	if (payload == NULL)
		payload = "";

	/*----------
	 * We need to append new elements to the end of the list in order to keep
	 * the order. However, on the other hand we'd like to check the list
	 * backwards in order to make duplicate-elimination a tad faster when the
	 * same condition is signaled many times in a row. So as a compromise we
	 * check the tail element first which we can access directly. If this
	 * doesn't match, we check the whole list.
	 *
	 * As we are not checking our parents' lists, we can still get duplicates
	 * in combination with subtransactions, like in:
	 *
	 * begin;
	 * notify foo '1';
	 * savepoint foo;
	 * notify foo '1';
	 * commit;
	 *----------
	 */
	n = (Notification *) llast(pendingNotifies);
	if (strcmp(n->channel, channel) == 0 &&
		strcmp(n->payload, payload) == 0)
		return true;

	foreach(p, pendingNotifies)
	{
		n = (Notification *) lfirst(p);

		if (strcmp(n->channel, channel) == 0 &&
			strcmp(n->payload, payload) == 0)
			return true;
	}

	return false;
}

/* Clear the pendingActions and pendingNotifies lists. */
static void
ClearPendingActionsAndNotifies(void)
{
	/*
	 * We used to have to explicitly deallocate the list members and nodes,
	 * because they were malloc'd.  Now, since we know they are palloc'd in
	 * CurTransactionContext, we need not do that --- they'll go away
	 * automatically at transaction exit.  We need only reset the list head
	 * pointers.
	 */
	pendingActions = NIL;
	pendingNotifies = NIL;
}
