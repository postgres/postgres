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
 *	  $PostgreSQL: pgsql/src/port/path.c,v 1.21 2004/07/10 22:58:42 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#include <ctype.h>

#include "pg_config_paths.h"


#ifndef WIN32
#define	IS_DIR_SEP(ch)	((ch) == '/')
#else
#define	IS_DIR_SEP(ch)	((ch) == '/' || (ch) == '\\')
#endif

#ifndef WIN32
#define	IS_PATH_SEP(ch)	((ch) == ':')
#else
#define	IS_PATH_SEP(ch)	((ch) == ';')
#endif

const static char *relative_path(const char *bin_path, const char *other_path);
static void trim_directory(char *path);
static void trim_trailing_separator(char *path);

/* Move to last of consecutive separators or to null byte */
#define MOVE_TO_SEP_END(p) \
{ \
	while (IS_DIR_SEP((p)[0]) && (IS_DIR_SEP((p)[1]) || !(p)[1])) \
		(p)++; \
}

/* Macro creates a relative path */
#define MAKE_RELATIVE \
do { \
		StrNCpy(path, my_exec_path, MAXPGPATH); \
		trim_directory(path); \
		trim_directory(path); \
		snprintf(ret_path, MAXPGPATH, "%s/%s", path, p); \
} while (0)

/*
 *	first_dir_separator
 */
char *
first_dir_separator(const char *filename)
{
	char	   *p;

	for (p = (char *)filename; *p; p++)
		if (IS_DIR_SEP(*p))
			return p;
	return NULL;
}

/*
 *	first_path_separator
 */
char *
first_path_separator(const char *filename)
{
	char	   *p;

	for (p = (char *)filename; *p; p++)
		if (IS_PATH_SEP(*p))
			return p;
	return NULL;
}

/*
 *	last_dir_separator
 */
char *
last_dir_separator(const char *filename)
{
	char	   *p, *ret = NULL;

	for (p = (char *)filename; *p; p++)
		if (IS_DIR_SEP(*p))
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
	if (!last_dir_separator(argv0))
		return argv0;
	else
		return last_dir_separator(argv0) + 1;
}


/*
 *	get_share_path
 */
void
get_share_path(const char *my_exec_path, char *ret_path)
{
	char path[MAXPGPATH];
	const char *p;
	
	if ((p = relative_path(PGBINDIR, PGSHAREDIR)))
		MAKE_RELATIVE;
	else
		StrNCpy(ret_path, PGSHAREDIR, MAXPGPATH);
}



/*
 *	get_etc_path
 */
void
get_etc_path(const char *my_exec_path, char *ret_path)
{
	char path[MAXPGPATH];
	const char *p;
	
	if ((p = relative_path(PGBINDIR, SYSCONFDIR)))
		MAKE_RELATIVE;
	else
		StrNCpy(ret_path, SYSCONFDIR, MAXPGPATH);
}



/*
 *	get_include_path
 */
void
get_include_path(const char *my_exec_path, char *ret_path)
{
	char path[MAXPGPATH];
	const char *p;
	
	if ((p = relative_path(PGBINDIR, INCLUDEDIR)))
		MAKE_RELATIVE;
	else
		StrNCpy(ret_path, INCLUDEDIR, MAXPGPATH);
}



/*
 *	get_pkginclude_path
 */
void
get_pkginclude_path(const char *my_exec_path, char *ret_path)
{
	char path[MAXPGPATH];
	const char *p;
	
	if ((p = relative_path(PGBINDIR, PKGINCLUDEDIR)))
		MAKE_RELATIVE;
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
	char path[MAXPGPATH];
	const char *p;
	
	if ((p = relative_path(PGBINDIR, PKGLIBDIR)))
		MAKE_RELATIVE;
	else
		StrNCpy(ret_path, PKGLIBDIR, MAXPGPATH);
}



/*
 *	get_locale_path
 *
 *	Return locale path, either relative to /bin or hardcoded
 */
