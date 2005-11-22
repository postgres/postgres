/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2005, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/bin/psql/startup.c,v 1.126.2.2 2005/11/22 18:23:27 momjian Exp $
 */
#include "postgres_fe.h"

#include <sys/types.h>

#ifndef WIN32
#include <unistd.h>
#else							/* WIN32 */
#include <io.h>
#include <win32.h>
#endif   /* WIN32 */

#include "getopt_long.h"

#ifndef HAVE_INT_OPTRESET
int			optreset;
#endif

#include <locale.h>

#include "libpq-fe.h"

#include "command.h"
#include "common.h"
#include "describe.h"
#include "help.h"
#include "input.h"
#include "mainloop.h"
#include "print.h"
#include "settings.h"
#include "variables.h"

#include "mb/pg_wchar.h"


/*
 * Global psql options
 */
PsqlSettings pset;

#ifndef WIN32
#define SYSPSQLRC	"psqlrc"
#define PSQLRC		".psqlrc"
#else
#define SYSPSQLRC	"psqlrc"
#define PSQLRC		"psqlrc.conf"
#endif

/*
 * Structures to pass information between the option parsing routine
 * and the main function
 */
enum _actions
{
	ACT_NOTHING = 0,
	ACT_SINGLE_SLASH,
	ACT_LIST_DB,
	ACT_SINGLE_QUERY,
	ACT_FILE
};

struct adhoc_opts
{
	char	   *dbname;
	char	   *host;
	char	   *port;
	char	   *username;
	char	   *logfilename;
	enum _actions action;
	char	   *action_string;
	bool		no_readline;
	bool		no_psqlrc;
};

static int	parse_version(const char *versionString);
static void parse_psql_options(int argc, char *argv[],
				   struct adhoc_opts * options);
static void process_psqlrc(char *argv0);
static void process_psqlrc_file(char *filename);
static void showVersion(void);

#ifdef USE_SSL
static void printSSLInfo(void);
#endif

#ifdef WIN32
static void
			checkWin32Codepage(void);
#endif

/*
 *
 * main
 *
 */
int
main(int argc, char *argv[])
{
	struct adhoc_opts options;
	int			successResult;

	char	   *username = NULL;
	char	   *password = NULL;
	char	   *password_prompt = NULL;
	bool		need_pass;

	set_pglocale_pgservice(argv[0], "psql");

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage();
			exit(EXIT_SUCCESS);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			showVersion();
			exit(EXIT_SUCCESS);
		}
	}

	pset.progname = get_progname(argv[0]);

#ifdef WIN32
	setvbuf(stderr, NULL, _IONBF, 0);
	setup_win32_locks();
#endif
	setDecimalLocale();
	pset.cur_cmd_source = stdin;
	pset.cur_cmd_interactive = false;
	pset.encoding = PQenv2encoding();

	pset.vars = CreateVariableSpace();
	if (!pset.vars)
	{
		fprintf(stderr, _("%s: out of memory\n"), pset.progname);
		exit(EXIT_FAILURE);
	}
	pset.popt.topt.format = PRINT_ALIGNED;
	pset.queryFout = stdout;
	pset.popt.topt.border = 1;
	pset.popt.topt.pager = 1;
	pset.popt.default_footer = true;

	SetVariable(pset.vars, "VERSION", PG_VERSION_STR);

	/* Default values for variables */
	SetVariableBool(pset.vars, "AUTOCOMMIT");
	SetVariable(pset.vars, "VERBOSITY", "default");
	SetVariable(pset.vars, "PROMPT1", DEFAULT_PROMPT1);
	SetVariable(pset.vars, "PROMPT2", DEFAULT_PROMPT2);
	SetVariable(pset.vars, "PROMPT3", DEFAULT_PROMPT3);

	pset.verbosity = PQERRORS_DEFAULT;

	pset.notty = (!isatty(fileno(stdin)) || !isatty(fileno(stdout)));

	/* This is obsolete and should be removed sometime. */
#ifdef PSQL_ALWAYS_GET_PASSWORDS
	pset.getPassword = true;
#else
	pset.getPassword = false;
