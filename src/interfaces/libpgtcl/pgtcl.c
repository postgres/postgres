/*-------------------------------------------------------------------------
 *
 * pgtcl.c
 *
 *	libpgtcl is a tcl package for front-ends to interface with PostgreSQL.
 *	It's a Tcl wrapper for libpq.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/interfaces/libpgtcl/pgtcl.c,v 1.30 2004/01/07 18:56:29 neilc Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"
#include "libpgtcl.h"
#include "pgtclCmds.h"
#include "pgtclId.h"

/*
 * Pgtcl_Init
 *	  initialization package for the PGTCL Tcl package
 *
 */

int
Pgtcl_Init(Tcl_Interp *interp)
{
	double		tclversion;

	/*
	 * finish off the ChannelType struct.  Much easier to do it here then
	 * to guess where it might be by position in the struct.  This is
	 * needed for Tcl7.6 *only*, which has the getfileproc.
	 */
#if HAVE_TCL_GETFILEPROC
	Pg_ConnType.getFileProc = PgGetFileProc;
#endif

	/*
	 * Tcl versions >= 8.1 use UTF-8 for their internal string
	 * representation. Therefore PGCLIENTENCODING must be set to UNICODE
	 * for these versions.
	 */
	Tcl_GetDouble(interp, Tcl_GetVar(interp, "tcl_version", TCL_GLOBAL_ONLY), &tclversion);
	if (tclversion >= 8.1)
		putenv("PGCLIENTENCODING=UNICODE");

	/* register all pgtcl commands */
	Tcl_CreateCommand(interp,
					  "pg_conndefaults",
					  Pg_conndefaults,
					  NULL, NULL);

	Tcl_CreateCommand(interp,
					  "pg_connect",
					  Pg_connect,
					  NULL, NULL);

	Tcl_CreateCommand(interp,
					  "pg_disconnect",
					  Pg_disconnect,
					  NULL, NULL);

	Tcl_CreateCommand(interp,
					  "pg_exec",
					  Pg_exec,
					  NULL, NULL);

	Tcl_CreateCommand(interp,
					  "pg_select",
					  Pg_select,
					  NULL, NULL);

	Tcl_CreateCommand(interp,
					  "pg_result",
					  Pg_result,
					  NULL, NULL);

	Tcl_CreateCommand(interp,
					  "pg_execute",
					  Pg_execute,
					  NULL, NULL);

	Tcl_CreateCommand(interp,
					  "pg_lo_open",
					  Pg_lo_open,
					  NULL, NULL);

	Tcl_CreateCommand(interp,
					  "pg_lo_close",
					  Pg_lo_close,
					  NULL, NULL);

#ifdef PGTCL_USE_TCLOBJ
	Tcl_CreateObjCommand(interp,
						 "pg_lo_read",
						 Pg_lo_read,
						 NULL, NULL);

	Tcl_CreateObjCommand(interp,
						 "pg_lo_write",
						 Pg_lo_write,
						 NULL, NULL);
#else
	Tcl_CreateCommand(interp,
					  "pg_lo_read",
					  Pg_lo_read,
					  NULL, NULL);

	Tcl_CreateCommand(interp,
					  "pg_lo_write",
					  Pg_lo_write,
					  NULL, NULL);
#endif

	Tcl_CreateCommand(interp,
					  "pg_lo_lseek",
					  Pg_lo_lseek,
					  NULL, NULL);

	Tcl_CreateCommand(interp,
					  "pg_lo_creat",
					  Pg_lo_creat,
					  NULL, NULL);

	Tcl_CreateCommand(interp,
					  "pg_lo_tell",
					  Pg_lo_tell,
					  NULL, NULL);

	Tcl_CreateCommand(interp,
					  "pg_lo_unlink",
					  Pg_lo_unlink,
					  NULL, NULL);

	Tcl_CreateCommand(interp,
					  "pg_lo_import",
					  Pg_lo_import,
					  NULL, NULL);

	Tcl_CreateCommand(interp,
					  "pg_lo_export",
					  Pg_lo_export,
					  NULL, NULL);

	Tcl_CreateCommand(interp,
					  "pg_listen",
					  Pg_listen,
					  NULL, NULL);

	Tcl_CreateCommand(interp,
					  "pg_on_connection_loss",
					  Pg_on_connection_loss,
					  NULL, NULL);

	Tcl_PkgProvide(interp, "Pgtcl", "1.4");

	return TCL_OK;
}


int
Pgtcl_SafeInit(Tcl_Interp *interp)
{
	return Pgtcl_Init(interp);
}
