/*-------------------------------------------------------------------------
 *
 * async.c--
 *	  Asynchronous notification
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/async.c,v 1.25 1997/12/06 22:56:24 momjian Exp $
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
 *		   signal the corresponding frontend over the comm channel using the
 *		   out-of-band channel.
 *	  2.b  For all other listening processes, we send kill(2) to wake up
 *		   the listening backend.
 * 3. Upon receiving a kill(2) signal from another backend process notifying
 *	  that one of the relation that we are listening is being notified,
 *	  we can be in either of two following states:
 *	  3.a  We are sleeping, wake up and signal our frontend.
 *	  3.b  We are in middle of another transaction, wait until the end of
 *		   of the current transaction and signal our frontend.
 * 4. Each frontend receives this notification and prcesses accordingly.
 *
 * -- jw, 12/28/93
 *
 */
/*
 * The following is the old model which does not work.
 */
/*
 * Model is:
 * 1. Multiple backends on same machine.
 *
 * 2. Query on one backend sends stuff over an asynchronous portal by
 *	  appending to a relation, and then doing an async. notification
 *	  (which takes place after commit) to all listeners on this relation.
 *
 * 3. Async. notification results in all backends listening on relation
 *	  to be woken up, by a process signal kill(2), with name of relation
 *	  passed in shared memory.
 *
 * 4. Each backend notifies its respective frontend over the comm
 *	  channel using the out-of-band channel.
 *
 * 5. Each frontend receives this notification and processes accordingly.
 *
 * #4,#5 are changing soon with pending rewrite of portal/protocol.
 *
 */
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>			/* Needed by in.h on Ultrix */
#include <netinet/in.h>

#include <postgres.h>

#include <utils/syscache.h>
#include <access/relscan.h>
#include <access/xact.h>
#include <lib/dllist.h>
#include <tcop/dest.h>
#include <catalog/pg_proc.h>
#include <catalog/catname.h>
#include <catalog/pg_listener.h>
#include <access/heapam.h>
#include <storage/bufmgr.h>
#include <nodes/memnodes.h>
#include <utils/mcxt.h>
#include <commands/async.h>
#include <libpq/libpq.h>

#include <port-protos.h>		/* for strdup() */

#include <storage/lmgr.h>

static int	notifyFrontEndPending = 0;
static int	notifyIssued = 0;
static Dllist *pendingNotifies = NULL;


static int	AsyncExistsPendingNotify(char *);
static void ClearPendingNotify(void);
static void Async_NotifyFrontEnd(void);
void        Async_Unlisten(char *relname, int pid);
static void Async_UnlistenOnExit(int code, char *relname);

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
	extern TransactionState CurrentTransactionState;

	if ((CurrentTransactionState->state == TRANS_DEFAULT) &&
		(CurrentTransactionState->blockState == TRANS_DEFAULT))
	{

#ifdef ASYNC_DEBUG
		elog(DEBUG, "Waking up sleeping backend process");
#endif
		Async_NotifyFrontEnd();

	}
	else
	{
#ifdef ASYNC_DEBUG
		elog(DEBUG, "Process is in the middle of another transaction, state = %d, block state = %d",
			 CurrentTransactionState->state,
			 CurrentTransactionState->blockState);
#endif
		notifyFrontEndPending = 1;
	}
}

/*
 *--------------------------------------------------------------
 * Async_Notify --
 *
 *		Adds the relation to the list of pending notifies.
 *		All notification happens at end of commit.
 *		^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
 *
 *		All notification of backend processes happens here,
 *		then each backend notifies its corresponding front end at
 *		the end of commit.
 *
 *		This correspond to 'notify <relname>' command
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
	Buffer		b;
	Datum		d,
				value[3];
	bool		isnull;
	char		repl[3],
				nulls[3];

	char	   *notifyName;

#ifdef ASYNC_DEBUG
	elog(DEBUG, "Async_Notify: %s", relname);
#endif

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
						   NameEqualRegProcedure,
						   PointerGetDatum(notifyName));

	lRel = heap_openr(ListenerRelationName);
	tdesc = RelationGetTupleDescriptor(lRel);
	RelationSetLockForWrite(lRel);
	sRel = heap_beginscan(lRel, 0, false, 1, &key);

	nulls[0] = nulls[1] = nulls[2] = ' ';
	repl[0] = repl[1] = repl[2] = ' ';
	repl[Anum_pg_listener_notify - 1] = 'r';
	value[0] = value[1] = value[2] = (Datum) 0;
	value[Anum_pg_listener_notify - 1] = Int32GetDatum(1);

	while (HeapTupleIsValid(lTuple = heap_getnext(sRel, 0, &b)))
	{
		d = heap_getattr(lTuple, b, Anum_pg_listener_notify,
						 tdesc, &isnull);
		if (!DatumGetInt32(d))
		{
			rTuple = heap_modifytuple(lTuple, b, lRel, value, nulls, repl);
			heap_replace(lRel, &lTuple->t_ctid, rTuple);
		}
		ReleaseBuffer(b);
	}
	heap_endscan(sRel);
	RelationUnsetLockForWrite(lRel);
	heap_close(lRel);
	notifyIssued = 1;
}

/*
 *--------------------------------------------------------------
 * Async_NotifyAtCommit --
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
	int			ourpid;
	bool		isnull;
	Buffer		b;
	extern TransactionState CurrentTransactionState;

	if (!pendingNotifies)
		pendingNotifies = DLNewList();

	if ((CurrentTransactionState->state == TRANS_DEFAULT) &&
		(CurrentTransactionState->blockState == TRANS_DEFAULT))
	{

		if (notifyIssued)
		{						/* 'notify <relname>' issued by us */
			notifyIssued = 0;
			StartTransactionCommand();
