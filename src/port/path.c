/*-------------------------------------------------------------------------
 *
 * path.c
 *	  portable path handling routines
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
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
static char *trim_directory(char *path);
static void trim_trailing_separator(char *path);
static char *append_subdir_to_path(char *path, char *subdir);


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
	 * We used to try to simplify some cases involving "." and "..", but now
	 * we just leave that to be done by canonicalize_path() later.
	 */

	if (*tail)
	{
		/* only separate with slash if head wasn't empty */
		snprintf(ret_path + strlen(ret_path), MAXPGPATH - strlen(ret_path),
				 "%s%s",
				 (*(skip_drive(head)) != '\0') ? "/" : "",
				 tail);
	}
}


/* State-machine states for canonicalize_path */
typedef enum
{
	ABSOLUTE_PATH_INIT,			/* Just past the leading '/' (and Windows
								 * drive name if any) of an absolute path */
	ABSOLUTE_WITH_N_DEPTH,		/* We collected 'pathdepth' directories in an
								 * absolute path */
	RELATIVE_PATH_INIT,			/* At start of a relative path */
	RELATIVE_WITH_N_DEPTH,		/* We collected 'pathdepth' directories in a
								 * relative path */
	RELATIVE_WITH_PARENT_REF	/* Relative path containing only double-dots */
} canonicalize_state;

/*
 *	Clean up path by:
 *		o  make Win32 path use Unix slashes
 *		o  remove trailing quote on Win32
 *		o  remove trailing slash
 *		o  remove duplicate (adjacent) separators
 *		o  remove '.' (unless path reduces to only '.')
 *		o  process '..' ourselves, removing it if possible
 */
void
canonicalize_path(char *path)
{
	char	   *p,
			   *to_p;
	char	   *spath;
	char	   *parsed;
	char	   *unparse;
	bool		was_sep = false;
	canonicalize_state state;
	int			pathdepth = 0;	/* counts collected regular directory names */

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
	 * Remove any uses of "." and process ".." ourselves
	 *
	 * Note that "/../.." should reduce to just "/", while "../.." has to be
	 * kept as-is.  Also note that we want a Windows drive spec to be visible
	 * to trim_directory(), but it's not part of the logic that's looking at
	 * the name components; hence distinction between path and spath.
	 *
	 * This loop overwrites the path in-place.  This is safe since we'll never
	 * make the path longer.  "unparse" points to where we are reading the
	 * path, "parse" to where we are writing.
	 */
	spath = skip_drive(path);
	if (*spath == '\0')
		return;					/* empty path is returned as-is */

	if (*spath == '/')
	{
		state = ABSOLUTE_PATH_INIT;
		/* Skip the leading slash for absolute path */
		parsed = unparse = (spath + 1);
	}
	else
	{
		state = RELATIVE_PATH_INIT;
		parsed = unparse = spath;
	}

	while (*unparse != '\0')
	{
		char	   *unparse_next;
		bool		is_double_dot;

		/* Split off this dir name, and set unparse_next to the next one */
		unparse_next = unparse;
		while (*unparse_next && *unparse_next != '/')
			unparse_next++;
		if (*unparse_next != '\0')
			*unparse_next++ = '\0';

		/* Identify type of this dir name */
		if (strcmp(unparse, ".") == 0)
		{
			/* We can ignore "." components in all cases */
			unparse = unparse_next;
			continue;
		}

		if (strcmp(unparse, "..") == 0)
			is_double_dot = true;
		else
		{
			/* adjacent separators were eliminated above */
			Assert(*unparse != '\0');
			is_double_dot = false;
		}

		switch (state)
		{
			case ABSOLUTE_PATH_INIT:
				/* We can ignore ".." immediately after / */
				if (!is_double_dot)
				{
					/* Append first dir name (we already have leading slash) */
					parsed = append_subdir_to_path(parsed, unparse);
					state = ABSOLUTE_WITH_N_DEPTH;
					pathdepth++;
				}
				break;
			case ABSOLUTE_WITH_N_DEPTH:
				if (is_double_dot)
				{
					/* Remove last parsed dir */
					/* (trim_directory won't remove the leading slash) */
					*parsed = '\0';
					parsed = trim_directory(path);
					if (--pathdepth == 0)
						state = ABSOLUTE_PATH_INIT;
				}
				else
				{
					/* Append normal dir */
					*parsed++ = '/';
					parsed = append_subdir_to_path(parsed, unparse);
					pathdepth++;
				}
				break;
			case RELATIVE_PATH_INIT:
				if (is_double_dot)
				{
					/* Append irreducible double-dot (..) */
					parsed = append_subdir_to_path(parsed, unparse);
					state = RELATIVE_WITH_PARENT_REF;
				}
				else
				{
					/* Append normal dir */
					parsed = append_subdir_to_path(parsed, unparse);
					state = RELATIVE_WITH_N_DEPTH;
					pathdepth++;
				}
				break;
			case RELATIVE_WITH_N_DEPTH:
				if (is_double_dot)
				{
					/* Remove last parsed dir */
					*parsed = '\0';
					parsed = trim_directory(path);
					if (--pathdepth == 0)
					{
						/*
						 * If the output path is now empty, we're back to the
						 * INIT state.  However, we could have processed a
						 * path like "../dir/.." and now be down to "..", in
						 * which case enter the correct state for that.
						 */
						if (parsed == spath)
							state = RELATIVE_PATH_INIT;
						else
							state = RELATIVE_WITH_PARENT_REF;
					}
				}
				else
				{
					/* Append normal dir */
					*parsed++ = '/';
					parsed = append_subdir_to_path(parsed, unparse);
					pathdepth++;
				}
				break;
			case RELATIVE_WITH_PARENT_REF:
				if (is_double_dot)
				{
					/* Append next irreducible double-dot (..) */
					*parsed++ = '/';
					parsed = append_subdir_to_path(parsed, unparse);
				}
				else
				{
					/* Append normal dir */
					*parsed++ = '/';
					parsed = append_subdir_to_path(parsed, unparse);

					/*
					 * We can now start counting normal dirs.  But if later
					 * double-dots make us remove this dir again, we'd better
					 * revert to RELATIVE_WITH_PARENT_REF not INIT state.
					 */
					state = RELATIVE_WITH_N_DEPTH;
					pathdepth = 1;
				}
				break;
		}

		unparse = unparse_next;
	}

	/*
	 * If our output path is empty at this point, insert ".".  We don't want
	 * to do this any earlier because it'd result in an extra dot in corner
	 * cases such as "../dir/..".  Since we rejected the wholly-empty-path
	 * case above, there is certainly room.
	 */
	if (parsed == spath)
		*parsed++ = '.';

	/* And finally, ensure the output path is nul-terminated. */
	*parsed = '\0';
}

