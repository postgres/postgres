/*-------------------------------------------------------------------------
 *
 * async.c--
 *	  Asynchronous notification
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/async.c,v 1.40 1998/09/01 04:27:42 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/* New Async Notification Model:
 * 1. Multiple backends on same machine.  Multiple backends listening on
 *	  one relation.
 *
 * 2. One of the backend does a 'notify <relname>'.  For all backends that
 *	  are listening to this relation (all notifications take place at the
 *	  end of commit),
 *	  2.a  If the process is the same as the backend process that issued
 *		   notification (we are notifying something that we are listening),
 *		   signal the corresponding frontend over the comm channel.
 *	  2.b  For all other listening processes, we send kill(SIGUSR2) to wake up
 *		   the listening backend.
 * 3. Upon receiving a kill(SIGUSR2) signal from another backend process
 *	  notifying that one of the relation that we are listening is being
 *	  notified, we can be in either of two following states:
 *	  3.a  We are sleeping, wake up and signal our frontend.
 *	  3.b  We are in middle of another transaction, wait until the end of
 *		   of the current transaction and signal our frontend.
 * 4. Each frontend receives this notification and processes accordingly.
 *
 * -- jw, 12/28/93
 *
 */

#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>			/* Needed by in.h on Ultrix */
#include <netinet/in.h>

#include "postgres.h"

#include "access/heapam.h"
#include "access/relscan.h"
#include "access/xact.h"
#include "catalog/catname.h"
#include "catalog/pg_listener.h"
#include "commands/async.h"
#include "fmgr.h"
#include "lib/dllist.h"
#include "libpq/libpq.h"
#include "miscadmin.h"
#include "nodes/memnodes.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "tcop/dest.h"
#include "utils/mcxt.h"
#include "utils/syscache.h"
#include <utils/trace.h>
#include <utils/ps_status.h>

#define NotifyUnlock pg_options[OPT_NOTIFYUNLOCK]
#define NotifyHack	 pg_options[OPT_NOTIFYHACK]

extern TransactionState CurrentTransactionState;
extern CommandDest whereToSendOutput;

GlobalMemory notifyContext = NULL;

static int	notifyFrontEndPending = 0;
static int	notifyIssued = 0;
static Dllist *pendingNotifies = NULL;

static int	AsyncExistsPendingNotify(char *);
static void ClearPendingNotify(void);
static void Async_NotifyFrontEnd(void);
static void Async_NotifyFrontEnd_Aux(void);
void		Async_Unlisten(char *relname, int pid);
static void Async_UnlistenOnExit(int code, char *relname);
static void Async_UnlistenAll(void);

/*
 *--------------------------------------------------------------
 * Async_NotifyHandler --
 *
 *		This is the signal handler for SIGUSR2.  When the backend
 *		is signaled, the backend can be in two states.
 *		1. If the backend is in the middle of another transaction,
 *		   we set the flag, notifyFrontEndPending, and wait until
 *		   the end of the transaction to notify the front end.
 *		2. If the backend is not in the middle of another transaction,
 *		   we notify the front end immediately.
 *
 *		-- jw, 12/28/93
 * Results:
 *		none
 *
 * Side effects:
 *		none
 */
void
Async_NotifyHandler(SIGNAL_ARGS)
{
	TPRINTF(TRACE_NOTIFY, "Async_NotifyHandler");

	if ((CurrentTransactionState->state == TRANS_DEFAULT) &&
		(CurrentTransactionState->blockState == TRANS_DEFAULT))
	{
		TPRINTF(TRACE_NOTIFY, "Async_NotifyHandler: "
				"waking up sleeping backend process");
		PS_SET_STATUS("async_notify");
		Async_NotifyFrontEnd();
		PS_SET_STATUS("idle");
	}
	else
	{
		TPRINTF(TRACE_NOTIFY, "Async_NotifyHandler: "
			 "process in middle of transaction, state=%d, blockstate=%d",
				CurrentTransactionState->state,
				CurrentTransactionState->blockState);
		notifyFrontEndPending = 1;
		TPRINTF(TRACE_NOTIFY, "Async_NotifyHandler: notify frontend pending");
	}

	TPRINTF(TRACE_NOTIFY, "Async_NotifyHandler: done");
}

/*
 *--------------------------------------------------------------
 * Async_Notify --
 *
 *		This is executed by the SQL notify command.
 *
 *		Adds the relation to the list of pending notifies.
 *		All notification happens at end of commit.
 *		^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
 *
 *		All notification of backend processes happens here,
 *		then each backend notifies its corresponding front end at
 *		the end of commit.
 *
 *		-- jw, 12/28/93
 *
 * Results:
 *		XXX
 *
 * Side effects:
 *		All tuples for relname in pg_listener are updated.
 *
 *--------------------------------------------------------------
 */