#endif

	parse_psql_options(argc, argv, &options);

	if (!pset.popt.topt.fieldSep)
		pset.popt.topt.fieldSep = pg_strdup(DEFAULT_FIELD_SEP);
	if (!pset.popt.topt.recordSep)
		pset.popt.topt.recordSep = pg_strdup(DEFAULT_RECORD_SEP);

	if (options.username)
	{
		/*
		 * The \001 is a hack to support the deprecated -u option which issues
		 * a username prompt. The recommended option is -U followed by the
		 * name on the command line.
		 */
		if (strcmp(options.username, "\001") == 0)
			username = simple_prompt("User name: ", 100, true);
		else
			username = pg_strdup(options.username);
	}

	if (options.username == NULL)
		password_prompt = pg_strdup(_("Password: "));
	else
	{
		password_prompt = malloc(strlen(_("Password for user %s: ")) - 2 +
								 strlen(options.username) + 1);
		sprintf(password_prompt, _("Password for user %s: "), options.username);
	}

	if (pset.getPassword)
		password = simple_prompt(password_prompt, 100, false);

	/* loop until we have a password if requested by backend */
	do
	{
		need_pass = false;
		pset.db = PQsetdbLogin(options.host, options.port, NULL, NULL,
					options.action == ACT_LIST_DB && options.dbname == NULL ?
							   "postgres" : options.dbname,
							   username, password);

		if (PQstatus(pset.db) == CONNECTION_BAD &&
			strcmp(PQerrorMessage(pset.db), PQnoPasswordSupplied) == 0 &&
			!feof(stdin))
		{
			PQfinish(pset.db);
			need_pass = true;
			free(password);
			password = NULL;
			password = simple_prompt(password_prompt, 100, false);
		}
	} while (need_pass);

	free(username);
	free(password);
	free(password_prompt);

	if (PQstatus(pset.db) == CONNECTION_BAD)
	{
		fprintf(stderr, "%s: %s", pset.progname, PQerrorMessage(pset.db));
		PQfinish(pset.db);
		exit(EXIT_BADCONN);
	}

	PQsetNoticeProcessor(pset.db, NoticeProcessor, NULL);

	SyncVariables();

	/* Grab the backend server version */
	pset.sversion = PQserverVersion(pset.db);

	if (options.action == ACT_LIST_DB)
	{
		int			success = listAllDbs(false);

		PQfinish(pset.db);
		exit(success ? EXIT_SUCCESS : EXIT_FAILURE);
	}

	if (options.logfilename)
	{
		pset.logfile = fopen(options.logfilename, "a");
		if (!pset.logfile)
			fprintf(stderr, _("%s: could not open log file \"%s\": %s\n"),
					pset.progname, options.logfilename, strerror(errno));
	}

	/*
	 * Now find something to do
	 */

	/*
	 * process file given by -f
	 */
	if (options.action == ACT_FILE && strcmp(options.action_string, "-") != 0)
	{
		if (!options.no_psqlrc)
			process_psqlrc(argv[0]);

		successResult = process_file(options.action_string);
	}

	/*
	 * process slash command if one was given to -c
	 */
	else if (options.action == ACT_SINGLE_SLASH)
	{
		PsqlScanState scan_state;

		if (VariableEquals(pset.vars, "ECHO", "all"))
			puts(options.action_string);

		scan_state = psql_scan_create();
		psql_scan_setup(scan_state,
						options.action_string,
						strlen(options.action_string));

		successResult = HandleSlashCmds(scan_state, NULL) != CMD_ERROR
			? EXIT_SUCCESS : EXIT_FAILURE;

		psql_scan_destroy(scan_state);
	}

	/*
	 * If the query given to -c was a normal one, send it
	 */
	else if (options.action == ACT_SINGLE_QUERY)
	{
		if (VariableEquals(pset.vars, "ECHO", "all"))
			puts(options.action_string);

		successResult = SendQuery(options.action_string)
			? EXIT_SUCCESS : EXIT_FAILURE;
	}

	/*
	 * or otherwise enter interactive main loop
	 */
	else
	{
		if (!options.no_psqlrc)
			process_psqlrc(argv[0]);

		if (!QUIET() && !pset.notty)
		{
			int			client_ver = parse_version(PG_VERSION);

			if (pset.sversion != client_ver)
			{
				const char *server_version;
				char		server_ver_str[16];

				/* Try to get full text form, might include "devel" etc */
				server_version = PQparameterStatus(pset.db, "server_version");
				if (!server_version)
				{
					snprintf(server_ver_str, sizeof(server_ver_str),
							 "%d.%d.%d",
							 pset.sversion / 10000,
							 (pset.sversion / 100) % 100,
							 pset.sversion % 100);
					server_version = server_ver_str;
				}

				printf(_("Welcome to %s %s (server %s), the PostgreSQL interactive terminal.\n\n"),
					   pset.progname, PG_VERSION, server_version);
			}
			else
				printf(_("Welcome to %s %s, the PostgreSQL interactive terminal.\n\n"),
					   pset.progname, PG_VERSION);

			printf(_("Type:  \\copyright for distribution terms\n"
					 "       \\h for help with SQL commands\n"
					 "       \\? for help with psql commands\n"
				  "       \\g or terminate with semicolon to execute query\n"
					 "       \\q to quit\n\n"));

			if (pset.sversion / 100 != client_ver / 100)
				printf(_("WARNING:  You are connected to a server with major version %d.%d,\n"
						 "but your %s client is major version %d.%d.  Some backslash commands,\n"
						 "such as \\d, might not work properly.\n\n"),
					   pset.sversion / 10000, (pset.sversion / 100) % 100,
					   pset.progname,
					   client_ver / 10000, (client_ver / 100) % 100);

#ifdef USE_SSL
			printSSLInfo();
#endif
#ifdef WIN32
			checkWin32Codepage();
#endif
		}

		if (!pset.notty)
			initializeInput(options.no_readline ? 0 : 1);
		if (options.action_string)		/* -f - was used */
			pset.inputfile = "<stdin>";

		successResult = MainLoop(stdin);
	}

	/* clean up */
	if (pset.logfile)
		fclose(pset.logfile);
	PQfinish(pset.db);
	setQFout(NULL);

	return successResult;
}


