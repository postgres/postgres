/*-------------------------------------------------------------------------
 *
 * pgtcl.c--
 *    
 *    libpgtcl is a tcl package for front-ends to interface with pglite
 *   It's the tcl equivalent of the old libpq C interface.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/interfaces/libpgtcl/Attic/pgtcl.c,v 1.5 1996/11/11 12:14:38 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <stdlib.h>

#include "postgres.h"
#include "tcl.h"
#include "libpgtcl.h"
#include "pgtclCmds.h"
#include "pgtclId.h"

/*
 * Pgtcl_Init 
 *    initialization package for the PGLITE Tcl package
 *
 */

/*
 * Tidy up forgotten postgres connection at Tcl_Exit
 */
static void
Pgtcl_AtExit (ClientData cData)
{
  Pg_clientData *cd = (Pg_clientData *)cData;
  Tcl_HashEntry		*hent;
  Tcl_HashSearch	hsearch;
  Pg_ConnectionId	*connid;
  PGconn		*conn;

  while((hent = Tcl_FirstHashEntry(&(cd->dbh_hash), &hsearch)) != NULL) {
      connid = (Pg_ConnectionId *)Tcl_GetHashValue(hent);
      conn = connid->conn;
      PgDelConnectionId(cd, connid->id);
      PQfinish(conn);
  }

  Tcl_DeleteHashTable(&(cd->dbh_hash));
  Tcl_DeleteHashTable(&(cd->res_hash));

  Tcl_DeleteExitHandler(Pgtcl_AtExit, cData);
}

/*
 * Tidy up forgotten postgres connections on Interpreter deletion
 */
static void
Pgtcl_Shutdown (ClientData cData, Tcl_Interp *interp)
{
  Pgtcl_AtExit(cData);
}

int
Pgtcl_Init (Tcl_Interp *interp)
{
  Pg_clientData	*cd;

  /* Create and initialize the client data area */
  cd = (Pg_clientData *)ckalloc(sizeof(Pg_clientData));
  Tcl_InitHashTable(&(cd->dbh_hash), TCL_STRING_KEYS);
  Tcl_InitHashTable(&(cd->res_hash), TCL_STRING_KEYS);
  cd->dbh_count = 0L;
  cd->res_count = 0L;

  /* Arrange for tidy up when interpreter is deleted or Tcl exits */
  Tcl_CallWhenDeleted(interp, Pgtcl_Shutdown, (ClientData)cd);
  Tcl_CreateExitHandler(Pgtcl_AtExit, (ClientData)cd);

  /* register all pgtcl commands */
  Tcl_CreateCommand(interp,
		    "pg_conndefaults",
		    Pg_conndefaults,
		    (ClientData)cd, (Tcl_CmdDeleteProc*)NULL);

  Tcl_CreateCommand(interp,
		    "pg_connect",
		    Pg_connect,
		    (ClientData)cd, (Tcl_CmdDeleteProc*)NULL);

  Tcl_CreateCommand(interp,
		    "pg_disconnect",
		    Pg_disconnect,
		    (ClientData)cd, (Tcl_CmdDeleteProc*)NULL);
  
  Tcl_CreateCommand(interp,
		    "pg_exec",
		    Pg_exec,
		    (ClientData)cd, (Tcl_CmdDeleteProc*)NULL);
  
  Tcl_CreateCommand(interp,
		    "pg_select",
		    Pg_select,
		    (ClientData)cd, (Tcl_CmdDeleteProc*)NULL);
  
  Tcl_CreateCommand(interp,
		    "pg_result",
		    Pg_result,
		    (ClientData)cd, (Tcl_CmdDeleteProc*)NULL);
  
  Tcl_CreateCommand(interp,
		    "pg_lo_open",
		    Pg_lo_open,
		    (ClientData)cd, (Tcl_CmdDeleteProc*)NULL);
  
  Tcl_CreateCommand(interp,
		    "pg_lo_close",
		    Pg_lo_close,
		    (ClientData)cd, (Tcl_CmdDeleteProc*)NULL);

  Tcl_CreateCommand(interp,
		    "pg_lo_read",
		    Pg_lo_read,
		    (ClientData)cd, (Tcl_CmdDeleteProc*)NULL);

  Tcl_CreateCommand(interp,
		    "pg_lo_write",
		    Pg_lo_write,
		    (ClientData)cd, (Tcl_CmdDeleteProc*)NULL);

  Tcl_CreateCommand(interp,
		    "pg_lo_lseek",
		    Pg_lo_lseek,
		    (ClientData)cd, (Tcl_CmdDeleteProc*)NULL);

  Tcl_CreateCommand(interp,
		    "pg_lo_creat",
		    Pg_lo_creat,
		    (ClientData)cd, (Tcl_CmdDeleteProc*)NULL);

  Tcl_CreateCommand(interp,
		    "pg_lo_tell",
		    Pg_lo_tell,
		    (ClientData)cd, (Tcl_CmdDeleteProc*)NULL);

  Tcl_CreateCommand(interp,
		    "pg_lo_unlink",
		    Pg_lo_unlink,
		    (ClientData)cd, (Tcl_CmdDeleteProc*)NULL);

  Tcl_CreateCommand(interp,
		    "pg_lo_import",
		    Pg_lo_import,
		    (ClientData)cd, (Tcl_CmdDeleteProc*)NULL);
  
  Tcl_CreateCommand(interp,
		    "pg_lo_export",
		    Pg_lo_export,
		    (ClientData)cd, (Tcl_CmdDeleteProc*)NULL);
  
  Tcl_PkgProvide(interp, "Pgtcl", "1.0");

  return TCL_OK;
}


int
Pgtcl_SafeInit (Tcl_Interp *interp)
{
    return Pgtcl_Init(interp);
}
