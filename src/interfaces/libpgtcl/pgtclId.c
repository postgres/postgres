/*-------------------------------------------------------------------------
 *
 * pgtclId.c--
 *    useful routines to convert between strings and pointers
 *  Needed because everything in tcl is a string, but we want pointers
 *  to data structures
 *
 *  ASSUMPTION:  sizeof(long) >= sizeof(void*)
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/libpgtcl/Attic/pgtclId.c,v 1.10 1998/05/06 23:53:30 momjian Exp $
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

int PgEndCopy(Pg_ConnectionId *connid, int *errorCodePtr)
{
    connid->res_copyStatus = RES_COPY_NONE;
    if (PQendcopy(connid->conn)) {
	connid->results[connid->res_copy]->resultStatus = PGRES_BAD_RESPONSE;
	connid->res_copy = -1;
	*errorCodePtr = EIO;
	return -1;
    } else {
	connid->results[connid->res_copy]->resultStatus = PGRES_COMMAND_OK;
	connid->res_copy = -1;
	return 0;
    }
}

/*
 *  Called when reading data (via gets) for a copy <rel> to stdout
 */
int PgInputProc(DRIVER_INPUT_PROTO)
{
    Pg_ConnectionId	*connid;
    PGconn		*conn;
    char		c;
    int			avail;

    connid = (Pg_ConnectionId *)cData;
    conn = connid->conn;

    if (connid->res_copy < 0 ||
      connid->results[connid->res_copy]->resultStatus != PGRES_COPY_OUT) {
	*errorCodePtr = EBUSY;
	return -1;
    }

    /* Try to load any newly arrived data */
    errno = 0;

    if (pqReadData(conn) < 0) {
	*errorCodePtr = errno ? errno : EIO;
	return -1;
    }

    /* Move data from libpq's buffer to Tcl's.
     * We want to accept data only in units of whole lines,
     * not partial lines.  This ensures that we can recognize
     * the terminator line "\\.\n".  (Otherwise, if it happened
     * to cross a packet/buffer boundary, we might hand the first
     * one or two characters off to Tcl, which we shouldn't.)
     */

    conn->inCursor = conn->inStart;

    avail = bufSize;
    while (avail > 0 &&
	   pqGetc(&c, conn) == 0) {
	*buf++ = c;
	--avail;
	if (c == '\n') {
	    /* Got a complete line; mark the data removed from libpq */
	    conn->inStart = conn->inCursor;
	    /* Is it the endmarker line? */
	    if (bufSize-avail == 3 && buf[-3] == '\\' && buf[-2] == '.') {
		/* Yes, change state and return 0 */
		return PgEndCopy(connid, errorCodePtr);
	    }
	    /* No, return the data to Tcl */
	    /* fprintf(stderr, "returning %d chars\n", bufSize - avail); */
	    return bufSize - avail;
	}
    }

    /* We don't have a complete line.
     * We'd prefer to leave it in libpq's buffer until the rest arrives,
     * but there is a special case: what if the line is longer than the
     * buffer Tcl is offering us?  In that case we'd better hand over
     * a partial line, else we'd get into an infinite loop.
     * Do this in a way that ensures we can't misrecognize a terminator
     * line later: leave last 3 characters in libpq buffer.
     */
    if (avail == 0 && bufSize > 3) {
	conn->inStart = conn->inCursor - 3;
	return bufSize - 3;
    }
    return 0;
}

/*
 *  Called when writing data (via puts) for a copy <rel> from stdin
 */
