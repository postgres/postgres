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
 *	  $PostgreSQL: pgsql/src/port/path.c,v 1.9 2004/05/18 03:36:45 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"
#include <ctype.h>

#ifndef WIN32
#define	ISSEP(ch)	((ch) == '/')
#else
#define	ISSEP(ch)	((ch) == '/' || (ch) == '\\')
#endif

static bool relative_path(const char *path1, const char *path2);
static void trim_directory(char *path);
static void trim_trailing_separator(char *path);

/* Move to last of consecutive separators or to null byte */
#define MOVE_TO_SEP_END(p) \
{ \
	while (ISSEP((p)[0]) && (ISSEP((p)[1]) || !(p)[1])) \
		(p)++; \
}


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
	char	   *p;

	for (p = (char *)filename; *p; p++)
		if (ISSEP(*p))
			return p;
	return NULL;
}


/*
 *	last_path_separator
 */
char *
last_path_separator(const char *filename)
{
	char	   *p, *ret = NULL;

	for (p = (char *)filename; *p; p++)
		if (ISSEP(*p))
			ret = p;
	return ret;
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
#ifdef WIN32
	char	   *p;

	for (p = path; *p; p++)
	{
		if (*p == '\\')
			*p = '/';
	}
#endif

	trim_trailing_separator(path);
}


/*
 * Extracts the actual name of the program as called.
 */
const char *
get_progname(const char *argv0)
{
	if (!last_path_separator(argv0))
		return argv0;
	else
		return last_path_separator(argv0) + 1;
}


/*
 *	get_share_path
 */
void
get_share_path(const char *my_exec_path, char *ret_path)
{
	if (relative_path(PGBINDIR, PGDATADIR))
	{
		/* Autoconf calls our /share 'datadir' */
		StrNCpy(ret_path, my_exec_path, MAXPGPATH);
		trim_directory(ret_path);	/* trim off binary */
		trim_directory(ret_path);	/* trim off /bin */
		strcat(ret_path, "/share");	/* add /share */
	}
	else
		StrNCpy(ret_path, PGDATADIR, MAXPGPATH);
}



/*
 *	get_etc_path
 */
void
get_etc_path(const char *my_exec_path, char *ret_path)
{
	if (relative_path(PGBINDIR, SYSCONFDIR))
	{
		StrNCpy(ret_path, my_exec_path, MAXPGPATH);
		trim_directory(ret_path);
		trim_directory(ret_path);
		strcat(ret_path, "/etc");
	}
	else
		StrNCpy(ret_path, SYSCONFDIR, MAXPGPATH);
}



/*
 *	get_include_path
 */
void
get_include_path(const char *my_exec_path, char *ret_path)
{
	if (relative_path(PGBINDIR, INCLUDEDIR))
	{
		StrNCpy(ret_path, my_exec_path, MAXPGPATH);
		trim_directory(ret_path);
		trim_directory(ret_path);
		strcat(ret_path, "/include");
	}
	else
		StrNCpy(ret_path, INCLUDEDIR, MAXPGPATH);
}



/*
 *	get_pkginclude_path
 */
void
get_pkginclude_path(const char *my_exec_path, char *ret_path)
{
	if (relative_path(PGBINDIR, PKGINCLUDEDIR))
	{
		StrNCpy(ret_path, my_exec_path, MAXPGPATH);
		trim_directory(ret_path);
		trim_directory(ret_path);
		strcat(ret_path, "/include");
	}
	else
		StrNCpy(ret_path, PKGINCLUDEDIR, MAXPGPATH);
}



/*
 *	get_pkglib_path
 *
 *	Return library path, either relative to /bin or hardcoded
 */
void
get_pkglib_path(const char *my_exec_path, char *ret_path)
{
	if (relative_path(PGBINDIR, PKGLIBDIR))
	{
		StrNCpy(ret_path, my_exec_path, MAXPGPATH);
		trim_directory(ret_path);
		trim_directory(ret_path);
		strcat(ret_path, "/lib");
	}
	else
		StrNCpy(ret_path, PKGLIBDIR, MAXPGPATH);
}



/*
 *	relative_path
 *
 *	Do the supplied paths differ only in their last component?
 */
static bool
relative_path(const char *path1, const char *path2)
{

#ifdef WIN32
	/* Driver letters match? */
	if (isalpha(*path1) && path1[1] == ':' &&
		(!isalpha(*path2) || !path2[1] == ':'))
		return false;
	if ((!isalpha(*path1) || !path1[1] == ':') &&
		(isalpha(*path2) && path2[1] == ':'))
		return false;
	if (isalpha(*path1) && path1[1] == ':' &&
		isalpha(*path2) && path2[1] == ':')
	{
		if (toupper(*path1) != toupper(*path2))
			return false;
		path1 += 2;
		path2 += 2;
	}
#endif

	while (1)
	{
		/* Move past adjacent slashes like //, and trailing ones */
		MOVE_TO_SEP_END(path1);
		MOVE_TO_SEP_END(path2);

		/* One of the paths is done? */
		if (!*path1 || !*path2)
			break;

		/* Win32 filesystem is case insensitive */
#ifndef WIN32
		if (*path1 != *path2)
#else
		if (toupper((unsigned char) *path1) != toupper((unsigned char)*path2))
#endif
			break;

		path1++;
		path2++;
	}

	/* both done, identical? */
	if (!*path1 && !*path2)
		return false;

	/* advance past directory name */	
	while (!ISSEP(*path1) && *path1)
		path1++;
	while (!ISSEP(*path2) && *path2)
		path2++;

	MOVE_TO_SEP_END(path1);
	MOVE_TO_SEP_END(path2);

	/* Are both strings done? */
	if (!*path1 && !*path2)
		return true;
	else
		return false;
}


/*
 *	trim_directory
 *
 *	Trim trailing directory from path
 */
static void
trim_directory(char *path)
{
	char *p;
	
	if (path[0] == '\0')
		return;

	for (p = path + strlen(path) - 1; ISSEP(*p) && p > path; p--)
		;
	for (; !ISSEP(*p) && p > path; p--)
		;
	*p = '\0';
	return;
}



/*
 *	trim_trailing_separator
 */
static void
trim_trailing_separator(char *path)
{
	char *p = path + strlen(path);
	
	/* trim off trailing slashes */
	if (p > path)
		for (p--; p >= path && ISSEP(*p); p--)
			*p = '\0';
}

