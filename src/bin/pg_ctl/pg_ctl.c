/*-------------------------------------------------------------------------
 *
 * pg_ctl --- start/stops/restarts the PostgreSQL server
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/bin/pg_ctl/pg_ctl.c,v 1.11 2004/06/10 17:45:09 momjian Exp $
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
	KILL_COMMAND
}	CtlCommand;


static bool do_wait = false;
static bool wait_set = false;
static int	wait_seconds = 60;
static bool silence_echo = false;
static ShutdownMode shutdown_mode = SMART_MODE;
static int	sig = SIGTERM;	/* default */
static CtlCommand ctl_command = NO_COMMAND;
static char *pg_data_opts = NULL;
static char *pg_data = NULL;
static char *post_opts = NULL;
static const char *progname;
static char *log_file = NULL;
static char *postgres_path = NULL;
static char *argv0 = NULL;

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
static pgpid_t get_pgpid(void);
static char **readfile(char *path);
static int start_postmaster(void);
static bool test_postmaster_connection(void);

static char def_postopts_file[MAXPGPATH];
static char postopts_file[MAXPGPATH];
static char pid_file[MAXPGPATH];
static char conf_file[MAXPGPATH];

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
		fprintf(stderr, _("%s: out of memory\n"), progname);
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
		fprintf(stderr, _("%s: out of memory\n"), progname);
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

	if (log_file != NULL)
		/* Win32 needs START rather than "&" */
