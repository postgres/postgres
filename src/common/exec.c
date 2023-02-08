/*-------------------------------------------------------------------------
 *
 * exec.c
 *		Functions for finding and validating executable files
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/common/exec.c
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

#ifdef EXEC_BACKEND
#if defined(HAVE_SYS_PERSONALITY_H)
#include <sys/personality.h>
#elif defined(HAVE_SYS_PROCCTL_H)
#include <sys/procctl.h>
#endif
#endif

/* Inhibit mingw CRT's auto-globbing of command line arguments */
#if defined(WIN32) && !defined(_MSC_VER)
extern int _CRT_glob = 0; /* 0 turns off globbing; 1 turns it on */
#endif

/*
 * Hacky solution to allow expressing both frontend and backend error reports
 * in one macro call.  First argument of log_error is an errcode() call of
 * some sort (ignored if FRONTEND); the rest are errmsg_internal() arguments,
 * i.e. message string and any parameters for it.
 *
 * Caller must provide the gettext wrapper around the message string, if
 * appropriate, so that it gets translated in the FRONTEND case; this
 * motivates using errmsg_internal() not errmsg().  We handle appending a
 * newline, if needed, inside the macro, so that there's only one translatable
 * string per call not two.
 */
#ifndef FRONTEND
#define log_error(errcodefn, ...) \
	ereport(LOG, (errcodefn, errmsg_internal(__VA_ARGS__)))
#else
#define log_error(errcodefn, ...) \
	(fprintf(stderr, __VA_ARGS__), fputc('\n', stderr))
#endif

#ifdef _MSC_VER
#define getcwd(cwd,len)  GetCurrentDirectory(len, cwd)
#endif

static int	validate_exec(const char *path);
static int	resolve_symlinks(char *path);

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
		strlcpy(path_exe, path, sizeof(path_exe) - 4);
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
		log_error(errcode_for_file_access(),
				  _("could not identify current directory: %m"));
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

		log_error(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				  _("invalid binary \"%s\""), retpath);
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
				case 0:			/* found ok */
					return resolve_symlinks(retpath);
				case -1:		/* wasn't even a candidate, keep looking */
					break;
				case -2:		/* found but disqualified */
					log_error(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							  _("could not read binary \"%s\""),
							  retpath);
					break;
			}
		} while (*endp);
	}

	log_error(errcode(ERRCODE_UNDEFINED_FILE),
			  _("could not find a \"%s\" to execute"), argv0);
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
		log_error(errcode_for_file_access(),
				  _("could not identify current directory: %m"));
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
				log_error(errcode_for_file_access(),
						  _("could not change directory to \"%s\": %m"), path);
				return -1;
			}
			fname = lsep + 1;
		}
		else
			fname = path;

		if (lstat(fname, &buf) < 0 ||
			!S_ISLNK(buf.st_mode))
			break;

		errno = 0;
		rllen = readlink(fname, link_buf, sizeof(link_buf));
		if (rllen < 0 || rllen >= sizeof(link_buf))
		{
			log_error(errcode_for_file_access(),
					  _("could not read symbolic link \"%s\": %m"), fname);
			return -1;
		}
		link_buf[rllen] = '\0';
		strcpy(path, link_buf);
	}

	/* must copy final component out of 'path' temporarily */
	strlcpy(link_buf, fname, sizeof(link_buf));

	if (!getcwd(path, MAXPGPATH))
	{
		log_error(errcode_for_file_access(),
				  _("could not identify current directory: %m"));
		return -1;
	}
	join_path_components(path, path, link_buf);
	canonicalize_path(path);

	if (chdir(orig_wd) == -1)
	{
		log_error(errcode_for_file_access(),
				  _("could not change directory to \"%s\": %m"), orig_wd);
		return -1;
	}
#endif							/* HAVE_READLINK */

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
	char		line[MAXPGPATH];

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
 * Execute a command in a pipe and read the first line from it.
 */
char *
pipe_read_line(char *cmd, char *line, int maxsize)
{
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
		log_error(errcode(ERRCODE_SYSTEM_ERROR),
				  _("pclose failed: %m"));
	}
	else
	{
		reason = wait_result_to_str(exitstatus);
		log_error(errcode(ERRCODE_SYSTEM_ERROR),
				  "%s", reason);
		pfree(reason);
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
	char	   *dup_path;

	/* don't set LC_ALL in the backend */
	if (strcmp(app, PG_TEXTDOMAIN("postgres")) != 0)
	{
		setlocale(LC_ALL, "");

		/*
		 * One could make a case for reproducing here PostmasterMain()'s test
		 * for whether the process is multithreaded.  Unlike the postmaster,
		 * no frontend program calls sigprocmask() or otherwise provides for
		 * mutual exclusion between signal handlers.  While frontends using
		 * fork(), if multithreaded, are formally exposed to undefined
		 * behavior, we have not witnessed a concrete bug.  Therefore,
		 * complaining about multithreading here may be mere pedantry.
		 */
	}

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
		dup_path = strdup(env_path);
		if (dup_path)
			putenv(dup_path);
	}