/*
 * Detect whether a path contains any parent-directory references ("..")
 *
 * The input *must* have been put through canonicalize_path previously.
 */
bool
path_contains_parent_reference(const char *path)
{
	/*
	 * Once canonicalized, an absolute path cannot contain any ".." at all,
	 * while a relative path could contain ".."(s) only at the start.  So it
	 * is sufficient to check the start of the path, after skipping any
	 * Windows drive/network specifier.
	 */
	path = skip_drive(path);	/* C: shouldn't affect our conclusion */

	if (path[0] == '.' &&
		path[1] == '.' &&
		(path[2] == '\0' || path[2] == '/'))
		return true;

	return false;
}

/*
 * Detect whether a path is only in or below the current working directory.
 *
 * The input *must* have been put through canonicalize_path previously.
 *
 * An absolute path that matches the current working directory should
 * return false (we only want relative to the cwd).
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
 *		my_exec_path = '/opt/pgsql/bin/postgres'
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
	/*
	 * We first consult $HOME.  If that's unset, try to get the info from
	 * <pwd.h>.
	 */
	const char *home;

	home = getenv("HOME");
	if (home == NULL || home[0] == '\0')
		return pg_get_user_home_dir(geteuid(), ret_path, MAXPGPATH);
	strlcpy(ret_path, home, MAXPGPATH);
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
 *
 * For the convenience of canonicalize_path, the path's new end location
 * is returned.
 */
static char *
trim_directory(char *path)
{
	char	   *p;

	path = skip_drive(path);

	if (path[0] == '\0')
		return path;

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
	return p;
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

/*
 *	append_subdir_to_path
 *
 * Append the currently-considered subdirectory name to the output
 * path in canonicalize_path.  Return the new end location of the
 * output path.
 *
 * Since canonicalize_path updates the path in-place, we must use
 * memmove not memcpy, and we don't yet terminate the path with '\0'.
 */
static char *
append_subdir_to_path(char *path, char *subdir)
{
	size_t		len = strlen(subdir);

	/* No need to copy data if path and subdir are the same. */
	if (path != subdir)
		memmove(path, subdir, len);

	return path + len;
}