#ifndef WIN32
		snprintf(cmd, MAXPGPATH, "%s\"%s\" %s < %s >> \"%s\" 2>&1 &%s",
#else
		snprintf(cmd, MAXPGPATH, "START %s\"%s\" %s < %s >> \"%s\" 2>&1%s",
#endif
				 SYSTEMQUOTE, postgres_path, post_opts, DEVNULL, log_file,
				 SYSTEMQUOTE);
	else
#ifndef WIN32
		snprintf(cmd, MAXPGPATH, "%s\"%s\" %s < \"%s\" 2>&1 &%s",
#else
		snprintf(cmd, MAXPGPATH, "START %s\"%s\" %s < \"%s\" 2>&1%s",
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
		while (!isspace(*p))
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
			fprintf(stderr,
					_("%s: Another postmaster may be running. "
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
				fprintf(stderr, _("%s: cannot read %s\n"), progname, postopts_file);
				exit(1);
			}
		}
		else if (optlines[0] == NULL || optlines[1] != NULL)
		{
			fprintf(stderr, _("%s: option file %s must have exactly 1 line\n"),
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
				fprintf(stderr,
						_("The program \"postmaster\" is needed by %s "
						  "but was not found in the same directory as "
						  "\"%s\".\n"
						  "Check your installation.\n"),
						progname, progname);
			else
				fprintf(stderr,
						_("The program \"postmaster\" was found by %s "
						  "but was not the same version as \"%s\".\n"
						  "Check your installation.\n"),
						progname, progname);
			exit(1);
		}
		postgres_path = postmaster_path;
	}

	if (start_postmaster() != 0)
	{
		fprintf(stderr, _("Unable to run the postmaster binary\n"));
		exit(1);
	}

	if (old_pid != 0)
	{
		pg_usleep(1000000);
		pid = get_pgpid();
		if (pid == old_pid)
		{
			fprintf(stderr,
					_("%s: cannot start postmaster\n"
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
		fprintf(stderr, _("%s: could not find %s\n"), progname, pid_file);
		fprintf(stderr, _("Is postmaster running?\n"));
		exit(1);
	}
	else if (pid < 0)			/* standalone backend, not postmaster */
	{
		pid = -pid;
		fprintf(stderr,
				_("%s: cannot stop postmaster; "
				"postgres is running (PID: %ld)\n"),
				progname, pid);
		exit(1);
	}

	if (kill((pid_t) pid, sig) != 0)
	{
		fprintf(stderr, _("stop signal failed (PID: %ld): %s\n"), pid,
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
	
			fprintf(stderr, _("%s: postmaster does not shut down\n"), progname);
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
		fprintf(stderr, _("%s: could not find %s\n"), progname, pid_file);
		fprintf(stderr, _("Is postmaster running?\nstarting postmaster anyway\n"));
		do_start();
		return;
	}
	else if (pid < 0)			/* standalone backend, not postmaster */
	{
		pid = -pid;
		fprintf(stderr,
				_("%s: cannot restart postmaster; "
				"postgres is running (PID: %ld)\n"),
				progname, pid);
		fprintf(stderr, _("Please terminate postgres and try again.\n"));
		exit(1);
	}

	if (kill((pid_t) pid, sig) != 0)
	{
		fprintf(stderr, _("stop signal failed (PID: %ld): %s\n"), pid,
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

		fprintf(stderr, _("%s: postmaster does not shut down\n"), progname);
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
		fprintf(stderr, _("%s: could not find %s\n"), progname, pid_file);
		fprintf(stderr, _("Is postmaster running?\n"));
		exit(1);
	}
	else if (pid < 0)			/* standalone backend, not postmaster */
	{
		pid = -pid;
		fprintf(stderr,
				_("%s: cannot reload postmaster; "
				"postgres is running (PID: %ld)\n"),
				progname, pid);
		fprintf(stderr, _("Please terminate postgres and try again.\n"));
		exit(1);
	}

	if (kill((pid_t) pid, sig) != 0)
	{
		fprintf(stderr, _("reload signal failed (PID: %ld): %s\n"), pid,
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
		fprintf(stderr, _("%s: postmaster or postgres not running\n"), progname);
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
		fprintf(stderr, _("signal %d failed (PID: %ld): %s\n"), sig, pid,
				strerror(errno));
		exit(1);
	}
}



static void
do_advice(void)
{
	fprintf(stderr, _("\nTry \"%s --help\" for more information.\n"), progname);
}



static void
do_help(void)
{
	printf(_("%s is a utility to start, stop, restart, reload configuration files,\n"), progname);
	printf(_("report the status of a PostgreSQL server, or kill a PostgreSQL process\n\n"));
	printf(_("Usage:\n"));
	printf(_("  %s start   [-w] [-D DATADIR] [-s] [-l FILENAME] [-o \"OPTIONS\"]\n"), progname);
	printf(_("  %s stop    [-W] [-D DATADIR] [-s] [-m SHUTDOWN-MODE]\n"), progname);
	printf(_("  %s restart [-w] [-D DATADIR] [-s] [-m SHUTDOWN-MODE] [-o \"OPTIONS\"]\n"), progname);
	printf(_("  %s reload  [-D DATADIR] [-s]\n"), progname);
	printf(_("  %s status  [-D DATADIR]\n"), progname);
	printf(_("  %s kill    SIGNALNAME PROCESSID\n"), progname);
	printf(_("Common options:\n"));
	printf(_("  -D, --pgdata DATADIR   location of the database storage area\n"));
	printf(_("  -s, --silent only print errors, no informational messages\n"));
	printf(_("  -w           wait until operation completes\n"));
	printf(_("  -W           do not wait until operation completes\n"));
	printf(_("  --help       show this help, then exit\n"));
	printf(_("  --version    output version information, then exit\n"));
	printf(_("(The default is to wait for shutdown, but not for start or restart.)\n\n"));
	printf(_("If the -D option is omitted, the environment variable PGDATA is used.\n\n"));
	printf(_("Options for start or restart:\n"));
	printf(_("  -l, --log FILENAME      write (or append) server log to FILENAME.  The\n"));
	printf(_("                          use of this option is highly recommended.\n"));
	printf(_("  -o OPTIONS              command line options to pass to the postmaster\n"));
	printf(_("                          (PostgreSQL server executable)\n"));
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
		fprintf(stderr, _("%s: invalid shutdown mode %s\n"), progname, modeopt);
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
		fprintf(stderr, _("%s: invalid signal \"%s\"\n"), progname, signame);
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
		while ((c = getopt_long(argc, argv, "D:l:m:o:p:swW", long_options, &option_index)) != -1)
		{
			switch (c)
			{
				case 'D':
				{
					int			len = strlen(optarg) + 4;
					char	   *env_var;
		
					pg_data_opts = xmalloc(len);
					snprintf(pg_data_opts, len, "-D %s", optarg);
					env_var = xmalloc(len + sizeof("PGDATA="));
					snprintf(env_var, len + sizeof("PGDATA="), "PGDATA=%s", optarg);
					putenv(env_var);
					break;
				}
				case 'l':
					log_file = xstrdup(optarg);
					break;
				case 'm':
					set_mode(optarg);
					break;
				case 'o':
					post_opts = xstrdup(optarg);
					break;
				case 'p':
					postgres_path = xstrdup(optarg);
					break;
				case 's':
					silence_echo = true;
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
					fprintf(stderr, _("%s: invalid option %s\n"), progname, optarg);
					do_advice();
					exit(1);
			}
		}
	
		/* Process an action */
		if (optind < argc)
		{
			if (ctl_command != NO_COMMAND)
			{
				fprintf(stderr, _("%s: extra operation mode %s\n"), progname, argv[optind]);
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
					fprintf(stderr, _("%s: invalid kill syntax\n"), progname);
					do_advice();
					exit(1);
				}
				ctl_command = KILL_COMMAND;
				set_sig(argv[++optind]);
				killproc = atol(argv[++optind]);
			}
			else
			{
				fprintf(stderr, _("%s: invalid operation mode %s\n"), progname, argv[optind]);
				do_advice();
				exit(1);
			}
			optind++;
		}
	}
	
	if (ctl_command == NO_COMMAND)
	{
		fprintf(stderr, _("%s: no operation specified\n"), progname);
		do_advice();
		exit(1);
	}

	pg_data = getenv("PGDATA");
	canonicalize_path(pg_data);

	if (pg_data == NULL && ctl_command != KILL_COMMAND)
	{
		fprintf(stderr,
				_("%s: no database directory specified "
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
		default:
			break;
	}

	exit(0);
}
