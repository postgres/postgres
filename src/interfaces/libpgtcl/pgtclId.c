/*-------------------------------------------------------------------------
 *
 * pgtclId.c
 *
 *	Contains Tcl "channel" interface routines, plus useful routines
 *	to convert between strings and pointers.  These are needed because
 *	everything in Tcl is a string, but in C, pointers to data structures
 *	are needed.
 *
 *	ASSUMPTION:  sizeof(long) >= sizeof(void*)
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/libpgtcl/Attic/pgtclId.c,v 1.43 2003/08/04 02:40:16 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <errno.h>

#include "pgtclCmds.h"
#include "pgtclId.h"


static int
PgEndCopy(Pg_ConnectionId * connid, int *errorCodePtr)
{
	connid->res_copyStatus = RES_COPY_NONE;
	if (PQendcopy(connid->conn))
	{
		PQclear(connid->results[connid->res_copy]);
		connid->results[connid->res_copy] =
			PQmakeEmptyPGresult(connid->conn, PGRES_BAD_RESPONSE);
		connid->res_copy = -1;
		*errorCodePtr = EIO;
		return -1;
	}
	else
	{
		PQclear(connid->results[connid->res_copy]);
		connid->results[connid->res_copy] =
			PQmakeEmptyPGresult(connid->conn, PGRES_COMMAND_OK);
		connid->res_copy = -1;
		return 0;
	}
}

/*
 *	Called when reading data (via gets) for a copy <rel> to stdout.
 */
int
PgInputProc(DRIVER_INPUT_PROTO)
{
	Pg_ConnectionId *connid;
	PGconn	   *conn;
	int			avail;

	connid = (Pg_ConnectionId *) cData;
	conn = connid->conn;

	if (connid->res_copy < 0 ||
	 PQresultStatus(connid->results[connid->res_copy]) != PGRES_COPY_OUT)
	{
		*errorCodePtr = EBUSY;
		return -1;
	}

	/*
	 * Read any newly arrived data into libpq's buffer, thereby clearing
	 * the socket's read-ready condition.
	 */
	if (!PQconsumeInput(conn))
	{
		*errorCodePtr = EIO;
		return -1;
	}

	/* Move data from libpq's buffer to Tcl's. */

	avail = PQgetlineAsync(conn, buf, bufSize);

	if (avail < 0)
	{
		/* Endmarker detected, change state and return 0 */
		return PgEndCopy(connid, errorCodePtr);
	}

	return avail;
}

/*
 *	Called when writing data (via puts) for a copy <rel> from stdin
 */
int
PgOutputProc(DRIVER_OUTPUT_PROTO)
{
	Pg_ConnectionId *connid;
	PGconn	   *conn;

	connid = (Pg_ConnectionId *) cData;
	conn = connid->conn;

	if (connid->res_copy < 0 ||
	  PQresultStatus(connid->results[connid->res_copy]) != PGRES_COPY_IN)
	{
		*errorCodePtr = EBUSY;
		return -1;
	}

	if (PQputnbytes(conn, buf, bufSize))
	{
		*errorCodePtr = EIO;
		return -1;
	}

	/*
	 * This assumes Tcl script will write the terminator line in a single
	 * operation; maybe not such a good assumption?
	 */
	if (bufSize >= 3 && strncmp(&buf[bufSize - 3], "\\.\n", 3) == 0)
	{
		if (PgEndCopy(connid, errorCodePtr) == -1)
			return -1;
	}
	return bufSize;
}

#if HAVE_TCL_GETFILEPROC

Tcl_File
PgGetFileProc(ClientData cData, int direction)
{
	return (Tcl_File) NULL;
}
#endif

/*
 * The WatchProc and GetHandleProc are no-ops but must be present.
 */
static void
PgWatchProc(ClientData instanceData, int mask)
{
}

static int
PgGetHandleProc(ClientData instanceData, int direction,
				ClientData *handlePtr)
{
	return TCL_ERROR;
}

