/*-------------------------------------------------------------------------
 *
 * async.c
 *	  Asynchronous notification: NOTIFY, LISTEN, UNLISTEN
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/commands/async.c,v 1.134.2.1 2008/03/12 20:12:01 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

/*-------------------------------------------------------------------------
 * New Async Notification Model:
 * 1. Multiple backends on same machine.  Multiple backends listening on
 *	  one relation.  (Note: "listening on a relation" is not really the
 *	  right way to think about it, since the notify names need not have
 *	  anything to do with the names of relations actually in the database.
 *	  But this terminology is all over the code and docs, and I don't feel
 *	  like trying to replace it.)
 *
 * 2. There is a tuple in relation "pg_listener" for each active LISTEN,
 *	  ie, each relname/listenerPID pair.  The "notification" field of the
 *	  tuple is zero when no NOTIFY is pending for that listener, or the PID
 *	  of the originating backend when a cross-backend NOTIFY is pending.
 *	  (We skip writing to pg_listener when doing a self-NOTIFY, so the
 *	  notification field should never be equal to the listenerPID field.)
 *
 * 3. The NOTIFY statement itself (routine Async_Notify) just adds the target
 *	  relname to a list of outstanding NOTIFY requests.  Actual processing
 *	  happens if and only if we reach transaction commit.  At that time (in
 *	  routine AtCommit_Notify) we scan pg_listener for matching relnames.
 *	  If the listenerPID in a matching tuple is ours, we just send a notify
 *	  message to our own front end.  If it is not ours, and "notification"
 *	  is not already nonzero, we set notification to our own PID and send a
 *	  SIGUSR2 signal to the receiving process (indicated by listenerPID).
 *	  BTW: if the signal operation fails, we presume that the listener backend
 *	  crashed without removing this tuple, and remove the tuple for it.
 *
 * 4. Upon receipt of a SIGUSR2 signal, the signal handler can call inbound-
 *	  notify processing immediately if this backend is idle (ie, it is
 *	  waiting for a frontend command and is not within a transaction block).
 *	  Otherwise the handler may only set a flag, which will cause the
 *	  processing to occur just before we next go idle.
 *
 * 5. Inbound-notify processing consists of scanning pg_listener for tuples
 *	  matching our own listenerPID and having nonzero notification fields.
 *	  For each such tuple, we send a message to our frontend and clear the
 *	  notification field.  BTW: this routine has to start/commit its own
 *	  transaction, since by assumption it is only called from outside any
 *	  transaction.
 *
 * Like NOTIFY, LISTEN and UNLISTEN just add the desired action to a list
 * of pending actions.  If we reach transaction commit, the changes are
 * applied to pg_listener just before executing any pending NOTIFYs.  This
 * method is necessary because to avoid race conditions, we must hold lock
 * on pg_listener from when we insert a new listener tuple until we commit.
 * To do that and not create undue hazard of deadlock, we don't want to
 * touch pg_listener until we are otherwise done with the transaction;
 * in particular it'd be uncool to still be taking user-commanded locks
 * while holding the pg_listener lock.
 *
 * Although we grab ExclusiveLock on pg_listener for any operation,
 * the lock is never held very long, so it shouldn't cause too much of
 * a performance problem.  (Previously we used AccessExclusiveLock, but
 * there's no real reason to forbid concurrent reads.)
 *
 * An application that listens on the same relname it notifies will get
 * NOTIFY messages for its own NOTIFYs.  These can be ignored, if not useful,
 * by comparing be_pid in the NOTIFY message to the application's own backend's
 * PID.  (As of FE/BE protocol 2.0, the backend's PID is provided to the
 * frontend during startup.)  The above design guarantees that notifies from
 * other backends will never be missed by ignoring self-notifies.  Note,
 * however, that we do *not* guarantee that a separate frontend message will
 * be sent for every outside NOTIFY.  Since there is only room for one
 * originating PID in pg_listener, outside notifies occurring at about the
 * same time may be collapsed into a single message bearing the PID of the
 * first outside backend to perform the NOTIFY.
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>
#include <signal.h>

#include "access/heapam.h"
#include "access/twophase_rmgr.h"
#include "access/xact.h"
#include "catalog/pg_listener.h"
#include "commands/async.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/sinval.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"


/*
 * State for pending LISTEN/UNLISTEN actions consists of an ordered list of
 * all actions requested in the current transaction.  As explained above,
 * we don't actually modify pg_listener until we reach transaction commit.
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
	char		condname[1];				/* actually, as long as needed */
} ListenAction;