/*
 * Convert a version string into a number.
 */
static int
parse_version(const char *versionString)
{
	int			cnt;
	int			vmaj,
				vmin,
				vrev;

	cnt = sscanf(versionString, "%d.%d.%d", &vmaj, &vmin, &vrev);

	if (cnt < 2)
		return -1;

	if (cnt == 2)
		vrev = 0;

	return (100 * vmaj + vmin) * 100 + vrev;
}


/*
 * Parse command line options
 */

static void
parse_psql_options(int argc, char *argv[], struct adhoc_opts * options)
{
	static struct option long_options[] =
	{
		{"echo-all", no_argument, NULL, 'a'},
		{"no-align", no_argument, NULL, 'A'},
		{"command", required_argument, NULL, 'c'},
		{"dbname", required_argument, NULL, 'd'},
		{"echo-queries", no_argument, NULL, 'e'},
		{"echo-hidden", no_argument, NULL, 'E'},
		{"file", required_argument, NULL, 'f'},
		{"field-separator", required_argument, NULL, 'F'},
		{"host", required_argument, NULL, 'h'},
		{"html", no_argument, NULL, 'H'},
		{"list", no_argument, NULL, 'l'},
		{"log-file", required_argument, NULL, 'L'},
		{"no-readline", no_argument, NULL, 'n'},
		{"output", required_argument, NULL, 'o'},
		{"port", required_argument, NULL, 'p'},
		{"pset", required_argument, NULL, 'P'},
		{"quiet", no_argument, NULL, 'q'},
		{"record-separator", required_argument, NULL, 'R'},
		{"single-step", no_argument, NULL, 's'},
		{"single-line", no_argument, NULL, 'S'},
		{"tuples-only", no_argument, NULL, 't'},
		{"table-attr", required_argument, NULL, 'T'},
		{"username", required_argument, NULL, 'U'},
		{"set", required_argument, NULL, 'v'},
		{"variable", required_argument, NULL, 'v'},
		{"version", no_argument, NULL, 'V'},
		{"password", no_argument, NULL, 'W'},
		{"expanded", no_argument, NULL, 'x'},
		{"no-psqlrc", no_argument, NULL, 'X'},
		{"help", no_argument, NULL, '?'},
		{NULL, 0, NULL, 0}
	};

	int			optindex;
	extern char *optarg;
	extern int	optind;
	int			c;
	bool		used_old_u_option = false;

	memset(options, 0, sizeof *options);

	while ((c = getopt_long(argc, argv, "aAc:d:eEf:F:h:HlL:no:p:P:qR:sStT:uU:v:VWxX?",
							long_options, &optindex)) != -1)
	{
		switch (c)
		{
			case 'a':
				SetVariable(pset.vars, "ECHO", "all");
				break;
			case 'A':
				pset.popt.topt.format = PRINT_UNALIGNED;
				break;
			case 'c':
				options->action_string = optarg;
				if (optarg[0] == '\\')
				{
					options->action = ACT_SINGLE_SLASH;
					options->action_string++;
				}
				else
					options->action = ACT_SINGLE_QUERY;
				break;
			case 'd':
				options->dbname = optarg;
				break;
			case 'e':
				SetVariable(pset.vars, "ECHO", "queries");
				break;
			case 'E':
				SetVariableBool(pset.vars, "ECHO_HIDDEN");
				break;
			case 'f':
				options->action = ACT_FILE;
				options->action_string = optarg;
				break;
			case 'F':
				pset.popt.topt.fieldSep = pg_strdup(optarg);
				break;
			case 'h':
				options->host = optarg;
				break;
			case 'H':
				pset.popt.topt.format = PRINT_HTML;
				break;
			case 'l':
				options->action = ACT_LIST_DB;
				break;
			case 'L':
				options->logfilename = optarg;
				break;
			case 'n':
				options->no_readline = true;
				break;
			case 'o':
				setQFout(optarg);
				break;
			case 'p':
				options->port = optarg;
				break;
			case 'P':
				{
					char	   *value;
					char	   *equal_loc;
					bool		result;

					value = pg_strdup(optarg);
					equal_loc = strchr(value, '=');
					if (!equal_loc)
						result = do_pset(value, NULL, &pset.popt, true);
					else
					{
						*equal_loc = '\0';
						result = do_pset(value, equal_loc + 1, &pset.popt, true);
					}

					if (!result)
					{
						fprintf(stderr, _("%s: could not set printing parameter \"%s\"\n"), pset.progname, value);
						exit(EXIT_FAILURE);
					}

					free(value);
					break;
				}
			case 'q':
				SetVariableBool(pset.vars, "QUIET");
				break;
			case 'R':
				pset.popt.topt.recordSep = pg_strdup(optarg);
				break;
			case 's':
				SetVariableBool(pset.vars, "SINGLESTEP");
				break;
			case 'S':
				SetVariableBool(pset.vars, "SINGLELINE");
				break;
			case 't':
				pset.popt.topt.tuples_only = true;
				break;
			case 'T':
				pset.popt.topt.tableAttr = pg_strdup(optarg);
				break;
			case 'u':
				pset.getPassword = true;
				options->username = "\001";		/* hopefully nobody has that
												 * username */
				/* this option is out */
				used_old_u_option = true;
				break;
			case 'U':
				options->username = optarg;
				break;
			case 'v':
				{
					char	   *value;
					char	   *equal_loc;

					value = pg_strdup(optarg);
					equal_loc = strchr(value, '=');
					if (!equal_loc)
					{
						if (!DeleteVariable(pset.vars, value))
						{
							fprintf(stderr, _("%s: could not delete variable \"%s\"\n"),
									pset.progname, value);
							exit(EXIT_FAILURE);
						}
					}
					else
					{
						*equal_loc = '\0';
						if (!SetVariable(pset.vars, value, equal_loc + 1))
						{
							fprintf(stderr, _("%s: could not set variable \"%s\"\n"),
									pset.progname, value);
							exit(EXIT_FAILURE);
						}
					}

					free(value);
					break;
				}
			case 'V':
				showVersion();
				exit(EXIT_SUCCESS);
			case 'W':
				pset.getPassword = true;
				break;
			case 'x':
				pset.popt.topt.expanded = true;
				break;
			case 'X':
				options->no_psqlrc = true;
				break;
			case '?':
				/* Actual help option given */
				if (strcmp(argv[optind - 1], "-?") == 0 || strcmp(argv[optind - 1], "--help") == 0)
				{
					usage();
					exit(EXIT_SUCCESS);
				}
				/* unknown option reported by getopt */
				else
				{
					fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
							pset.progname);
					exit(EXIT_FAILURE);
				}
				break;
			default:
				fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
						pset.progname);
				exit(EXIT_FAILURE);
				break;
		}
	}

	/*
	 * if we still have arguments, use it as the database name and username
	 */
	while (argc - optind >= 1)
	{
		if (!options->dbname)
			options->dbname = argv[optind];
		else if (!options->username)
			options->username = argv[optind];
		else if (!QUIET())
			fprintf(stderr, _("%s: warning: extra command-line argument \"%s\" ignored\n"),
					pset.progname, argv[optind]);

		optind++;
	}

	if (used_old_u_option && !QUIET())
		fprintf(stderr, _("%s: Warning: The -u option is deprecated. Use -U.\n"), pset.progname);

}


