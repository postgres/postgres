/*-------------------------------------------------------------------------
 *
 * util.c
 *	  general routines for backend libpq modules
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	$Header: /cvsroot/pgsql/src/backend/libpq/Attic/util.c,v 1.16 2000/07/08 03:04:41 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 *	 UTILITY ROUTINES
 *		pqdebug			- send a string to the debugging output port
 *		PQtrace			- turn on pqdebug() tracing
 *		PQuntrace		- turn off pqdebug() tracing
 */

#include "postgres.h"

#include "libpq/libpq.h"


/* ----------------
 *		global variables for backend libpq
 * ----------------
 */
char		PQerrormsg[PQERRORMSG_LENGTH];

/*
 * These are not really global --- they are referred to nowhere else.
 * We declare them as global symbols to make them easier to set in a debugger.
 */

int			PQtracep = 0;		/* 1 to print out debugging messages */

FILE	   *debug_port = (FILE *) NULL;

/* ----------------------------------------------------------------
 *						PQ utility routines
 * ----------------------------------------------------------------
 */

void
pqdebug(char *fmt, char *msg)
{
	if (!fmt)
		return;

	if (PQtracep)
	{

		/*
		 * if nothing else was suggested default to stderr
		 */
		if (!debug_port)
			debug_port = stderr;
		fprintf(debug_port, fmt, msg);
		fprintf(debug_port, "\n");
	}
}

/* --------------------------------
 *		PQtrace() / PQuntrace()
 * --------------------------------
 */
void
PQtrace()
{
	PQtracep = 1;
}

void
PQuntrace()
{
	PQtracep = 0;
}
