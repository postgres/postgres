/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2003, PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/startup.c,v 1.80 2003/09/29 18:21:33 momjian Exp $
 */
#include "postgres_fe.h"

#include <sys/types.h>

#ifndef WIN32
#include <unistd.h>
#else							/* WIN32 */
#include <io.h>
#include <windows.h>
#include <win32.h>
#endif   /* WIN32 */

#include "getopt_long.h"

#ifndef HAVE_OPTRESET
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

#define PSQLRC ".psqlrc"

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
	enum _actions action;
	char	   *action_string;
	bool		no_readline;
	bool		no_psqlrc;
};

static void parse_psql_options(int argc, char *argv[],
				   struct adhoc_opts * options);
static void process_psqlrc(void);
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
	bool		need_pass;

	setlocale(LC_ALL, "");
#ifdef ENABLE_NLS
	bindtextdomain("psql", LOCALEDIR);
	textdomain("psql");
#endif

	pset.progname = get_progname(argv[0]);

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

	pset.cur_cmd_source = stdin;
	pset.cur_cmd_interactive = false;
	pset.encoding = PQenv2encoding();

	pset.vars = CreateVariableSpace();
	if (!pset.vars)
	{
		fprintf(stderr, gettext("%s: out of memory\n"), pset.progname);
		exit(EXIT_FAILURE);
	}
	pset.popt.topt.format = PRINT_ALIGNED;
	pset.queryFout = stdout;
	pset.popt.topt.border = 1;
	pset.popt.topt.pager = 1;
	pset.popt.default_footer = true;

	SetVariable(pset.vars, "VERSION", PG_VERSION_STR);

	/* Default values for variables that are used in noninteractive cases */
	SetVariableBool(pset.vars, "AUTOCOMMIT");
	SetVariable(pset.vars, "VERBOSITY", "default");

	pset.notty = (!isatty(fileno(stdin)) || !isatty(fileno(stdout)));

	/* This is obsolete and should be removed sometime. */
#ifdef PSQL_ALWAYS_GET_PASSWORDS
	pset.getPassword = true;
#else
	pset.getPassword = false;
