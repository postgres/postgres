/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2006, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/bin/psql/startup.c,v 1.138 2006/10/04 00:30:06 momjian Exp $
 */
#include "postgres_fe.h"

#include <sys/types.h>
#ifdef USE_SSL
#include <openssl/ssl.h>
#endif

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


#include "command.h"
#include "common.h"
#include "describe.h"
#include "help.h"
#include "input.h"
#include "mainloop.h"
#include "settings.h"



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
	bool		single_txn;
};

static int	parse_version(const char *versionString);
static void parse_psql_options(int argc, char *argv[],
				   struct adhoc_opts * options);
static void process_psqlrc(char *argv0);
static void process_psqlrc_file(char *filename);
static void showVersion(void);
static void EstablishVariableSpace(void);

#ifdef USE_SSL
static void printSSLInfo(void);
#endif

#ifdef WIN32
static void checkWin32Codepage(void);
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

#ifdef WIN32
	setvbuf(stderr, NULL, _IONBF, 0);
#endif

	setup_cancel_handler();

	pset.progname = get_progname(argv[0]);

	pset.db = NULL;
	setDecimalLocale();
	pset.encoding = PQenv2encoding();
	pset.queryFout = stdout;
	pset.queryFoutPipe = false;
	pset.cur_cmd_source = stdin;
	pset.cur_cmd_interactive = false;

	pset.popt.topt.format = PRINT_ALIGNED;
	pset.popt.topt.border = 1;
	pset.popt.topt.pager = 1;
	pset.popt.topt.start_table = true;
	pset.popt.topt.stop_table = true;
	pset.popt.default_footer = true;

	pset.notty = (!isatty(fileno(stdin)) || !isatty(fileno(stdout)));

	/* This is obsolete and should be removed sometime. */
#ifdef PSQL_ALWAYS_GET_PASSWORDS
	pset.getPassword = true;
#else
	pset.getPassword = false;