void
Async_Notify(char *relname)
{

	HeapTuple	lTuple,
				rTuple;
	Relation	lRel;
	HeapScanDesc sRel;
	TupleDesc	tdesc;
	ScanKeyData key;
	Datum		d,
				value[3];
	bool		isnull;
	char		repl[3],
				nulls[3];

	char	   *notifyName;

	TPRINTF(TRACE_NOTIFY, "Async_Notify: %s", relname);

	if (!pendingNotifies)
		pendingNotifies = DLNewList();

	/*
	 * Allocate memory from the global malloc pool because it needs to be
	 * referenced also when the transaction is finished.  DZ - 26-08-1996
	 */
	notifyName = strdup(relname);
	DLAddHead(pendingNotifies, DLNewElem(notifyName));

	ScanKeyEntryInitialize(&key, 0,
						   Anum_pg_listener_relname,
						   F_NAMEEQ,
						   PointerGetDatum(notifyName));

	lRel = heap_openr(ListenerRelationName);
	tdesc = RelationGetDescr(lRel);
	RelationSetLockForWrite(lRel);
	sRel = heap_beginscan(lRel, 0, SnapshotNow, 1, &key);

	nulls[0] = nulls[1] = nulls[2] = ' ';
	repl[0] = repl[1] = repl[2] = ' ';
	repl[Anum_pg_listener_notify - 1] = 'r';
	value[0] = value[1] = value[2] = (Datum) 0;
	value[Anum_pg_listener_notify - 1] = Int32GetDatum(1);

	while (HeapTupleIsValid(lTuple = heap_getnext(sRel, 0)))
	{
		d = heap_getattr(lTuple, Anum_pg_listener_notify, tdesc, &isnull);
		if (!DatumGetInt32(d))
		{
			rTuple = heap_modifytuple(lTuple, lRel, value, nulls, repl);
			heap_replace(lRel, &lTuple->t_ctid, rTuple);
			/* notify is really issued only if a tuple has been changed */
			notifyIssued = 1;
		}
	}
	heap_endscan(sRel);

	/*
	 * Note: if the write lock is unset we can get multiple tuples with
	 * same oid if other backends notify the same relation. Use this
	 * option at your own risk.
	 */
	if (NotifyUnlock)
		RelationUnsetLockForWrite(lRel);

	heap_close(lRel);

	TPRINTF(TRACE_NOTIFY, "Async_Notify: done %s", relname);
}

/*
 *--------------------------------------------------------------
 * Async_NotifyAtCommit --
 *
 *		This is called at transaction commit.
 *
 *		Signal our corresponding frontend process on relations that
 *		were notified.	Signal all other backend process that
 *		are listening also.
 *
 *		-- jw, 12/28/93
 *
 * Results:
 *		XXX
 *
 * Side effects:
 *		Tuples in pg_listener that has our listenerpid are updated so
 *		that the notification is 0.  We do not want to notify frontend
 *		more than once.
 *
 *		-- jw, 12/28/93
 *
 *--------------------------------------------------------------
 */
