/*-------------------------------------------------------------------------
 *
 * exec.c
 *		Functions for finding and validating executable files
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/port/exec.c,v 1.57 2008/01/01 19:46:00 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef S_IRUSR					/* XXX [TRH] should be in a header */
#define S_IRUSR		 S_IREAD
#define S_IWUSR		 S_IWRITE
#define S_IXUSR		 S_IEXEC
#define S_IRGRP		 ((S_IRUSR)>>3)
#define S_IWGRP		 ((S_IWUSR)>>3)
#define S_IXGRP		 ((S_IXUSR)>>3)
#define S_IROTH		 ((S_IRUSR)>>6)
#define S_IWOTH		 ((S_IWUSR)>>6)
#define S_IXOTH		 ((S_IXUSR)>>6)
#endif

#ifndef FRONTEND
/* We use only 3-parameter elog calls in this file, for simplicity */
/* NOTE: caller must provide gettext call around str! */
#define log_error(str, param)	elog(LOG, str, param)
#else
#define log_error(str, param)	(fprintf(stderr, str, param), fputc('\n', stderr))
#endif

#ifdef WIN32_ONLY_COMPILER
#define getcwd(cwd,len)  GetCurrentDirectory(len, cwd)
#endif

static int	validate_exec(const char *path);
static int	resolve_symlinks(char *path);
static char *pipe_read_line(char *cmd, char *line, int maxsize);


/*
 * validate_exec -- validate "path" as an executable file
 *
 * returns 0 if the file is found and no error is encountered.
 *		  -1 if the regular file "path" does not exist or cannot be executed.
 *		  -2 if the file is otherwise valid but cannot be read.
 */
static int
validate_exec(const char *path)
{
	struct stat buf;

#ifndef WIN32
	uid_t		euid;
	struct group *gp;
	struct passwd *pwp;
	int			i;
	int			in_grp = 0;
#else
	char		path_exe[MAXPGPATH + sizeof(".exe") - 1];
#endif
	int			is_r = 0;
	int			is_x = 0;

#ifdef WIN32
	/* Win32 requires a .exe suffix for stat() */
	if (strlen(path) >= strlen(".exe") &&
		pg_strcasecmp(path + strlen(path) - strlen(".exe"), ".exe") != 0)
	{
		strcpy(path_exe, path);
		strcat(path_exe, ".exe");
		path = path_exe;
	}
#endif

	/*
	 * Ensure that the file exists and is a regular file.
	 *
	 * XXX if you have a broken system where stat() looks at the symlink
	 * instead of the underlying file, you lose.
	 */
	if (stat(path, &buf) < 0)
		return -1;

	if ((buf.st_mode & S_IFMT) != S_IFREG)
		return -1;

	/*
	 * Ensure that we are using an authorized executable.
	 */

	/*
	 * Ensure that the file is both executable and readable (required for
	 * dynamic loading).
	 */
#ifdef WIN32
	is_r = buf.st_mode & S_IRUSR;
	is_x = buf.st_mode & S_IXUSR;
	return is_x ? (is_r ? 0 : -2) : -1;
#else
	euid = geteuid();

	/* If owned by us, just check owner bits */
	if (euid == buf.st_uid)
	{
		is_r = buf.st_mode & S_IRUSR;
		is_x = buf.st_mode & S_IXUSR;
		return is_x ? (is_r ? 0 : -2) : -1;
	}

	/* OK, check group bits */

	pwp = getpwuid(euid);		/* not thread-safe */
	if (pwp)
	{
		if (pwp->pw_gid == buf.st_gid)	/* my primary group? */
			++in_grp;
		else if (pwp->pw_name &&
				 (gp = getgrgid(buf.st_gid)) != NULL && /* not thread-safe */
				 gp->gr_mem != NULL)
		{						/* try list of member groups */
			for (i = 0; gp->gr_mem[i]; ++i)
			{
				if (!strcmp(gp->gr_mem[i], pwp->pw_name))
				{
					++in_grp;
					break;
				}
			}
		}
		if (in_grp)
		{
			is_r = buf.st_mode & S_IRGRP;
			is_x = buf.st_mode & S_IXGRP;
			return is_x ? (is_r ? 0 : -2) : -1;
		}
	}

	/* Check "other" bits */
	is_r = buf.st_mode & S_IROTH;
	is_x = buf.st_mode & S_IXOTH;
	return is_x ? (is_r ? 0 : -2) : -1;
#endif
}


