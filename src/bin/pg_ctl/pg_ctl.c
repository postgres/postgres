/*-------------------------------------------------------------------------
 *
 * pg_ctl --- start/stops/restarts the PostgreSQL server
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/bin/pg_ctl/pg_ctl.c,v 1.61.2.2 2006/01/14 16:16:08 petere Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"
#include "libpq-fe.h"

#include <locale.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "libpq/pqsignal.h"
#include "getopt_long.h"

#if defined(__CYGWIN__)
#include <sys/cygwin.h>
#include <windows.h>
/* Cygwin defines WIN32 in windows.h, but we don't want it. */
#undef WIN32
#endif

#ifndef HAVE_INT_OPTRESET
int			optreset;
#endif

/* PID can be negative for standalone backend */
typedef long pgpid_t;


#define WHITESPACE "\f\n\r\t\v" /* as defined by isspace() */

/* postmaster version ident string */
#define PM_VERSIONSTR "postmaster (PostgreSQL) " PG_VERSION "\n"


typedef enum
{
	SMART_MODE,
	FAST_MODE,
	IMMEDIATE_MODE
} ShutdownMode;


typedef enum
{
	NO_COMMAND = 0,
	START_COMMAND,
	STOP_COMMAND,
	RESTART_COMMAND,
	RELOAD_COMMAND,
	STATUS_COMMAND,
	KILL_COMMAND,
	REGISTER_COMMAND,
	UNREGISTER_COMMAND,
	RUN_AS_SERVICE_COMMAND
} CtlCommand;


static bool do_wait = false;
static bool wait_set = false;
static int	wait_seconds = 60;
static bool silent_mode = false;
static ShutdownMode shutdown_mode = SMART_MODE;
static int	sig = SIGTERM;		/* default */
static CtlCommand ctl_command = NO_COMMAND;
static char *pg_data = NULL;
static char *pgdata_opt = NULL;
static char *post_opts = NULL;
static const char *progname;
static char *log_file = NULL;
static char *postgres_path = NULL;
static char *register_servicename = "PostgreSQL";		/* FIXME: + version ID? */
static char *register_username = NULL;
static char *register_password = NULL;
static char *argv0 = NULL;

static void
write_stderr(const char *fmt,...)
/* This extension allows gcc to check the format string for consistency with
   the supplied arguments. */
__attribute__((format(printf, 1, 2)));
static void *pg_malloc(size_t size);
static char *xstrdup(const char *s);
static void do_advice(void);
static void do_help(void);
static void set_mode(char *modeopt);
static void set_sig(char *signame);
static void do_start(void);
static void do_stop(void);
static void do_restart(void);
static void do_reload(void);
static void do_status(void);
static void do_kill(pgpid_t pid);
static void print_msg(const char *msg);

#if defined(WIN32) || defined(__CYGWIN__)
static bool pgwin32_IsInstalled(SC_HANDLE);
static char *pgwin32_CommandLine(bool);
static void pgwin32_doRegister(void);
static void pgwin32_doUnregister(void);
static void pgwin32_SetServiceStatus(DWORD);
static void WINAPI pgwin32_ServiceHandler(DWORD);
static void WINAPI pgwin32_ServiceMain(DWORD, LPTSTR *);
static void pgwin32_doRunAsService(void);
#endif
static pgpid_t get_pgpid(void);
static char **readfile(const char *path);
static int	start_postmaster(void);
static bool test_postmaster_connection(void);
static bool postmaster_is_alive(pid_t pid);

static char def_postopts_file[MAXPGPATH];
static char postopts_file[MAXPGPATH];
static char pid_file[MAXPGPATH];
static char conf_file[MAXPGPATH];


#if defined(WIN32) || defined(__CYGWIN__)
static void
write_eventlog(int level, const char *line)
{
	static HANDLE evtHandle = INVALID_HANDLE_VALUE;

	if (evtHandle == INVALID_HANDLE_VALUE)
	{
		evtHandle = RegisterEventSource(NULL, "PostgreSQL");
		if (evtHandle == NULL)
		{
			evtHandle = INVALID_HANDLE_VALUE;
			return;
		}
	}

	ReportEvent(evtHandle,
				level,
				0,
				0,				/* All events are Id 0 */
				NULL,
				1,
				0,
				&line,
				NULL);
}
#endif

/*
 * Write errors to stderr (or by equal means when stderr is
 * not available).
 */
static void
write_stderr(const char *fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
#if !defined(WIN32) && !defined(__CYGWIN__)
	/* On Unix, we just fprintf to stderr */
	vfprintf(stderr, fmt, ap);
#else

	/*
	 * On Win32, we print to stderr if running on a console, or write to
	 * eventlog if running as a service
	 */
	if (!isatty(fileno(stderr)))	/* Running as a service */
	{
		char		errbuf[2048];		/* Arbitrary size? */

		vsnprintf(errbuf, sizeof(errbuf), fmt, ap);

		write_eventlog(EVENTLOG_ERROR_TYPE, errbuf);
	}
	else
		/* Not running as service, write to stderr */
		vfprintf(stderr, fmt, ap);
#endif
	va_end(ap);
}

/*
 * routines to check memory allocations and fail noisily.
 */

static void *
pg_malloc(size_t size)
{
	void	   *result;

	result = malloc(size);
	if (!result)
	{
		write_stderr(_("%s: out of memory\n"), progname);
		exit(1);
	}
	return result;
}