void
Async_NotifyAtCommit()
{
	HeapTuple	lTuple;
	Relation	lRel;
	HeapScanDesc sRel;
	TupleDesc	tdesc;
	ScanKeyData key;
	Datum		d;
	bool		isnull;
	extern TransactionState CurrentTransactionState;

	if (!pendingNotifies)
		pendingNotifies = DLNewList();

	if ((CurrentTransactionState->state == TRANS_DEFAULT) &&
		(CurrentTransactionState->blockState == TRANS_DEFAULT))
	{
		if (notifyIssued)
		{
			/* 'notify <relname>' issued by us */
			notifyIssued = 0;
			StartTransactionCommand();
			TPRINTF(TRACE_NOTIFY, "Async_NotifyAtCommit");
			ScanKeyEntryInitialize(&key, 0,
								   Anum_pg_listener_notify,
								   F_INT4EQ,
								   Int32GetDatum(1));
			lRel = heap_openr(ListenerRelationName);
			RelationSetLockForWrite(lRel);
			sRel = heap_beginscan(lRel, 0, SnapshotNow, 1, &key);
			tdesc = RelationGetDescr(lRel);

			while (HeapTupleIsValid(lTuple = heap_getnext(sRel, 0)))
			{
				d = heap_getattr(lTuple, Anum_pg_listener_relname,
								 tdesc, &isnull);

				if (AsyncExistsPendingNotify((char *) DatumGetPointer(d)))
				{
					d = heap_getattr(lTuple, Anum_pg_listener_pid,
									 tdesc, &isnull);

					if (MyProcPid == DatumGetInt32(d))
					{
						notifyFrontEndPending = 1;
						TPRINTF(TRACE_NOTIFY,
								"Async_NotifyAtCommit: notifying self");
					}
					else
					{
						TPRINTF(TRACE_NOTIFY,
								"Async_NotifyAtCommit: notifying pid %d",
								DatumGetInt32(d));
#ifdef HAVE_KILL
						if (kill(DatumGetInt32(d), SIGUSR2) < 0)
						{
							if (errno == ESRCH)
								heap_delete(lRel, &lTuple->t_ctid);
						}
#endif
					}
				}
			}
			heap_endscan(sRel);
			heap_close(lRel);

			/*
			 * Notify the frontend inside the current transaction while we
			 * still have a valid write lock on pg_listeners. This avoid
			 * waiting until all other backends have finished with
			 * pg_listener.
			 */
			if (notifyFrontEndPending)
			{
				/* The aux version is called inside transaction */
				Async_NotifyFrontEnd_Aux();
			}

			TPRINTF(TRACE_NOTIFY, "Async_NotifyAtCommit: done");
			CommitTransactionCommand();
		}
		else
		{

			/*
			 * No notifies issued by us. If notifyFrontEndPending has been
			 * set by Async_NotifyHandler notify the frontend of pending
			 * notifies from other backends.
			 */
			if (notifyFrontEndPending)
				Async_NotifyFrontEnd();
		}

		ClearPendingNotify();
	}
}

/*
 *--------------------------------------------------------------
 * Async_NotifyAtAbort --
 *
 *		This is called at transaction commit.
 *
 *		Gets rid of pending notifies.  List elements are automatically
 *		freed through memory context.
 *
 *
 * Results:
 *		XXX
 *
 * Side effects:
 *		XXX
 *
 *--------------------------------------------------------------
 */
void
Async_NotifyAtAbort()
{
	if (pendingNotifies)
	{
		ClearPendingNotify();
		DLFreeList(pendingNotifies);
	}
	pendingNotifies = DLNewList();
	notifyIssued = 0;

	if ((CurrentTransactionState->state == TRANS_DEFAULT) &&
		(CurrentTransactionState->blockState == TRANS_DEFAULT))
	{
		/* don't forget to notify front end */
		if (notifyFrontEndPending)
			Async_NotifyFrontEnd();
	}
}

/*
 *--------------------------------------------------------------
 * Async_Listen --
 *
 *		This is executed by the SQL listen command.
 *
 *		Register a backend (identified by its Unix PID) as listening
 *		on the specified relation.
 *
 *		One listener per relation, pg_listener relation is keyed
 *		on (relname,pid) to provide multiple listeners in future.
 *
 * Results:
 *		pg_listeners is updated.
 *
 * Side effects:
 *		XXX
 *
 *--------------------------------------------------------------
 */
void
Async_Listen(char *relname, int pid)
{
	Datum		values[Natts_pg_listener];
	char		nulls[Natts_pg_listener];
	TupleDesc	tdesc;
	HeapScanDesc scan;
	HeapTuple	tuple,
				newtup;
	Relation	lDesc;
	Datum		d;
	int			i;
	bool		isnull;
	int			alreadyListener = 0;
	char	   *relnamei;
	TupleDesc	tupDesc;

	if (whereToSendOutput != Remote)
	{
		elog(NOTICE, "Async_Listen: "
			 "listen not available on interactive sessions");
		return;
	}

	TPRINTF(TRACE_NOTIFY, "Async_Listen: %s", relname);
	for (i = 0; i < Natts_pg_listener; i++)
	{
		nulls[i] = ' ';
		values[i] = PointerGetDatum(NULL);
	}

	i = 0;
	values[i++] = (Datum) relname;
	values[i++] = (Datum) pid;
	values[i++] = (Datum) 0;	/* no notifies pending */

	lDesc = heap_openr(ListenerRelationName);
	RelationSetLockForWrite(lDesc);

	/* is someone already listening.  One listener per relation */
	tdesc = RelationGetDescr(lDesc);
	scan = heap_beginscan(lDesc, 0, SnapshotNow, 0, (ScanKey) NULL);
	while (HeapTupleIsValid(tuple = heap_getnext(scan, 0)))
	{
		d = heap_getattr(tuple, Anum_pg_listener_relname, tdesc,
						 &isnull);
		relnamei = DatumGetPointer(d);
		if (!strncmp(relnamei, relname, NAMEDATALEN))
		{
			d = heap_getattr(tuple, Anum_pg_listener_pid, tdesc, &isnull);
			pid = DatumGetInt32(d);
			if (pid == MyProcPid)
				alreadyListener = 1;
		}
		if (alreadyListener)
		{
			/* No need to scan the rest of the table */
			break;
		}
	}
	heap_endscan(scan);

	if (alreadyListener)
	{
		elog(NOTICE, "Async_Listen: We are already listening on %s",
			 relname);
		RelationUnsetLockForWrite(lDesc);
		heap_close(lDesc);
		return;
	}

	tupDesc = lDesc->rd_att;
	newtup = heap_formtuple(tupDesc, values, nulls);
	heap_insert(lDesc, newtup);
	pfree(newtup);

	/*
	 * if (alreadyListener) { elog(NOTICE,"Async_Listen: already one
	 * listener on %s (possibly dead)",relname); }
	 */

	RelationUnsetLockForWrite(lDesc);
	heap_close(lDesc);

	/*
	 * now that we are listening, we should make a note to ourselves to
	 * unlisten prior to dying.
	 */
	relnamei = malloc(NAMEDATALEN);		/* persists to process exit */
	StrNCpy(relnamei, relname, NAMEDATALEN);
	on_shmem_exit(Async_UnlistenOnExit, (caddr_t) relnamei);
}