static List *pendingActions = NIL;			/* list of ListenAction */

static List *upperPendingActions = NIL;		/* list of upper-xact lists */

/*
 * State for outbound notifies consists of a list of all relnames NOTIFYed
 * in the current transaction.	We do not actually perform a NOTIFY until
 * and unless the transaction commits.	pendingNotifies is NIL if no
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
static List *pendingNotifies = NIL;				/* list of C strings */

static List *upperPendingNotifies = NIL;		/* list of upper-xact lists */

/*
 * State for inbound notifies consists of two flags: one saying whether
 * the signal handler is currently allowed to call ProcessIncomingNotify
 * directly, and one saying whether the signal has occurred but the handler
 * was not allowed to call ProcessIncomingNotify at the time.
 *
 * NB: the "volatile" on these declarations is critical!  If your compiler
 * does not grok "volatile", you'd be best advised to compile this file
 * with all optimization turned off.
 */
static volatile sig_atomic_t notifyInterruptEnabled = 0;
static volatile sig_atomic_t notifyInterruptOccurred = 0;

/* True if we've registered an on_shmem_exit cleanup */
static bool unlistenExitRegistered = false;

bool		Trace_notify = false;


static void queue_listen(ListenActionKind action, const char *condname);
static void Async_UnlistenAll(void);
static void Async_UnlistenOnExit(int code, Datum arg);
static void Exec_Listen(Relation lRel, const char *relname);
static void Exec_Unlisten(Relation lRel, const char *relname);
static void Exec_UnlistenAll(Relation lRel);
static void Send_Notify(Relation lRel);
static void ProcessIncomingNotify(void);
static void NotifyMyFrontEnd(char *relname, int32 listenerPID);
static bool AsyncExistsPendingNotify(const char *relname);
static void ClearPendingActionsAndNotifies(void);


/*
 * Async_Notify
 *
 *		This is executed by the SQL notify command.
 *
 *		Adds the relation to the list of pending notifies.
 *		Actual notification happens during transaction commit.
 *		^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
 */
void
Async_Notify(const char *relname)
{
	if (Trace_notify)
		elog(DEBUG1, "Async_Notify(%s)", relname);

	/* no point in making duplicate entries in the list ... */
	if (!AsyncExistsPendingNotify(relname))
	{
		/*
		 * The name list needs to live until end of transaction, so store it
		 * in the transaction context.
		 */
		MemoryContext oldcontext;

		oldcontext = MemoryContextSwitchTo(CurTransactionContext);

		/*
		 * Ordering of the list isn't important.  We choose to put new
		 * entries on the front, as this might make duplicate-elimination
		 * a tad faster when the same condition is signaled many times in
		 * a row.
		 */
		pendingNotifies = lcons(pstrdup(relname), pendingNotifies);

		MemoryContextSwitchTo(oldcontext);
	}
}

/*
 * queue_listen
 *		Common code for listen, unlisten, unlisten all commands.
 *
 *		Adds the request to the list of pending actions.
 *		Actual update of pg_listener happens during transaction commit.
 *		^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
 */