static char *
xstrdup(const char *s)
{
	char	   *result;

	result = strdup(s);
	if (!result)
	{
		write_stderr(_("%s: out of memory\n"), progname);
		exit(1);
	}
	return result;
}

/*
 * Given an already-localized string, print it to stdout unless the
 * user has specified that no messages should be printed.
 */
static void
print_msg(const char *msg)
{
	if (!silent_mode)
	{
		fputs(msg, stdout);
		fflush(stdout);
	}
}

static pgpid_t
get_pgpid(void)
{
	FILE	   *pidf;
	long		pid;

	pidf = fopen(pid_file, "r");
	if (pidf == NULL)
	{
		/* No pid file, not an error on startup */
		if (errno == ENOENT)
			return 0;
		else
		{
			write_stderr(_("%s: could not open PID file \"%s\": %s\n"),
						 progname, pid_file, strerror(errno));
			exit(1);
		}
	}
	if (fscanf(pidf, "%ld", &pid) != 1)
	{
		write_stderr(_("%s: invalid data in PID file \"%s\"\n"),
					 progname, pid_file);
		exit(1);
	}
	fclose(pidf);
	return (pgpid_t) pid;
}


/*
 * get the lines from a text file - return NULL if file can't be opened
 */
static char **
readfile(const char *path)
{
	FILE	   *infile;
	int			maxlength = 0,
				linelen = 0;
	int			nlines = 0;
	char	  **result;
	char	   *buffer;
	int			c;

	if ((infile = fopen(path, "r")) == NULL)
		return NULL;

	/* pass over the file twice - the first time to size the result */

	while ((c = fgetc(infile)) != EOF)
	{
		linelen++;
		if (c == '\n')
		{
			nlines++;
			if (linelen > maxlength)
				maxlength = linelen;
			linelen = 0;
		}
	}

	/* handle last line without a terminating newline (yuck) */
	if (linelen)
		nlines++;
	if (linelen > maxlength)
		maxlength = linelen;

	/* set up the result and the line buffer */
	result = (char **) pg_malloc((nlines + 1) * sizeof(char *));
	buffer = (char *) pg_malloc(maxlength + 1);

	/* now reprocess the file and store the lines */
	rewind(infile);
	nlines = 0;
	while (fgets(buffer, maxlength + 1, infile) != NULL)
		result[nlines++] = xstrdup(buffer);

	fclose(infile);
	result[nlines] = NULL;

	return result;
}



/*
 * start/test/stop routines
 */

static int
start_postmaster(void)
{
	/*
	 * Since there might be quotes to handle here, it is easier simply to pass
	 * everything to a shell to process them.
	 */
	char		cmd[MAXPGPATH];

	/*
	 * Win32 needs START /B rather than "&".
	 *
	 * Win32 has a problem with START and quoted executable names. You must
	 * add a "" as the title at the beginning so you can quote the executable
	 * name: http://www.winnetmag.com/Article/ArticleID/14589/14589.html
	 * http://dev.remotenetworktechnology.com/cmd/cmdfaq.htm
	 */
	if (log_file != NULL)
#ifndef WIN32					/* Cygwin doesn't have START */
		snprintf(cmd, MAXPGPATH, "%s\"%s\" %s%s < \"%s\" >> \"%s\" 2>&1 &%s",
				 SYSTEMQUOTE, postgres_path, pgdata_opt, post_opts,
				 DEVNULL, log_file, SYSTEMQUOTE);
#else
		snprintf(cmd, MAXPGPATH, "%sSTART /B \"\" \"%s\" %s%s < \"%s\" >> \"%s\" 2>&1%s",
				 SYSTEMQUOTE, postgres_path, pgdata_opt, post_opts,
				 DEVNULL, log_file, SYSTEMQUOTE);
#endif
	else
#ifndef WIN32					/* Cygwin doesn't have START */
		snprintf(cmd, MAXPGPATH, "%s\"%s\" %s%s < \"%s\" 2>&1 &%s",
				 SYSTEMQUOTE, postgres_path, pgdata_opt, post_opts,
				 DEVNULL, SYSTEMQUOTE);
#else
		snprintf(cmd, MAXPGPATH, "%sSTART /B \"\" \"%s\" %s%s < \"%s\" 2>&1%s",
				 SYSTEMQUOTE, postgres_path, pgdata_opt, post_opts,
				 DEVNULL, SYSTEMQUOTE);
#endif

	return system(cmd);
}