#endif

	if (getenv("PGSYSCONFDIR") == NULL)
	{
		get_etc_path(my_exec_path, path);

		/* set for libpq to use */
		snprintf(env_path, sizeof(env_path), "PGSYSCONFDIR=%s", path);
		canonicalize_path(env_path + 13);
		dup_path = strdup(env_path);
		if (dup_path)
			putenv(dup_path);
	}
}

#ifdef EXEC_BACKEND
/*
 * For the benefit of PostgreSQL developers testing EXEC_BACKEND on Unix
 * systems (code paths normally exercised only on Windows), provide a way to
 * disable address space layout randomization, if we know how on this platform.
 * Otherwise, backends may fail to attach to shared memory at the fixed address
 * chosen by the postmaster.  (See also the macOS-specific hack in
 * sysv_shmem.c.)
 */
int
pg_disable_aslr(void)
{
#if defined(HAVE_SYS_PERSONALITY_H)
	return personality(ADDR_NO_RANDOMIZE);
#elif defined(HAVE_SYS_PROCCTL_H) && defined(PROC_ASLR_FORCE_DISABLE)
	int			data = PROC_ASLR_FORCE_DISABLE;

	return procctl(P_PID, 0, PROC_ASLR_CTL, &data);
#else
	errno = ENOSYS;
	return -1;
#endif
}
#endif

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
				log_error(errcode(ERRCODE_OUT_OF_MEMORY),
						  _("out of memory"));
				goto cleanup;
			}

			if (!GetTokenInformation(hToken, tic, (LPVOID) ptdd, dwSize, &dwSize))
			{
				log_error(errcode(ERRCODE_SYSTEM_ERROR),
						  "could not get token information: error code %lu",
						  GetLastError());
				goto cleanup;
			}
		}
		else
		{
			log_error(errcode(ERRCODE_SYSTEM_ERROR),
					  "could not get token information buffer size: error code %lu",
					  GetLastError());
			goto cleanup;
		}
	}

	/* Get the ACL info */
	if (!GetAclInformation(ptdd->DefaultDacl, (LPVOID) &asi,
						   (DWORD) sizeof(ACL_SIZE_INFORMATION),
						   AclSizeInformation))
	{
		log_error(errcode(ERRCODE_SYSTEM_ERROR),
				  "could not get ACL information: error code %lu",
				  GetLastError());
		goto cleanup;
	}

	/* Get the current user SID */
	if (!GetTokenUser(hToken, &pTokenUser))
		goto cleanup;			/* callee printed a message */

	/* Figure out the size of the new ACL */
	dwNewAclSize = asi.AclBytesInUse + sizeof(ACCESS_ALLOWED_ACE) +
		GetLengthSid(pTokenUser->User.Sid) - sizeof(DWORD);

	/* Allocate the ACL buffer & initialize it */
	pacl = (PACL) LocalAlloc(LPTR, dwNewAclSize);
	if (pacl == NULL)
	{
		log_error(errcode(ERRCODE_OUT_OF_MEMORY),
				  _("out of memory"));
		goto cleanup;
	}

	if (!InitializeAcl(pacl, dwNewAclSize, ACL_REVISION))
	{
		log_error(errcode(ERRCODE_SYSTEM_ERROR),
				  "could not initialize ACL: error code %lu", GetLastError());
		goto cleanup;
	}

	/* Loop through the existing ACEs, and build the new ACL */
	for (i = 0; i < (int) asi.AceCount; i++)
	{
		if (!GetAce(ptdd->DefaultDacl, i, (LPVOID *) &pace))
		{
			log_error(errcode(ERRCODE_SYSTEM_ERROR),
					  "could not get ACE: error code %lu", GetLastError());
			goto cleanup;
		}

		if (!AddAce(pacl, ACL_REVISION, MAXDWORD, pace, ((PACE_HEADER) pace)->AceSize))
		{
			log_error(errcode(ERRCODE_SYSTEM_ERROR),
					  "could not add ACE: error code %lu", GetLastError());
			goto cleanup;
		}
	}

	/* Add the new ACE for the current user */
	if (!AddAccessAllowedAceEx(pacl, ACL_REVISION, OBJECT_INHERIT_ACE, GENERIC_ALL, pTokenUser->User.Sid))
	{
		log_error(errcode(ERRCODE_SYSTEM_ERROR),
				  "could not add access allowed ACE: error code %lu",
				  GetLastError());
		goto cleanup;
	}

	/* Set the new DACL in the token */
	tddNew.DefaultDacl = pacl;

	if (!SetTokenInformation(hToken, tic, (LPVOID) &tddNew, dwNewAclSize))
	{
		log_error(errcode(ERRCODE_SYSTEM_ERROR),
				  "could not set token information: error code %lu",
				  GetLastError());
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
				log_error(errcode(ERRCODE_OUT_OF_MEMORY),
						  _("out of memory"));
				return FALSE;
			}
		}
		else
		{
			log_error(errcode(ERRCODE_SYSTEM_ERROR),
					  "could not get token information buffer size: error code %lu",
					  GetLastError());
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

		log_error(errcode(ERRCODE_SYSTEM_ERROR),
				  "could not get token information: error code %lu",
				  GetLastError());
		return FALSE;
	}

	/* Memory in *ppTokenUser is LocalFree():d by the caller */
	return TRUE;
}

#endif