/*
 * find_my_exec -- find an absolute path to a valid executable
 *
 *	argv0 is the name passed on the command line
 *	retpath is the output area (must be of size MAXPGPATH)
 *	Returns 0 if OK, -1 if error.
 *
 * The reason we have to work so hard to find an absolute path is that
 * on some platforms we can't do dynamic loading unless we know the
 * executable's location.  Also, we need a full path not a relative
 * path because we will later change working directory.  Finally, we want
 * a true path not a symlink location, so that we can locate other files
 * that are part of our installation relative to the executable.
 *
 * This function is not thread-safe because it calls validate_exec(),
 * which calls getgrgid().	This function should be used only in
 * non-threaded binaries, not in library routines.
 */
int
find_my_exec(const char *argv0, char *retpath)
{
	char		cwd[MAXPGPATH],
				test_path[MAXPGPATH];
	char	   *path;

	if (!getcwd(cwd, MAXPGPATH))
	{
		log_error(_("could not identify current directory: %s"),
				  strerror(errno));
		return -1;
	}

	/*
	 * If argv0 contains a separator, then PATH wasn't used.
	 */
	if (first_dir_separator(argv0) != NULL)
	{
		if (is_absolute_path(argv0))
			StrNCpy(retpath, argv0, MAXPGPATH);
		else
			join_path_components(retpath, cwd, argv0);
		canonicalize_path(retpath);

		if (validate_exec(retpath) == 0)
			return resolve_symlinks(retpath);

		log_error(_("invalid binary \"%s\""), retpath);
		return -1;
	}

#ifdef WIN32
	/* Win32 checks the current directory first for names without slashes */
	join_path_components(retpath, cwd, argv0);
	if (validate_exec(retpath) == 0)
		return resolve_symlinks(retpath);
#endif

	/*
	 * Since no explicit path was supplied, the user must have been relying on
	 * PATH.  We'll search the same PATH.
	 */
	if ((path = getenv("PATH")) && *path)
	{
		char	   *startp = NULL,
				   *endp = NULL;

		do
		{
			if (!startp)
				startp = path;
			else
				startp = endp + 1;

			endp = first_path_separator(startp);
			if (!endp)
				endp = startp + strlen(startp); /* point to end */

			StrNCpy(test_path, startp, Min(endp - startp + 1, MAXPGPATH));

			if (is_absolute_path(test_path))
				join_path_components(retpath, test_path, argv0);
			else
			{
				join_path_components(retpath, cwd, test_path);
				join_path_components(retpath, retpath, argv0);
			}
			canonicalize_path(retpath);

			switch (validate_exec(retpath))
			{
				case 0: /* found ok */
					return resolve_symlinks(retpath);
				case -1:		/* wasn't even a candidate, keep looking */
					break;
				case -2:		/* found but disqualified */
					log_error(_("could not read binary \"%s\""),
							  retpath);
					break;
			}
		} while (*endp);
	}

	log_error(_("could not find a \"%s\" to execute"), argv0);
	return -1;
}


/*
 * resolve_symlinks - resolve symlinks to the underlying file
 *
 * Replace "path" by the absolute path to the referenced file.
 *
 * Returns 0 if OK, -1 if error.
 *
 * Note: we are not particularly tense about producing nice error messages
 * because we are not really expecting error here; we just determined that
 * the symlink does point to a valid executable.
 */