int PgOutputProc(DRIVER_OUTPUT_PROTO)
{
    Pg_ConnectionId	*connid;
    PGconn		*conn;

    connid = (Pg_ConnectionId *)cData;
    conn = connid->conn;

    if (connid->res_copy < 0 ||
      connid->results[connid->res_copy]->resultStatus != PGRES_COPY_IN) {
	*errorCodePtr = EBUSY;
	return -1;
    }

    errno = 0;

    if (pqPutnchar(buf, bufSize, conn)) {
	*errorCodePtr = errno ? errno : EIO;
	return -1;
    }

    /* This assumes Tcl script will write the terminator line
     * in a single operation; maybe not such a good assumption?
     */
    if (bufSize >= 3 && strncmp(&buf[bufSize-3], "\\.\n", 3) == 0) {
	(void) pqFlush(conn);
	if (PgEndCopy(connid, errorCodePtr) == -1)
	    return -1;
    }
    return bufSize;
}

#if (TCL_MAJOR_VERSION == 7 && TCL_MINOR_VERSION == 6)
Tcl_File
PgGetFileProc(ClientData cData, int direction)
{
    return (Tcl_File)NULL;
}
#endif

Tcl_ChannelType Pg_ConnType = {
    "pgsql",			/* channel type */
    NULL,			/* blockmodeproc */
    PgDelConnectionId,		/* closeproc */
    PgInputProc,		/* inputproc */
    PgOutputProc,		/* outputproc */
    /*  Note the additional stuff can be left NULL,
	or is initialized during a PgSetConnectionId */
};

/*
 * Create and register a new channel for the connection
 */
