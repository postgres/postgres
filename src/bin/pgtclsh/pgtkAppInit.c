/*
 * pgtkAppInit.c
 *
 *		a skeletal Tcl_AppInit that provides pgtcl initialization
 *	  to create a tclsh that can talk to pglite backends
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1993 The Regents of the University of California.
 * Copyright (c) 1994 Sun Microsystems, Inc.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include <tk.h>
#include "libpgtcl.h"

/*
 * The following variable is a special hack that is needed in order for
 * Sun shared libraries to be used for Tcl.
 */

#ifdef NEED_MATHERR
extern int	matherr();
int		   *tclDummyMathPtr = (int *) matherr;

#endif


/*
 *----------------------------------------------------------------------
 *
 * main
 *
 *		This is the main program for the application.
 *
 * Results:
 *		None: Tk_Main never returns here, so this procedure never
 *		returns either.
 *
 * Side effects:
 *		Whatever the application does.
 *
 *----------------------------------------------------------------------
 */

int
main(int argc, char **argv)
{
	Tk_Main(argc, argv, Tcl_AppInit);
	return 0;					/* Needed only to prevent compiler
								 * warning. */
}


/*
 *----------------------------------------------------------------------
 *
 * Tcl_AppInit
 *
 *		This procedure performs application-specific initialization.
 *		Most applications, especially those that incorporate additional
 *		packages, will have their own version of this procedure.
 *
 * Results:
 *		Returns a standard Tcl completion code, and leaves an error
 *		message in interp->result if an error occurs.
 *
 * Side effects:
 *		Depends on the startup script.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_AppInit(Tcl_Interp *interp)
{
	if (Tcl_Init(interp) == TCL_ERROR)
		return TCL_ERROR;
	if (Tk_Init(interp) == TCL_ERROR)
		return TCL_ERROR;

	/*
	 * Call the init procedures for included packages.	Each call should
	 * look like this:
	 *
	 * if (Mod_Init(interp) == TCL_ERROR) { return TCL_ERROR; }
	 *
	 * where "Mod" is the name of the module.
	 */

	if (Pgtcl_Init(interp) == TCL_ERROR)
		return TCL_ERROR;

	/*
	 * Call Tcl_CreateCommand for application-specific commands, if they
	 * weren't already created by the init procedures called above.
	 */

	/*
	 * Specify a user-specific startup file to invoke if the application
	 * is run interactively.  Typically the startup file is "~/.apprc"
	 * where "app" is the name of the application.	If this line is
	 * deleted then no user-specific startup file will be run under any
	 * conditions.
	 */

#if (TCL_MAJOR_VERSION <= 7) && (TCL_MINOR_VERSION < 5)
	tcl_RcFileName = "~/.wishrc";
#else
	Tcl_SetVar(interp, "tcl_rcFileName", "~/.wishrc", TCL_GLOBAL_ONLY);
#endif

	return TCL_OK;
}
