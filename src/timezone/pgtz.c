/*-------------------------------------------------------------------------
 *
 * pgtz.c
 *    Timezone Library Integration Functions
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/timezone/pgtz.c,v 1.1 2004/04/30 04:09:23 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "pgtz.h"
#include "tzfile.h"


#ifdef WIN32
static char tzdir[MAXPGPATH];
static int done_tzdir = 0;
char *pgwin32_TZDIR(void) {
	char *p;
	if (done_tzdir)
		return tzdir;

	if (GetModuleFileName(NULL,tzdir,MAXPGPATH) == 0)
		return NULL;
	
	canonicalize_path(tzdir);
	if ((p = last_path_separator(tzdir)) == NULL)
		return NULL;
	else
		*p = '\0';
	
	strcat(tzdir,"/../share/timezone");

	done_tzdir=1;
	return tzdir;
}
#else
#error pgwin32_TZDIR not implemented on non win32 yet!
#endif