Tcl_ChannelType Pg_ConnType = {
	"pgsql",					/* channel type */
	NULL,						/* blockmodeproc */
	PgDelConnectionId,			/* closeproc */
	PgInputProc,				/* inputproc */
	PgOutputProc,				/* outputproc */
	NULL,						/* SeekProc, Not used */
	NULL,						/* SetOptionProc, Not used */
	NULL,						/* GetOptionProc, Not used */
	PgWatchProc,				/* WatchProc, must be defined */
	PgGetHandleProc,			/* GetHandleProc, must be defined */
	NULL						/* Close2Proc, Not used */
};

/*
 * Create and register a new channel for the connection
 */
void
PgSetConnectionId(Tcl_Interp *interp, PGconn *conn)
{
	Tcl_Channel conn_chan;
	Pg_ConnectionId *connid;
	int			i;

	connid = (Pg_ConnectionId *) ckalloc(sizeof(Pg_ConnectionId));
	connid->conn = conn;
	connid->res_count = 0;
	connid->res_last = -1;
	connid->res_max = RES_START;
	connid->res_hardmax = RES_HARD_MAX;
	connid->res_copy = -1;
	connid->res_copyStatus = RES_COPY_NONE;
	connid->results = (PGresult **) ckalloc(sizeof(PGresult *) * RES_START);
	for (i = 0; i < RES_START; i++)
		connid->results[i] = NULL;
	connid->notify_list = NULL;
	connid->notifier_running = 0;

	sprintf(connid->id, "pgsql%d", PQsocket(conn));

#if TCL_MAJOR_VERSION >= 8
	connid->notifier_channel = Tcl_MakeTcpClientChannel((ClientData) PQsocket(conn));
	Tcl_RegisterChannel(NULL, connid->notifier_channel);
#else
	connid->notifier_socket = -1;
#endif

#if TCL_MAJOR_VERSION == 7 && TCL_MINOR_VERSION == 5
	/* Original signature (only seen in Tcl 7.5) */
	conn_chan = Tcl_CreateChannel(&Pg_ConnType, connid->id, NULL, NULL, (ClientData) connid);
#else
	/* Tcl 7.6 and later use this */
	conn_chan = Tcl_CreateChannel(&Pg_ConnType, connid->id, (ClientData) connid,
								  TCL_READABLE | TCL_WRITABLE);
#endif

	Tcl_SetChannelOption(interp, conn_chan, "-buffering", "line");
	Tcl_SetResult(interp, connid->id, TCL_VOLATILE);
	Tcl_RegisterChannel(interp, conn_chan);
}


/*
 * Get back the connection from the Id
 */
PGconn *
PgGetConnectionId(Tcl_Interp *interp, CONST84 char *id,
				  Pg_ConnectionId ** connid_p)
{
	Tcl_Channel conn_chan;
	Pg_ConnectionId *connid;

	conn_chan = Tcl_GetChannel(interp, id, 0);
	if (conn_chan == NULL || Tcl_GetChannelType(conn_chan) != &Pg_ConnType)
	{
		Tcl_ResetResult(interp);
		Tcl_AppendResult(interp, id, " is not a valid postgresql connection", 0);
		if (connid_p)
			*connid_p = NULL;
		return (PGconn *) NULL;
	}

	connid = (Pg_ConnectionId *) Tcl_GetChannelInstanceData(conn_chan);
	if (connid_p)
		*connid_p = connid;
	return connid->conn;
}


/*
 * Remove a connection Id from the hash table and
 * close all portals the user forgot.
 */
