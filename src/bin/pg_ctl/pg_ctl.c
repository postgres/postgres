/*-------------------------------------------------------------------------
 *
 * pg_ctl --- start/stops/restarts the PostgreSQL server
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/bin/pg_ctl/pg_ctl.c,v 1.24 2004/07/29 16:11:11 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"
#include "libpq-fe.h"

#include <locale.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "libpq/pqsignal.h"
#include "getopt_long.h"

#ifndef HAVE_OPTRESET
int			optreset;
#endif

/* PID can be negative for standalone backend */
typedef long pgpid_t;

#define _(x) gettext((x))

#define WHITESPACE "\f\n\r\t\v"		/* as defined by isspace() */

/* postmaster version ident string */
#define PM_VERSIONSTR "postmaster (PostgreSQL) " PG_VERSION "\n"


typedef enum
{
	SMART_MODE,
	FAST_MODE,
	IMMEDIATE_MODE
}	ShutdownMode;


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
}	CtlCommand;


static bool do_wait = false;
static bool wait_set = false;
static int	wait_seconds = 60;
static bool silence_echo = false;
static ShutdownMode shutdown_mode = SMART_MODE;
static int	sig = SIGTERM;	/* default */
static CtlCommand ctl_command = NO_COMMAND;
static char *pg_data = NULL;
static char *post_opts = NULL;
static const char *progname;
static char *log_file = NULL;
static char *postgres_path = NULL;
static char *register_servicename = "PostgreSQL"; /* FIXME: + version ID? */
static char *register_username = NULL;
static char *register_password = NULL;
static char *argv0 = NULL;

static void write_stderr(const char *fmt,...)
/* This extension allows gcc to check the format string for consistency with
   the supplied arguments. */
__attribute__((format(printf, 1, 2)));
static void *xmalloc(size_t size);
static char *xstrdup(const char *s);
static void do_advice(void);
static void do_help(void);
static void set_mode(char *modeopt);
static void set_sig(char *signame);
static void do_start();
static void do_stop(void);
static void do_restart(void);
static void do_reload(void);
static void do_status(void);
static void do_kill(pgpid_t pid);
#ifdef WIN32
static bool  pgwin32_IsInstalled(SC_HANDLE);
static char* pgwin32_CommandLine(bool);
static void pgwin32_doRegister();
static void pgwin32_doUnregister();
static void pgwin32_SetServiceStatus(DWORD);
static void WINAPI pgwin32_ServiceHandler(DWORD);
static void WINAPI pgwin32_ServiceMain(DWORD, LPTSTR*);
static void pgwin32_doRunAsService();
#endif
static pgpid_t get_pgpid(void);
static char **readfile(char *path);
static int start_postmaster(void);
static bool test_postmaster_connection(void);

static char def_postopts_file[MAXPGPATH];
static char postopts_file[MAXPGPATH];
static char pid_file[MAXPGPATH];
static char conf_file[MAXPGPATH];