#endif

	EstablishVariableSpace();

	SetVariable(pset.vars, "VERSION", PG_VERSION_STR);

	/* Default values for variables */
	SetVariableBool(pset.vars, "AUTOCOMMIT");
	SetVariable(pset.vars, "VERBOSITY", "default");
	SetVariable(pset.vars, "PROMPT1", DEFAULT_PROMPT1);
	SetVariable(pset.vars, "PROMPT2", DEFAULT_PROMPT2);
	SetVariable(pset.vars, "PROMPT3", DEFAULT_PROMPT3);

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

		successResult = process_file(options.action_string, options.single_txn);
	}

	/*
	 * process slash command if one was given to -c
	 */
	else if (options.action == ACT_SINGLE_SLASH)
	{
		PsqlScanState scan_state;

		if (pset.echo == PSQL_ECHO_ALL)
			puts(options.action_string);

		scan_state = psql_scan_create();
		psql_scan_setup(scan_state,
						options.action_string,
						strlen(options.action_string));

		successResult = HandleSlashCmds(scan_state, NULL) != PSQL_CMD_ERROR
			? EXIT_SUCCESS : EXIT_FAILURE;

		psql_scan_destroy(scan_state);
	}

	/*
	 * If the query given to -c was a normal one, send it
	 */
	else if (options.action == ACT_SINGLE_QUERY)
	{
		if (pset.echo == PSQL_ECHO_ALL)
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

		if (!pset.quiet && !pset.notty)
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
		{"single-transaction", no_argument, NULL, '1'},
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

	while ((c = getopt_long(argc, argv, "aAc:d:eEf:F:h:HlL:no:p:P:qR:sStT:uU:v:VWxX?1",
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
			case '1':
				options->single_txn = true;
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
		else if (!pset.quiet)
			fprintf(stderr, _("%s: warning: extra command-line argument \"%s\" ignored\n"),
					pset.progname, argv[optind]);

		optind++;
	}

	if (used_old_u_option && !pset.quiet)
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
		(void) process_file(psqlrc, false);
	else if (access(filename, R_OK) == 0)
		(void) process_file(filename, false);
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


/*
 * Assign hooks for psql variables.
 *
 * This isn't an amazingly good place for them, but neither is anywhere else.
 */

static void
autocommit_hook(const char *newval)
{
	pset.autocommit = ParseVariableBool(newval);
}

static void
on_error_stop_hook(const char *newval)
{
	pset.on_error_stop = ParseVariableBool(newval);
}

static void
quiet_hook(const char *newval)
{
	pset.quiet = ParseVariableBool(newval);
}

static void
singleline_hook(const char *newval)
{
	pset.singleline = ParseVariableBool(newval);
}

static void
singlestep_hook(const char *newval)
{
	pset.singlestep = ParseVariableBool(newval);
}

static void
fetch_count_hook(const char *newval)
{
	pset.fetch_count = ParseVariableNum(newval, -1, -1, false);
}

static void
echo_hook(const char *newval)
{
	if (newval == NULL)
		pset.echo = PSQL_ECHO_NONE;
	else if (strcmp(newval, "queries") == 0)
		pset.echo = PSQL_ECHO_QUERIES;
	else if (strcmp(newval, "all") == 0)
		pset.echo = PSQL_ECHO_ALL;
	else
		pset.echo = PSQL_ECHO_NONE;
}

static void
echo_hidden_hook(const char *newval)
{
	if (newval == NULL)
		pset.echo_hidden = PSQL_ECHO_HIDDEN_OFF;
	else if (strcmp(newval, "noexec") == 0)
		pset.echo_hidden = PSQL_ECHO_HIDDEN_NOEXEC;
	else if (pg_strcasecmp(newval, "off") == 0)
		pset.echo_hidden = PSQL_ECHO_HIDDEN_OFF;
	else
		pset.echo_hidden = PSQL_ECHO_HIDDEN_ON;
}

static void
on_error_rollback_hook(const char *newval)
{
	if (newval == NULL)
		pset.on_error_rollback = PSQL_ERROR_ROLLBACK_OFF;
	else if (pg_strcasecmp(newval, "interactive") == 0)
		pset.on_error_rollback = PSQL_ERROR_ROLLBACK_INTERACTIVE;
	else if (pg_strcasecmp(newval, "off") == 0)
		pset.on_error_rollback = PSQL_ERROR_ROLLBACK_OFF;
	else
		pset.on_error_rollback = PSQL_ERROR_ROLLBACK_ON;
}

static void
histcontrol_hook(const char *newval)
{
	if (newval == NULL)
		pset.histcontrol = hctl_none;
	else if (strcmp(newval, "ignorespace") == 0)
		pset.histcontrol = hctl_ignorespace;
	else if (strcmp(newval, "ignoredups") == 0)
		pset.histcontrol = hctl_ignoredups;
	else if (strcmp(newval, "ignoreboth") == 0)
		pset.histcontrol = hctl_ignoreboth;
	else
		pset.histcontrol = hctl_none;
}

static void
prompt1_hook(const char *newval)
{
	pset.prompt1 = newval ? newval : "";
}

static void
prompt2_hook(const char *newval)
{
	pset.prompt2 = newval ? newval : "";
}

static void
prompt3_hook(const char *newval)
{
	pset.prompt3 = newval ? newval : "";
}

static void
verbosity_hook(const char *newval)
{
	if (newval == NULL)
		pset.verbosity = PQERRORS_DEFAULT;
	else if (strcmp(newval, "default") == 0)
		pset.verbosity = PQERRORS_DEFAULT;
	else if (strcmp(newval, "terse") == 0)
		pset.verbosity = PQERRORS_TERSE;
	else if (strcmp(newval, "verbose") == 0)
		pset.verbosity = PQERRORS_VERBOSE;
	else
		pset.verbosity = PQERRORS_DEFAULT;

	if (pset.db)
		PQsetErrorVerbosity(pset.db, pset.verbosity);
}


static void
EstablishVariableSpace(void)
{
	pset.vars = CreateVariableSpace();

	SetVariableAssignHook(pset.vars, "AUTOCOMMIT", autocommit_hook);
	SetVariableAssignHook(pset.vars, "ON_ERROR_STOP", on_error_stop_hook);
	SetVariableAssignHook(pset.vars, "QUIET", quiet_hook);
	SetVariableAssignHook(pset.vars, "SINGLELINE", singleline_hook);
	SetVariableAssignHook(pset.vars, "SINGLESTEP", singlestep_hook);
	SetVariableAssignHook(pset.vars, "FETCH_COUNT", fetch_count_hook);
	SetVariableAssignHook(pset.vars, "ECHO", echo_hook);
	SetVariableAssignHook(pset.vars, "ECHO_HIDDEN", echo_hidden_hook);
	SetVariableAssignHook(pset.vars, "ON_ERROR_ROLLBACK", on_error_rollback_hook);
	SetVariableAssignHook(pset.vars, "HISTCONTROL", histcontrol_hook);
	SetVariableAssignHook(pset.vars, "PROMPT1", prompt1_hook);
	SetVariableAssignHook(pset.vars, "PROMPT2", prompt2_hook);
	SetVariableAssignHook(pset.vars, "PROMPT3", prompt3_hook);
	SetVariableAssignHook(pset.vars, "VERBOSITY", verbosity_hook);
}