/* Find the pgport and try a connection */
static bool
test_postmaster_connection(void)
{
	PGconn	   *conn;
	bool		success = false;
	int			i;
	char		portstr[32];
	char	   *p;

	*portstr = '\0';

	/* post_opts */
	for (p = post_opts; *p;)
	{
		/* advance past whitespace/quoting */
		while (isspace((unsigned char) *p) || *p == '\'' || *p == '"')
			p++;

		if (strncmp(p, "-p", strlen("-p")) == 0)
		{
			p += strlen("-p");
			/* advance past whitespace/quoting */
			while (isspace((unsigned char) *p) || *p == '\'' || *p == '"')
				p++;
			StrNCpy(portstr, p, Min(strcspn(p, "\"'" WHITESPACE) + 1,
									sizeof(portstr)));
			/* keep looking, maybe there is another -p */
		}
		/* Advance to next whitespace */
		while (*p && !isspace((unsigned char) *p))
			p++;
	}

	/* config file */
	if (!*portstr)
	{
		char	  **optlines;

		optlines = readfile(conf_file);
		if (optlines != NULL)
		{
			for (; *optlines != NULL; optlines++)
			{
				p = *optlines;

				while (isspace((unsigned char) *p))
					p++;
				if (strncmp(p, "port", strlen("port")) != 0)
					continue;
				p += strlen("port");
				while (isspace((unsigned char) *p))
					p++;
				if (*p != '=')
					continue;
				p++;
				while (isspace((unsigned char) *p))
					p++;
				StrNCpy(portstr, p, Min(strcspn(p, "#" WHITESPACE) + 1,
										sizeof(portstr)));
				/* keep looking, maybe there is another */
			}
		}
	}

	/* environment */
	if (!*portstr && getenv("PGPORT") != NULL)
		StrNCpy(portstr, getenv("PGPORT"), sizeof(portstr));

	/* default */
	if (!*portstr)
		snprintf(portstr, sizeof(portstr), "%d", DEF_PGPORT);

	for (i = 0; i < wait_seconds; i++)
	{
		if ((conn = PQsetdbLogin(NULL, portstr, NULL, NULL,
								 "postgres", NULL, NULL)) != NULL &&
			(PQstatus(conn) == CONNECTION_OK ||
			 (strcmp(PQerrorMessage(conn),
					 PQnoPasswordSupplied) == 0)))
		{
			PQfinish(conn);
			success = true;
			break;
		}
		else
		{
			print_msg(".");
			pg_usleep(1000000); /* 1 sec */
		}
	}

	return success;
}



static void
do_start(void)
{
	pgpid_t		pid;
	pgpid_t		old_pid = 0;
	char	   *optline = NULL;
	int			exitcode;

	if (ctl_command != RESTART_COMMAND)
	{
		old_pid = get_pgpid();
		if (old_pid != 0)
			write_stderr(_("%s: another postmaster may be running; "
						   "trying to start postmaster anyway\n"),
						 progname);
	}

	if (post_opts == NULL)
	{
		char	  **optlines;
		int			len;

		optlines = readfile(ctl_command == RESTART_COMMAND ?
							postopts_file : def_postopts_file);
		if (optlines == NULL)
		{
			if (ctl_command == START_COMMAND)
				post_opts = "";
			else
			{
				write_stderr(_("%s: could not read file \"%s\"\n"), progname, postopts_file);
				exit(1);
			}
		}
		else if (optlines[0] == NULL || optlines[1] != NULL)
		{
			write_stderr(_("%s: option file \"%s\" must have exactly one line\n"),
						 progname, ctl_command == RESTART_COMMAND ?
						 postopts_file : def_postopts_file);
			exit(1);
		}
		else
		{
			optline = optlines[0];
			len = strcspn(optline, "\r\n");
			optline[len] = '\0';

			if (ctl_command == RESTART_COMMAND)
			{
				char	   *arg1;

				arg1 = strchr(optline, *SYSTEMQUOTE);
				if (arg1 == NULL || arg1 == optline)
					post_opts = "";
				else
				{
					*(arg1 - 1) = '\0'; /* this should be a space */
					post_opts = arg1;
				}
				if (postgres_path != NULL)
					postgres_path = optline;
			}
			else
				post_opts = optline;
		}
	}

	/* No -D or -D already added during server start */
	if (ctl_command == RESTART_COMMAND || pgdata_opt == NULL)
		pgdata_opt = "";

	if (postgres_path == NULL)
	{
		char	   *postmaster_path;
		int			ret;

		postmaster_path = pg_malloc(MAXPGPATH);

		if ((ret = find_other_exec(argv0, "postmaster", PM_VERSIONSTR,
								   postmaster_path)) < 0)
		{
			char		full_path[MAXPGPATH];

			if (find_my_exec(argv0, full_path) < 0)
				StrNCpy(full_path, progname, MAXPGPATH);

			if (ret == -1)
				write_stderr(_("The program \"postmaster\" is needed by %s "
							   "but was not found in the\n"
							   "same directory as \"%s\".\n"
							   "Check your installation.\n"),
							 progname, full_path);
			else
				write_stderr(_("The program \"postmaster\" was found by \"%s\"\n"
							   "but was not the same version as %s.\n"
							   "Check your installation.\n"),
							 full_path, progname);
			exit(1);
		}
		postgres_path = postmaster_path;
	}

	exitcode = start_postmaster();
	if (exitcode != 0)
	{
		write_stderr(_("%s: could not start postmaster: exit code was %d\n"),
					 progname, exitcode);
		exit(1);
	}

	if (old_pid != 0)
	{
		pg_usleep(1000000);
		pid = get_pgpid();
		if (pid == old_pid)
		{
			write_stderr(_("%s: could not start postmaster\n"
						   "Examine the log output.\n"),
						 progname);
			exit(1);
		}
	}

	if (do_wait)
	{
		print_msg(_("waiting for postmaster to start..."));

		if (test_postmaster_connection() == false)
		{
			printf(_("could not start postmaster\n"));
			exit(1);
		}
		else
		{
			print_msg(_(" done\n"));
			print_msg(_("postmaster started\n"));
		}
	}
	else
		print_msg(_("postmaster starting\n"));
}


