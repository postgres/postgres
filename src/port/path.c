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
 *	  $PostgreSQL: pgsql/src/port/path.c,v 1.5 2004/03/09 04:49:02 momjian Exp $
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
 * make all paths look like unix, with forward slashes
 * also strip any trailing slash.
 *
 * The Windows command processor will accept suitably quoted paths
 * with forward slashes, but barfs badly with mixed forward and back
 * slashes. Removing the trailing slash on a path means we never get
 * ugly double slashes.  Don't remove a leading slash, though.
 */
void
canonicalize_path(char *path)
{
	char	   *p;

	for (p = path; *p; p++)
	{
#ifdef WIN32
		if (*p == '\\')
			*p = '/';
#endif
	}
	if (p > path+1 && *--p == '/')
		*p = '\0';
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