#endif

	parse_psql_options(argc, argv, &options);

	if (!pset.popt.topt.fieldSep)
		pset.popt.topt.fieldSep = xstrdup(DEFAULT_FIELD_SEP);
	if (!pset.popt.topt.recordSep)
		pset.popt.topt.recordSep = xstrdup(DEFAULT_RECORD_SEP);

	if (options.username)
	{
		/*
		 * The \001 is a hack to support the deprecated -u option which
		 * issues a username prompt. The recommended option is -U followed
		 * by the name on the command line.
		 */
		if (strcmp(options.username, "\001") == 0)
			username = simple_prompt("User name: ", 100, true);
		else
			username = strdup(options.username);
	}

	if (pset.getPassword)
		password = simple_prompt("Password: ", 100, false);

	/* loop until we have a password if requested by backend */
	do
	{
		need_pass = false;
		pset.db = PQsetdbLogin(options.host, options.port, NULL, NULL,
			options.action == ACT_LIST_DB ? "template1" : options.dbname,
							   username, password);

		if (PQstatus(pset.db) == CONNECTION_BAD &&
			strcmp(PQerrorMessage(pset.db), "fe_sendauth: no password supplied\n") == 0 &&
			!feof(stdin))
		{
			PQfinish(pset.db);
			need_pass = true;
			free(password);
			password = NULL;
			password = simple_prompt("Password: ", 100, false);
		}
	} while (need_pass);

	free(username);
	free(password);

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

	/*
	 * Now find something to do
	 */

	/*
	 * process file given by -f
	 */
	if (options.action == ACT_FILE && strcmp(options.action_string, "-") != 0)
	{
		if (!options.no_psqlrc)
			process_psqlrc();

		successResult = process_file(options.action_string);
	}

	/*
	 * process slash command if one was given to -c
	 */
	else if (options.action == ACT_SINGLE_SLASH)
	{
		if (VariableEquals(pset.vars, "ECHO", "all"))
			puts(options.action_string);

		successResult = HandleSlashCmds(options.action_string, NULL, NULL, NULL) != CMD_ERROR
			? EXIT_SUCCESS : EXIT_FAILURE;
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
		if (!QUIET() && !pset.notty)
		{
			printf(gettext("Welcome to %s %s, the PostgreSQL interactive terminal.\n\n"
						   "Type:  \\copyright for distribution terms\n"
						   "       \\h for help with SQL commands\n"
					   "       \\? for help on internal slash commands\n"
			  "       \\g or terminate with semicolon to execute query\n"
						   "       \\q to quit\n\n"),
				   pset.progname, PG_VERSION);
#ifdef USE_SSL
			printSSLInfo();
#endif
#ifdef WIN32
			checkWin32Codepage();
#endif
		}

		/* Default values for variables that are used in interactive case */
		SetVariable(pset.vars, "PROMPT1", DEFAULT_PROMPT1);
		SetVariable(pset.vars, "PROMPT2", DEFAULT_PROMPT2);
		SetVariable(pset.vars, "PROMPT3", DEFAULT_PROMPT3);

		if (!options.no_psqlrc)
			process_psqlrc();
		if (!pset.notty)
			initializeInput(options.no_readline ? 0 : 1);
		if (options.action_string)		/* -f - was used */
			pset.inputfile = "<stdin>";

		successResult = MainLoop(stdin);
	}

	/* clean up */
	PQfinish(pset.db);
	setQFout(NULL);

	return successResult;
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

	while ((c = getopt_long(argc, argv, "aAc:d:eEf:F:h:Hlno:p:P:qR:sStT:uU:v:VWxX?",
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
				pset.popt.topt.fieldSep = xstrdup(optarg);
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

					value = xstrdup(optarg);
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
						fprintf(stderr, gettext("%s: couldn't set printing parameter \"%s\"\n"), pset.progname, value);
						exit(EXIT_FAILURE);
					}

					free(value);
					break;
				}
			case 'q':
				SetVariableBool(pset.vars, "QUIET");
				break;
			case 'R':
				pset.popt.topt.recordSep = xstrdup(optarg);
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
				pset.popt.topt.tableAttr = xstrdup(optarg);
				break;
			case 'u':
				pset.getPassword = true;
				options->username = "\001";		/* hopefully nobody has
												 * that username */
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

					value = xstrdup(optarg);
					equal_loc = strchr(value, '=');
					if (!equal_loc)
					{
						if (!DeleteVariable(pset.vars, value))
						{
							fprintf(stderr, gettext("%s: could not delete variable \"%s\"\n"),
									pset.progname, value);
							exit(EXIT_FAILURE);
						}
					}
					else
					{
						*equal_loc = '\0';
						if (!SetVariable(pset.vars, value, equal_loc + 1))
						{
							fprintf(stderr, gettext("%s: could not set variable \"%s\"\n"),
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
					fprintf(stderr, gettext("Try \"%s --help\" for more information.\n"),
							pset.progname);
					exit(EXIT_FAILURE);
				}
				break;
			default:
				fprintf(stderr, gettext("Try \"%s --help\" for more information.\n"),
						pset.progname);
				exit(EXIT_FAILURE);
				break;
		}
	}

	/*
	 * if we still have arguments, use it as the database name and
	 * username
	 */
	while (argc - optind >= 1)
	{
		if (!options->dbname)
			options->dbname = argv[optind];
		else if (!options->username)
			options->username = argv[optind];
		else if (!QUIET())
			fprintf(stderr, gettext("%s: warning: extra command-line argument \"%s\" ignored\n"),
					pset.progname, argv[optind]);

		optind++;
	}

	if (used_old_u_option && !QUIET())
		fprintf(stderr, gettext("%s: Warning: The -u option is deprecated. Use -U.\n"), pset.progname);

}



/*
 * Load .psqlrc file, if found.
 */
static void
process_psqlrc(void)
{
	char	   *psqlrc;
	char	   *home;

#ifdef WIN32
#define R_OK 0
#endif

	/* Look for one in the home dir */
	home = getenv("HOME");

	if (home)
	{
		psqlrc = malloc(strlen(home) + 1 + strlen(PSQLRC) + 1 +
						strlen(PG_VERSION) + 1);
		if (!psqlrc)
		{
			fprintf(stderr, gettext("%s: out of memory\n"), pset.progname);
			exit(EXIT_FAILURE);
		}

		sprintf(psqlrc, "%s/%s-%s", home, PSQLRC, PG_VERSION);
		if (access(psqlrc, R_OK) == 0)
			process_file(psqlrc);
		else
		{
			sprintf(psqlrc, "%s/%s", home, PSQLRC);
			if (access(psqlrc, R_OK) == 0)
				process_file(psqlrc);
		}
		free(psqlrc);
	}
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
	puts(gettext("contains support for command-line editing"));
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
	printf(gettext("SSL connection (cipher: %s, bits: %i)\n\n"),
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
	unsigned int wincp, concp;

	wincp = GetACP();
	concp = GetConsoleCP();
	if (wincp != concp) {
	  printf("Warning: Console codepage (%u) differs from windows codepage (%u)\n"
			 "         8-bit characters will not work correctly. See PostgreSQL\n"
			 "         documentation \"Installation on Windows\" for details.\n\n",
			 concp, wincp);
	}
}
#endif