static void
do_stop(void)
{
	int			cnt;
	pgpid_t		pid;

	pid = get_pgpid();

	if (pid == 0)				/* no pid file */
	{
		write_stderr(_("%s: PID file \"%s\" does not exist\n"), progname, pid_file);
		write_stderr(_("Is postmaster running?\n"));
		exit(1);
	}
	else if (pid < 0)			/* standalone backend, not postmaster */
	{
		pid = -pid;
		write_stderr(_("%s: cannot stop postmaster; "
					   "postgres is running (PID: %ld)\n"),
					 progname, pid);
		exit(1);
	}

	if (kill((pid_t) pid, sig) != 0)
	{
		write_stderr(_("%s: could not send stop signal (PID: %ld): %s\n"), progname, pid,
					 strerror(errno));
		exit(1);
	}

	if (!do_wait)
	{
		print_msg(_("postmaster shutting down\n"));
		return;
	}
	else
	{
		print_msg(_("waiting for postmaster to shut down..."));

		for (cnt = 0; cnt < wait_seconds; cnt++)
		{
			if ((pid = get_pgpid()) != 0)
			{
				print_msg(".");
				pg_usleep(1000000);		/* 1 sec */
			}
			else
				break;
		}

		if (pid != 0)			/* pid file still exists */
		{
			print_msg(_(" failed\n"));

			write_stderr(_("%s: postmaster does not shut down\n"), progname);
			exit(1);
		}
		print_msg(_(" done\n"));

		printf(_("postmaster stopped\n"));
	}
}


/*
 *	restart/reload routines
 */

static void
do_restart(void)
{
	int			cnt;
	pgpid_t		pid;

	pid = get_pgpid();

	if (pid == 0)				/* no pid file */
	{
		write_stderr(_("%s: PID file \"%s\" does not exist\n"),
					 progname, pid_file);
		write_stderr(_("Is postmaster running?\n"));
		write_stderr(_("starting postmaster anyway\n"));
		do_start();
		return;
	}
	else if (pid < 0)			/* standalone backend, not postmaster */
	{
		pid = -pid;
		if (postmaster_is_alive((pid_t) pid))
		{
			write_stderr(_("%s: cannot restart postmaster; "
						   "postgres is running (PID: %ld)\n"),
						 progname, pid);
			write_stderr(_("Please terminate postgres and try again.\n"));
			exit(1);
		}
	}

	if (postmaster_is_alive((pid_t) pid))
	{
		if (kill((pid_t) pid, sig) != 0)
		{
			write_stderr(_("%s: could not send stop signal (PID: %ld): %s\n"), progname, pid,
						 strerror(errno));
			exit(1);
		}

		print_msg(_("waiting for postmaster to shut down..."));

		/* always wait for restart */

		for (cnt = 0; cnt < wait_seconds; cnt++)
		{
			if ((pid = get_pgpid()) != 0)
			{
				print_msg(".");
				pg_usleep(1000000);		/* 1 sec */
			}
			else
				break;
		}

		if (pid != 0)			/* pid file still exists */
		{
			print_msg(_(" failed\n"));

			write_stderr(_("%s: postmaster does not shut down\n"), progname);
			exit(1);
		}

		print_msg(_(" done\n"));
		printf(_("postmaster stopped\n"));
	}
	else
	{
		write_stderr(_("%s: old postmaster process (PID: %ld) seems to be gone\n"),
					 progname, pid);
		write_stderr(_("starting postmaster anyway\n"));
	}

	do_start();
}


static void
do_reload(void)
{
	pgpid_t		pid;

	pid = get_pgpid();
	if (pid == 0)				/* no pid file */
	{
		write_stderr(_("%s: PID file \"%s\" does not exist\n"), progname, pid_file);
		write_stderr(_("Is postmaster running?\n"));
		exit(1);
	}
	else if (pid < 0)			/* standalone backend, not postmaster */
	{
		pid = -pid;
		write_stderr(_("%s: cannot reload postmaster; "
					   "postgres is running (PID: %ld)\n"),
					 progname, pid);
		write_stderr(_("Please terminate postgres and try again.\n"));
		exit(1);
	}

	if (kill((pid_t) pid, sig) != 0)
	{
		write_stderr(_("%s: could not send reload signal (PID: %ld): %s\n"),
					 progname, pid, strerror(errno));
		exit(1);
	}

	print_msg(_("postmaster signaled\n"));
}

/*
 *	utility routines
 */

static bool
postmaster_is_alive(pid_t pid)
{
	/*
	 * Test to see if the process is still there.  Note that we do not
	 * consider an EPERM failure to mean that the process is still there;
	 * EPERM must mean that the given PID belongs to some other userid, and
	 * considering the permissions on $PGDATA, that means it's not the
	 * postmaster we are after.
	 *
	 * Don't believe that our own PID or parent shell's PID is the postmaster,
	 * either.	(Windows hasn't got getppid(), though.)
	 */
	if (pid == getpid())
		return false;
#ifndef WIN32
	if (pid == getppid())
		return false;
#endif
	if (kill(pid, 0) == 0)
		return true;
	return false;
}

static void
do_status(void)
{
	pgpid_t		pid;

	pid = get_pgpid();
	if (pid != 0)				/* 0 means no pid file */
	{
		if (pid < 0)			/* standalone backend */
		{
			pid = -pid;
			if (postmaster_is_alive((pid_t) pid))
			{
				printf(_("%s: a standalone backend \"postgres\" is running (PID: %ld)\n"),
					   progname, pid);
				return;
			}
		}
		else
			/* postmaster */
		{
			if (postmaster_is_alive((pid_t) pid))
			{
				char	  **optlines;

				printf(_("%s: postmaster is running (PID: %ld)\n"),
					   progname, pid);

				optlines = readfile(postopts_file);
				if (optlines != NULL)
					for (; *optlines != NULL; optlines++)
						fputs(*optlines, stdout);
				return;
			}
		}
	}
	printf(_("%s: neither postmaster nor postgres running\n"), progname);
	exit(1);
}