/*
 * Load .psqlrc file, if found.
 */
static void
process_psqlrc(char *argv0)
{
	char		home[MAXPGPATH];
	char		rc_file[MAXPGPATH];
	char		my_exec_path[MAXPGPATH];
	char		etc_path[MAXPGPATH];

	find_my_exec(argv0, my_exec_path);
	get_etc_path(my_exec_path, etc_path);

	snprintf(rc_file, MAXPGPATH, "%s/%s", etc_path, SYSPSQLRC);
	process_psqlrc_file(rc_file);

	if (get_home_path(home))
	{
		snprintf(rc_file, MAXPGPATH, "%s/%s", home, PSQLRC);
		process_psqlrc_file(rc_file);
	}
}



static void
process_psqlrc_file(char *filename)
{
	char	   *psqlrc;

#if defined(WIN32) && (!defined(__MINGW32__))
#define R_OK 4
#endif

	psqlrc = pg_malloc(strlen(filename) + 1 + strlen(PG_VERSION) + 1);
	sprintf(psqlrc, "%s-%s", filename, PG_VERSION);

	if (access(psqlrc, R_OK) == 0)
		(void) process_file(psqlrc);
	else if (access(filename, R_OK) == 0)
		(void) process_file(filename);
	free(psqlrc);
}