#ifdef ASYNC_DEBUG
			elog(DEBUG, "Async_NotifyAtCommit.");
#endif
			ScanKeyEntryInitialize(&key, 0,
								   Anum_pg_listener_notify,
								   Integer32EqualRegProcedure,
								   Int32GetDatum(1));
			lRel = heap_openr(ListenerRelationName);
			RelationSetLockForWrite(lRel);
			sRel = heap_beginscan(lRel, 0, false, 1, &key);
			tdesc = RelationGetTupleDescriptor(lRel);
			ourpid = getpid();

			while (HeapTupleIsValid(lTuple = heap_getnext(sRel, 0, &b)))
			{
				d = heap_getattr(lTuple, b, Anum_pg_listener_relname,
								 tdesc, &isnull);

				if (AsyncExistsPendingNotify((char *) DatumGetPointer(d)))
				{
					d = heap_getattr(lTuple, b, Anum_pg_listener_pid,
									 tdesc, &isnull);

					if (ourpid == DatumGetInt32(d))
					{
#ifdef ASYNC_DEBUG
						elog(DEBUG, "Notifying self, setting notifyFronEndPending to 1");
#endif
						notifyFrontEndPending = 1;
					}
					else
					{
#ifdef ASYNC_DEBUG
						elog(DEBUG, "Notifying others");
#endif
#ifdef HAVE_KILL
						if (kill(DatumGetInt32(d), SIGUSR2) < 0)
						{
							if (errno == ESRCH)
							{
								heap_delete(lRel, &lTuple->t_ctid);
							}
						}
#endif
					}
				}
				ReleaseBuffer(b);
			}
			heap_endscan(sRel);
			RelationUnsetLockForWrite(lRel);
			heap_close(lRel);

			CommitTransactionCommand();
			ClearPendingNotify();
		}

		if (notifyFrontEndPending)
		{						/* we need to notify the frontend of all
								 * pending notifies. */
			notifyFrontEndPending = 1;
			Async_NotifyFrontEnd();
		}
	}
}

/*
 *--------------------------------------------------------------
 * Async_NotifyAtAbort --
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
	extern TransactionState CurrentTransactionState;

	if (notifyIssued)
	{
		ClearPendingNotify();
	}
	notifyIssued = 0;
	if (pendingNotifies)
		DLFreeList(pendingNotifies);
	pendingNotifies = DLNewList();

	if ((CurrentTransactionState->state == TRANS_DEFAULT) &&
		(CurrentTransactionState->blockState == TRANS_DEFAULT))
	{
		if (notifyFrontEndPending)
		{						/* don't forget to notify front end */
			Async_NotifyFrontEnd();
		}
	}
}

