/*-------------------------------------------------------------------------
 *
 * pgtz.c
 *    Timezone Library Integration Functions
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/timezone/pgtz.c,v 1.3 2004/04/30 20:23:28 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "pgtz.h"
#include "tzfile.h"


static char tzdir[MAXPGPATH];
static int done_tzdir = 0;
char *pgwin32_TZDIR(void) {
	char *p;
	if (done_tzdir)
		return tzdir;

#ifndef WIN32
	StrNCpy(tzdir, PGDATADIR, MAXPGPATH);
#else
	if (GetModuleFileName(NULL,tzdir,MAXPGPATH) == 0)
		return NULL;
#endif
	
	canonicalize_path(tzdir);
	if ((p = last_path_separator(tzdir)) == NULL)
		return NULL;
	else
		*p = '\0';
	
	strcat(tzdir,"/timezone");

	done_tzdir=1;
	return tzdir;
}
