/*-------------------------------------------------------------------------
 *
 * path.c
 *	  portable path handling routines
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/port/path.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include <ctype.h>
#include <sys/stat.h>
#ifdef WIN32
#ifdef _WIN32_IE
#undef _WIN32_IE
#endif
#define _WIN32_IE 0x0500
#ifdef near
#undef near
#endif
#define near
#include <shlobj.h>
#else
#include <unistd.h>
#endif

#include "pg_config_paths.h"


#ifndef WIN32
#define IS_PATH_VAR_SEP(ch) ((ch) == ':')
#else
#define IS_PATH_VAR_SEP(ch) ((ch) == ';')
#endif

static void make_relative_path(char *ret_path, const char *target_path,
							   const char *bin_path, const char *my_exec_path);
static void trim_directory(char *path);
static void trim_trailing_separator(char *path);


/*
 * skip_drive
 *
 * On Windows, a path may begin with "C:" or "//network/".  Advance over
 * this and point to the effective start of the path.
 */
#ifdef WIN32

static char *
skip_drive(const char *path)
{
	if (IS_DIR_SEP(path[0]) && IS_DIR_SEP(path[1]))
	{
		path += 2;
		while (*path && !IS_DIR_SEP(*path))
			path++;
	}
	else if (isalpha((unsigned char) path[0]) && path[1] == ':')
	{
		path += 2;
	}
	return (char *) path;
}
#else

#define skip_drive(path)	(path)
#endif

/*
 *	has_drive_prefix
 *
 * Return true if the given pathname has a drive prefix.
 */
bool
has_drive_prefix(const char *path)
{
#ifdef WIN32
	return skip_drive(path) != path;
#else
	return false;
#endif
}

/*
 *	first_dir_separator
 *
 * Find the location of the first directory separator, return
 * NULL if not found.
 */
char *
first_dir_separator(const char *filename)
{
	const char *p;

	for (p = skip_drive(filename); *p; p++)
		if (IS_DIR_SEP(*p))
			return unconstify(char *, p);
	return NULL;
}

/*
 *	first_path_var_separator
 *
 * Find the location of the first path separator (i.e. ':' on
 * Unix, ';' on Windows), return NULL if not found.
 */
char *
first_path_var_separator(const char *pathlist)
{
	const char *p;

	/* skip_drive is not needed */
	for (p = pathlist; *p; p++)
		if (IS_PATH_VAR_SEP(*p))
			return unconstify(char *, p);
	return NULL;
}

/*
 *	last_dir_separator
 *
 * Find the location of the last directory separator, return
 * NULL if not found.
 */
char *
last_dir_separator(const char *filename)
{
	const char *p,
			   *ret = NULL;

	for (p = skip_drive(filename); *p; p++)
		if (IS_DIR_SEP(*p))
			ret = p;
	return unconstify(char *, ret);
}


/*
 *	make_native_path - on WIN32, change / to \ in the path
 *
 *	This effectively undoes canonicalize_path.
 *
 *	This is required because WIN32 COPY is an internal CMD.EXE
 *	command and doesn't process forward slashes in the same way
 *	as external commands.  Quoting the first argument to COPY
 *	does not convert forward to backward slashes, but COPY does
 *	properly process quoted forward slashes in the second argument.
 *
 *	COPY works with quoted forward slashes in the first argument
 *	only if the current directory is the same as the directory
 *	of the first argument.
 */
void
make_native_path(char *filename)
{
#ifdef WIN32
	char	   *p;

	for (p = filename; *p; p++)
		if (*p == '/')
			*p = '\\';
#endif
}


/*
 * This function cleans up the paths for use with either cmd.exe or Msys
 * on Windows. We need them to use filenames without spaces, for which a
 * short filename is the safest equivalent, eg:
 *		C:/Progra~1/
 */
