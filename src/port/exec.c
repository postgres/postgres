/*-------------------------------------------------------------------------
 *
 * exec.c
 *		Functions for finding and validating executable files
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/port/exec.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef FRONTEND
/* We use only 3- and 4-parameter elog calls in this file, for simplicity */
/* NOTE: caller must provide gettext call around str! */
#define log_error(str, param)	elog(LOG, str, param)
#define log_error4(str, param, arg1)	elog(LOG, str, param, arg1)
#else
#define log_error(str, param)	(fprintf(stderr, str, param), fputc('\n', stderr))
#define log_error4(str, param, arg1)	(fprintf(stderr, str, param, arg1), fputc('\n', stderr))
#endif

#ifdef WIN32_ONLY_COMPILER
#define getcwd(cwd,len)  GetCurrentDirectory(len, cwd)
#endif

static int	validate_exec(const char *path);
static int	resolve_symlinks(char *path);
static char *pipe_read_line(char *cmd, char *line, int maxsize);

#ifdef WIN32
static BOOL GetTokenUser(HANDLE hToken, PTOKEN_USER *ppTokenUser);
#endif

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
	int			is_r;
	int			is_x;

#ifdef WIN32
	char		path_exe[MAXPGPATH + sizeof(".exe") - 1];

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

	if (!S_ISREG(buf.st_mode))
		return -1;

	/*
	 * Ensure that the file is both executable and readable (required for
	 * dynamic loading).
	 */
#ifndef WIN32
	is_r = (access(path, R_OK) == 0);
	is_x = (access(path, X_OK) == 0);
#else
	is_r = buf.st_mode & S_IRUSR;
	is_x = buf.st_mode & S_IXUSR;