void
PgSetConnectionId(Tcl_Interp *interp, PGconn *conn)
{
    Tcl_Channel		conn_chan;
    Pg_ConnectionId	*connid;
    int			i;

    connid = (Pg_ConnectionId *)ckalloc(sizeof(Pg_ConnectionId));
    connid->conn = conn;
    connid->res_count = 0;
    connid->res_last = -1;
    connid->res_max = RES_START;
    connid->res_hardmax = RES_HARD_MAX;
    connid->res_copy = -1;
    connid->res_copyStatus = RES_COPY_NONE;
    connid->results = (PGresult**)ckalloc(sizeof(PGresult*) * RES_START);
    for (i = 0; i < RES_START; i++) connid->results[i] = NULL;
    Tcl_InitHashTable(&connid->notify_hash, TCL_STRING_KEYS);

    sprintf(connid->id, "pgsql%d", PQsocket(conn));

#if TCL_MAJOR_VERSION == 7 && TCL_MINOR_VERSION == 5
    conn_chan = Tcl_CreateChannel(&Pg_ConnType, connid->id, NULL, NULL, (ClientData)connid);
#else
    conn_chan = Tcl_CreateChannel(&Pg_ConnType, connid->id, (ClientData)connid,
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
PgGetConnectionId(Tcl_Interp *interp, char *id, Pg_ConnectionId **connid_p)
{
    Tcl_Channel conn_chan;
    Pg_ConnectionId	*connid;

    conn_chan = Tcl_GetChannel(interp, id, 0);
    if(conn_chan == NULL || Tcl_GetChannelType(conn_chan) != &Pg_ConnType) {
	Tcl_ResetResult(interp);
	Tcl_AppendResult(interp, id, " is not a valid postgresql connection\n", 0);
        return (PGconn *)NULL;
    }

    connid = (Pg_ConnectionId *)Tcl_GetChannelInstanceData(conn_chan);
    if (connid_p)
	*connid_p = connid;
    return connid->conn;
}


/*
 * Remove a connection Id from the hash table and
 * close all portals the user forgot.
 */
int PgDelConnectionId(DRIVER_DEL_PROTO)
{
    Tcl_HashEntry	*entry;
    char		*hval;
    Tcl_HashSearch	hsearch;
    Pg_ConnectionId	*connid;
    int			i;

    connid = (Pg_ConnectionId *)cData;

    for (i = 0; i < connid->res_max; i++) {
	if (connid->results[i])
	    PQclear(connid->results[i]);
    }
    ckfree((void*)connid->results);

    for (entry = Tcl_FirstHashEntry(&(connid->notify_hash), &hsearch);
	entry != NULL;
	entry = Tcl_NextHashEntry(&hsearch))
    {
	hval = (char*)Tcl_GetHashValue(entry);
	ckfree(hval);
    }
    
    Tcl_DeleteHashTable(&connid->notify_hash);
    PQfinish(connid->conn);
    ckfree((void*)connid);
    return 0;
}


/*
 * Find a slot for a new result id.  If the table is full, expand it by
 * a factor of 2.  However, do not expand past the hard max, as the client
 * is probably just not clearing result handles like they should.
 */
int
PgSetResultId(Tcl_Interp *interp, char *connid_c, PGresult *res)
{
    Tcl_Channel		conn_chan;
    Pg_ConnectionId	*connid;
    int			resid, i;
    char		buf[32];


    conn_chan = Tcl_GetChannel(interp, connid_c, 0);
    if(conn_chan == NULL)
        return TCL_ERROR;
    connid = (Pg_ConnectionId *)Tcl_GetChannelInstanceData(conn_chan);

    for (resid = connid->res_last+1; resid != connid->res_last; resid++) {
	if (resid == connid->res_max)
	    resid = 0;
	if (!connid->results[resid])
	{
	    connid->res_last = resid;
	    break;
	}
    }

    if (connid->results[resid]) {
	if (connid->res_max == connid->res_hardmax) {
	    Tcl_SetResult(interp, "hard limit on result handles reached",
		TCL_STATIC);
	    return TCL_ERROR;
	}
	connid->res_last = connid->res_max;
	resid = connid->res_max;
	connid->res_max *= 2;
	if (connid->res_max > connid->res_hardmax)
	    connid->res_max = connid->res_hardmax;
	connid->results = (PGresult**)ckrealloc((void*)connid->results,
	    sizeof(PGresult*) * connid->res_max);
	for (i = connid->res_last; i < connid->res_max; i++)
	    connid->results[i] = NULL;
    }

    connid->results[resid] = res;
    sprintf(buf, "%s.%d", connid_c, resid);
    Tcl_SetResult(interp, buf, TCL_VOLATILE);
    return resid;
}

static int getresid(Tcl_Interp *interp, char *id, Pg_ConnectionId **connid_p)
{
    Tcl_Channel		conn_chan;
    char		*mark;
    int			resid;
    Pg_ConnectionId	*connid;

    if (!(mark = strchr(id, '.')))
	return -1;
    *mark = '\0';
    conn_chan = Tcl_GetChannel(interp, id, 0);
    *mark = '.';
    if(conn_chan == NULL || Tcl_GetChannelType(conn_chan) != &Pg_ConnType) {
	Tcl_SetResult(interp, "Invalid connection handle", TCL_STATIC);
        return -1;
    }

    if (Tcl_GetInt(interp, mark + 1, &resid) == TCL_ERROR) {
	Tcl_SetResult(interp, "Poorly formated result handle", TCL_STATIC);
	return -1;
    }

    connid = (Pg_ConnectionId *)Tcl_GetChannelInstanceData(conn_chan);

    if (resid < 0 || resid > connid->res_max || connid->results[resid] == NULL) {
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
PgGetResultId(Tcl_Interp *interp, char *id)
{
    Pg_ConnectionId	*connid;
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
PgDelResultId(Tcl_Interp *interp, char *id)
{
    Pg_ConnectionId	*connid;
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
PgGetConnByResultId(Tcl_Interp *interp, char *resid_c)
{
    char		*mark;
    Tcl_Channel		conn_chan;

    if (!(mark = strchr(resid_c, '.')))
	goto error_out;
    *mark = '\0';
    conn_chan = Tcl_GetChannel(interp, resid_c, 0);
    *mark = '.';
    if(conn_chan && Tcl_GetChannelType(conn_chan) != &Pg_ConnType) {
	Tcl_SetResult(interp, Tcl_GetChannelName(conn_chan), TCL_VOLATILE);
	return TCL_OK;
    }

  error_out:
    Tcl_ResetResult(interp);
    Tcl_AppendResult(interp, resid_c, " is not a valid connection\n", 0);
    return TCL_ERROR;
}