void
cleanup_path(char *path)
{
#ifdef WIN32
	char	   *ptr;

	/*
	 * GetShortPathName() will fail if the path does not exist, or short names
	 * are disabled on this file system.  In both cases, we just return the
	 * original path.  This is particularly useful for --sysconfdir, which
	 * might not exist.
	 */
	GetShortPathName(path, path, MAXPGPATH - 1);

	/* Replace '\' with '/' */
	for (ptr = path; *ptr; ptr++)
	{
		if (*ptr == '\\')
			*ptr = '/';
	}
#endif
}


/*
 * join_path_components - join two path components, inserting a slash
 *
 * We omit the slash if either given component is empty.
 *
 * ret_path is the output area (must be of size MAXPGPATH)
 *
 * ret_path can be the same as head, but not the same as tail.
 */
void
join_path_components(char *ret_path,
					 const char *head, const char *tail)
{
	if (ret_path != head)
		strlcpy(ret_path, head, MAXPGPATH);

	/*
	 * Remove any leading "." in the tail component.
	 *
	 * Note: we used to try to remove ".." as well, but that's tricky to get
	 * right; now we just leave it to be done by canonicalize_path() later.
	 */
	while (tail[0] == '.' && IS_DIR_SEP(tail[1]))
		tail += 2;

	if (*tail)
	{
		/* only separate with slash if head wasn't empty */
		snprintf(ret_path + strlen(ret_path), MAXPGPATH - strlen(ret_path),
				 "%s%s",
				 (*(skip_drive(head)) != '\0') ? "/" : "",
				 tail);
	}
}


/*
 *	Clean up path by:
 *		o  make Win32 path use Unix slashes
 *		o  remove trailing quote on Win32
 *		o  remove trailing slash
 *		o  remove duplicate adjacent separators
 *		o  remove trailing '.'
 *		o  process trailing '..' ourselves
 */
void
canonicalize_path(char *path)
{
	char	   *p,
			   *to_p;
	char	   *spath;
	bool		was_sep = false;
	int			pending_strips;

#ifdef WIN32

	/*
	 * The Windows command processor will accept suitably quoted paths with
	 * forward slashes, but barfs badly with mixed forward and back slashes.
	 */
	for (p = path; *p; p++)
	{
		if (*p == '\\')
			*p = '/';
	}

	/*
	 * In Win32, if you do: prog.exe "a b" "\c\d\" the system will pass \c\d"
	 * as argv[2], so trim off trailing quote.
	 */
	if (p > path && *(p - 1) == '"')
		*(p - 1) = '/';
#endif

	/*
	 * Removing the trailing slash on a path means we never get ugly double
	 * trailing slashes. Also, Win32 can't stat() a directory with a trailing
	 * slash. Don't remove a leading slash, though.
	 */
	trim_trailing_separator(path);

	/*
	 * Remove duplicate adjacent separators
	 */
	p = path;
#ifdef WIN32
	/* Don't remove leading double-slash on Win32 */
	if (*p)
		p++;
#endif
	to_p = p;
	for (; *p; p++, to_p++)
	{
		/* Handle many adjacent slashes, like "/a///b" */
		while (*p == '/' && was_sep)
			p++;
		if (to_p != p)
			*to_p = *p;
		was_sep = (*p == '/');
	}
	*to_p = '\0';

	/*
	 * Remove any trailing uses of "." and process ".." ourselves
	 *
	 * Note that "/../.." should reduce to just "/", while "../.." has to be
	 * kept as-is.  In the latter case we put back mistakenly trimmed ".."
	 * components below.  Also note that we want a Windows drive spec to be
	 * visible to trim_directory(), but it's not part of the logic that's
	 * looking at the name components; hence distinction between path and
	 * spath.
	 */
	spath = skip_drive(path);
	pending_strips = 0;
	for (;;)
	{
		int			len = strlen(spath);

		if (len >= 2 && strcmp(spath + len - 2, "/.") == 0)
			trim_directory(path);
		else if (strcmp(spath, ".") == 0)
		{
			/* Want to leave "." alone, but "./.." has to become ".." */
			if (pending_strips > 0)
				*spath = '\0';
			break;
		}
		else if ((len >= 3 && strcmp(spath + len - 3, "/..") == 0) ||
				 strcmp(spath, "..") == 0)
		{
			trim_directory(path);
			pending_strips++;
		}
		else if (pending_strips > 0 && *spath != '\0')
		{
			/* trim a regular directory name canceled by ".." */
			trim_directory(path);
			pending_strips--;
			/* foo/.. should become ".", not empty */
			if (*spath == '\0')
				strcpy(spath, ".");
		}
		else
			break;
	}

	if (pending_strips > 0)
	{
		/*
		 * We could only get here if path is now totally empty (other than a
		 * possible drive specifier on Windows). We have to put back one or
		 * more ".."'s that we took off.
		 */
		while (--pending_strips > 0)
			strcat(path, "../");
		strcat(path, "..");
	}
}