/* showVersion
 *
 * This output format is intended to match GNU standards.
 */
static void
showVersion(void)
{
	puts("psql (PostgreSQL) " PG_VERSION);

#if defined(USE_READLINE)
	puts(_("contains support for command-line editing"));
#endif
}



/*
 * printSSLInfo
 *
 * Prints information about the current SSL connection, if SSL is in use
 */
#ifdef USE_SSL
static void
printSSLInfo(void)
{
	int			sslbits = -1;
	SSL		   *ssl;

	ssl = PQgetssl(pset.db);
	if (!ssl)
		return;					/* no SSL */

	SSL_get_cipher_bits(ssl, &sslbits);
	printf(_("SSL connection (cipher: %s, bits: %i)\n\n"),
		   SSL_get_cipher(ssl), sslbits);
}
#endif



/*
 * checkWin32Codepage
 *
 * Prints a warning when win32 console codepage differs from Windows codepage
 */
#ifdef WIN32
static void
checkWin32Codepage(void)
{
	unsigned int wincp,
				concp;

	wincp = GetACP();
	concp = GetConsoleCP();
	if (wincp != concp)
	{
		printf(_("Warning: Console code page (%u) differs from Windows code page (%u)\n"
				 "         8-bit characters may not work correctly. See psql reference\n"
			   "         page \"Notes for Windows users\" for details.\n\n"),
			   concp, wincp);
	}
}

#endif