static void
queue_listen(ListenActionKind action, const char *condname)
{
	MemoryContext oldcontext;
	ListenAction *actrec;

	/*
	 * Unlike Async_Notify, we don't try to collapse out duplicates.
	 * It would be too complicated to ensure we get the right interactions
	 * of conflicting LISTEN/UNLISTEN/UNLISTEN_ALL, and it's unlikely that
	 * there would be any performance benefit anyway in sane applications.
	 */
	oldcontext = MemoryContextSwitchTo(CurTransactionContext);

	/* space for terminating null is included in sizeof(ListenAction) */
	actrec = (ListenAction *) palloc(sizeof(ListenAction) + strlen(condname));
	actrec->action = action;
	strcpy(actrec->condname, condname);

	pendingActions = lappend(pendingActions, actrec);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * Async_Listen
 *
 *		This is executed by the SQL listen command.
 */
void
Async_Listen(const char *relname)
{
	if (Trace_notify)
		elog(DEBUG1, "Async_Listen(%s,%d)", relname, MyProcPid);

	queue_listen(LISTEN_LISTEN, relname);
}

/*
 * Async_Unlisten
 *
 *		This is executed by the SQL unlisten command.
 */
void
Async_Unlisten(const char *relname)
{
	/* Handle specially the `unlisten "*"' command */
	if ((!relname) || (*relname == '\0') || (strcmp(relname, "*") == 0))
	{
		Async_UnlistenAll();
	}
	else
	{
		if (Trace_notify)
			elog(DEBUG1, "Async_Unlisten(%s,%d)", relname, MyProcPid);

		queue_listen(LISTEN_UNLISTEN, relname);
	}
}

/*
 * Async_UnlistenAll
 *
 *		This is invoked by UNLISTEN "*" command, and also at backend exit.
 */
static void
Async_UnlistenAll(void)
{
	if (Trace_notify)
		elog(DEBUG1, "Async_UnlistenAll(%d)", MyProcPid);

	queue_listen(LISTEN_UNLISTEN_ALL, "");
}

/*
 * Async_UnlistenOnExit
 *
 *		Clean up the pg_listener table at backend exit.
 *
 *		This is executed if we have done any LISTENs in this backend.
 *		It might not be necessary anymore, if the user UNLISTENed everything,
 *		but we don't try to detect that case.
 */
static void
Async_UnlistenOnExit(int code, Datum arg)
{
	/*
	 * We need to start/commit a transaction for the unlisten, but if there is
	 * already an active transaction we had better abort that one first.
	 * Otherwise we'd end up committing changes that probably ought to be
	 * discarded.
	 */
	AbortOutOfAnyTransaction();
	/* Now we can do the unlisten */
	StartTransactionCommand();
	Async_UnlistenAll();
	CommitTransactionCommand();
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
	ListCell   *p;

	/* It's not sensible to have any pending LISTEN/UNLISTEN actions */
	if (pendingActions)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot PREPARE a transaction that has executed LISTEN or UNLISTEN")));

	/* We can deal with pending NOTIFY though */
	foreach(p, pendingNotifies)
	{
		const char *relname = (const char *) lfirst(p);

		RegisterTwoPhaseRecord(TWOPHASE_RM_NOTIFY_ID, 0,
							   relname, strlen(relname) + 1);
	}

	/*
	 * We can clear the state immediately, rather than needing a separate
	 * PostPrepare call, because if the transaction fails we'd just discard
	 * the state anyway.
	 */
	ClearPendingActionsAndNotifies();
}

/*
 * AtCommit_Notify
 *
 *		This is called at transaction commit.
 *
 *		If there are pending LISTEN/UNLISTEN actions, insert or delete
 *		tuples in pg_listener accordingly.
 *
 *		If there are outbound notify requests in the pendingNotifies list,
 *		scan pg_listener for matching tuples, and either signal the other
 *		backend or send a message to our own frontend.
 *
 *		NOTE: we are still inside the current transaction, therefore can
 *		piggyback on its committing of changes.
 */
void
AtCommit_Notify(void)
{
	Relation	lRel;
	ListCell   *p;

	if (pendingActions == NIL && pendingNotifies == NIL)
		return;					/* no relevant statements in this xact */

	/*
	 * NOTIFY is disabled if not normal processing mode. This test used to be
	 * in xact.c, but it seems cleaner to do it here.
	 */
	if (!IsNormalProcessingMode())
	{
		ClearPendingActionsAndNotifies();
		return;
	}

	if (Trace_notify)
		elog(DEBUG1, "AtCommit_Notify");

	/* Acquire ExclusiveLock on pg_listener */
	lRel = heap_open(ListenerRelationId, ExclusiveLock);

	/* Perform any pending listen/unlisten actions */
	foreach(p, pendingActions)
	{
		ListenAction *actrec = (ListenAction *) lfirst(p);

		switch (actrec->action)
		{
			case LISTEN_LISTEN:
				Exec_Listen(lRel, actrec->condname);
				break;
			case LISTEN_UNLISTEN:
				Exec_Unlisten(lRel, actrec->condname);
				break;
			case LISTEN_UNLISTEN_ALL:
				Exec_UnlistenAll(lRel);
				break;
		}

		/* We must CCI after each action in case of conflicting actions */
		CommandCounterIncrement();
	}

	/* Perform any pending notifies */
	if (pendingNotifies)
		Send_Notify(lRel);

	/*
	 * We do NOT release the lock on pg_listener here; we need to hold it
	 * until end of transaction (which is about to happen, anyway) to ensure
	 * that notified backends see our tuple updates when they look. Else they
	 * might disregard the signal, which would make the application programmer
	 * very unhappy.  Also, this prevents race conditions when we have just
	 * inserted a listening tuple.
	 */
	heap_close(lRel, NoLock);

	ClearPendingActionsAndNotifies();

	if (Trace_notify)
		elog(DEBUG1, "AtCommit_Notify: done");
}

/*
 * Exec_Listen --- subroutine for AtCommit_Notify
 *
 *		Register the current backend as listening on the specified relation.
 */
static void
Exec_Listen(Relation lRel, const char *relname)
{
	HeapScanDesc scan;
	HeapTuple	tuple;
	Datum		values[Natts_pg_listener];
	char		nulls[Natts_pg_listener];
	NameData	condname;
	bool		alreadyListener = false;

	if (Trace_notify)
		elog(DEBUG1, "Exec_Listen(%s,%d)", relname, MyProcPid);

	/* Detect whether we are already listening on this relname */
	scan = heap_beginscan(lRel, SnapshotNow, 0, NULL);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_listener listener = (Form_pg_listener) GETSTRUCT(tuple);

		if (listener->listenerpid == MyProcPid &&
			strncmp(NameStr(listener->relname), relname, NAMEDATALEN) == 0)
		{
			alreadyListener = true;
			/* No need to scan the rest of the table */
			break;
		}
	}
	heap_endscan(scan);

	if (alreadyListener)
		return;

	/*
	 * OK to insert a new tuple
	 */
	memset(nulls, ' ', sizeof(nulls));

	namestrcpy(&condname, relname);
	values[Anum_pg_listener_relname - 1] = NameGetDatum(&condname);
	values[Anum_pg_listener_pid - 1] = Int32GetDatum(MyProcPid);
	values[Anum_pg_listener_notify - 1] = Int32GetDatum(0);	/* no notifies pending */

	tuple = heap_formtuple(RelationGetDescr(lRel), values, nulls);

	simple_heap_insert(lRel, tuple);

#ifdef NOT_USED					/* currently there are no indexes */
	CatalogUpdateIndexes(lRel, tuple);
#endif

	heap_freetuple(tuple);

	/*
	 * now that we are listening, make sure we will unlisten before dying.
	 */
	if (!unlistenExitRegistered)
	{
		on_shmem_exit(Async_UnlistenOnExit, 0);
		unlistenExitRegistered = true;
	}
}