/*
 * Detect whether a path contains any parent-directory references ("..")
 *
 * The input *must* have been put through canonicalize_path previously.
 *
 * This is a bit tricky because we mustn't be fooled by "..a.." (legal)
 * nor "C:.." (legal on Unix but not Windows).
 */
bool
path_contains_parent_reference(const char *path)
{
	int			path_len;

	path = skip_drive(path);	/* C: shouldn't affect our conclusion */

	path_len = strlen(path);

	/*
	 * ".." could be the whole path; otherwise, if it's present it must be at
	 * the beginning, in the middle, or at the end.
	 */
	if (strcmp(path, "..") == 0 ||
		strncmp(path, "../", 3) == 0 ||
		strstr(path, "/../") != NULL ||
		(path_len >= 3 && strcmp(path + path_len - 3, "/..") == 0))
		return true;

	return false;
}

/*
 * Detect whether a path is only in or below the current working directory.
 * An absolute path that matches the current working directory should
 * return false (we only want relative to the cwd).  We don't allow
 * "/../" even if that would keep us under the cwd (it is too hard to
 * track that).
 */
bool
path_is_relative_and_below_cwd(const char *path)
{
	if (is_absolute_path(path))
		return false;
	/* don't allow anything above the cwd */
	else if (path_contains_parent_reference(path))
		return false;
#ifdef WIN32

	/*
	 * On Win32, a drive letter _not_ followed by a slash, e.g. 'E:abc', is
	 * relative to the cwd on that drive, or the drive's root directory if
	 * that drive has no cwd.  Because the path itself cannot tell us which is
	 * the case, we have to assume the worst, i.e. that it is not below the
	 * cwd.  We could use GetFullPathName() to find the full path but that
	 * could change if the current directory for the drive changes underneath
	 * us, so we just disallow it.
	 */
	else if (isalpha((unsigned char) path[0]) && path[1] == ':' &&
			 !IS_DIR_SEP(path[2]))
		return false;
#endif
	else
		return true;
}

/*
 * Detect whether path1 is a prefix of path2 (including equality).
 *
 * This is pretty trivial, but it seems better to export a function than
 * to export IS_DIR_SEP.
 */
bool
path_is_prefix_of_path(const char *path1, const char *path2)
{
	int			path1_len = strlen(path1);

	if (strncmp(path1, path2, path1_len) == 0 &&
		(IS_DIR_SEP(path2[path1_len]) || path2[path1_len] == '\0'))
		return true;
	return false;
}

/*
 * Extracts the actual name of the program as called -
 * stripped of .exe suffix if any
 */