static int
resolve_symlinks(char *path)
{
#ifdef HAVE_READLINK
	struct stat buf;
	char		orig_wd[MAXPGPATH],
				link_buf[MAXPGPATH];
	char	   *fname;

	/*
	 * To resolve a symlink properly, we have to chdir into its directory and
	 * then chdir to where the symlink points; otherwise we may fail to
	 * resolve relative links correctly (consider cases involving mount
	 * points, for example).  After following the final symlink, we use
	 * getcwd() to figure out where the heck we're at.
	 *
	 * One might think we could skip all this if path doesn't point to a
	 * symlink to start with, but that's wrong.  We also want to get rid of
	 * any directory symlinks that are present in the given path. We expect
	 * getcwd() to give us an accurate, symlink-free path.
	 */
	if (!getcwd(orig_wd, MAXPGPATH))
	{
		log_error(_("could not identify current directory: %s"),
				  strerror(errno));
		return -1;
	}

	for (;;)
	{
		char	   *lsep;
		int			rllen;

		lsep = last_dir_separator(path);
		if (lsep)
		{
			*lsep = '\0';
			if (chdir(path) == -1)
			{
				log_error(_("could not change directory to \"%s\""), path);
				return -1;
			}
			fname = lsep + 1;
		}
		else
			fname = path;

		if (lstat(fname, &buf) < 0 ||
			(buf.st_mode & S_IFMT) != S_IFLNK)
			break;

		rllen = readlink(fname, link_buf, sizeof(link_buf));
		if (rllen < 0 || rllen >= sizeof(link_buf))
		{
			log_error(_("could not read symbolic link \"%s\""), fname);
			return -1;
		}
		link_buf[rllen] = '\0';
		strcpy(path, link_buf);
	}

	/* must copy final component out of 'path' temporarily */
	strcpy(link_buf, fname);

	if (!getcwd(path, MAXPGPATH))
	{
		log_error(_("could not identify current directory: %s"),
				  strerror(errno));
		return -1;
	}
	join_path_components(path, path, link_buf);
	canonicalize_path(path);

	if (chdir(orig_wd) == -1)
	{
		log_error(_("could not change directory to \"%s\""), orig_wd);
		return -1;
	}
#endif   /* HAVE_READLINK */

	return 0;
}


/*
 * Find another program in our binary's directory,
 * then make sure it is the proper version.
 */
int
find_other_exec(const char *argv0, const char *target,
				const char *versionstr, char *retpath)
{
	char		cmd[MAXPGPATH];
	char		line[100];

	if (find_my_exec(argv0, retpath) < 0)
		return -1;

	/* Trim off program name and keep just directory */
	*last_dir_separator(retpath) = '\0';
	canonicalize_path(retpath);

	/* Now append the other program's name */
	snprintf(retpath + strlen(retpath), MAXPGPATH - strlen(retpath),
			 "/%s%s", target, EXE);

	if (validate_exec(retpath) != 0)
		return -1;

	snprintf(cmd, sizeof(cmd), "\"%s\" -V 2>%s", retpath, DEVNULL);

	if (!pipe_read_line(cmd, line, sizeof(line)))
		return -1;

	if (strcmp(line, versionstr) != 0)
		return -2;

	return 0;
}


/*
 * The runtime library's popen() on win32 does not work when being
 * called from a service when running on windows <= 2000, because
 * there is no stdin/stdout/stderr.
 *
 * Executing a command in a pipe and reading the first line from it
 * is all we need.
 */
static char *
pipe_read_line(char *cmd, char *line, int maxsize)
{
#ifndef WIN32
	FILE	   *pgver;

	/* flush output buffers in case popen does not... */
	fflush(stdout);
	fflush(stderr);

	if ((pgver = popen(cmd, "r")) == NULL)
		return NULL;

	if (fgets(line, maxsize, pgver) == NULL)
	{
		perror("fgets failure");
		return NULL;
	}

	if (pclose_check(pgver))
		return NULL;

	return line;
#else							/* WIN32 */

	SECURITY_ATTRIBUTES sattr;
	HANDLE		childstdoutrd,
				childstdoutwr,
				childstdoutrddup;
	PROCESS_INFORMATION pi;
	STARTUPINFO si;
	char	   *retval = NULL;

	sattr.nLength = sizeof(SECURITY_ATTRIBUTES);
	sattr.bInheritHandle = TRUE;
	sattr.lpSecurityDescriptor = NULL;

	if (!CreatePipe(&childstdoutrd, &childstdoutwr, &sattr, 0))
		return NULL;

	if (!DuplicateHandle(GetCurrentProcess(),
						 childstdoutrd,
						 GetCurrentProcess(),
						 &childstdoutrddup,
						 0,
						 FALSE,
						 DUPLICATE_SAME_ACCESS))
	{
		CloseHandle(childstdoutrd);
		CloseHandle(childstdoutwr);
		return NULL;
	}

	CloseHandle(childstdoutrd);

	ZeroMemory(&pi, sizeof(pi));
	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdError = childstdoutwr;
	si.hStdOutput = childstdoutwr;
	si.hStdInput = INVALID_HANDLE_VALUE;

	if (CreateProcess(NULL,
					  cmd,
					  NULL,
					  NULL,
					  TRUE,
					  0,
					  NULL,
					  NULL,
					  &si,
					  &pi))
	{
		/* Successfully started the process */
		char	   *lineptr;

		ZeroMemory(line, maxsize);

		/* Try to read at least one line from the pipe */
		/* This may require more than one wait/read attempt */
		for (lineptr = line; lineptr < line + maxsize - 1;)
		{
			DWORD		bytesread = 0;

			/* Let's see if we can read */
			if (WaitForSingleObject(childstdoutrddup, 10000) != WAIT_OBJECT_0)
				break;			/* Timeout, but perhaps we got a line already */

			if (!ReadFile(childstdoutrddup, lineptr, maxsize - (lineptr - line),
						  &bytesread, NULL))
				break;			/* Error, but perhaps we got a line already */

			lineptr += strlen(lineptr);

			if (!bytesread)
				break;			/* EOF */

			if (strchr(line, '\n'))
				break;			/* One or more lines read */
		}

		if (lineptr != line)
		{
			/* OK, we read some data */
			int			len;

			/* If we got more than one line, cut off after the first \n */
			lineptr = strchr(line, '\n');
			if (lineptr)
				*(lineptr + 1) = '\0';

			len = strlen(line);

			/*
			 * If EOL is \r\n, convert to just \n. Because stdout is a
			 * text-mode stream, the \n output by the child process is
			 * received as \r\n, so we convert it to \n.  The server main.c
			 * sets setvbuf(stdout, NULL, _IONBF, 0) which has the effect of
			 * disabling \n to \r\n expansion for stdout.
			 */
			if (len >= 2 && line[len - 2] == '\r' && line[len - 1] == '\n')
			{
				line[len - 2] = '\n';
				line[len - 1] = '\0';
				len--;
			}

			/*
			 * We emulate fgets() behaviour. So if there is no newline at the
			 * end, we add one...
			 */
			if (len == 0 || line[len - 1] != '\n')
				strcat(line, "\n");

			retval = line;
		}

		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
	}

	CloseHandle(childstdoutwr);
	CloseHandle(childstdoutrddup);

	return retval;
#endif   /* WIN32 */
}


