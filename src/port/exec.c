/*-------------------------------------------------------------------------
 *
 * exec.c
 *
 * Portions Copyright (c) 1996-2004, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/port/exec.c,v 1.30 2004/10/18 19:08:58 momjian Exp $
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
#include <sys/stat.h>
#include <sys/wait.h>
#ifndef WIN32_CLIENT_ONLY
#include <unistd.h>
#endif

#define _(x) gettext(x)

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
#define log_error(str, param)	elog(LOG, (str), (param))
#else
#define log_error(str, param)	fprintf(stderr, (str), (param))
#endif


static void win32_make_absolute(char *path);


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
 * The reason we have to work so hard to find an absolute path is that
 * on some platforms we can't do dynamic loading unless we know the
 * executable's location.  Also, we need a full path not a relative
 * path because we will later change working directory.
 *
 * This function is not thread-safe because of it calls validate_exec(),
 * which calls getgrgid().	This function should be used only in
 * non-threaded binaries, not in library routines.
 */
int
find_my_exec(const char *argv0, char *retpath)
{
	char		cwd[MAXPGPATH],
				test_path[MAXPGPATH];
	char	   *path;

#ifndef WIN32_CLIENT_ONLY
	if (!getcwd(cwd, MAXPGPATH))
#else
	if (!GetCurrentDirectory(MAXPGPATH, cwd))
#endif
		cwd[0] = '\0';

	/*
	 * First try: use the binary that's located in the same directory if
	 * it was invoked with an explicit path. Presumably the user used an
	 * explicit path because it wasn't in PATH, and we don't want to use
	 * incompatible executables.
	 *
	 * For the binary: First try: if we're given some kind of path, use it
	 * (making sure that a relative path is made absolute before returning
	 * it).
	 */
	/* Does argv0 have a separator? */
	if ((path = last_dir_separator(argv0)))
	{
		if (*++path == '\0')
		{
			log_error("argv[0] ends with a path separator \"%s\"", argv0);
			return -1;
		}

		if (is_absolute_path(argv0))
			StrNCpy(retpath, argv0, MAXPGPATH);
		else
			snprintf(retpath, MAXPGPATH, "%s/%s", cwd, argv0);

		canonicalize_path(retpath);
		if (validate_exec(retpath) == 0)
		{
			win32_make_absolute(retpath);
			return 0;
		}
		else
		{
			log_error("invalid binary \"%s\"", retpath);
			return -1;
		}
	}

#ifdef WIN32
	/* Win32 checks the current directory first for names without slashes */
	if (validate_exec(argv0) == 0)
	{
		snprintf(retpath, MAXPGPATH, "%s/%s", cwd, argv0);
		win32_make_absolute(retpath);
		return 0;
	}
#endif

	/*
	 * Second try: since no explicit path was supplied, the user must have
	 * been relying on PATH.  We'll use the same PATH.
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
				snprintf(retpath, MAXPGPATH, "%s/%s", test_path, argv0);
			else
				snprintf(retpath, MAXPGPATH, "%s/%s/%s", cwd, test_path, argv0);

			canonicalize_path(retpath);
			switch (validate_exec(retpath))
			{
				case 0: /* found ok */
					win32_make_absolute(retpath);
					return 0;
				case -1:		/* wasn't even a candidate, keep looking */
					continue;
				case -2:		/* found but disqualified */
					log_error("could not read binary \"%s\"", retpath);
					continue;
			}
		} while (*endp);
	}

	log_error("could not find a \"%s\" to execute", argv0);
	return -1;

#if NOT_USED
	/*
	 * Win32 has a native way to find the executable name, but the above
	 * method works too.
	 */
	if (GetModuleFileName(NULL, retpath, MAXPGPATH) == 0)
		log_error("GetModuleFileName failed (%i)", (int) GetLastError());
#endif
}

/*
 * The runtime librarys popen() on win32 does not work when being
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
#else
	/* Win32 */
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
		DWORD		bytesread = 0;

		/* Successfully started the process */

		ZeroMemory(line, maxsize);

		/* Let's see if we can read */
		if (WaitForSingleObject(childstdoutrddup, 10000) != WAIT_OBJECT_0)
		{
			/* Got timeout */
			CloseHandle(pi.hProcess);
			CloseHandle(pi.hThread);
			CloseHandle(childstdoutwr);
			CloseHandle(childstdoutrddup);
			return NULL;
		}

		/* We try just once */
		if (ReadFile(childstdoutrddup, line, maxsize, &bytesread, NULL) &&
			bytesread > 0)
		{
			/* So we read some data */
			int			len = strlen(line);
			retval = line;

			/*
			 * If EOL is \r\n, convert to just \n. Because stdout is a
			 * text-mode stream, the \n output by the child process is
			 * received as \r\n, so we convert it to \n.  The server
			 * main.c sets setvbuf(stdout, NULL, _IONBF, 0) which has the
			 * effect of disabling \n to \r\n expansion for stdout.
			 */
			if (len >= 2 && line[len - 2] == '\r' && line[len - 1] == '\n')
			{
				line[len - 2] = '\n';
				line[len - 1] = '\0';
				len--;
			}

			/*
			 * We emulate fgets() behaviour. So if there is no newline at
			 * the end, we add one...
			 */
			if (len == 0 || line[len - 1] != '\n')
				strcat(line, "\n");
		}

		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
	}

	CloseHandle(childstdoutwr);
	CloseHandle(childstdoutrddup);

	return retval;
#endif
}



/*
 * Find our binary directory, then make sure the "target" executable
 * is the proper version.
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

	if (validate_exec(retpath))
		return -1;

	snprintf(cmd, sizeof(cmd), "\"%s\" -V 2>%s", retpath, DEVNULL);

	if (!pipe_read_line(cmd, line, sizeof(line)))
		return -1;

	if (strcmp(line, versionstr) != 0)
		return -2;

	return 0;
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
	{
		log_error(_("child process exited with exit code %d\n"),
				  WEXITSTATUS(exitstatus));
	}
	else if (WIFSIGNALED(exitstatus))
	{
		log_error(_("child process was terminated by signal %d\n"),
				  WTERMSIG(exitstatus));
	}
	else
	{
		log_error(_("child process exited with unrecognized status %d\n"),
				  exitstatus);
	}

	return -1;
}


/*
 * Windows doesn't like relative paths to executables (other things work fine)
 * so we call its builtin function to expand them. Elsewhere this is a NOOP
 */
static void
win32_make_absolute(char *path)
{
#ifdef WIN32
	char		abspath[MAXPGPATH];

	if (_fullpath(abspath, path, MAXPGPATH) == NULL)
	{
		log_error("Win32 path expansion failed: %s", strerror(errno));
		StrNCpy(abspath, path, MAXPGPATH);
	}
	canonicalize_path(abspath);

	StrNCpy(path, abspath, MAXPGPATH);
#endif
}
