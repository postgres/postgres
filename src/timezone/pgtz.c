/*-------------------------------------------------------------------------
 *
 * pgtz.c
 *	  Timezone Library Integration Functions
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/timezone/pgtz.c,v 1.9 2004/05/18 03:36:45 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "pgtz.h"
#include "tzfile.h"


static char tzdir[MAXPGPATH];
static int	done_tzdir = 0;

char *
pg_TZDIR(void)
{
	if (done_tzdir)
		return tzdir;

	get_share_path(my_exec_path, tzdir);
	strcat(tzdir, "/timezone");

	done_tzdir = 1;
	return tzdir;
}