const char *
get_progname(const char *argv0)
{
	const char *nodir_name;
	char	   *progname;

	nodir_name = last_dir_separator(argv0);
	if (nodir_name)
		nodir_name++;
	else
		nodir_name = skip_drive(argv0);

	/*
	 * Make a copy in case argv[0] is modified by ps_status. Leaks memory, but
	 * called only once.
	 */
	progname = strdup(nodir_name);
	if (progname == NULL)
	{
		fprintf(stderr, "%s: out of memory\n", nodir_name);
		abort();				/* This could exit the postmaster */
	}

#if defined(__CYGWIN__) || defined(WIN32)
	/* strip ".exe" suffix, regardless of case */
	if (strlen(progname) > sizeof(EXE) - 1 &&
		pg_strcasecmp(progname + strlen(progname) - (sizeof(EXE) - 1), EXE) == 0)
		progname[strlen(progname) - (sizeof(EXE) - 1)] = '\0';
#endif

	return progname;
}


/*
 * dir_strcmp: strcmp except any two DIR_SEP characters are considered equal,
 * and we honor filesystem case insensitivity if known
 */
static int
dir_strcmp(const char *s1, const char *s2)
{
	while (*s1 && *s2)
	{
		if (
#ifndef WIN32
			*s1 != *s2
#else
		/* On windows, paths are case-insensitive */
			pg_tolower((unsigned char) *s1) != pg_tolower((unsigned char) *s2)
#endif
			&& !(IS_DIR_SEP(*s1) && IS_DIR_SEP(*s2)))
			return (int) *s1 - (int) *s2;
		s1++, s2++;
	}
	if (*s1)
		return 1;				/* s1 longer */
	if (*s2)
		return -1;				/* s2 longer */
	return 0;
}


/*
 * make_relative_path - make a path relative to the actual binary location
 *
 * This function exists to support relocation of installation trees.
 *
 *	ret_path is the output area (must be of size MAXPGPATH)
 *	target_path is the compiled-in path to the directory we want to find
 *	bin_path is the compiled-in path to the directory of executables
 *	my_exec_path is the actual location of my executable
 *
 * We determine the common prefix of target_path and bin_path, then compare
 * the remainder of bin_path to the last directory component(s) of
 * my_exec_path.  If they match, build the result as the part of my_exec_path
 * preceding the match, joined to the remainder of target_path.  If no match,
 * return target_path as-is.
 *
 * For example:
 *		target_path  = '/usr/local/share/postgresql'
 *		bin_path	 = '/usr/local/bin'
 *		my_exec_path = '/opt/pgsql/bin/postmaster'
 * Given these inputs, the common prefix is '/usr/local/', the tail of
 * bin_path is 'bin' which does match the last directory component of
 * my_exec_path, so we would return '/opt/pgsql/share/postgresql'
 */
static void
make_relative_path(char *ret_path, const char *target_path,
				   const char *bin_path, const char *my_exec_path)
{
	int			prefix_len;
	int			tail_start;
	int			tail_len;
	int			i;

	/*
	 * Determine the common prefix --- note we require it to end on a
	 * directory separator, consider eg '/usr/lib' and '/usr/libexec'.
	 */
	prefix_len = 0;
	for (i = 0; target_path[i] && bin_path[i]; i++)
	{
		if (IS_DIR_SEP(target_path[i]) && IS_DIR_SEP(bin_path[i]))
			prefix_len = i + 1;
		else if (target_path[i] != bin_path[i])
			break;
	}
	if (prefix_len == 0)
		goto no_match;			/* no common prefix? */
	tail_len = strlen(bin_path) - prefix_len;

	/*
	 * Set up my_exec_path without the actual executable name, and
	 * canonicalize to simplify comparison to bin_path.
	 */
	strlcpy(ret_path, my_exec_path, MAXPGPATH);
	trim_directory(ret_path);	/* remove my executable name */
	canonicalize_path(ret_path);

	/*
	 * Tail match?
	 */
	tail_start = (int) strlen(ret_path) - tail_len;
	if (tail_start > 0 &&
		IS_DIR_SEP(ret_path[tail_start - 1]) &&
		dir_strcmp(ret_path + tail_start, bin_path + prefix_len) == 0)
	{
		ret_path[tail_start] = '\0';
		trim_trailing_separator(ret_path);
		join_path_components(ret_path, ret_path, target_path + prefix_len);
		canonicalize_path(ret_path);
		return;
	}

no_match:
	strlcpy(ret_path, target_path, MAXPGPATH);
	canonicalize_path(ret_path);
}