int
PgDelConnectionId(DRIVER_DEL_PROTO)
{
	Tcl_HashEntry *entry;
	Tcl_HashSearch hsearch;
	Pg_ConnectionId *connid;
	Pg_TclNotifies *notifies;
	int			i;

	connid = (Pg_ConnectionId *) cData;

	for (i = 0; i < connid->res_max; i++)
	{
		if (connid->results[i])
			PQclear(connid->results[i]);
	}
	ckfree((void *) connid->results);

	/* Release associated notify info */
	while ((notifies = connid->notify_list) != NULL)
	{
		connid->notify_list = notifies->next;
		for (entry = Tcl_FirstHashEntry(&notifies->notify_hash, &hsearch);
			 entry != NULL;
			 entry = Tcl_NextHashEntry(&hsearch))
			ckfree((char *) Tcl_GetHashValue(entry));
		Tcl_DeleteHashTable(&notifies->notify_hash);
		if (notifies->conn_loss_cmd)
			ckfree((void *) notifies->conn_loss_cmd);
		if (notifies->interp)
			Tcl_DontCallWhenDeleted(notifies->interp, PgNotifyInterpDelete,
									(ClientData) notifies);
		ckfree((void *) notifies);
	}

	/*
	 * Turn off the Tcl event source for this connection, and delete any
	 * pending notify and connection-loss events.
	 */
	PgStopNotifyEventSource(connid, true);

	/* Close the libpq connection too */
	PQfinish(connid->conn);
	connid->conn = NULL;

	/*
	 * Kill the notifier channel, too.	We must not do this until after
	 * we've closed the libpq connection, because Tcl will try to close
	 * the socket itself!
	 *
	 * XXX Unfortunately, while this works fine if we are closing due to
	 * explicit pg_disconnect, all Tcl versions through 8.4.1 dump core if
	 * we try to do it during interpreter shutdown.  Not clear why. For
	 * now, we kill the channel during pg_disconnect, but during interp
	 * shutdown we just accept leakage of the (fairly small) amount of
	 * memory taken for the channel state representation. (Note we are not
	 * leaking a socket, since libpq closed that already.) We tell the
	 * difference between pg_disconnect and interpreter shutdown by
	 * testing for interp != NULL, which is an undocumented but apparently
	 * safe way to tell.
	 */
#if TCL_MAJOR_VERSION >= 8
	if (connid->notifier_channel != NULL && interp != NULL)
		Tcl_UnregisterChannel(NULL, connid->notifier_channel);
#endif

	/*
	 * We must use Tcl_EventuallyFree because we don't want the connid
	 * struct to vanish instantly if Pg_Notify_EventProc is active for it.
	 * (Otherwise, closing the connection from inside a pg_listen callback
	 * could lead to coredump.)  Pg_Notify_EventProc can detect that the
	 * connection has been deleted from under it by checking connid->conn.
	 */
	Tcl_EventuallyFree((ClientData) connid, TCL_DYNAMIC);

	return 0;
}


/*
 * Find a slot for a new result id.  If the table is full, expand it by
 * a factor of 2.  However, do not expand past the hard max, as the client
 * is probably just not clearing result handles like they should.
 */
int
PgSetResultId(Tcl_Interp *interp, CONST84 char *connid_c, PGresult *res)
{
	Tcl_Channel conn_chan;
	Pg_ConnectionId *connid;
	int			resid,
				i;
	char		buf[32];


	conn_chan = Tcl_GetChannel(interp, connid_c, 0);
	if (conn_chan == NULL)
		return TCL_ERROR;
	connid = (Pg_ConnectionId *) Tcl_GetChannelInstanceData(conn_chan);

	/* search, starting at slot after the last one used */
	resid = connid->res_last;
	for (;;)
	{
		/* advance, with wraparound */
		if (++resid >= connid->res_max)
			resid = 0;
		/* this slot empty? */
		if (!connid->results[resid])
		{
			connid->res_last = resid;
			break;				/* success exit */
		}
		/* checked all slots? */
		if (resid == connid->res_last)
			break;				/* failure exit */
	}

	if (connid->results[resid])
	{
		/* no free slot found, so try to enlarge array */
		if (connid->res_max >= connid->res_hardmax)
		{
			Tcl_SetResult(interp, "hard limit on result handles reached",
						  TCL_STATIC);
			return TCL_ERROR;
		}
		connid->res_last = resid = connid->res_max;
		connid->res_max *= 2;
		if (connid->res_max > connid->res_hardmax)
			connid->res_max = connid->res_hardmax;
		connid->results = (PGresult **) ckrealloc((void *) connid->results,
								   sizeof(PGresult *) * connid->res_max);
		for (i = connid->res_last; i < connid->res_max; i++)
			connid->results[i] = NULL;
	}

	connid->results[resid] = res;
	sprintf(buf, "%s.%d", connid_c, resid);
	Tcl_SetResult(interp, buf, TCL_VOLATILE);
	return resid;
}

