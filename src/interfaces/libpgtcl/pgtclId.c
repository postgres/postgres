/*-------------------------------------------------------------------------
 *
 * pgtclId.c--
 *	  useful routines to convert between strings and pointers
 *	Needed because everything in tcl is a string, but we want pointers
 *	to data structures
 *
 *	ASSUMPTION:  sizeof(long) >= sizeof(void*)
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/libpgtcl/Attic/pgtclId.c,v 1.15 1998/09/03 02:10:44 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <tcl.h>

#include "postgres.h"
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
 *  Called when reading data (via gets) for a copy <rel> to stdout.
 */
int PgInputProc(DRIVER_INPUT_PROTO)
{
    Pg_ConnectionId	*connid;
    PGconn		*conn;
    int			avail;

    connid = (Pg_ConnectionId *)cData;
    conn = connid->conn;

    if (connid->res_copy < 0 ||
	PQresultStatus(connid->results[connid->res_copy]) != PGRES_COPY_OUT)
	{
		*errorCodePtr = EBUSY;
		return -1;
    }

    /* Read any newly arrived data into libpq's buffer,
     * thereby clearing the socket's read-ready condition.
     */
    if (! PQconsumeInput(conn))
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

Tcl_ChannelType Pg_ConnType = {
	"pgsql",					/* channel type */
	NULL,						/* blockmodeproc */
	PgDelConnectionId,			/* closeproc */
	PgInputProc,				/* inputproc */
	PgOutputProc,				/* outputproc */

	/*
	 * Note the additional stuff can be left NULL, or is initialized
	 * during a PgSetConnectionId
	 */
};

/*
 * Create and register a new channel for the connection
 */
void
PgSetConnectionId(Tcl_Interp * interp, PGconn *conn)
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
PgGetConnectionId(Tcl_Interp * interp, char *id, Pg_ConnectionId ** connid_p)
{
	Tcl_Channel conn_chan;
	Pg_ConnectionId *connid;

	conn_chan = Tcl_GetChannel(interp, id, 0);
	if (conn_chan == NULL || Tcl_GetChannelType(conn_chan) != &Pg_ConnType)
	{
		Tcl_ResetResult(interp);
		Tcl_AppendResult(interp, id, " is not a valid postgresql connection", 0);
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
		Tcl_DontCallWhenDeleted(notifies->interp, PgNotifyInterpDelete,
								(ClientData) notifies);
		ckfree((void *) notifies);
	}

	/*
	 * Turn off the Tcl event source for this connection, and delete any
	 * pending notify events.
	 */
	PgStopNotifyEventSource(connid);

	/* Close the libpq connection too */
	PQfinish(connid->conn);
	connid->conn = NULL;

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
PgSetResultId(Tcl_Interp * interp, char *connid_c, PGresult *res)
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

	for (resid = connid->res_last + 1; resid != connid->res_last; resid++)
	{
		if (resid == connid->res_max)
			resid = 0;
		if (!connid->results[resid])
		{
			connid->res_last = resid;
			break;
		}
	}

	if (connid->results[resid])
	{
		if (connid->res_max == connid->res_hardmax)
		{
			Tcl_SetResult(interp, "hard limit on result handles reached",
						  TCL_STATIC);
			return TCL_ERROR;
		}
		connid->res_last = connid->res_max;
		resid = connid->res_max;
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
getresid(Tcl_Interp * interp, char *id, Pg_ConnectionId ** connid_p)
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
PGresult   *
PgGetResultId(Tcl_Interp * interp, char *id)
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
PgDelResultId(Tcl_Interp * interp, char *id)
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
PgGetConnByResultId(Tcl_Interp * interp, char *resid_c)
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
		Tcl_SetResult(interp, Tcl_GetChannelName(conn_chan), TCL_VOLATILE);
		return TCL_OK;
	}

error_out:
	Tcl_ResetResult(interp);
	Tcl_AppendResult(interp, resid_c, " is not a valid connection\n", 0);
	return TCL_ERROR;
}




/********************************************
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
  Upon closure of the channel, we immediately delete any pending events
  that reference it.  But for interpreter deletion, we just set any
  matching interp pointers in the Pg_TclNotifies list to NULL.	The
  list item stays around until the connection is deleted.  (This avoids
  trouble with walking through a list whose members may get deleted under us.)
  *******************************************/

typedef struct
{
	Tcl_Event	header;			/* Standard Tcl event info */
	PGnotify	info;			/* Notify name from SQL server */
	Pg_ConnectionId *connid;	/* Connection for server */
}			NotifyEvent;

/* Setup before waiting in event loop */

static void
Pg_Notify_SetupProc(ClientData clientData, int flags)
{
	Pg_ConnectionId *connid = (Pg_ConnectionId *) clientData;
	Tcl_File	handle;
	int			pqsock;

	/* We classify SQL notifies as Tcl file events. */
	if (!(flags & TCL_FILE_EVENTS))
		return;

	/* Set up to watch for asynchronous data arrival on backend channel */
	pqsock = PQsocket(connid->conn);
	if (pqsock < 0)
		return;

	handle = Tcl_GetFile((ClientData) pqsock, TCL_UNIX_FD);
	Tcl_WatchFile(handle, TCL_READABLE);
}

/* Check to see if events have arrived in event loop */

static void
Pg_Notify_CheckProc(ClientData clientData, int flags)
{
	Pg_ConnectionId *connid = (Pg_ConnectionId *) clientData;
	Tcl_File	handle;
	int			pqsock;

	/* We classify SQL notifies as Tcl file events. */
	if (!(flags & TCL_FILE_EVENTS))
		return;

	/*
	 * Consume any data available from the SQL server (this just buffers
	 * it internally to libpq). We use Tcl_FileReady to avoid a useless
	 * kernel call when no data is available.
	 */
	pqsock = PQsocket(connid->conn);
	if (pqsock < 0)
		return;

	handle = Tcl_GetFile((ClientData) pqsock, TCL_UNIX_FD);
	if (Tcl_FileReady(handle, TCL_READABLE) != 0)
		PQconsumeInput(connid->conn);

	/* Transfer notify events from libpq to Tcl event queue. */
	PgNotifyTransferEvents(connid);
}

/* Dispatch an event that has reached the front of the event queue */

static int
Pg_Notify_EventProc(Tcl_Event * evPtr, int flags)
{
	NotifyEvent *event = (NotifyEvent *) evPtr;
	Pg_TclNotifies *notifies;
	Tcl_HashEntry *entry;
	char	   *callback;
	char	   *svcallback;

	/* We classify SQL notifies as Tcl file events. */
	if (!(flags & TCL_FILE_EVENTS))
		return 0;

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
		entry = Tcl_FindHashEntry(&notifies->notify_hash,
								  event->info.relname);
		if (entry == NULL)
			continue;			/* no pg_listen in this interpreter */
		callback = (char *) Tcl_GetHashValue(entry);
		if (callback == NULL)
			continue;			/* safety check -- shouldn't happen */

		/*
		 * We have to copy the callback string in case the user executes a
		 * new pg_listen during the callback.
		 */
		svcallback = (char *) ckalloc((unsigned) (strlen(callback) + 1));
		strcpy(svcallback, callback);

		/*
		 * Execute the callback.
		 */
		Tcl_Preserve((ClientData) interp);
		if (Tcl_GlobalEval(interp, svcallback) != TCL_OK)
		{
			Tcl_AddErrorInfo(interp, "\n    (\"pg_listen\" script)");
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

	return 1;
}

/*
 * Transfer any notify events available from libpq into the Tcl event queue.
 * Note that this must be called after each PQexec (to capture notifies
 * that arrive during command execution) as well as in Pg_Notify_CheckProc
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
		event->info = *notify;
		event->connid = connid;
		Tcl_QueueEvent((Tcl_Event *) event, TCL_QUEUE_TAIL);
		free(notify);
	}
}

/*
 * Cleanup code for coping when an interpreter or a channel is deleted.
 *
 * PgNotifyInterpDelete is registered as an interpreter deletion callback
 * for each extant Pg_TclNotifies structure.
 * NotifyEventDeleteProc is used by PgStopNotifyEventSource to get
 * rid of pending Tcl events that reference a dying connection.
 */

void
PgNotifyInterpDelete(ClientData clientData, Tcl_Interp * interp)
{
	/* Mark the interpreter dead, but don't do anything else yet */
	Pg_TclNotifies *notifies = (Pg_TclNotifies *) clientData;

	notifies->interp = NULL;
}

/* Comparison routine for detecting events to be removed by DeleteEvent */
static int
NotifyEventDeleteProc(Tcl_Event * evPtr, ClientData clientData)
{
	NotifyEvent *event;
	Pg_ConnectionId *connid = (Pg_ConnectionId *) clientData;

	if (evPtr->proc != Pg_Notify_EventProc)
		return 0;
	event = (NotifyEvent *) evPtr;
	if (event->connid != connid)
		return 0;
	return 1;
}

/* Start and stop the notify event source for a connection.
 * We do not bother to run the notifier unless at least one
 * pg_listen has been executed on the connection.  Currently,
 * once started the notifier is run until the connection is
 * closed.
 */

void
PgStartNotifyEventSource(Pg_ConnectionId * connid)
{
	/* Start the notify event source if it isn't already running */
	if (!connid->notifier_running)
	{
		Tcl_CreateEventSource(Pg_Notify_SetupProc, Pg_Notify_CheckProc,
							  (ClientData) connid);
		connid->notifier_running = 1;
	}
}

void
PgStopNotifyEventSource(Pg_ConnectionId * connid)
{
	/* Remove the event source */
	if (connid->notifier_running)
	{
		Tcl_DeleteEventSource(Pg_Notify_SetupProc, Pg_Notify_CheckProc,
							  (ClientData) connid);
		connid->notifier_running = 0;
	}
	/* Kill any queued Tcl events that reference this channel */
	Tcl_DeleteEvents(NotifyEventDeleteProc, (ClientData) connid);
}