/*
 *--------------------------------------------------------------
 * Async_Unlisten --
 *
 *		This is executed by the SQL unlisten command.
 *
 *		Remove the backend from the list of listening backends
 *		for the specified relation.
 *
 * Results:
 *		pg_listeners is updated.
 *
 * Side effects:
 *		XXX
 *
 *--------------------------------------------------------------
 */
void
Async_Unlisten(char *relname, int pid)
{
	Relation	lDesc;
	HeapTuple	lTuple;

	/* Handle specially the `unlisten "*"' command */
	if ((!relname) || (*relname == '\0') || (strcmp(relname, "*") == 0))
	{
		Async_UnlistenAll();
		return;
	}

	TPRINTF(TRACE_NOTIFY, "Async_Unlisten %s", relname);

	lTuple = SearchSysCacheTuple(LISTENREL, PointerGetDatum(relname),
								 Int32GetDatum(pid),
								 0, 0);
	if (lTuple != NULL)
	{
		lDesc = heap_openr(ListenerRelationName);
		RelationSetLockForWrite(lDesc);
		heap_delete(lDesc, &lTuple->t_ctid);
		RelationUnsetLockForWrite(lDesc);
		heap_close(lDesc);
	}
}

/*
 *--------------------------------------------------------------
 * Async_UnlistenAll --
 *
 *		Unlisten all relations for this backend.
 *
 * Results:
 *		pg_listeners is updated.
 *
 * Side effects:
 *		XXX
 *
 *--------------------------------------------------------------
 */
static void
Async_UnlistenAll()
{
	HeapTuple	lTuple;
	Relation	lRel;
	HeapScanDesc sRel;
	TupleDesc	tdesc;
	ScanKeyData key[1];

	TPRINTF(TRACE_NOTIFY, "Async_UnlistenAll");
	ScanKeyEntryInitialize(&key[0], 0,
						   Anum_pg_listener_pid,
						   F_INT4EQ,
						   Int32GetDatum(MyProcPid));
	lRel = heap_openr(ListenerRelationName);
	RelationSetLockForWrite(lRel);
	tdesc = RelationGetDescr(lRel);
	sRel = heap_beginscan(lRel, 0, SnapshotNow, 1, key);

	while (HeapTupleIsValid(lTuple = heap_getnext(sRel, 0)))
		heap_delete(lRel, &lTuple->t_ctid);
	heap_endscan(sRel);
	RelationUnsetLockForWrite(lRel);
	heap_close(lRel);
	TPRINTF(TRACE_NOTIFY, "Async_UnlistenAll: done");
}

/*
 * --------------------------------------------------------------
 * Async_UnlistenOnExit --
 *
 *		This is called at backend exit for each registered listen.
 *
 * Results:
 *		XXX
 *
 * --------------------------------------------------------------
 */
static void
Async_UnlistenOnExit(int code,	/* from exitpg */
					 char *relname)
{
	Async_Unlisten((char *) relname, MyProcPid);
}

/*
 * --------------------------------------------------------------
 * Async_NotifyFrontEnd --
 *
 *		This is called outside transactions. The real work is done
 *		by Async_NotifyFrontEnd_Aux().
 *
 * --------------------------------------------------------------
 */