static int
getresid(Tcl_Interp *interp, CONST84 char *id, Pg_ConnectionId ** connid_p)
{
	Tcl_Channel conn_chan;
	char	   *mark;
	int			resid;
	Pg_ConnectionId *connid;

	if (!(mark = strchr(id, '.')))
		return -1;
	*mark = '\0';
	conn_chan = Tcl_GetChannel(interp, id, 0);
	*mark = '.';
	if (conn_chan == NULL || Tcl_GetChannelType(conn_chan) != &Pg_ConnType)
	{
		Tcl_SetResult(interp, "Invalid connection handle", TCL_STATIC);
		return -1;
	}

	if (Tcl_GetInt(interp, mark + 1, &resid) == TCL_ERROR)
	{
		Tcl_SetResult(interp, "Poorly formated result handle", TCL_STATIC);
		return -1;
	}

	connid = (Pg_ConnectionId *) Tcl_GetChannelInstanceData(conn_chan);

	if (resid < 0 || resid >= connid->res_max || connid->results[resid] == NULL)
	{
		Tcl_SetResult(interp, "Invalid result handle", TCL_STATIC);
		return -1;
	}

	*connid_p = connid;

	return resid;
}


/*
 * Get back the result pointer from the Id
 */
PGresult *
PgGetResultId(Tcl_Interp *interp, CONST84 char *id)
{
	Pg_ConnectionId *connid;
	int			resid;

	if (!id)
		return NULL;
	resid = getresid(interp, id, &connid);
	if (resid == -1)
		return NULL;
	return connid->results[resid];
}


/*
 * Remove a result Id from the hash tables
 */
void
PgDelResultId(Tcl_Interp *interp, CONST84 char *id)
{
	Pg_ConnectionId *connid;
	int			resid;

	resid = getresid(interp, id, &connid);
	if (resid == -1)
		return;
	connid->results[resid] = 0;
}


/*
 * Get the connection Id from the result Id
 */
int
PgGetConnByResultId(Tcl_Interp *interp, CONST84 char *resid_c)
{
	char	   *mark;
	Tcl_Channel conn_chan;

	if (!(mark = strchr(resid_c, '.')))
		goto error_out;
	*mark = '\0';
	conn_chan = Tcl_GetChannel(interp, resid_c, 0);
	*mark = '.';
	if (conn_chan && Tcl_GetChannelType(conn_chan) == &Pg_ConnType)
	{
		Tcl_SetResult(interp, (char *) Tcl_GetChannelName(conn_chan),
					  TCL_VOLATILE);
		return TCL_OK;
	}

error_out:
	Tcl_ResetResult(interp);
	Tcl_AppendResult(interp, resid_c, " is not a valid connection\n", 0);
	return TCL_ERROR;
}