/*
 * make_absolute_path
 *
 * If the given pathname isn't already absolute, make it so, interpreting
 * it relative to the current working directory.
 *
 * Also canonicalizes the path.  The result is always a malloc'd copy.
 *
 * In backend, failure cases result in ereport(ERROR); in frontend,
 * we write a complaint on stderr and return NULL.
 *
 * Note: interpretation of relative-path arguments during postmaster startup
 * should happen before doing ChangeToDataDir(), else the user will probably
 * not like the results.
 */
char *
make_absolute_path(const char *path)
{
	char	   *new;

	/* Returning null for null input is convenient for some callers */
	if (path == NULL)
		return NULL;

	if (!is_absolute_path(path))
	{
		char	   *buf;
		size_t		buflen;

		buflen = MAXPGPATH;
		for (;;)
		{
			buf = malloc(buflen);
			if (!buf)
			{
#ifndef FRONTEND
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory")));
#else
				fprintf(stderr, _("out of memory\n"));
				return NULL;
#endif
			}

			if (getcwd(buf, buflen))
				break;
			else if (errno == ERANGE)
			{
				free(buf);
				buflen *= 2;
				continue;
			}
			else
			{
				int			save_errno = errno;

				free(buf);
				errno = save_errno;
#ifndef FRONTEND
				elog(ERROR, "could not get current working directory: %m");
#else
				fprintf(stderr, _("could not get current working directory: %s\n"),
						strerror(errno));
				return NULL;
#endif
			}
		}

		new = malloc(strlen(buf) + strlen(path) + 2);
		if (!new)
		{
			free(buf);
#ifndef FRONTEND
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
#else
			fprintf(stderr, _("out of memory\n"));
			return NULL;
#endif
		}
		sprintf(new, "%s/%s", buf, path);
		free(buf);
	}
	else
	{
		new = strdup(path);
		if (!new)
		{
#ifndef FRONTEND
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
#else
			fprintf(stderr, _("out of memory\n"));
			return NULL;
#endif
		}
	}

	/* Make sure punctuation is canonical, too */
	canonicalize_path(new);

	return new;
}


/*
 *	get_share_path
 */
void
get_share_path(const char *my_exec_path, char *ret_path)
{
	make_relative_path(ret_path, PGSHAREDIR, PGBINDIR, my_exec_path);
}

/*
 *	get_etc_path
 */
void
get_etc_path(const char *my_exec_path, char *ret_path)
{
	make_relative_path(ret_path, SYSCONFDIR, PGBINDIR, my_exec_path);
}

/*
 *	get_include_path
 */
void
get_include_path(const char *my_exec_path, char *ret_path)
{
	make_relative_path(ret_path, INCLUDEDIR, PGBINDIR, my_exec_path);
}

/*
 *	get_pkginclude_path
 */
void
get_pkginclude_path(const char *my_exec_path, char *ret_path)
{
	make_relative_path(ret_path, PKGINCLUDEDIR, PGBINDIR, my_exec_path);
}

/*
 *	get_includeserver_path
 */
void
get_includeserver_path(const char *my_exec_path, char *ret_path)
{
	make_relative_path(ret_path, INCLUDEDIRSERVER, PGBINDIR, my_exec_path);
}

/*
 *	get_lib_path
 */
void
get_lib_path(const char *my_exec_path, char *ret_path)
{
	make_relative_path(ret_path, LIBDIR, PGBINDIR, my_exec_path);
}

/*
 *	get_pkglib_path
 */
void
get_pkglib_path(const char *my_exec_path, char *ret_path)
{
	make_relative_path(ret_path, PKGLIBDIR, PGBINDIR, my_exec_path);
}

