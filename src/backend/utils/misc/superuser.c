/*-------------------------------------------------------------------------
 *
 * superuser.c--
 *
 *	  The superuser() function.  Determines if user has superuser privilege.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/misc/superuser.c,v 1.7 1998/09/01 04:33:32 momjian Exp $
 *
 * DESCRIPTION
 *	  See superuser().
 *-------------------------------------------------------------------------
 */

#include <postgres.h>
#include <utils/syscache.h>
#include <catalog/pg_shadow.h>

bool
superuser(void)
{
/*--------------------------------------------------------------------------
	The Postgres user running this command has Postgres superuser
	privileges.
--------------------------------------------------------------------------*/
	extern char *UserName;		/* defined in global.c */

	HeapTuple	utup;

	utup = SearchSysCacheTuple(USENAME,
							   PointerGetDatum(UserName),
							   0, 0, 0);
	Assert(utup != NULL);
	return ((Form_pg_shadow) GETSTRUCT(utup))->usesuper;
}
