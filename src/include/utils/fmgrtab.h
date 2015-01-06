/*-------------------------------------------------------------------------
 *
 * fmgrtab.h
 *	  The function manager's table of internal functions.
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/fmgrtab.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FMGRTAB_H
#define FMGRTAB_H

#include "fmgr.h"


/*
 * This table stores info about all the built-in functions (ie, functions
 * that are compiled into the Postgres executable).  The table entries are
 * required to appear in Oid order, so that binary search can be used.
 */

typedef struct
{
	Oid			foid;			/* OID of the function */
	const char *funcName;		/* C name of the function */
	short		nargs;			/* 0..FUNC_MAX_ARGS, or -1 if variable count */
	bool		strict;			/* T if function is "strict" */
	bool		retset;			/* T if function returns a set */
	PGFunction	func;			/* pointer to compiled function */
} FmgrBuiltin;

extern const FmgrBuiltin fmgr_builtins[];

extern const int fmgr_nbuiltins;	/* number of entries in table */

#endif   /* FMGRTAB_H */