/*
 * Exec_Unlisten --- subroutine for AtCommit_Notify
 *
 *		Remove the current backend from the list of listening backends
 *		for the specified relation.
 */
static void
Exec_Unlisten(Relation lRel, const char *relname)
{
	HeapScanDesc scan;
	HeapTuple	tuple;

	if (Trace_notify)
		elog(DEBUG1, "Exec_Unlisten(%s,%d)", relname, MyProcPid);

	scan = heap_beginscan(lRel, SnapshotNow, 0, NULL);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_listener listener = (Form_pg_listener) GETSTRUCT(tuple);

		if (listener->listenerpid == MyProcPid &&
			strncmp(NameStr(listener->relname), relname, NAMEDATALEN) == 0)
		{
			/* Found the matching tuple, delete it */
			simple_heap_delete(lRel, &tuple->t_self);

			/*
			 * We assume there can be only one match, so no need to scan the
			 * rest of the table
			 */
			break;
		}
	}
	heap_endscan(scan);

	/*
	 * We do not complain about unlistening something not being listened;
	 * should we?
	 */
}

/*
 * Exec_UnlistenAll --- subroutine for AtCommit_Notify
 *
 *		Update pg_listener to unlisten all relations for this backend.
 */
static void
Exec_UnlistenAll(Relation lRel)
{
	HeapScanDesc scan;
	HeapTuple	lTuple;
	ScanKeyData key[1];

	if (Trace_notify)
		elog(DEBUG1, "Exec_UnlistenAll");

	/* Find and delete all entries with my listenerPID */
	ScanKeyInit(&key[0],
				Anum_pg_listener_pid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(MyProcPid));
	scan = heap_beginscan(lRel, SnapshotNow, 1, key);

	while ((lTuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
		simple_heap_delete(lRel, &lTuple->t_self);

	heap_endscan(scan);
}

/*
 * Send_Notify --- subroutine for AtCommit_Notify
 *
 *		Scan pg_listener for tuples matching our pending notifies, and
 *		either signal the other backend or send a message to our own frontend.
 */
static void
Send_Notify(Relation lRel)
{
	TupleDesc	tdesc = RelationGetDescr(lRel);
	HeapScanDesc scan;
	HeapTuple	lTuple,
				rTuple;
	Datum		value[Natts_pg_listener];
	char		repl[Natts_pg_listener],
				nulls[Natts_pg_listener];

	/* preset data to update notify column to MyProcPid */
	nulls[0] = nulls[1] = nulls[2] = ' ';
	repl[0] = repl[1] = repl[2] = ' ';
	repl[Anum_pg_listener_notify - 1] = 'r';
	value[0] = value[1] = value[2] = (Datum) 0;
	value[Anum_pg_listener_notify - 1] = Int32GetDatum(MyProcPid);

	scan = heap_beginscan(lRel, SnapshotNow, 0, NULL);

	while ((lTuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_listener listener = (Form_pg_listener) GETSTRUCT(lTuple);
		char	   *relname = NameStr(listener->relname);
		int32		listenerPID = listener->listenerpid;

		if (!AsyncExistsPendingNotify(relname))
			continue;

		if (listenerPID == MyProcPid)
		{
			/*
			 * Self-notify: no need to bother with table update. Indeed, we
			 * *must not* clear the notification field in this path, or we
			 * could lose an outside notify, which'd be bad for applications
			 * that ignore self-notify messages.
			 */
			if (Trace_notify)
				elog(DEBUG1, "AtCommit_Notify: notifying self");

			NotifyMyFrontEnd(relname, listenerPID);
		}
		else
		{
			if (Trace_notify)
				elog(DEBUG1, "AtCommit_Notify: notifying pid %d",
					 listenerPID);

			/*
			 * If someone has already notified this listener, we don't bother
			 * modifying the table, but we do still send a SIGUSR2 signal,
			 * just in case that backend missed the earlier signal for some
			 * reason.	It's OK to send the signal first, because the other
			 * guy can't read pg_listener until we unlock it.
			 */
			if (kill(listenerPID, SIGUSR2) < 0)
			{
				/*
				 * Get rid of pg_listener entry if it refers to a PID that no
				 * longer exists.  Presumably, that backend crashed without
				 * deleting its pg_listener entries. This code used to only
				 * delete the entry if errno==ESRCH, but as far as I can see
				 * we should just do it for any failure (certainly at least
				 * for EPERM too...)
				 */
				simple_heap_delete(lRel, &lTuple->t_self);
			}
			else if (listener->notification == 0)
			{
				/* Rewrite the tuple with my PID in notification column */
				rTuple = heap_modifytuple(lTuple, tdesc, value, nulls, repl);
				simple_heap_update(lRel, &lTuple->t_self, rTuple);

#ifdef NOT_USED					/* currently there are no indexes */
				CatalogUpdateIndexes(lRel, rTuple);
#endif
			}
		}
	}

	heap_endscan(scan);
}

/*
 * AtAbort_Notify
 *
 *		This is called at transaction abort.
 *
 *		Gets rid of pending actions and outbound notifies that we would have
 *		executed if the transaction got committed.
 */
void
AtAbort_Notify(void)
{
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
	 * All we have to do is pop the stack --- the actions/notifies made in this
	 * subxact are no longer interesting, and the space will be freed when
	 * CurTransactionContext is recycled.
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
 * NotifyInterruptHandler
 *
 *		This is the signal handler for SIGUSR2.
 *
 *		If we are idle (notifyInterruptEnabled is set), we can safely invoke
 *		ProcessIncomingNotify directly.  Otherwise, just set a flag
 *		to do it later.
 */
void
NotifyInterruptHandler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	/*
	 * Note: this is a SIGNAL HANDLER.	You must be very wary what you do
	 * here. Some helpful soul had this routine sprinkled with TPRINTFs, which
	 * would likely lead to corruption of stdio buffers if they were ever
	 * turned on.
	 */

	/* Don't joggle the elbow of proc_exit */
	if (proc_exit_inprogress)
		return;

	if (notifyInterruptEnabled)
	{
		bool		save_ImmediateInterruptOK = ImmediateInterruptOK;

		/*
		 * We may be called while ImmediateInterruptOK is true; turn it off
		 * while messing with the NOTIFY state.  (We would have to save and
		 * restore it anyway, because PGSemaphore operations inside
		 * ProcessIncomingNotify() might reset it.)
		 */
		ImmediateInterruptOK = false;

		/*
		 * I'm not sure whether some flavors of Unix might allow another
		 * SIGUSR2 occurrence to recursively interrupt this routine. To cope
		 * with the possibility, we do the same sort of dance that
		 * EnableNotifyInterrupt must do --- see that routine for comments.
		 */
		notifyInterruptEnabled = 0;		/* disable any recursive signal */
		notifyInterruptOccurred = 1;	/* do at least one iteration */
		for (;;)
		{
			notifyInterruptEnabled = 1;
			if (!notifyInterruptOccurred)
				break;
			notifyInterruptEnabled = 0;
			if (notifyInterruptOccurred)
			{
				/* Here, it is finally safe to do stuff. */
				if (Trace_notify)
					elog(DEBUG1, "NotifyInterruptHandler: perform async notify");

				ProcessIncomingNotify();

				if (Trace_notify)
					elog(DEBUG1, "NotifyInterruptHandler: done");
			}
		}

		/*
		 * Restore ImmediateInterruptOK, and check for interrupts if needed.
		 */
		ImmediateInterruptOK = save_ImmediateInterruptOK;
		if (save_ImmediateInterruptOK)
			CHECK_FOR_INTERRUPTS();
	}
	else
	{
		/*
		 * In this path it is NOT SAFE to do much of anything, except this:
		 */
		notifyInterruptOccurred = 1;
	}

	errno = save_errno;
}

/*
 * EnableNotifyInterrupt
 *
 *		This is called by the PostgresMain main loop just before waiting
 *		for a frontend command.  If we are truly idle (ie, *not* inside
 *		a transaction block), then process any pending inbound notifies,
 *		and enable the signal handler to process future notifies directly.
 *
 *		NOTE: the signal handler starts out disabled, and stays so until
 *		PostgresMain calls this the first time.
 */
void
EnableNotifyInterrupt(void)
{
	if (IsTransactionOrTransactionBlock())
		return;					/* not really idle */

	/*
	 * This code is tricky because we are communicating with a signal handler
	 * that could interrupt us at any point.  If we just checked
	 * notifyInterruptOccurred and then set notifyInterruptEnabled, we could
	 * fail to respond promptly to a signal that happens in between those two
	 * steps.  (A very small time window, perhaps, but Murphy's Law says you
	 * can hit it...)  Instead, we first set the enable flag, then test the
	 * occurred flag.  If we see an unserviced interrupt has occurred, we
	 * re-clear the enable flag before going off to do the service work. (That
	 * prevents re-entrant invocation of ProcessIncomingNotify() if another
	 * interrupt occurs.) If an interrupt comes in between the setting and
	 * clearing of notifyInterruptEnabled, then it will have done the service
	 * work and left notifyInterruptOccurred zero, so we have to check again
	 * after clearing enable.  The whole thing has to be in a loop in case
	 * another interrupt occurs while we're servicing the first. Once we get
	 * out of the loop, enable is set and we know there is no unserviced
	 * interrupt.
	 *
	 * NB: an overenthusiastic optimizing compiler could easily break this
	 * code. Hopefully, they all understand what "volatile" means these days.
	 */
	for (;;)
	{
		notifyInterruptEnabled = 1;
		if (!notifyInterruptOccurred)
			break;
		notifyInterruptEnabled = 0;
		if (notifyInterruptOccurred)
		{
			if (Trace_notify)
				elog(DEBUG1, "EnableNotifyInterrupt: perform async notify");

			ProcessIncomingNotify();

			if (Trace_notify)
				elog(DEBUG1, "EnableNotifyInterrupt: done");
		}
	}
}

/*
 * DisableNotifyInterrupt
 *
 *		This is called by the PostgresMain main loop just after receiving
 *		a frontend command.  Signal handler execution of inbound notifies
 *		is disabled until the next EnableNotifyInterrupt call.
 *
 *		The SIGUSR1 signal handler also needs to call this, so as to
 *		prevent conflicts if one signal interrupts the other.  So we
 *		must return the previous state of the flag.
 */
bool
DisableNotifyInterrupt(void)
{
	bool		result = (notifyInterruptEnabled != 0);

	notifyInterruptEnabled = 0;

	return result;
}

/*
 * ProcessIncomingNotify
 *
 *		Deal with arriving NOTIFYs from other backends.
 *		This is called either directly from the SIGUSR2 signal handler,
 *		or the next time control reaches the outer idle loop.
 *		Scan pg_listener for arriving notifies, report them to my front end,
 *		and clear the notification field in pg_listener until next time.
 *
 *		NOTE: since we are outside any transaction, we must create our own.
 */
static void
ProcessIncomingNotify(void)
{
	Relation	lRel;
	TupleDesc	tdesc;
	ScanKeyData key[1];
	HeapScanDesc scan;
	HeapTuple	lTuple,
				rTuple;
	Datum		value[Natts_pg_listener];
	char		repl[Natts_pg_listener],
				nulls[Natts_pg_listener];
	bool		catchup_enabled;

	/* Must prevent SIGUSR1 interrupt while I am running */
	catchup_enabled = DisableCatchupInterrupt();

	if (Trace_notify)
		elog(DEBUG1, "ProcessIncomingNotify");

	set_ps_display("notify interrupt", false);

	notifyInterruptOccurred = 0;

	StartTransactionCommand();

	lRel = heap_open(ListenerRelationId, ExclusiveLock);
	tdesc = RelationGetDescr(lRel);

	/* Scan only entries with my listenerPID */
	ScanKeyInit(&key[0],
				Anum_pg_listener_pid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(MyProcPid));
	scan = heap_beginscan(lRel, SnapshotNow, 1, key);

	/* Prepare data for rewriting 0 into notification field */
	nulls[0] = nulls[1] = nulls[2] = ' ';
	repl[0] = repl[1] = repl[2] = ' ';
	repl[Anum_pg_listener_notify - 1] = 'r';
	value[0] = value[1] = value[2] = (Datum) 0;
	value[Anum_pg_listener_notify - 1] = Int32GetDatum(0);

	while ((lTuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_listener listener = (Form_pg_listener) GETSTRUCT(lTuple);
		char	   *relname = NameStr(listener->relname);
		int32		sourcePID = listener->notification;

		if (sourcePID != 0)
		{
			/* Notify the frontend */

			if (Trace_notify)
				elog(DEBUG1, "ProcessIncomingNotify: received %s from %d",
					 relname, (int) sourcePID);

			NotifyMyFrontEnd(relname, sourcePID);

			/*
			 * Rewrite the tuple with 0 in notification column.
			 */
			rTuple = heap_modifytuple(lTuple, tdesc, value, nulls, repl);
			simple_heap_update(lRel, &lTuple->t_self, rTuple);

#ifdef NOT_USED					/* currently there are no indexes */
			CatalogUpdateIndexes(lRel, rTuple);
#endif
		}
	}
	heap_endscan(scan);

	/*
	 * We do NOT release the lock on pg_listener here; we need to hold it
	 * until end of transaction (which is about to happen, anyway) to ensure
	 * that other backends see our tuple updates when they look. Otherwise, a
	 * transaction started after this one might mistakenly think it doesn't
	 * need to send this backend a new NOTIFY.
	 */
	heap_close(lRel, NoLock);

	CommitTransactionCommand();

	/*
	 * Must flush the notify messages to ensure frontend gets them promptly.
	 */
	pq_flush();

	set_ps_display("idle", false);

	if (Trace_notify)
		elog(DEBUG1, "ProcessIncomingNotify: done");

	if (catchup_enabled)
		EnableCatchupInterrupt();
}

/*
 * Send NOTIFY message to my front end.
 */
static void
NotifyMyFrontEnd(char *relname, int32 listenerPID)
{
	if (whereToSendOutput == DestRemote)
	{
		StringInfoData buf;

		pq_beginmessage(&buf, 'A');
		pq_sendint(&buf, listenerPID, sizeof(int32));
		pq_sendstring(&buf, relname);
		if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 3)
		{
			/* XXX Add parameter string here later */
			pq_sendstring(&buf, "");
		}
		pq_endmessage(&buf);

		/*
		 * NOTE: we do not do pq_flush() here.	For a self-notify, it will
		 * happen at the end of the transaction, and for incoming notifies
		 * ProcessIncomingNotify will do it after finding all the notifies.
		 */
	}
	else
		elog(INFO, "NOTIFY for %s", relname);
}

/* Does pendingNotifies include the given relname? */
static bool
AsyncExistsPendingNotify(const char *relname)
{
	ListCell   *p;

	foreach(p, pendingNotifies)
	{
		const char *prelname = (const char *) lfirst(p);

		if (strcmp(prelname, relname) == 0)
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

/*
 * 2PC processing routine for COMMIT PREPARED case.
 *
 * (We don't have to do anything for ROLLBACK PREPARED.)
 */
void
notify_twophase_postcommit(TransactionId xid, uint16 info,
						   void *recdata, uint32 len)
{
	/*
	 * Set up to issue the NOTIFY at the end of my own current transaction.
	 * (XXX this has some issues if my own transaction later rolls back, or if
	 * there is any significant delay before I commit.	OK for now because we
	 * disallow COMMIT PREPARED inside a transaction block.)
	 */
	Async_Notify((char *) recdata);
}
