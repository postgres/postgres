/*-------------------------------------------------------------------------
 *
 * palloc.c
 *	  POSTGRES memory allocator code.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/mmgr/Attic/palloc.c,v 1.12.2.1 1999/08/02 05:25:16 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */


#include "postgres.h"

#include "nodes/memnodes.h"


/* ----------------------------------------------------------------
 *		User library functions
 * ----------------------------------------------------------------
 */

/* ----------
 * palloc(), pfree() and repalloc() are now macros in palloc.h
 * ----------
 */

char *
pstrdup(char *string)
{
	char	   *nstr;
	int			len;

	nstr = palloc(len = strlen(string) + 1);
	MemoryCopy(nstr, string, len);

	return nstr;
}