/*
 * pclose() plus useful error reporting
 * Is this necessary?  bjm 2004-05-11
 * It is better here because pipe.c has win32 backend linkage.
 */
int
pclose_check(FILE *stream)
{
	int			exitstatus;

	exitstatus = pclose(stream);

	if (exitstatus == 0)
		return 0;				/* all is well */

	if (exitstatus == -1)
	{
		/* pclose() itself failed, and hopefully set errno */
		perror("pclose failed");
	}
	else if (WIFEXITED(exitstatus))
		log_error(_("child process exited with exit code %d"),
				  WEXITSTATUS(exitstatus));
	else if (WIFSIGNALED(exitstatus))
#if defined(WIN32)
		log_error(_("child process was terminated by exception 0x%X"),
				  WTERMSIG(exitstatus));
#elif defined(HAVE_DECL_SYS_SIGLIST) && HAVE_DECL_SYS_SIGLIST
	{
		char		str[256];

		snprintf(str, sizeof(str), "%d: %s", WTERMSIG(exitstatus),
				 WTERMSIG(exitstatus) < NSIG ?
				 sys_siglist[WTERMSIG(exitstatus)] : "(unknown)");
		log_error(_("child process was terminated by signal %s"), str);
	}
#else
		log_error(_("child process was terminated by signal %d"),
				  WTERMSIG(exitstatus));
#endif
	else
		log_error(_("child process exited with unrecognized status %d"),
				  exitstatus);

	return -1;
}


/*
 *	set_pglocale_pgservice
 *
 *	Set application-specific locale and service directory
 *
 *	This function takes the value of argv[0] rather than a full path.
 *
 * (You may be wondering why this is in exec.c.  It requires this module's
 * services and doesn't introduce any new dependencies, so this seems as
 * good as anyplace.)
 */
void
set_pglocale_pgservice(const char *argv0, const char *app)
{
	char		path[MAXPGPATH];
	char		my_exec_path[MAXPGPATH];
	char		env_path[MAXPGPATH + sizeof("PGSYSCONFDIR=")];	/* longer than
																 * PGLOCALEDIR */

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
		canonicalize_path(env_path + 12);
		putenv(strdup(env_path));
	}
#endif

	if (getenv("PGSYSCONFDIR") == NULL)
	{
		get_etc_path(my_exec_path, path);

		/* set for libpq to use */
		snprintf(env_path, sizeof(env_path), "PGSYSCONFDIR=%s", path);
		canonicalize_path(env_path + 13);
		putenv(strdup(env_path));
	}
}
