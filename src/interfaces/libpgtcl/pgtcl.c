/*-------------------------------------------------------------------------
 *
 * pgtcl.c--
 *
 *	  libpgtcl is a tcl package for front-ends to interface with pglite
 *	 It's the tcl equivalent of the old libpq C interface.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/libpgtcl/Attic/pgtcl.c,v 1.12 1998/09/01 04:39:55 momjian Exp $
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
 *	  initialization package for the PGLITE Tcl package
 *
 */

int
Pgtcl_Init(Tcl_Interp * interp)
{

	/*
	 * finish off the ChannelType struct.  Much easier to do it here then
	 * to guess where it might be by position in the struct.  This is
	 * needed for Tcl7.6 and beyond, which have the getfileproc.
	 */
#if HAVE_TCL_GETFILEPROC
	Pg_ConnType.getFileProc = PgGetFileProc;
#endif

	/* register all pgtcl commands */
	Tcl_CreateCommand(interp,
					  "pg_conndefaults",
					  Pg_conndefaults,
					  (ClientData) NULL, (Tcl_CmdDeleteProc *) NULL);

	Tcl_CreateCommand(interp,
					  "pg_connect",
					  Pg_connect,
					  (ClientData) NULL, (Tcl_CmdDeleteProc *) NULL);

	Tcl_CreateCommand(interp,
					  "pg_disconnect",
					  Pg_disconnect,
					  (ClientData) NULL, (Tcl_CmdDeleteProc *) NULL);

	Tcl_CreateCommand(interp,
					  "pg_exec",
					  Pg_exec,
					  (ClientData) NULL, (Tcl_CmdDeleteProc *) NULL);

	Tcl_CreateCommand(interp,
					  "pg_select",
					  Pg_select,
					  (ClientData) NULL, (Tcl_CmdDeleteProc *) NULL);

	Tcl_CreateCommand(interp,
					  "pg_result",
					  Pg_result,
					  (ClientData) NULL, (Tcl_CmdDeleteProc *) NULL);

	Tcl_CreateCommand(interp,
					  "pg_lo_open",
					  Pg_lo_open,
					  (ClientData) NULL, (Tcl_CmdDeleteProc *) NULL);

	Tcl_CreateCommand(interp,
					  "pg_lo_close",
					  Pg_lo_close,
					  (ClientData) NULL, (Tcl_CmdDeleteProc *) NULL);

	Tcl_CreateCommand(interp,
					  "pg_lo_read",
					  Pg_lo_read,
					  (ClientData) NULL, (Tcl_CmdDeleteProc *) NULL);

	Tcl_CreateCommand(interp,
					  "pg_lo_write",
					  Pg_lo_write,
					  (ClientData) NULL, (Tcl_CmdDeleteProc *) NULL);

	Tcl_CreateCommand(interp,
					  "pg_lo_lseek",
					  Pg_lo_lseek,
					  (ClientData) NULL, (Tcl_CmdDeleteProc *) NULL);

	Tcl_CreateCommand(interp,
					  "pg_lo_creat",
					  Pg_lo_creat,
					  (ClientData) NULL, (Tcl_CmdDeleteProc *) NULL);

	Tcl_CreateCommand(interp,
					  "pg_lo_tell",
					  Pg_lo_tell,
					  (ClientData) NULL, (Tcl_CmdDeleteProc *) NULL);

	Tcl_CreateCommand(interp,
					  "pg_lo_unlink",
					  Pg_lo_unlink,
					  (ClientData) NULL, (Tcl_CmdDeleteProc *) NULL);

	Tcl_CreateCommand(interp,
					  "pg_lo_import",
					  Pg_lo_import,
					  (ClientData) NULL, (Tcl_CmdDeleteProc *) NULL);

	Tcl_CreateCommand(interp,
					  "pg_lo_export",
					  Pg_lo_export,
					  (ClientData) NULL, (Tcl_CmdDeleteProc *) NULL);

	Tcl_CreateCommand(interp,
					  "pg_listen",
					  Pg_listen,
					  (ClientData) NULL, (Tcl_CmdDeleteProc *) NULL);

	Tcl_PkgProvide(interp, "Pgtcl", "1.2");

	return TCL_OK;
}


int
Pgtcl_SafeInit(Tcl_Interp * interp)
{
	return Pgtcl_Init(interp);
}