/*
 *--------------------------------------------------------------
 * Async_Listen --
 *
 *		Register a backend (identified by its Unix PID) as listening
 *		on the specified relation.
 *
 *		This corresponds to the 'listen <relation>' command in SQL
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
	HeapScanDesc s;
	HeapTuple	htup,
				tup;
	Relation	lDesc;
	Buffer		b;
	Datum		d;
	int			i;
	bool		isnull;
	int			alreadyListener = 0;
	int			ourPid = getpid();
	char	   *relnamei;
	TupleDesc	tupDesc;

#ifdef ASYNC_DEBUG
	elog(DEBUG, "Async_Listen: %s", relname);
#endif
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
	tdesc = RelationGetTupleDescriptor(lDesc);
	s = heap_beginscan(lDesc, 0, false, 0, (ScanKey) NULL);
	while (HeapTupleIsValid(htup = heap_getnext(s, 0, &b)))
	{
		d = heap_getattr(htup, b, Anum_pg_listener_relname, tdesc,
						 &isnull);
		relnamei = DatumGetPointer(d);
		if (!strncmp(relnamei, relname, NAMEDATALEN))
		{
			d = heap_getattr(htup, b, Anum_pg_listener_pid, tdesc, &isnull);
			pid = DatumGetInt32(d);
			if (pid == ourPid)
			{
				alreadyListener = 1;
			}
		}
		ReleaseBuffer(b);
	}
	heap_endscan(s);

	if (alreadyListener)
	{
		elog(NOTICE, "Async_Listen: We are already listening on %s",
			 relname);
		return;
	}

	tupDesc = lDesc->rd_att;
	tup = heap_formtuple(tupDesc,
						 values,
						 nulls);
	heap_insert(lDesc, tup);

	pfree(tup);

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
	on_exitpg(Async_UnlistenOnExit, (caddr_t) relnamei);
}

/*
 *--------------------------------------------------------------
 * Async_Unlisten --
 *
 *		Remove the backend from the list of listening backends
 *		for the specified relation.
 *
 *		This would correspond to the 'unlisten <relation>'
 *		command, but there isn't one yet.
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

	lTuple = SearchSysCacheTuple(LISTENREL, PointerGetDatum(relname),
								 Int32GetDatum(pid),
								 0, 0);
	lDesc = heap_openr(ListenerRelationName);
	RelationSetLockForWrite(lDesc);

	if (lTuple != NULL)
	{
		heap_delete(lDesc, &lTuple->t_ctid);
	}

	RelationUnsetLockForWrite(lDesc);
	heap_close(lDesc);
}

static void
Async_UnlistenOnExit(int code,	/* from exitpg */
					 char *relname)
{
	Async_Unlisten((char *) relname, getpid());
}

/*
 * --------------------------------------------------------------
 * Async_NotifyFrontEnd --
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
 * Side effects:
 *
 *		We make use of the out-of-band channel to transmit the
 *		notification to the front end.	The actual data transfer takes
 *		place at the front end's request.
 *
 * --------------------------------------------------------------
 */
GlobalMemory notifyContext = NULL;

static void
Async_NotifyFrontEnd()
{
	extern CommandDest whereToSendOutput;
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
	Buffer		b;
	int			ourpid;
	bool		isnull;

	notifyFrontEndPending = 0;

#ifdef ASYNC_DEBUG
	elog(DEBUG, "Async_NotifyFrontEnd: notifying front end.");
#endif

	StartTransactionCommand();
	ourpid = getpid();
	ScanKeyEntryInitialize(&key[0], 0,
						   Anum_pg_listener_notify,
						   Integer32EqualRegProcedure,
						   Int32GetDatum(1));
	ScanKeyEntryInitialize(&key[1], 0,
						   Anum_pg_listener_pid,
						   Integer32EqualRegProcedure,
						   Int32GetDatum(ourpid));
	lRel = heap_openr(ListenerRelationName);
	RelationSetLockForWrite(lRel);
	tdesc = RelationGetTupleDescriptor(lRel);
	sRel = heap_beginscan(lRel, 0, false, 2, key);

	nulls[0] = nulls[1] = nulls[2] = ' ';
	repl[0] = repl[1] = repl[2] = ' ';
	repl[Anum_pg_listener_notify - 1] = 'r';
	value[0] = value[1] = value[2] = (Datum) 0;
	value[Anum_pg_listener_notify - 1] = Int32GetDatum(0);

	while (HeapTupleIsValid(lTuple = heap_getnext(sRel, 0, &b)))
	{
		d = heap_getattr(lTuple, b, Anum_pg_listener_relname,
						 tdesc, &isnull);
		rTuple = heap_modifytuple(lTuple, b, lRel, value, nulls, repl);
		heap_replace(lRel, &lTuple->t_ctid, rTuple);

		/* notifying the front end */

		if (whereToSendOutput == Remote)
		{
			pq_putnchar("A", 1);
			pq_putint(ourpid, sizeof(ourpid));
			pq_putstr(DatumGetName(d)->data);
			pq_flush();
		}
		else
		{
			elog(NOTICE, "Async_NotifyFrontEnd: no asynchronous notification to frontend on interactive sessions");
		}
		ReleaseBuffer(b);
	}
	CommitTransactionCommand();
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