/*-------------------------------------------
  Notify event source

  These functions allow asynchronous notify messages arriving from
  the SQL server to be dispatched as Tcl events.  See the Tcl
  Notifier(3) man page for more info.

  The main trick in this code is that we have to cope with status changes
  between the queueing and the execution of a Tcl event.  For example,
  if the user changes or cancels the pg_listen callback command, we should
  use the new setting; we do that by not resolving the notify relation
  name until the last possible moment.
  We also have to handle closure of the channel or deletion of the interpreter
  to be used for the callback (note that with multiple interpreters,
  the channel can outlive the interpreter it was created by!)
  Upon closure of the channel, we immediately delete the file event handler
  for it, which has the effect of disabling any file-ready events that might
  be hanging about in the Tcl event queue.	But for interpreter deletion,
  we just set any matching interp pointers in the Pg_TclNotifies list to NULL.
  The list item stays around until the connection is deleted.  (This avoids
  trouble with walking through a list whose members may get deleted under us.)

  Another headache is that Ousterhout keeps changing the Tcl I/O interfaces.
  libpgtcl currently claims to work with Tcl 7.5, 7.6, and 8.0, and each of
  'em is different.  Worse, the Tcl_File type went away in 8.0, which means
  there is no longer any platform-independent way of waiting for file ready.
  So we now have to use a Unix-specific interface.	Grumble.

  In the current design, Pg_Notify_FileHandler is a file handler that
  we establish by calling Tcl_CreateFileHandler().	It gets invoked from
  the Tcl event loop whenever the underlying PGconn's socket is read-ready.
  We suck up any available data (to clear the OS-level read-ready condition)
  and then transfer any available PGnotify events into the Tcl event queue.
  Eventually these events will be dispatched to Pg_Notify_EventProc.  When
  we do an ordinary PQexec, we must also transfer PGnotify events into Tcl's
  event queue, since libpq might have read them when we weren't looking.
  ------------------------------------------*/

typedef struct
{
	Tcl_Event	header;			/* Standard Tcl event info */
	PGnotify   *notify;			/* Notify event from libpq, or NULL */
	/* We use a NULL notify pointer to denote a connection-loss event */
	Pg_ConnectionId *connid;	/* Connection for server */
}	NotifyEvent;

/* Dispatch a NotifyEvent that has reached the front of the event queue */

static int
Pg_Notify_EventProc(Tcl_Event *evPtr, int flags)
{
	NotifyEvent *event = (NotifyEvent *) evPtr;
	Pg_TclNotifies *notifies;
	char	   *callback;
	char	   *svcallback;

	/* We classify SQL notifies as Tcl file events. */
	if (!(flags & TCL_FILE_EVENTS))
		return 0;

	/* If connection's been closed, just forget the whole thing. */
	if (event->connid == NULL)
	{
		if (event->notify)
			PQfreemem(event->notify);
		return 1;
	}

	/*
	 * Preserve/Release to ensure the connection struct doesn't disappear
	 * underneath us.
	 */
	Tcl_Preserve((ClientData) event->connid);

	/*
	 * Loop for each interpreter that has ever registered on the
	 * connection. Each one can get a callback.
	 */

	for (notifies = event->connid->notify_list;
		 notifies != NULL;
		 notifies = notifies->next)
	{
		Tcl_Interp *interp = notifies->interp;

		if (interp == NULL)
			continue;			/* ignore deleted interpreter */

		/*
		 * Find the callback to be executed for this interpreter, if any.
		 */
		if (event->notify)
		{
			/* Ordinary NOTIFY event */
			Tcl_HashEntry *entry;

			entry = Tcl_FindHashEntry(&notifies->notify_hash,
									  event->notify->relname);
			if (entry == NULL)
				continue;		/* no pg_listen in this interpreter */
			callback = (char *) Tcl_GetHashValue(entry);
		}
		else
		{
			/* Connection-loss event */
			callback = notifies->conn_loss_cmd;
		}

		if (callback == NULL)
			continue;			/* nothing to do for this interpreter */

		/*
		 * We have to copy the callback string in case the user executes a
		 * new pg_listen or pg_on_connection_loss during the callback.
		 */
		svcallback = (char *) ckalloc((unsigned) (strlen(callback) + 1));
		strcpy(svcallback, callback);

		/*
		 * Execute the callback.
		 */
		Tcl_Preserve((ClientData) interp);
		if (Tcl_GlobalEval(interp, svcallback) != TCL_OK)
		{
			if (event->notify)
				Tcl_AddErrorInfo(interp, "\n    (\"pg_listen\" script)");
			else
				Tcl_AddErrorInfo(interp, "\n    (\"pg_on_connection_loss\" script)");
			Tcl_BackgroundError(interp);
		}
		Tcl_Release((ClientData) interp);
		ckfree(svcallback);

		/*
		 * Check for the possibility that the callback closed the
		 * connection.
		 */
		if (event->connid->conn == NULL)
			break;
	}

	Tcl_Release((ClientData) event->connid);

	if (event->notify)
		PQfreemem(event->notify);

	return 1;
}

