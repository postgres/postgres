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
 *    $Header: /cvsroot/pgsql/src/interfaces/libpgtcl/Attic/pgtclId.c,v 1.2 1996/10/30 06:18:41 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <stdlib.h>
#include <string.h>
#include "tcl.h"

#include "pgtclCmds.h"
#include "pgtclId.h"

/*
 * Create the Id for a new connection and hash it
 */
void
PgSetConnectionId(Pg_clientData *cd, char *id, PGconn *conn)
{
    Tcl_HashEntry	*hent;
    Pg_ConnectionId	*connid;
    int			hnew;

    connid = (Pg_ConnectionId *)ckalloc(sizeof(Pg_ConnectionId));
    connid->conn = conn;
    Tcl_InitHashTable(&(connid->res_hash), TCL_STRING_KEYS);
    sprintf(connid->id, "pgc%ld", cd->dbh_count++);
    strcpy(id, connid->id);

    hent = Tcl_CreateHashEntry(&(cd->dbh_hash), connid->id, &hnew);
    Tcl_SetHashValue(hent, (ClientData)connid);
}


/*
 * Get back the connection from the Id
 */
PGconn *
PgGetConnectionId(Pg_clientData *cd, char *id)
{
    Tcl_HashEntry	*hent;
    Pg_ConnectionId	*connid;

    hent = Tcl_FindHashEntry(&(cd->dbh_hash), id);
    if(hent == NULL) {
        return (PGconn *)NULL;
    }

    connid = (Pg_ConnectionId *)Tcl_GetHashValue(hent);
    return connid->conn;
}


/*
 * Remove a connection Id from the hash table and
 * close all portals the user forgot.
 */
void
PgDelConnectionId(Pg_clientData *cd, char *id)
{
    Tcl_HashEntry	*hent;
    Tcl_HashEntry	*hent2;
    Tcl_HashEntry	*hent3;
    Tcl_HashSearch	hsearch;
    Pg_ConnectionId	*connid;
    Pg_ResultId		*resid;

    hent = Tcl_FindHashEntry(&(cd->dbh_hash), id);
    if(hent == NULL) {
        return;
    }

    connid = (Pg_ConnectionId *)Tcl_GetHashValue(hent);

    hent2 = Tcl_FirstHashEntry(&(connid->res_hash), &hsearch);
    while(hent2 != NULL) {
        resid = (Pg_ResultId *)Tcl_GetHashValue(hent2);
	PQclear(resid->result);
	hent3 = Tcl_FindHashEntry(&(cd->res_hash), resid->id);
	if(hent3 != NULL) {
	    Tcl_DeleteHashEntry(hent3);
	}
	ckfree(resid);
	hent2 = Tcl_NextHashEntry(&hsearch);
    }
    Tcl_DeleteHashTable(&(connid->res_hash));
    Tcl_DeleteHashEntry(hent);
    ckfree(connid);
}


/*
 * Create a new result Id and hash it
 */
void
PgSetResultId(Pg_clientData *cd, char *id, char *connid_c, PGresult *res)
{
    Tcl_HashEntry	*hent;
    Pg_ConnectionId	*connid;
    Pg_ResultId		*resid;
    int			hnew;

    hent = Tcl_FindHashEntry(&(cd->dbh_hash), connid_c);
    if(hent == NULL) {
        connid = NULL;
    } else {
        connid = (Pg_ConnectionId *)Tcl_GetHashValue(hent);
    }

    resid = (Pg_ResultId *)ckalloc(sizeof(Pg_ResultId));
    resid->result = res;
    resid->connection = connid;
    sprintf(resid->id, "pgr%ld", cd->res_count++);
    strcpy(id, resid->id);

    hent = Tcl_CreateHashEntry(&(cd->res_hash), resid->id, &hnew);
    Tcl_SetHashValue(hent, (ClientData)resid);

    if(connid != NULL) {
        hent = Tcl_CreateHashEntry(&(connid->res_hash), resid->id, &hnew);
	Tcl_SetHashValue(hent, (ClientData)resid);
    }
}


/*
 * Get back the result pointer from the Id
 */
PGresult *
PgGetResultId(Pg_clientData *cd, char *id)
{
    Tcl_HashEntry	*hent;
    Pg_ResultId		*resid;

    hent = Tcl_FindHashEntry(&(cd->res_hash), id);
    if(hent == NULL) {
        return (PGresult *)NULL;
    }

    resid = (Pg_ResultId *)Tcl_GetHashValue(hent);
    return resid->result;
}


/*
 * Remove a result Id from the hash tables
 */
void
PgDelResultId(Pg_clientData *cd, char *id)
{
    Tcl_HashEntry	*hent;
    Tcl_HashEntry	*hent2;
    Pg_ResultId		*resid;

    hent = Tcl_FindHashEntry(&(cd->res_hash), id);
    if(hent == NULL) {
        return;
    }

    resid = (Pg_ResultId *)Tcl_GetHashValue(hent);
    if (resid->connection != NULL) {
        hent2 = Tcl_FindHashEntry(&(resid->connection->res_hash), id);
	if(hent2 != NULL) {
	    Tcl_DeleteHashEntry(hent2);
	}
    }

    Tcl_DeleteHashEntry(hent);
    ckfree(resid);
}


/*
 * Get the connection Id from the result Id
 */
void
PgGetConnByResultId(Pg_clientData *cd, char *id, char *resid_c)
{
    Tcl_HashEntry	*hent;
    Pg_ResultId		*resid;

    hent = Tcl_FindHashEntry(&(cd->res_hash), id);
    if(hent == NULL) {
        return;
    }

    resid = (Pg_ResultId *)Tcl_GetHashValue(hent);
    if (resid->connection != NULL) {
        strcpy(id, resid->connection->id);
    }
}


