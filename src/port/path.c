/*-------------------------------------------------------------------------
 *
 * path.c
 *	  portable path handling routines
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/port/path.c,v 1.4 2003/11/29 19:52:13 pgsql Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"
#include <ctype.h>

/*
 *	is_absolute_path
 */
bool
is_absolute_path(const char *filename)
{
	return filename[0] == '/'
#ifdef WIN32					/* WIN32 paths can either have forward or
								 * backward slashes */
		|| filename[0] == '\\'
		|| (isalpha(filename[0]) && filename[1] == ':'
			&& (filename[2] == '\\' || filename[2] == '/'))
#endif
		;
}



/*
 *	first_path_separator
 */
char *
first_path_separator(const char *filename)
{
#ifndef WIN32
	return strchr(filename, '/');
#else
	char	   *slash,
			   *bslash;

	/* How should we handle "C:file.c"? */
	slash = strchr(filename, '/');
	bslash = strchr(filename, '\\');
	if (slash == NULL)
		return bslash;
	else if (bslash == NULL)
		return slash;
	else
		return (slash < bslash) ? slash : bslash;
#endif
}


/*
 *	last_path_separator
 */
char *
last_path_separator(const char *filename)
{
#ifndef WIN32
	return strrchr(filename, '/');
#else
	char	   *slash,
			   *bslash;

	/* How should we handle "C:file.c"? */
	slash = strrchr(filename, '/');
	bslash = strrchr(filename, '\\');
	if (slash == NULL)
		return bslash;
	else if (bslash == NULL)
		return slash;
	else
		return (slash > bslash) ? slash : bslash;
#endif
}


/*
 * Extracts the actual name of the program as called.
 */
char *
get_progname(char *argv0)
{
	if (!last_path_separator(argv0))
		return argv0;
	else
		return last_path_separator(argv0) + 1;
}