/*
 * Transfer any notify events available from libpq into the Tcl event queue.
 * Note that this must be called after each PQexec (to capture notifies
 * that arrive during command execution) as well as in Pg_Notify_FileHandler
 * (to capture notifies that arrive when we're idle).
 */

void
PgNotifyTransferEvents(Pg_ConnectionId * connid)
{
	PGnotify   *notify;

	while ((notify = PQnotifies(connid->conn)) != NULL)
	{
		NotifyEvent *event = (NotifyEvent *) ckalloc(sizeof(NotifyEvent));

		event->header.proc = Pg_Notify_EventProc;
		event->notify = notify;
		event->connid = connid;
		Tcl_QueueEvent((Tcl_Event *) event, TCL_QUEUE_TAIL);
	}

	/*
	 * This is also a good place to check for unexpected closure of the
	 * connection (ie, backend crash), in which case we must shut down the
	 * notify event source to keep Tcl from trying to select() on the now-
	 * closed socket descriptor.  But don't kill on-connection-loss
	 * events; in fact, register one.
	 */
	if (PQsocket(connid->conn) < 0)
		PgConnLossTransferEvents(connid);
}

/*
 * Handle a connection-loss event
 */
void
PgConnLossTransferEvents(Pg_ConnectionId * connid)
{
	if (connid->notifier_running)
	{
		/* Put the on-connection-loss event in the Tcl queue */
		NotifyEvent *event = (NotifyEvent *) ckalloc(sizeof(NotifyEvent));

		event->header.proc = Pg_Notify_EventProc;
		event->notify = NULL;
		event->connid = connid;
		Tcl_QueueEvent((Tcl_Event *) event, TCL_QUEUE_TAIL);
	}

	/*
	 * Shut down the notify event source to keep Tcl from trying to
	 * select() on the now-closed socket descriptor.  And zap any
	 * unprocessed notify events ... but not, of course, the
	 * connection-loss event.
	 */
	PgStopNotifyEventSource(connid, false);
}

/*
 * Cleanup code for coping when an interpreter or a channel is deleted.
 *
 * PgNotifyInterpDelete is registered as an interpreter deletion callback
 * for each extant Pg_TclNotifies structure.
 * NotifyEventDeleteProc is used by PgStopNotifyEventSource to cancel
 * pending Tcl NotifyEvents that reference a dying connection.
 */

void
PgNotifyInterpDelete(ClientData clientData, Tcl_Interp *interp)
{
	/* Mark the interpreter dead, but don't do anything else yet */
	Pg_TclNotifies *notifies = (Pg_TclNotifies *) clientData;

	notifies->interp = NULL;
}

/*
 * Comparison routines for detecting events to be removed by Tcl_DeleteEvents.
 * NB: In (at least) Tcl versions 7.6 through 8.0.3, there is a serious
 * bug in Tcl_DeleteEvents: if there are multiple events on the queue and
 * you tell it to delete the last one, the event list pointers get corrupted,
 * with the result that events queued immediately thereafter get lost.
 * Therefore we daren't tell Tcl_DeleteEvents to actually delete anything!
 * We simply use it as a way of scanning the event queue.  Events matching
 * the about-to-be-deleted connid are marked dead by setting their connid
 * fields to NULL.	Then Pg_Notify_EventProc will do nothing when those
 * events are executed.
 */
static int
NotifyEventDeleteProc(Tcl_Event *evPtr, ClientData clientData)
{
	Pg_ConnectionId *connid = (Pg_ConnectionId *) clientData;

	if (evPtr->proc == Pg_Notify_EventProc)
	{
		NotifyEvent *event = (NotifyEvent *) evPtr;

		if (event->connid == connid && event->notify != NULL)
			event->connid = NULL;
	}
	return 0;
}