/*
 *	get_locale_path
 */
void
get_locale_path(const char *my_exec_path, char *ret_path)
{
	make_relative_path(ret_path, LOCALEDIR, PGBINDIR, my_exec_path);
}

/*
 *	get_doc_path
 */
void
get_doc_path(const char *my_exec_path, char *ret_path)
{
	make_relative_path(ret_path, DOCDIR, PGBINDIR, my_exec_path);
}

/*
 *	get_html_path
 */
void
get_html_path(const char *my_exec_path, char *ret_path)
{
	make_relative_path(ret_path, HTMLDIR, PGBINDIR, my_exec_path);
}

/*
 *	get_man_path
 */
void
get_man_path(const char *my_exec_path, char *ret_path)
{
	make_relative_path(ret_path, MANDIR, PGBINDIR, my_exec_path);
}


/*
 *	get_home_path
 *
 * On Unix, this actually returns the user's home directory.  On Windows
 * it returns the PostgreSQL-specific application data folder.
 */
bool
get_home_path(char *ret_path)
{
#ifndef WIN32
	char		pwdbuf[BUFSIZ];
	struct passwd pwdstr;
	struct passwd *pwd = NULL;

	(void) pqGetpwuid(geteuid(), &pwdstr, pwdbuf, sizeof(pwdbuf), &pwd);
	if (pwd == NULL)
		return false;
	strlcpy(ret_path, pwd->pw_dir, MAXPGPATH);
	return true;
#else
	char	   *tmppath;

	/*
	 * Note: We use getenv() here because the more modern SHGetFolderPath()
	 * would force the backend to link with shell32.lib, which eats valuable
	 * desktop heap.  XXX This function is used only in psql, which already
	 * brings in shell32 via libpq.  Moving this function to its own file
	 * would keep it out of the backend, freeing it from this concern.
	 */
	tmppath = getenv("APPDATA");
	if (!tmppath)
		return false;
	snprintf(ret_path, MAXPGPATH, "%s/postgresql", tmppath);
	return true;
#endif
}


/*
 * get_parent_directory
 *
 * Modify the given string in-place to name the parent directory of the
 * named file.
 *
 * If the input is just a file name with no directory part, the result is
 * an empty string, not ".".  This is appropriate when the next step is
 * join_path_components(), but might need special handling otherwise.
 *
 * Caution: this will not produce desirable results if the string ends
 * with "..".  For most callers this is not a problem since the string
 * is already known to name a regular file.  If in doubt, apply
 * canonicalize_path() first.
 */
void
get_parent_directory(char *path)
{
	trim_directory(path);
}


/*
 *	trim_directory
 *
 *	Trim trailing directory from path, that is, remove any trailing slashes,
 *	the last pathname component, and the slash just ahead of it --- but never
 *	remove a leading slash.
 */
static void
trim_directory(char *path)
{
	char	   *p;

	path = skip_drive(path);

	if (path[0] == '\0')
		return;

	/* back up over trailing slash(es) */
	for (p = path + strlen(path) - 1; IS_DIR_SEP(*p) && p > path; p--)
		;
	/* back up over directory name */
	for (; !IS_DIR_SEP(*p) && p > path; p--)
		;
	/* if multiple slashes before directory name, remove 'em all */
	for (; p > path && IS_DIR_SEP(*(p - 1)); p--)
		;
	/* don't erase a leading slash */
	if (p == path && IS_DIR_SEP(*p))
		p++;
	*p = '\0';
}


/*
 *	trim_trailing_separator
 *
 * trim off trailing slashes, but not a leading slash
 */
static void
trim_trailing_separator(char *path)
{
	char	   *p;

	path = skip_drive(path);
	p = path + strlen(path);
	if (p > path)
		for (p--; p > path && IS_DIR_SEP(*p); p--)
			*p = '\0';
}