static void
Async_NotifyFrontEnd()
{
	StartTransactionCommand();
	Async_NotifyFrontEnd_Aux();
	CommitTransactionCommand();
}

/*
 * --------------------------------------------------------------
 * Async_NotifyFrontEnd_Aux --
 *
 *		This must be called inside a transaction block.
 *
 *		Perform an asynchronous notification to front end over
 *		portal comm channel.  The name of the relation which contains the
 *		data is sent to the front end.
 *
 *		We remove the notification flag from the pg_listener tuple
 *		associated with our process.
 *
 * Results:
 *		XXX
 *
 * --------------------------------------------------------------
 */
static void
Async_NotifyFrontEnd_Aux()
{
	HeapTuple	lTuple,
				rTuple;
	Relation	lRel;
	HeapScanDesc sRel;
	TupleDesc	tdesc;
	ScanKeyData key[2];
	Datum		d,
				value[3];
	char		repl[3],
				nulls[3];
	bool		isnull;

#define MAX_DONE 64

	char	   *done[MAX_DONE];
	int			ndone = 0;
	int			i;

	notifyFrontEndPending = 0;

	TPRINTF(TRACE_NOTIFY, "Async_NotifyFrontEnd");
	StartTransactionCommand();
	ScanKeyEntryInitialize(&key[0], 0,
						   Anum_pg_listener_notify,
						   F_INT4EQ,
						   Int32GetDatum(1));
	ScanKeyEntryInitialize(&key[1], 0,
						   Anum_pg_listener_pid,
						   F_INT4EQ,
						   Int32GetDatum(MyProcPid));
	lRel = heap_openr(ListenerRelationName);
	RelationSetLockForWrite(lRel);
	tdesc = RelationGetDescr(lRel);
	sRel = heap_beginscan(lRel, 0, SnapshotNow, 2, key);

	nulls[0] = nulls[1] = nulls[2] = ' ';
	repl[0] = repl[1] = repl[2] = ' ';
	repl[Anum_pg_listener_notify - 1] = 'r';
	value[0] = value[1] = value[2] = (Datum) 0;
	value[Anum_pg_listener_notify - 1] = Int32GetDatum(0);

	while (HeapTupleIsValid(lTuple = heap_getnext(sRel, 0)))
	{
		d = heap_getattr(lTuple, Anum_pg_listener_relname, tdesc,
						 &isnull);

		/*
		 * This hack deletes duplicate tuples which can be left in the
		 * table if the NotifyUnlock option is set. I'm further
		 * investigating this.	-- dz
		 */
		if (NotifyHack)
		{
			for (i = 0; i < ndone; i++)
			{
				if (strcmp(DatumGetName(d)->data, done[i]) == 0)
				{
					TPRINTF(TRACE_NOTIFY,
							"Async_NotifyFrontEnd: duplicate %s",
							DatumGetName(d)->data);
					heap_delete(lRel, &lTuple->t_ctid);
					continue;
				}
			}
			if (ndone < MAX_DONE)
				done[ndone++] = pstrdup(DatumGetName(d)->data);
		}

		rTuple = heap_modifytuple(lTuple, lRel, value, nulls, repl);
		heap_replace(lRel, &lTuple->t_ctid, rTuple);

		/* notifying the front end */
		TPRINTF(TRACE_NOTIFY, "Async_NotifyFrontEnd: notifying %s",
				DatumGetName(d)->data);

		if (whereToSendOutput == Remote)
		{
			pq_putnchar("A", 1);
			pq_putint((int32) MyProcPid, sizeof(int32));
			pq_putstr(DatumGetName(d)->data);
			pq_flush();
		}
	}
	heap_endscan(sRel);
	RelationUnsetLockForWrite(lRel);
	heap_close(lRel);

	TPRINTF(TRACE_NOTIFY, "Async_NotifyFrontEnd: done");
}

static int
AsyncExistsPendingNotify(char *relname)
{
	Dlelem	   *p;

	for (p = DLGetHead(pendingNotifies);
		 p != NULL;
		 p = DLGetSucc(p))
	{
		/* Use NAMEDATALEN for relname comparison.	  DZ - 26-08-1996 */
		if (!strncmp((const char *) DLE_VAL(p), relname, NAMEDATALEN))
			return 1;
	}

	return 0;
}

static void
ClearPendingNotify()
{
	Dlelem	   *p;

	while ((p = DLRemHead(pendingNotifies)) != NULL)
		free(DLE_VAL(p));
}