static void
do_kill(pgpid_t pid)
{
	if (kill((pid_t) pid, sig) != 0)
	{
		write_stderr(_("%s: could not send signal %d (PID: %ld): %s\n"),
					 progname, sig, pid, strerror(errno));
		exit(1);
	}
}

#if defined(WIN32) || defined(__CYGWIN__)

static bool
pgwin32_IsInstalled(SC_HANDLE hSCM)
{
	SC_HANDLE	hService = OpenService(hSCM, register_servicename, SERVICE_QUERY_CONFIG);
	bool		bResult = (hService != NULL);

	if (bResult)
		CloseServiceHandle(hService);
	return bResult;
}

static char *
pgwin32_CommandLine(bool registration)
{
	static char cmdLine[MAXPGPATH];
	int			ret;

#ifdef __CYGWIN__
	char		buf[MAXPGPATH];
#endif

	if (registration)
	{
		ret = find_my_exec(argv0, cmdLine);
		if (ret != 0)
		{
			write_stderr(_("%s: could not find own program executable\n"), progname);
			exit(1);
		}
	}
	else
	{
		ret = find_other_exec(argv0, "postmaster", PM_VERSIONSTR, cmdLine);
		if (ret != 0)
		{
			write_stderr(_("%s: could not find postmaster program executable\n"), progname);
			exit(1);
		}
	}

#ifdef __CYGWIN__
	/* need to convert to windows path */
	cygwin_conv_to_full_win32_path(cmdLine, buf);
	strcpy(cmdLine, buf);
#endif

	if (registration)
	{
		if (pg_strcasecmp(cmdLine + strlen(cmdLine) - 4, ".exe"))
		{
			/* If commandline does not end in .exe, append it */
			strcat(cmdLine, ".exe");
		}
		strcat(cmdLine, " runservice -N \"");
		strcat(cmdLine, register_servicename);
		strcat(cmdLine, "\"");
	}

	if (pg_data)
	{
		strcat(cmdLine, " -D \"");
		strcat(cmdLine, pg_data);
		strcat(cmdLine, "\"");
	}

	if (do_wait)
		strcat(cmdLine, " -w");

	if (post_opts)
	{
		strcat(cmdLine, " ");
		if (registration)
			strcat(cmdLine, " -o \"");
		strcat(cmdLine, post_opts);
		if (registration)
			strcat(cmdLine, "\"");
	}

	return cmdLine;
}

static void
pgwin32_doRegister(void)
{
	SC_HANDLE	hService;
	SC_HANDLE	hSCM = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);

	if (hSCM == NULL)
	{
		write_stderr(_("%s: could not open service manager\n"), progname);
		exit(1);
	}
	if (pgwin32_IsInstalled(hSCM))
	{
		CloseServiceHandle(hSCM);
		write_stderr(_("%s: service \"%s\" already registered\n"), progname, register_servicename);
		exit(1);
	}

	if ((hService = CreateService(hSCM, register_servicename, register_servicename,
							   SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
								  SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
								  pgwin32_CommandLine(true),
	   NULL, NULL, "RPCSS\0", register_username, register_password)) == NULL)
	{
		CloseServiceHandle(hSCM);
		write_stderr(_("%s: could not register service \"%s\": error code %d\n"), progname, register_servicename, (int) GetLastError());
		exit(1);
	}
	CloseServiceHandle(hService);
	CloseServiceHandle(hSCM);
}

static void
pgwin32_doUnregister(void)
{
	SC_HANDLE	hService;
	SC_HANDLE	hSCM = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);

	if (hSCM == NULL)
	{
		write_stderr(_("%s: could not open service manager\n"), progname);
		exit(1);
	}
	if (!pgwin32_IsInstalled(hSCM))
	{
		CloseServiceHandle(hSCM);
		write_stderr(_("%s: service \"%s\" not registered\n"), progname, register_servicename);
		exit(1);
	}

	if ((hService = OpenService(hSCM, register_servicename, DELETE)) == NULL)
	{
		CloseServiceHandle(hSCM);
		write_stderr(_("%s: could not open service \"%s\": error code %d\n"), progname, register_servicename, (int) GetLastError());
		exit(1);
	}
	if (!DeleteService(hService))
	{
		CloseServiceHandle(hService);
		CloseServiceHandle(hSCM);
		write_stderr(_("%s: could not unregister service \"%s\": error code %d\n"), progname, register_servicename, (int) GetLastError());
		exit(1);
	}
	CloseServiceHandle(hService);
	CloseServiceHandle(hSCM);
}


static SERVICE_STATUS status;
static SERVICE_STATUS_HANDLE hStatus = (SERVICE_STATUS_HANDLE) 0;
static HANDLE shutdownHandles[2];
static pid_t postmasterPID = -1;

#define shutdownEvent	  shutdownHandles[0]
#define postmasterProcess shutdownHandles[1]

static void
pgwin32_SetServiceStatus(DWORD currentState)
{
	status.dwCurrentState = currentState;
	SetServiceStatus(hStatus, (LPSERVICE_STATUS) & status);
}