/* This version deletes on-connection-loss events too */
static int
AllNotifyEventDeleteProc(Tcl_Event *evPtr, ClientData clientData)
{
	Pg_ConnectionId *connid = (Pg_ConnectionId *) clientData;

	if (evPtr->proc == Pg_Notify_EventProc)
	{
		NotifyEvent *event = (NotifyEvent *) evPtr;

		if (event->connid == connid)
			event->connid = NULL;
	}
	return 0;
}

/*
 * File handler callback: called when Tcl has detected read-ready on socket.
 * The clientData is a pointer to the associated connection.
 * We can ignore the condition mask since we only ever ask about read-ready.
 */

static void
Pg_Notify_FileHandler(ClientData clientData, int mask)
{
	Pg_ConnectionId *connid = (Pg_ConnectionId *) clientData;

	/*
	 * Consume any data available from the SQL server (this just buffers
	 * it internally to libpq; but it will clear the read-ready
	 * condition).
	 */
	if (PQconsumeInput(connid->conn))
	{
		/* Transfer notify events from libpq to Tcl event queue. */
		PgNotifyTransferEvents(connid);
	}
	else
	{
		/*
		 * If there is no input but we have read-ready, assume this means
		 * we lost the connection.
		 */
		PgConnLossTransferEvents(connid);
	}
}


/*
 * Start and stop the notify event source for a connection.
 *
 * We do not bother to run the notifier unless at least one pg_listen
 * or pg_on_connection_loss has been executed on the connection.  Currently,
 * once started the notifier is run until the connection is closed.
 *
 * FIXME: if PQreset is executed on the underlying PGconn, the active
 * socket number could change.	How and when should we test for this
 * and update the Tcl file handler linkage?  (For that matter, we'd
 * also have to reissue LISTEN commands for active LISTENs, since the
 * new backend won't know about 'em.  I'm leaving this problem for
 * another day.)
 */

void
PgStartNotifyEventSource(Pg_ConnectionId * connid)
{
	/* Start the notify event source if it isn't already running */
	if (!connid->notifier_running)
	{
		int			pqsock = PQsocket(connid->conn);

		if (pqsock >= 0)
		{
#if TCL_MAJOR_VERSION >= 8
			Tcl_CreateChannelHandler(connid->notifier_channel,
									 TCL_READABLE,
									 Pg_Notify_FileHandler,
									 (ClientData) connid);
#else
			/* In Tcl 7.5 and 7.6, we need to gin up a Tcl_File. */
			Tcl_File	tclfile = Tcl_GetFile((ClientData) pqsock, TCL_UNIX_FD);

			Tcl_CreateFileHandler(tclfile, TCL_READABLE,
							 Pg_Notify_FileHandler, (ClientData) connid);
			connid->notifier_socket = pqsock;
#endif
			connid->notifier_running = 1;
		}
	}
}

void
PgStopNotifyEventSource(Pg_ConnectionId * connid, bool allevents)
{
	/* Remove the event source */
	if (connid->notifier_running)
	{
#if TCL_MAJOR_VERSION >= 8
		Tcl_DeleteChannelHandler(connid->notifier_channel,
								 Pg_Notify_FileHandler,
								 (ClientData) connid);
#else
		/* In Tcl 7.5 and 7.6, we need to gin up a Tcl_File. */
		Tcl_File	tclfile = Tcl_GetFile((ClientData) connid->notifier_socket,
										  TCL_UNIX_FD);

		Tcl_DeleteFileHandler(tclfile);
#endif
		connid->notifier_running = 0;
	}

	/* Kill queued Tcl events that reference this channel */
	if (allevents)
		Tcl_DeleteEvents(AllNotifyEventDeleteProc, (ClientData) connid);
	else
		Tcl_DeleteEvents(NotifyEventDeleteProc, (ClientData) connid);
}