#ifdef WIN32
static void
write_eventlog(int level, const char *line)
{
	static HANDLE evtHandle = INVALID_HANDLE_VALUE;

	if (evtHandle == INVALID_HANDLE_VALUE) {
		evtHandle = RegisterEventSource(NULL,"PostgreSQL");
		if (evtHandle == NULL) {
			evtHandle = INVALID_HANDLE_VALUE;
			return;
		}
	}

	ReportEvent(evtHandle,
				level,
				0,
				0, /* All events are Id 0 */
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
	va_list ap;

	va_start(ap, fmt);
#ifndef WIN32
	/* On Unix, we just fprintf to stderr */
	vfprintf(stderr, fmt, ap);
#else
	/* On Win32, we print to stderr if running on a console, or write to
	 * eventlog if running as a service */
	if (!isatty(fileno(stderr))) /* Running as a service */
	{
		char errbuf[2048]; /* Arbitrary size? */

		vsnprintf(errbuf, sizeof(errbuf), fmt, ap);

		write_eventlog(EVENTLOG_ERROR_TYPE, errbuf);
	}
	else /* Not running as service, write to stderr */
		vfprintf(stderr, fmt, ap);
#endif
	va_end(ap);
}

/*
 * routines to check memory allocations and fail noisily.
 */

static void *
xmalloc(size_t size)
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



static pgpid_t
get_pgpid(void)
{
	FILE	   *pidf;
	pgpid_t		pid;

	pidf = fopen(pid_file, "r");
	if (pidf == NULL)
	{
		/* No pid file, not an error on startup */
		if (errno == ENOENT)
			return 0;
		else
		{
			perror("openning pid file");
			exit(1);
		}
	}
	fscanf(pidf, "%ld", &pid);
	fclose(pidf);
	return pid;
}


/*
 * get the lines from a text file - return NULL if file can't be opened
 */
static char **
readfile(char *path)
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

	result = (char **) xmalloc((nlines + 1) * sizeof(char *));
	buffer = (char *) xmalloc(maxlength + 1);

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
	 * Since there might be quotes to handle here, it is easier simply
	 * to pass everything to a shell to process them.
	 */
	char		cmd[MAXPGPATH];
	
	/*
	 *	 Win32 needs START /B rather than "&".
	 *
	 *	Win32 has a problem with START and quoted executable names.
	 *	You must add a "" as the title at the beginning so you can quote
	 *	the executable name:
	 *		http://www.winnetmag.com/Article/ArticleID/14589/14589.html
	 *		http://dev.remotenetworktechnology.com/cmd/cmdfaq.htm
	 */
	if (log_file != NULL)
#ifndef WIN32
		snprintf(cmd, MAXPGPATH, "%s\"%s\" %s < \"%s\" >> \"%s\" 2>&1 &%s",
#else
		snprintf(cmd, MAXPGPATH, "%sSTART /B \"\" \"%s\" %s < \"%s\" >> \"%s\" 2>&1%s",
#endif
				 SYSTEMQUOTE, postgres_path, post_opts, DEVNULL, log_file,
				 SYSTEMQUOTE);
	else
#ifndef WIN32
		snprintf(cmd, MAXPGPATH, "%s\"%s\" %s < \"%s\" 2>&1 &%s",
#else
		snprintf(cmd, MAXPGPATH, "%sSTART /B \"\" \"%s\" %s < \"%s\" 2>&1%s",
#endif
				 SYSTEMQUOTE, postgres_path, post_opts, DEVNULL, SYSTEMQUOTE);

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
	char 		*p;


	*portstr = '\0';

	/* post_opts */
	for (p = post_opts; *p;)
	{
		/* advance past whitespace/quoting */
		while (isspace(*p) || *p == '\'' || *p == '"')
			p++;
		
		if (strncmp(p, "-p", strlen("-p")) == 0)
		{
			p += strlen("-p");
			/* advance past whitespace/quoting */
			while (isspace(*p) || *p == '\'' || *p == '"')
				p++;
			StrNCpy(portstr, p, Min(strcspn(p, "\"'"WHITESPACE) + 1,
									sizeof(portstr)));
			/* keep looking, maybe there is another -p */
		}
		/* Advance to next whitespace */
		while (*p && !isspace(*p))
			p++;
	}

	/* config file */
	if (!*portstr)
	{
		char	  **optlines;

		optlines = readfile(conf_file);
		if (optlines != NULL)
		{
			for (;*optlines != NULL; optlines++)
			{
				p = *optlines;

				while (isspace(*p))
					p++;
				if (strncmp(p, "port", strlen("port")) != 0)
					continue;
				p += strlen("port");
				while (isspace(*p))
					p++;
				if (*p != '=')
					continue;
				p++;
				while (isspace(*p))
					p++;
				StrNCpy(portstr, p, Min(strcspn(p, "#"WHITESPACE) + 1,
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
		if ((conn = PQsetdbLogin(NULL, portstr, NULL, NULL, "template1", NULL, NULL)) != NULL)
		{
			PQfinish(conn);
			success = true;
			break;
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

	if (ctl_command != RESTART_COMMAND)
	{
		old_pid = get_pgpid();
		if (old_pid != 0)
			write_stderr(_("%s: Another postmaster may be running. "
						   "Trying to start postmaster anyway.\n"),
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
				write_stderr(_("%s: cannot read %s\n"), progname, postopts_file);
				exit(1);
			}
		}
		else if (optlines[0] == NULL || optlines[1] != NULL)
		{
			write_stderr(_("%s: option file %s must have exactly 1 line\n"),
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
	
				arg1 = strchr(optline, '\'');
				if (arg1 == NULL || arg1 == optline)
					post_opts = "";
				else
				{
					*(arg1 - 1) = '\0';	/* this should be a space */
					post_opts = arg1;
				}
				if (postgres_path != NULL)
					postgres_path = optline;
			}
			else
				post_opts = optline;
		}
	}

	if (postgres_path == NULL)
	{
		char	   *postmaster_path;
		int			ret;

		postmaster_path = xmalloc(MAXPGPATH);

		if ((ret = find_other_exec(argv0, "postmaster", PM_VERSIONSTR,
								   postmaster_path)) < 0)
		{
			if (ret == -1)
				write_stderr(_("The program \"postmaster\" is needed by %s "
							   "but was not found in the same directory as "
							   "\"%s\".\n"
							   "Check your installation.\n"),
							 progname, progname);
			else
				write_stderr(_("The program \"postmaster\" was found by %s "
							   "but was not the same version as \"%s\".\n"
							   "Check your installation.\n"),
							 progname, progname);
			exit(1);
		}
		postgres_path = postmaster_path;
	}

	if (start_postmaster() != 0)
	{
		write_stderr(_("Unable to run the postmaster binary\n"));
		exit(1);
	}

	if (old_pid != 0)
	{
		pg_usleep(1000000);
		pid = get_pgpid();
		if (pid == old_pid)
		{
			write_stderr(_("%s: cannot start postmaster\n"
						   "Examine the log output\n"),
						 progname);
			exit(1);
		}
	}

	if (do_wait)
	{
		if (!silence_echo)
		{
			printf(_("waiting for postmaster to start..."));
			fflush(stdout);
		}

		if (test_postmaster_connection() == false)
			printf(_("could not start postmaster\n"));
		else if (!silence_echo)
			printf(_("done\npostmaster started\n"));
	}
	else if (!silence_echo)
		printf(_("postmaster starting\n"));
}



static void
do_stop(void)
{
	int			cnt;
	pgpid_t		pid;

	pid = get_pgpid();

	if (pid == 0)				/* no pid file */
	{
		write_stderr(_("%s: could not find %s\n"), progname, pid_file);
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
		write_stderr(_("stop signal failed (PID: %ld): %s\n"), pid,
				strerror(errno));
		exit(1);
	}

	if (!do_wait)
	{
		if (!silence_echo)
			printf(_("postmaster shutting down\n"));
		return;
	}
	else
	{
		if (!silence_echo)
		{
			printf(_("waiting for postmaster to shut down..."));
			fflush(stdout);
		}
	
		for (cnt = 0; cnt < wait_seconds; cnt++)
		{
			if ((pid = get_pgpid()) != 0)
			{
				if (!silence_echo)
				{
					printf(".");
					fflush(stdout);
				}
				pg_usleep(1000000); /* 1 sec */
			}
			else
				break;
		}
	
		if (pid != 0)				/* pid file still exists */
		{
			if (!silence_echo)
				printf(_(" failed\n"));
	
			write_stderr(_("%s: postmaster does not shut down\n"), progname);
			exit(1);
		}
		if (!silence_echo)
			printf(_("done\n"));

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
		write_stderr(_("%s: could not find %s\n"), progname, pid_file);
		write_stderr(_("Is postmaster running?\nstarting postmaster anyway\n"));
		do_start();
		return;
	}
	else if (pid < 0)			/* standalone backend, not postmaster */
	{
		pid = -pid;
		write_stderr(_("%s: cannot restart postmaster; "
					   "postgres is running (PID: %ld)\n"),
					 progname, pid);
		write_stderr(_("Please terminate postgres and try again.\n"));
		exit(1);
	}

	if (kill((pid_t) pid, sig) != 0)
	{
		write_stderr(_("stop signal failed (PID: %ld): %s\n"), pid,
				strerror(errno));
		exit(1);
	}

	if (!silence_echo)
	{
		printf(_("waiting for postmaster to shut down..."));
		fflush(stdout);
	}

	/* always wait for restart */

	for (cnt = 0; cnt < wait_seconds; cnt++)
	{
		if ((pid = get_pgpid()) != 0)
		{
			if (!silence_echo)
			{
				printf(".");
				fflush(stdout);
			}
			pg_usleep(1000000); /* 1 sec */
		}
		else
			break;
	}

	if (pid != 0)				/* pid file still exists */
	{
		if (!silence_echo)
			printf(_(" failed\n"));

		write_stderr(_("%s: postmaster does not shut down\n"), progname);
		exit(1);
	}

	if (!silence_echo)
		printf(_("done\n"));

	printf(_("postmaster stopped\n"));
	do_start();
}


static void
do_reload(void)
{
	pgpid_t		pid;

	pid = get_pgpid();
	if (pid == 0)				/* no pid file */
	{
		write_stderr(_("%s: could not find %s\n"), progname, pid_file);
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
		write_stderr(_("reload signal failed (PID: %ld): %s\n"), pid,
				strerror(errno));
		exit(1);
	}

	if (!silence_echo)
		fprintf(stdout, _("postmaster signaled\n"));
}

/*
 *	utility routines
 */

static void
do_status(void)
{
	pgpid_t		pid;

	pid = get_pgpid();
	if (pid == 0)				/* no pid file */
	{
		write_stderr(_("%s: postmaster or postgres not running\n"), progname);
		exit(1);
	}
	else if (pid < 0)			/* standalone backend */
	{
		pid = -pid;
		fprintf(stdout, _("%s: a standalone backend \"postgres\" is running (PID: %ld)\n"), progname, pid);
	}
	else						/* postmaster */
	{
		char	  **optlines;

		fprintf(stdout, _("%s: postmaster is running (PID: %ld)\n"), progname, pid);

		optlines = readfile(postopts_file);
		if (optlines != NULL)
			for (; *optlines != NULL; optlines++)
				fputs(*optlines, stdout);
	}
}



static void
do_kill(pgpid_t pid)
{
	if (kill((pid_t) pid, sig) != 0)
	{
		write_stderr(_("signal %d failed (PID: %ld): %s\n"), sig, pid,
				strerror(errno));
		exit(1);
	}
}

#ifdef WIN32

static bool pgwin32_IsInstalled(SC_HANDLE hSCM)
{
	SC_HANDLE hService = OpenService(hSCM, register_servicename, SERVICE_QUERY_CONFIG);
	bool bResult = (hService != NULL);
	if (bResult)
		CloseServiceHandle(hService);
	return bResult;
}

static char* pgwin32_CommandLine(bool registration)
{
	static char cmdLine[MAXPGPATH];
	int ret;
	if (registration)
		ret = find_my_exec(argv0, cmdLine);
	else
		ret = find_other_exec(argv0, "postmaster", PM_VERSIONSTR, cmdLine);
	if (ret != 0)
	{
		write_stderr(_("Unable to find exe"));
		exit(1);
	}

	if (registration)
	{
		if (strcasecmp(cmdLine+strlen(cmdLine)-4,".exe"))
		{
			/* If commandline does not end in .exe, append it */
			strcat(cmdLine,".exe");
		}
		strcat(cmdLine," runservice -N \"");
		strcat(cmdLine,register_servicename);
		strcat(cmdLine,"\"");
	}

	if (pg_data)
	{
		strcat(cmdLine," -D \"");
		strcat(cmdLine,pg_data);
		strcat(cmdLine,"\"");
	}

	if (post_opts)
	{
		strcat(cmdLine," ");
		if (registration)
			strcat(cmdLine," -o \"");
		strcat(cmdLine,post_opts);
		if (registration)
			strcat(cmdLine,"\"");
	}

	return cmdLine;
}

static void
pgwin32_doRegister()
{
	SC_HANDLE hService;
	SC_HANDLE hSCM = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (hSCM == NULL)
	{
		write_stderr(_("Unable to open service manager\n"));
		exit(1);
	}
	if (pgwin32_IsInstalled(hSCM))
	{
		CloseServiceHandle(hSCM);
		write_stderr(_("Service \"%s\" already registered\n"),register_servicename);
		exit(1);
	}

	if ((hService = CreateService(hSCM, register_servicename, register_servicename,
								  SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
								  SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
								  pgwin32_CommandLine(true),
								  NULL, NULL, "RPCSS\0", register_username, register_password)) == NULL)
	{
		CloseServiceHandle(hSCM);
		write_stderr(_("Unable to register service \"%s\" [%d]\n"), register_servicename, (int)GetLastError());
		exit(1);
	}
	CloseServiceHandle(hService);
	CloseServiceHandle(hSCM);
}

static void
pgwin32_doUnregister()
{
	SC_HANDLE hService;
	SC_HANDLE hSCM = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (hSCM == NULL)
	{
		write_stderr(_("Unable to open service manager\n"));
		exit(1);
	}
	if (!pgwin32_IsInstalled(hSCM))
	{
		CloseServiceHandle(hSCM);
		write_stderr(_("Service \"%s\" not registered\n"),register_servicename);
		exit(1);
	}

	if ((hService = OpenService(hSCM, register_servicename, DELETE)) == NULL)
	{
		CloseServiceHandle(hSCM);
		write_stderr(_("Unable to open service \"%s\" [%d]\n"), register_servicename, (int)GetLastError());
		exit(1);
	}
	if (!DeleteService(hService)) {
		CloseServiceHandle(hService);
		CloseServiceHandle(hSCM);
		write_stderr(_("Unable to unregister service \"%s\" [%d]\n"), register_servicename, (int)GetLastError());
		exit(1);
	}
	CloseServiceHandle(hService);
	CloseServiceHandle(hSCM);
}


static SERVICE_STATUS status;
static SERVICE_STATUS_HANDLE hStatus = (SERVICE_STATUS_HANDLE)0;
static HANDLE shutdownHandles[2];
static pid_t postmasterPID = -1;
#define shutdownEvent     shutdownHandles[0]
#define postmasterProcess shutdownHandles[1]

static void pgwin32_SetServiceStatus(DWORD currentState)
{
	status.dwCurrentState = currentState;
	SetServiceStatus(hStatus, (LPSERVICE_STATUS)&status);
}

static void WINAPI pgwin32_ServiceHandler(DWORD request)
{
	switch (request)
	{
		case SERVICE_CONTROL_STOP:
		case SERVICE_CONTROL_SHUTDOWN:
			pgwin32_SetServiceStatus(SERVICE_STOP_PENDING);
			SetEvent(shutdownEvent);
			return;

		case SERVICE_CONTROL_PAUSE:
			/* Win32 config reloading */
			kill(postmasterPID,SIGHUP);
			return;

		/* FIXME: These could be used to replace other signals etc */
		case SERVICE_CONTROL_CONTINUE:
		case SERVICE_CONTROL_INTERROGATE:
		default:
			break;
	}
}

static void WINAPI pgwin32_ServiceMain(DWORD argc, LPTSTR *argv)
{
	STARTUPINFO si;
	PROCESS_INFORMATION pi;
	DWORD ret;

	/* Initialize variables */
	status.dwWin32ExitCode	= S_OK;
	status.dwCheckPoint		= 0;
	status.dwWaitHint		= 0;
	status.dwServiceType	= SERVICE_WIN32_OWN_PROCESS;
	status.dwControlsAccepted			= SERVICE_ACCEPT_STOP|SERVICE_ACCEPT_PAUSE_CONTINUE;
	status.dwServiceSpecificExitCode	= 0;
	status.dwCurrentState = SERVICE_START_PENDING;

	memset(&pi,0,sizeof(pi));
	memset(&si,0,sizeof(si));
	si.cb = sizeof(si);

	/* Register the control request handler */
	if ((hStatus = RegisterServiceCtrlHandler(register_servicename, pgwin32_ServiceHandler)) == (SERVICE_STATUS_HANDLE)0)
		return;

	if ((shutdownEvent = CreateEvent(NULL,true,false,NULL)) == NULL)
		return;

	/* Start the postmaster */
	pgwin32_SetServiceStatus(SERVICE_START_PENDING);
	if (!CreateProcess(NULL,pgwin32_CommandLine(false),NULL,NULL,TRUE,0,NULL,NULL,&si,&pi))
	{
		pgwin32_SetServiceStatus(SERVICE_STOPPED);
		return;
	}
	postmasterPID		= pi.dwProcessId;
	postmasterProcess	= pi.hProcess;
	CloseHandle(pi.hThread);
	pgwin32_SetServiceStatus(SERVICE_RUNNING);

	/* Wait for quit... */
	ret = WaitForMultipleObjects(2,shutdownHandles,FALSE,INFINITE);
	pgwin32_SetServiceStatus(SERVICE_STOP_PENDING);
	switch (ret)
	{
		case WAIT_OBJECT_0: /* shutdown event */
			kill(postmasterPID,SIGINT);
			WaitForSingleObject(postmasterProcess,INFINITE);
			break;

		case (WAIT_OBJECT_0+1): /* postmaster went down */
			break;

		default:
			/* shouldn't get here? */
			break;
	}

	CloseHandle(shutdownEvent);
	CloseHandle(postmasterProcess);

	pgwin32_SetServiceStatus(SERVICE_STOPPED);
}

static void pgwin32_doRunAsService()
{
	SERVICE_TABLE_ENTRY st[] = {{ register_servicename, pgwin32_ServiceMain },
								{ NULL, NULL }};
	StartServiceCtrlDispatcher(st);
}

#endif

static void
do_advice(void)
{
	write_stderr(_("\nTry \"%s --help\" for more information.\n"), progname);
}



static void
do_help(void)
{
	printf(_("%s is a utility to start, stop, restart, reload configuration files,\n"
	         "report the status of a PostgreSQL server, or kill a PostgreSQL process\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s start   [-w] [-D DATADIR] [-s] [-l FILENAME] [-o \"OPTIONS\"]\n"), progname);
	printf(_("  %s stop    [-W] [-D DATADIR] [-s] [-m SHUTDOWN-MODE]\n"), progname);
	printf(_("  %s restart [-w] [-D DATADIR] [-s] [-m SHUTDOWN-MODE] [-o \"OPTIONS\"]\n"), progname);
	printf(_("  %s reload  [-D DATADIR] [-s]\n"), progname);
	printf(_("  %s status  [-D DATADIR]\n"), progname);
	printf(_("  %s kill    SIGNALNAME PROCESSID\n"), progname);
#ifdef WIN32
	printf(_("  %s register   [-N servicename] [-U username] [-P password] [-D DATADIR] [-o \"OPTIONS\"]\n"), progname);
	printf(_("  %s unregister [-N servicename]\n"), progname);
#endif
	printf(_("Common options:\n"));
	printf(_("  -D, --pgdata DATADIR   location of the database storage area\n"));
	printf(_("  -s, --silent only print errors, no informational messages\n"));
#ifdef WIN32
	printf(_("  -N       service name with which to register PostgreSQL server\n"));
	printf(_("  -P       password of account to register PostgreSQL server\n"));
	printf(_("  -U       user name of account to register PostgreSQL server\n"));
#endif
	printf(_("  -w           wait until operation completes\n"));
	printf(_("  -W           do not wait until operation completes\n"));
	printf(_("  --help       show this help, then exit\n"));
	printf(_("  --version    output version information, then exit\n"));
	printf(_("(The default is to wait for shutdown, but not for start or restart.)\n\n"));
	printf(_("If the -D option is omitted, the environment variable PGDATA is used.\n\n"));
	printf(_("Options for start or restart:\n"));
	printf(_("  -l, --log FILENAME      write (or append) server log to FILENAME.  The\n"
	         "                          use of this option is highly recommended.\n"));
	printf(_("  -o OPTIONS              command line options to pass to the postmaster\n"
	         "                          (PostgreSQL server executable)\n"));
	printf(_("  -p PATH-TO-POSTMASTER   normally not necessary\n\n"));
	printf(_("Options for stop or restart:\n"));
	printf(_("  -m SHUTDOWN-MODE   may be 'smart', 'fast', or 'immediate'\n\n"));
	printf(_("Allowed signal names for kill:\n"));
	printf(_("  HUP INT QUIT ABRT TERM USR1 USR2\n\n"));
	printf(_("Shutdown modes are:\n"));
	printf(_("  smart       quit after all clients have disconnected\n"));
	printf(_("  fast        quit directly, with proper shutdown\n"));
	printf(_("  immediate   quit without complete shutdown; will lead to recovery on restart\n\n"));
	printf(_("Report bugs to <pgsql-bugs@postgresql.org>.\n"));
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
		write_stderr(_("%s: invalid shutdown mode %s\n"), progname, modeopt);
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
		write_stderr(_("%s: invalid signal \"%s\"\n"), progname, signame);
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
		{0, 0, 0, 0}
	};

	int			option_index;
	int			c;
	pgpid_t		killproc = 0;
	
#ifdef WIN32
	setvbuf(stderr, NULL, _IONBF, 0);
#endif

	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], "pg_ctl");

	/*
	 * save argv[0] so do_start() can look for the postmaster if
	 * necessary. we don't look for postmaster here because in many cases
	 * we won't need it.
	 */
	argv0 = argv[0];

	umask(077);

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
	 *	'Action' can be before or after args so loop over both.
	 *	Some getopt_long() implementations will reorder argv[]
	 *	to place all flags first (GNU?), but we don't rely on it.
	 *	Our /port version doesn't do that.
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
					int			len = strlen(optarg);
					char	   *env_var;
		
					env_var = xmalloc(len + 8);
					snprintf(env_var, len + 8, "PGDATA=%s", optarg);
					putenv(env_var);
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
					register_password  = xstrdup(optarg);
					break;
				case 's':
					silence_echo = true;
					break;
				case 'U':
					if (strchr(optarg,'\\'))
						register_username  = xstrdup(optarg);
					else /* Prepend .\ for local accounts */
					{
						register_username = malloc(strlen(optarg)+3);
						if (!register_username)
						{
							write_stderr(_("%s: out of memory\n"), progname);
							exit(1);
						}
						strcpy(register_username,".\\");
						strcat(register_username,optarg);
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
				write_stderr(_("%s: extra operation mode %s\n"), progname, argv[optind]);
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
					write_stderr(_("%s: invalid kill syntax\n"), progname);
					do_advice();
					exit(1);
				}
				ctl_command = KILL_COMMAND;
				set_sig(argv[++optind]);
				killproc = atol(argv[++optind]);
			}
#ifdef WIN32
			else if (strcmp(argv[optind], "register") == 0)
				ctl_command = REGISTER_COMMAND;
			else if (strcmp(argv[optind], "unregister") == 0)
				ctl_command = UNREGISTER_COMMAND;
			else if (strcmp(argv[optind], "runservice") == 0)
				ctl_command = RUN_AS_SERVICE_COMMAND;
#endif
			else
			{
				write_stderr(_("%s: invalid operation mode %s\n"), progname, argv[optind]);
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

	snprintf(def_postopts_file, MAXPGPATH, "%s/postmaster.opts.default", pg_data);
	snprintf(postopts_file, MAXPGPATH, "%s/postmaster.opts", pg_data);
	snprintf(pid_file, MAXPGPATH, "%s/postmaster.pid", pg_data);
	snprintf(conf_file, MAXPGPATH, "%s/postgresql.conf", pg_data);

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
#ifdef WIN32
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