static void WINAPI
pgwin32_ServiceHandler(DWORD request)
{
	switch (request)
	{
		case SERVICE_CONTROL_STOP:
		case SERVICE_CONTROL_SHUTDOWN:

			/*
			 * We only need a short wait hint here as it just needs to wait
			 * for the next checkpoint. They occur every 5 seconds during
			 * shutdown
			 */
			status.dwWaitHint = 10000;
			pgwin32_SetServiceStatus(SERVICE_STOP_PENDING);
			SetEvent(shutdownEvent);
			return;

		case SERVICE_CONTROL_PAUSE:
			/* Win32 config reloading */
			status.dwWaitHint = 5000;
			kill(postmasterPID, SIGHUP);
			return;

			/* FIXME: These could be used to replace other signals etc */
		case SERVICE_CONTROL_CONTINUE:
		case SERVICE_CONTROL_INTERROGATE:
		default:
			break;
	}
}

static void WINAPI
pgwin32_ServiceMain(DWORD argc, LPTSTR * argv)
{
	STARTUPINFO si;
	PROCESS_INFORMATION pi;
	DWORD		ret;

	/* Initialize variables */
	status.dwWin32ExitCode = S_OK;
	status.dwCheckPoint = 0;
	status.dwWaitHint = 60000;
	status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
	status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_PAUSE_CONTINUE;
	status.dwServiceSpecificExitCode = 0;
	status.dwCurrentState = SERVICE_START_PENDING;

	memset(&pi, 0, sizeof(pi));
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);

	/* Register the control request handler */
	if ((hStatus = RegisterServiceCtrlHandler(register_servicename, pgwin32_ServiceHandler)) == (SERVICE_STATUS_HANDLE) 0)
		return;

	if ((shutdownEvent = CreateEvent(NULL, true, false, NULL)) == NULL)
		return;

	/* Start the postmaster */
	pgwin32_SetServiceStatus(SERVICE_START_PENDING);
	if (!CreateProcess(NULL, pgwin32_CommandLine(false), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
	{
		pgwin32_SetServiceStatus(SERVICE_STOPPED);
		return;
	}
	postmasterPID = pi.dwProcessId;
	postmasterProcess = pi.hProcess;
	CloseHandle(pi.hThread);
	pgwin32_SetServiceStatus(SERVICE_RUNNING);

	/* Wait for quit... */
	ret = WaitForMultipleObjects(2, shutdownHandles, FALSE, INFINITE);
	pgwin32_SetServiceStatus(SERVICE_STOP_PENDING);
	switch (ret)
	{
		case WAIT_OBJECT_0:		/* shutdown event */
			kill(postmasterPID, SIGINT);

			/*
			 * Increment the checkpoint and try again Abort after 12
			 * checkpoints as the postmaster has probably hung
			 */
			while (WaitForSingleObject(postmasterProcess, 5000) == WAIT_TIMEOUT && status.dwCheckPoint < 12)
				status.dwCheckPoint++;
			break;

		case (WAIT_OBJECT_0 + 1):		/* postmaster went down */
			break;

		default:
			/* shouldn't get here? */
			break;
	}

	CloseHandle(shutdownEvent);
	CloseHandle(postmasterProcess);

	pgwin32_SetServiceStatus(SERVICE_STOPPED);
}

static void
pgwin32_doRunAsService(void)
{
	SERVICE_TABLE_ENTRY st[] = {{register_servicename, pgwin32_ServiceMain},
	{NULL, NULL}};

	if (StartServiceCtrlDispatcher(st) == 0)
	{
		write_stderr(_("%s: could not start service \"%s\": error code %d\n"), progname, register_servicename, (int) GetLastError());
		exit(1);
	}
}
#endif

static void
do_advice(void)
{
	write_stderr(_("Try \"%s --help\" for more information.\n"), progname);
}



static void
do_help(void)
{
	printf(_("%s is a utility to start, stop, restart, reload configuration files,\n"
			 "report the status of a PostgreSQL server, or signal a PostgreSQL process.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s start   [-w] [-D DATADIR] [-s] [-l FILENAME] [-o \"OPTIONS\"]\n"), progname);
	printf(_("  %s stop    [-W] [-D DATADIR] [-s] [-m SHUTDOWN-MODE]\n"), progname);
	printf(_("  %s restart [-w] [-D DATADIR] [-s] [-m SHUTDOWN-MODE] [-o \"OPTIONS\"]\n"), progname);
	printf(_("  %s reload  [-D DATADIR] [-s]\n"), progname);
	printf(_("  %s status  [-D DATADIR]\n"), progname);
	printf(_("  %s kill    SIGNALNAME PID\n"), progname);
#if defined(WIN32) || defined(__CYGWIN__)
	printf(_("  %s register   [-N SERVICENAME] [-U USERNAME] [-P PASSWORD] [-D DATADIR]\n"
			 "                    [-w] [-o \"OPTIONS\"]\n"), progname);
	printf(_("  %s unregister [-N SERVICENAME]\n"), progname);
#endif

	printf(_("\nCommon options:\n"));
	printf(_("  -D, --pgdata DATADIR   location of the database storage area\n"));
	printf(_("  -s, --silent           only print errors, no informational messages\n"));
	printf(_("  -w                     wait until operation completes\n"));
	printf(_("  -W                     do not wait until operation completes\n"));
	printf(_("  --help                 show this help, then exit\n"));
	printf(_("  --version              output version information, then exit\n"));
	printf(_("(The default is to wait for shutdown, but not for start or restart.)\n\n"));
	printf(_("If the -D option is omitted, the environment variable PGDATA is used.\n"));

	printf(_("\nOptions for start or restart:\n"));
	printf(_("  -l, --log FILENAME     write (or append) server log to FILENAME\n"));
	printf(_("  -o OPTIONS             command line options to pass to the postmaster\n"
			 "                         (PostgreSQL server executable)\n"));
	printf(_("  -p PATH-TO-POSTMASTER  normally not necessary\n"));

	printf(_("\nOptions for stop or restart:\n"));
	printf(_("  -m SHUTDOWN-MODE   may be \"smart\", \"fast\", or \"immediate\"\n"));

	printf(_("\nShutdown modes are:\n"));
	printf(_("  smart       quit after all clients have disconnected\n"));
	printf(_("  fast        quit directly, with proper shutdown\n"));
	printf(_("  immediate   quit without complete shutdown; will lead to recovery on restart\n"));

	printf(_("\nAllowed signal names for kill:\n"));
	printf("  HUP INT QUIT ABRT TERM USR1 USR2\n");

#if defined(WIN32) || defined(__CYGWIN__)
	printf(_("\nOptions for register and unregister:\n"));
	printf(_("  -N SERVICENAME  service name with which to register PostgreSQL server\n"));
	printf(_("  -P PASSWORD     password of account to register PostgreSQL server\n"));
	printf(_("  -U USERNAME     user name of account to register PostgreSQL server\n"));
#endif

	printf(_("\nReport bugs to <pgsql-bugs@postgresql.org>.\n"));
}



static void
set_mode(char *modeopt)
{
	if (strcmp(modeopt, "s") == 0 || strcmp(modeopt, "smart") == 0)
	{
		shutdown_mode = SMART_MODE;
		sig = SIGTERM;
	}
	else if (strcmp(modeopt, "f") == 0 || strcmp(modeopt, "fast") == 0)
	{
		shutdown_mode = FAST_MODE;
		sig = SIGINT;
	}
	else if (strcmp(modeopt, "i") == 0 || strcmp(modeopt, "immediate") == 0)
	{
		shutdown_mode = IMMEDIATE_MODE;
		sig = SIGQUIT;
	}
	else
	{
		write_stderr(_("%s: unrecognized shutdown mode \"%s\"\n"), progname, modeopt);
		do_advice();
		exit(1);
	}
}



static void
set_sig(char *signame)
{
	if (!strcmp(signame, "HUP"))
		sig = SIGHUP;
	else if (!strcmp(signame, "INT"))
		sig = SIGINT;
	else if (!strcmp(signame, "QUIT"))
		sig = SIGQUIT;
	else if (!strcmp(signame, "ABRT"))
		sig = SIGABRT;

	/*
	 * probably should NOT provide SIGKILL
	 *
	 * else if (!strcmp(signame,"KILL")) sig = SIGKILL;
	 */
	else if (!strcmp(signame, "TERM"))
		sig = SIGTERM;
	else if (!strcmp(signame, "USR1"))
		sig = SIGUSR1;
	else if (!strcmp(signame, "USR2"))
		sig = SIGUSR2;
	else
	{
		write_stderr(_("%s: unrecognized signal name \"%s\"\n"), progname, signame);
		do_advice();
		exit(1);
	}

}



int
main(int argc, char **argv)
{
	static struct option long_options[] = {
		{"help", no_argument, NULL, '?'},
		{"version", no_argument, NULL, 'V'},
		{"log", required_argument, NULL, 'l'},
		{"mode", required_argument, NULL, 'm'},
		{"pgdata", required_argument, NULL, 'D'},
		{"silent", no_argument, NULL, 's'},
		{NULL, 0, NULL, 0}
	};

	int			option_index;
	int			c;
	pgpid_t		killproc = 0;

#if defined(WIN32) || defined(__CYGWIN__)
	setvbuf(stderr, NULL, _IONBF, 0);
#endif

	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], "pg_ctl");

	/*
	 * save argv[0] so do_start() can look for the postmaster if necessary. we
	 * don't look for postmaster here because in many cases we won't need it.
	 */
	argv0 = argv[0];

	umask(077);

	/* support --help and --version even if invoked as root */
	if (argc > 1)
	{
		if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0 ||
			strcmp(argv[1], "-?") == 0)
		{
			do_help();
			exit(0);
		}
		else if (strcmp(argv[1], "-V") == 0 || strcmp(argv[1], "--version") == 0)
		{
			printf("%s (PostgreSQL) %s\n", progname, PG_VERSION);
			exit(0);
		}
	}

	/*
	 * Disallow running as root, to forestall any possible security holes.
	 */