#endif
	return is_x ? (is_r ? 0 : -2) : -1;
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

			endp = first_path_var_separator(startp);
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
				log_error4(_("could not change directory to \"%s\": %s"), path, strerror(errno));
				return -1;
			}
			fname = lsep + 1;
		}
		else
			fname = path;

		if (lstat(fname, &buf) < 0 ||
			!S_ISLNK(buf.st_mode))
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
		log_error4(_("could not change directory to \"%s\": %s"), orig_wd, strerror(errno));
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

	snprintf(cmd, sizeof(cmd), "\"%s\" -V", retpath);

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

	errno = 0;
	if ((pgver = popen(cmd, "r")) == NULL)
	{
		perror("popen failure");
		return NULL;
	}

	errno = 0;
	if (fgets(line, maxsize, pgver) == NULL)
	{
		if (feof(pgver))
			fprintf(stderr, "no data was returned by command \"%s\"\n", cmd);
		else
			perror("fgets failure");
		pclose(pgver);			/* no error checking */
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
 */
int
pclose_check(FILE *stream)
{
	int			exitstatus;
	char	   *reason;

	exitstatus = pclose(stream);

	if (exitstatus == 0)
		return 0;				/* all is well */

	if (exitstatus == -1)
	{
		/* pclose() itself failed, and hopefully set errno */
		log_error(_("pclose failed: %s"), strerror(errno));
	}
	else
	{
		reason = wait_result_to_str(exitstatus);
		log_error("%s", reason);
#ifdef FRONTEND
		free(reason);
#else
		pfree(reason);
#endif
	}
	return exitstatus;
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
	if (strcmp(app, PG_TEXTDOMAIN("postgres")) != 0)
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

#ifdef WIN32

/*
 * AddUserToTokenDacl(HANDLE hToken)
 *
 * This function adds the current user account to the restricted
 * token used when we create a restricted process.
 *
 * This is required because of some security changes in Windows
 * that appeared in patches to XP/2K3 and in Vista/2008.
 *
 * On these machines, the Administrator account is not included in
 * the default DACL - you just get Administrators + System. For
 * regular users you get User + System. Because we strip Administrators
 * when we create the restricted token, we are left with only System
 * in the DACL which leads to access denied errors for later CreatePipe()
 * and CreateProcess() calls when running as Administrator.
 *
 * This function fixes this problem by modifying the DACL of the
 * token the process will use, and explicitly re-adding the current
 * user account.  This is still secure because the Administrator account
 * inherits its privileges from the Administrators group - it doesn't
 * have any of its own.
 */
BOOL
AddUserToTokenDacl(HANDLE hToken)
{
	int			i;
	ACL_SIZE_INFORMATION asi;
	ACCESS_ALLOWED_ACE *pace;
	DWORD		dwNewAclSize;
	DWORD		dwSize = 0;
	DWORD		dwTokenInfoLength = 0;
	PACL		pacl = NULL;
	PTOKEN_USER pTokenUser = NULL;
	TOKEN_DEFAULT_DACL tddNew;
	TOKEN_DEFAULT_DACL *ptdd = NULL;
	TOKEN_INFORMATION_CLASS tic = TokenDefaultDacl;
	BOOL		ret = FALSE;

	/* Figure out the buffer size for the DACL info */
	if (!GetTokenInformation(hToken, tic, (LPVOID) NULL, dwTokenInfoLength, &dwSize))
	{
		if (GetLastError() == ERROR_INSUFFICIENT_BUFFER)
		{
			ptdd = (TOKEN_DEFAULT_DACL *) LocalAlloc(LPTR, dwSize);
			if (ptdd == NULL)
			{
				log_error("could not allocate %lu bytes of memory", dwSize);
				goto cleanup;
			}

			if (!GetTokenInformation(hToken, tic, (LPVOID) ptdd, dwSize, &dwSize))
			{
				log_error("could not get token information: error code %lu", GetLastError());
				goto cleanup;
			}
		}
		else
		{
			log_error("could not get token information buffer size: error code %lu", GetLastError());
			goto cleanup;
		}
	}

	/* Get the ACL info */
	if (!GetAclInformation(ptdd->DefaultDacl, (LPVOID) &asi,
						   (DWORD) sizeof(ACL_SIZE_INFORMATION),
						   AclSizeInformation))
	{
		log_error("could not get ACL information: error code %lu", GetLastError());
		goto cleanup;
	}

	/*
	 * Get the user token for the current user, which provides us with the SID
	 * that is needed for creating the ACL.
	 */
	if (!GetTokenUser(hToken, &pTokenUser))
	{
		log_error("could not get user token: error code %lu", GetLastError());
		goto cleanup;
	}

	/* Figure out the size of the new ACL */
	dwNewAclSize = asi.AclBytesInUse + sizeof(ACCESS_ALLOWED_ACE) +
		GetLengthSid(pTokenUser->User.Sid) -sizeof(DWORD);

	/* Allocate the ACL buffer & initialize it */
	pacl = (PACL) LocalAlloc(LPTR, dwNewAclSize);
	if (pacl == NULL)
	{
		log_error("could not allocate %lu bytes of memory", dwNewAclSize);
		goto cleanup;
	}

	if (!InitializeAcl(pacl, dwNewAclSize, ACL_REVISION))
	{
		log_error("could not initialize ACL: error code %lu", GetLastError());
		goto cleanup;
	}

	/* Loop through the existing ACEs, and build the new ACL */
	for (i = 0; i < (int) asi.AceCount; i++)
	{
		if (!GetAce(ptdd->DefaultDacl, i, (LPVOID *) &pace))
		{
			log_error("could not get ACE: error code %lu", GetLastError());
			goto cleanup;
		}

		if (!AddAce(pacl, ACL_REVISION, MAXDWORD, pace, ((PACE_HEADER) pace)->AceSize))
		{
			log_error("could not add ACE: error code %lu", GetLastError());
			goto cleanup;
		}
	}

	/* Add the new ACE for the current user */
	if (!AddAccessAllowedAceEx(pacl, ACL_REVISION, OBJECT_INHERIT_ACE, GENERIC_ALL, pTokenUser->User.Sid))
	{
		log_error("could not add access allowed ACE: error code %lu", GetLastError());
		goto cleanup;
	}

	/* Set the new DACL in the token */
	tddNew.DefaultDacl = pacl;

	if (!SetTokenInformation(hToken, tic, (LPVOID) &tddNew, dwNewAclSize))
	{
		log_error("could not set token information: error code %lu", GetLastError());
		goto cleanup;
	}

	ret = TRUE;

cleanup:
	if (pTokenUser)
		LocalFree((HLOCAL) pTokenUser);

	if (pacl)
		LocalFree((HLOCAL) pacl);

	if (ptdd)
		LocalFree((HLOCAL) ptdd);

	return ret;
}

/*
 * GetTokenUser(HANDLE hToken, PTOKEN_USER *ppTokenUser)
 *
 * Get the users token information from a process token.
 *
 * The caller of this function is responsible for calling LocalFree() on the
 * returned TOKEN_USER memory.
 */
static BOOL
GetTokenUser(HANDLE hToken, PTOKEN_USER *ppTokenUser)
{
	DWORD		dwLength;

	*ppTokenUser = NULL;

	if (!GetTokenInformation(hToken,
							 TokenUser,
							 NULL,
							 0,
							 &dwLength))
	{
		if (GetLastError() == ERROR_INSUFFICIENT_BUFFER)
		{
			*ppTokenUser = (PTOKEN_USER) LocalAlloc(LPTR, dwLength);

			if (*ppTokenUser == NULL)
			{
				log_error("could not allocate %lu bytes of memory", dwLength);
				return FALSE;
			}
		}
		else
		{
			log_error("could not get token information buffer size: error code %lu", GetLastError());
			return FALSE;
		}
	}

	if (!GetTokenInformation(hToken,
							 TokenUser,
							 *ppTokenUser,
							 dwLength,
							 &dwLength))
	{
		LocalFree(*ppTokenUser);
		*ppTokenUser = NULL;

		log_error("could not get token information: error code %lu", GetLastError());
		return FALSE;
	}

	/* Memory in *ppTokenUser is LocalFree():d by the caller */
	return TRUE;
}

#endif