void
get_locale_path(const char *my_exec_path, char *ret_path)
{
	char path[MAXPGPATH];
	const char *p;
	
	if ((p = relative_path(PGBINDIR, LOCALEDIR)))
		MAKE_RELATIVE;
	else
		StrNCpy(ret_path, LOCALEDIR, MAXPGPATH);
}



/*
 *	set_pglocale_pgservice
 *
 *	Set application-specific locale and service directory
 *
 *	This function takes an argv[0] rather than a full path.
 */
void
set_pglocale_pgservice(const char *argv0, const char *app)
{
	char path[MAXPGPATH];
	char my_exec_path[MAXPGPATH];
	char env_path[MAXPGPATH + sizeof("PGLOCALEDIR=")]; /* longer than PGSYSCONFDIR */

	/* don't set LC_ALL in the backend */
	if (strcmp(app, "postgres") != 0)
		setlocale(LC_ALL, "");

	if (find_my_exec(argv0, my_exec_path) < 0)
		return;
		
#ifdef ENABLE_NLS
	get_locale_path(my_exec_path, path);
	bindtextdomain(app, path);
	textdomain(app);

	if (getenv("PGLOCALEDIR") == NULL)
	{
		/* set for libpq to use */
		snprintf(env_path, sizeof(env_path), "PGLOCALEDIR=%s", path);
		putenv(strdup(env_path));
	}
#endif

	if (getenv("PGSYSCONFDIR") == NULL)
	{
		get_etc_path(my_exec_path, path);
	
		/* set for libpq to use */
		snprintf(env_path, sizeof(env_path), "PGSYSCONFDIR=%s", path);
		putenv(strdup(env_path));
	}
}



/*
 *	relative_path
 *
 *	Do the supplied paths differ only in their last component?
 */
static const char *
relative_path(const char *bin_path, const char *other_path)
{
	const char *other_sep = other_path;
	
#ifdef WIN32
	/* Driver letters match? */
	if (isalpha(*bin_path) && bin_path[1] == ':' &&
		(!isalpha(*other_path) || !other_path[1] == ':'))
		return NULL;
	if ((!isalpha(*bin_path) || !bin_path[1] == ':') &&
		(isalpha(*other_path) && other_path[1] == ':'))
		return NULL;
	if (isalpha(*bin_path) && bin_path[1] == ':' &&
		isalpha(*other_path) && other_path[1] == ':')
	{
		if (toupper(*bin_path) != toupper(*other_path))
			return NULL;
		bin_path += 2;
		other_path += 2;
		other_sep = other_path + 1;		/* past separator */
	}
#endif

	while (1)
	{
		/* Move past adjacent slashes like //, and trailing ones */
		MOVE_TO_SEP_END(bin_path);
		MOVE_TO_SEP_END(other_path);

		/* One of the paths is done? */
		if (!*bin_path || !*other_path)
			break;

		/* Win32 filesystem is case insensitive */
		if ((!IS_DIR_SEP(*bin_path) || !IS_DIR_SEP(*other_path)) &&
#ifndef WIN32
			*bin_path != *other_path
#else
			toupper((unsigned char) *bin_path) != toupper((unsigned char)*other_path)
#endif
			)
			break;

		if (IS_DIR_SEP(*other_path))
			other_sep = other_path + 1;		/* past separator */
			
		bin_path++;
		other_path++;
	}

	/* identical? */
	if (!*bin_path && !*other_path)
		return NULL;

	/* advance past directory name */	
	while (!IS_DIR_SEP(*bin_path) && *bin_path)
		bin_path++;

	MOVE_TO_SEP_END(bin_path);

	/* Is bin done? */
	if (!*bin_path)
		return other_path;
	else
		return NULL;
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

	for (p = path + strlen(path) - 1; IS_DIR_SEP(*p) && p > path; p--)
		;
	for (; !IS_DIR_SEP(*p) && p > path; p--)
		;
	*p = '\0';
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
		for (p--; p > path && IS_DIR_SEP(*p); p--)
			*p = '\0';
}