#ifndef WIN32
#ifndef __BEOS__				/* no root check on BEOS */
	if (geteuid() == 0)
	{
		write_stderr(_("%s: cannot be run as root\n"
					   "Please log in (using, e.g., \"su\") as the "
					   "(unprivileged) user that will\n"
					   "own the server process.\n"),
					 progname);
		exit(1);
	}
#endif
#endif

	/*
	 * 'Action' can be before or after args so loop over both. Some
	 * getopt_long() implementations will reorder argv[] to place all flags
	 * first (GNU?), but we don't rely on it. Our /port version doesn't do
	 * that.
	 */
	optind = 1;

	/* process command-line options */
	while (optind < argc)
	{
		while ((c = getopt_long(argc, argv, "D:l:m:N:o:p:P:sU:wW", long_options, &option_index)) != -1)
		{
			switch (c)
			{
				case 'D':
					{
						char	   *pgdata_D;
						char	   *env_var = pg_malloc(strlen(optarg) + 8);

						pgdata_D = xstrdup(optarg);
						canonicalize_path(pgdata_D);
						snprintf(env_var, strlen(optarg) + 8, "PGDATA=%s",
								 pgdata_D);
						putenv(env_var);

						/*
						 * We could pass PGDATA just in an environment
						 * variable but we do -D too for clearer postmaster
						 * 'ps' display
						 */
						pgdata_opt = pg_malloc(strlen(pgdata_D) + 7);
						snprintf(pgdata_opt, strlen(pgdata_D) + 7,
								 "-D \"%s\" ",
								 pgdata_D);
						break;
					}
				case 'l':
					log_file = xstrdup(optarg);
					break;
				case 'm':
					set_mode(optarg);
					break;
				case 'N':
					register_servicename = xstrdup(optarg);
					break;
				case 'o':
					post_opts = xstrdup(optarg);
					break;
				case 'p':
					postgres_path = xstrdup(optarg);
					break;
				case 'P':
					register_password = xstrdup(optarg);
					break;
				case 's':
					silent_mode = true;
					break;
				case 'U':
					if (strchr(optarg, '\\'))
						register_username = xstrdup(optarg);
					else
						/* Prepend .\ for local accounts */
					{
						register_username = malloc(strlen(optarg) + 3);
						if (!register_username)
						{
							write_stderr(_("%s: out of memory\n"), progname);
							exit(1);
						}
						strcpy(register_username, ".\\");
						strcat(register_username, optarg);
					}
					break;
				case 'w':
					do_wait = true;
					wait_set = true;
					break;
				case 'W':
					do_wait = false;
					wait_set = true;
					break;
				default:
					write_stderr(_("%s: invalid option %s\n"), progname, optarg);
					do_advice();
					exit(1);
			}
		}

		/* Process an action */
		if (optind < argc)
		{
			if (ctl_command != NO_COMMAND)
			{
				write_stderr(_("%s: too many command-line arguments (first is \"%s\")\n"), progname, argv[optind]);
				do_advice();
				exit(1);
			}

			if (strcmp(argv[optind], "start") == 0)
				ctl_command = START_COMMAND;
			else if (strcmp(argv[optind], "stop") == 0)
				ctl_command = STOP_COMMAND;
			else if (strcmp(argv[optind], "restart") == 0)
				ctl_command = RESTART_COMMAND;
			else if (strcmp(argv[optind], "reload") == 0)
				ctl_command = RELOAD_COMMAND;
			else if (strcmp(argv[optind], "status") == 0)
				ctl_command = STATUS_COMMAND;
			else if (strcmp(argv[optind], "kill") == 0)
			{
				if (argc - optind < 3)
				{
					write_stderr(_("%s: missing arguments for kill mode\n"), progname);
					do_advice();
					exit(1);
				}
				ctl_command = KILL_COMMAND;
				set_sig(argv[++optind]);
				killproc = atol(argv[++optind]);
			}
#if defined(WIN32) || defined(__CYGWIN__)
			else if (strcmp(argv[optind], "register") == 0)
				ctl_command = REGISTER_COMMAND;
			else if (strcmp(argv[optind], "unregister") == 0)
				ctl_command = UNREGISTER_COMMAND;
			else if (strcmp(argv[optind], "runservice") == 0)
				ctl_command = RUN_AS_SERVICE_COMMAND;
#endif
			else
			{
				write_stderr(_("%s: unrecognized operation mode \"%s\"\n"), progname, argv[optind]);
				do_advice();
				exit(1);
			}
			optind++;
		}
	}

	if (ctl_command == NO_COMMAND)
	{
		write_stderr(_("%s: no operation specified\n"), progname);
		do_advice();
		exit(1);
	}

	/* Note we put any -D switch into the env var above */
	pg_data = getenv("PGDATA");
	if (pg_data)
	{
		pg_data = xstrdup(pg_data);
		canonicalize_path(pg_data);
	}

	if (pg_data == NULL &&
		ctl_command != KILL_COMMAND && ctl_command != UNREGISTER_COMMAND)
	{
		write_stderr(_("%s: no database directory specified "
					   "and environment variable PGDATA unset\n"),
					 progname);
		do_advice();
		exit(1);
	}

	if (!wait_set)
	{
		switch (ctl_command)
		{
			case RESTART_COMMAND:
			case START_COMMAND:
				do_wait = false;
				break;
			case STOP_COMMAND:
				do_wait = true;
				break;
			default:
				break;
		}
	}

	if (ctl_command == RELOAD_COMMAND)
	{
		sig = SIGHUP;
		do_wait = false;
	}

	if (pg_data != NULL)
	{
		snprintf(def_postopts_file, MAXPGPATH, "%s/postmaster.opts.default", pg_data);
		snprintf(postopts_file, MAXPGPATH, "%s/postmaster.opts", pg_data);
		snprintf(pid_file, MAXPGPATH, "%s/postmaster.pid", pg_data);
		snprintf(conf_file, MAXPGPATH, "%s/postgresql.conf", pg_data);
	}

	switch (ctl_command)
	{
		case STATUS_COMMAND:
			do_status();
			break;
		case START_COMMAND:
			do_start();
			break;
		case STOP_COMMAND:
			do_stop();
			break;
		case RESTART_COMMAND:
			do_restart();
			break;
		case RELOAD_COMMAND:
			do_reload();
			break;
		case KILL_COMMAND:
			do_kill(killproc);
			break;
#if defined(WIN32) || defined(__CYGWIN__)
		case REGISTER_COMMAND:
			pgwin32_doRegister();
			break;
		case UNREGISTER_COMMAND:
			pgwin32_doUnregister();
			break;
		case RUN_AS_SERVICE_COMMAND:
			pgwin32_doRunAsService();
			break;
#endif
		default:
			break;
	}

	exit(0);
}
