#include <config.h>
#include <c.h>

#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#ifdef WIN32
#include <io.h>
#include <window.h>
#else
#include <unistd.h>
#endif

#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif

#include <libpq-fe.h>
#include <pqsignal.h>
#include <version.h>

#include "settings.h"
#include "command.h"
#include "help.h"
#include "mainloop.h"
#include "common.h"
#include "input.h"
#include "variables.h"
#include "print.h"
#include "describe.h"



static void
			process_psqlrc(PsqlSettings *pset);

static void
			showVersion(PsqlSettings *pset, bool verbose);


/* Structures to pass information between the option parsing routine
 * and the main function
 */
enum _actions
{
	ACT_NOTHING = 0,
	ACT_SINGLE_SLASH,
	ACT_LIST_DB,
	ACT_SHOW_VER,
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
};

static void
			parse_options(int argc, char *argv[], PsqlSettings *pset, struct adhoc_opts * options);



/*
 *
 * main()
 *
 */
int
main(int argc, char **argv)
{
	PsqlSettings settings;
	struct adhoc_opts options;
	int			successResult;

	char	   *username = NULL;
	char	   *password = NULL;
	bool		need_pass;

	MemSet(&settings, 0, sizeof settings);

	settings.cur_cmd_source = stdin;
	settings.cur_cmd_interactive = false;

	settings.vars = CreateVariableSpace();
	settings.popt.topt.format = PRINT_ALIGNED;
	settings.queryFout = stdout;
	settings.popt.topt.fieldSep = strdup(DEFAULT_FIELD_SEP);
	settings.popt.topt.border = 1;
	settings.popt.topt.pager = 1;

	SetVariable(settings.vars, "prompt1", DEFAULT_PROMPT1);
	SetVariable(settings.vars, "prompt2", DEFAULT_PROMPT2);
	SetVariable(settings.vars, "prompt3", DEFAULT_PROMPT3);

	settings.notty = (!isatty(fileno(stdin)) || !isatty(fileno(stdout)));

	/* This is obsolete and will be removed very soon. */
#ifdef PSQL_ALWAYS_GET_PASSWORDS
	settings.getPassword = true;
#else
	settings.getPassword = false;
#endif

#ifdef MULTIBYTE
	settings.has_client_encoding = (getenv("PGCLIENTENCODING") != NULL);
#endif

	parse_options(argc, argv, &settings, &options);

	if (options.action == ACT_LIST_DB || options.action == ACT_SHOW_VER)
		options.dbname = "template1";

	if (options.username)
	{
		if (strcmp(options.username, "?") == 0)
			username = simple_prompt("Username: ", 100, true);
		else
			username = strdup(options.username);
	}

	if (settings.getPassword)
		password = simple_prompt("Password: ", 100, false);

	/* loop until we have a password if requested by backend */
	do
	{
		need_pass = false;
		settings.db = PQsetdbLogin(options.host, options.port, NULL, NULL, options.dbname, username, password);

		if (PQstatus(settings.db) == CONNECTION_BAD &&
			strcmp(PQerrorMessage(settings.db), "fe_sendauth: no password supplied\n") == 0)
		{
			need_pass = true;
			free(password);
			password = NULL;
			password = simple_prompt("Password: ", 100, false);
		}
	} while (need_pass);

	free(username);
	free(password);

	if (PQstatus(settings.db) == CONNECTION_BAD)
	{
		fprintf(stderr, "Connection to database '%s' failed.\n%s\n", PQdb(settings.db), PQerrorMessage(settings.db));
		PQfinish(settings.db);
		exit(EXIT_BADCONN);
	}

	if (options.action == ACT_LIST_DB)
	{
		int			success = listAllDbs(&settings);

		PQfinish(settings.db);
		exit(!success);
	}

	if (options.action == ACT_SHOW_VER)
	{
		showVersion(&settings, true);
		PQfinish(settings.db);
		exit(EXIT_SUCCESS);
	}


	if (!GetVariable(settings.vars, "quiet") && !settings.notty && !options.action)
	{
		puts("Welcome to psql, the PostgreSQL interactive terminal.\n"
			 "Type \\copyright for distribution terms");

		//showVersion(&settings, false);

		puts("     \\h for help with SQL commands\n"
			 "     \\? for help on internal slash commands\n"
			 "     \\g or terminate with semicolon to execute query\n"
			 "     \\q to quit\n");
	}

	process_psqlrc(&settings);

	initializeInput(options.no_readline ? 0 : 1);

	/* Now find something to do */

	/* process file given by -f */
	if (options.action == ACT_FILE)
		successResult = process_file(options.action_string, &settings) ? 0 : 1;
	/* process slash command if one was given to -c */
	else if (options.action == ACT_SINGLE_SLASH)
		successResult = HandleSlashCmds(&settings, options.action_string, NULL, NULL) != CMD_ERROR ? 0 : 1;
	/* If the query given to -c was a normal one, send it */
	else if (options.action == ACT_SINGLE_QUERY)
		successResult = SendQuery(&settings, options.action_string) ? 0 : 1;
	/* or otherwise enter interactive main loop */
	else
		successResult = MainLoop(&settings, stdin);

	/* clean up */
	finishInput();
	PQfinish(settings.db);
	setQFout(NULL, &settings);
	DestroyVariableSpace(settings.vars);

	return successResult;
}



/*
 * Parse command line options
 */

#ifdef WIN32
/* getopt is not in the standard includes on Win32 */
int			getopt(int, char *const[], const char *);

#endif

static void
parse_options(int argc, char *argv[], PsqlSettings *pset, struct adhoc_opts * options)
{
#ifdef HAVE_GETOPT_LONG
	static struct option long_options[] = {
		{"no-align", no_argument, NULL, 'A'},
		{"command", required_argument, NULL, 'c'},
		{"database", required_argument, NULL, 'd'},
		{"dbname", required_argument, NULL, 'd'},
		{"echo", no_argument, NULL, 'e'},
		{"echo-queries", no_argument, NULL, 'e'},
		{"echo-all", no_argument, NULL, 'E'},
		{"echo-all-queries", no_argument, NULL, 'E'},
		{"file", required_argument, NULL, 'f'},
		{"field-sep", required_argument, NULL, 'F'},
		{"host", required_argument, NULL, 'h'},
		{"html", no_argument, NULL, 'H'},
		{"list", no_argument, NULL, 'l'},
		{"no-readline", no_argument, NULL, 'n'},
		{"out", required_argument, NULL, 'o'},
		{"to-file", required_argument, NULL, 'o'},
		{"port", required_argument, NULL, 'p'},
		{"pset", required_argument, NULL, 'P'},
		{"quiet", no_argument, NULL, 'q'},
		{"single-step", no_argument, NULL, 's'},
		{"single-line", no_argument, NULL, 'S'},
		{"tuples-only", no_argument, NULL, 't'},
		{"table-attr", required_argument, NULL, 'T'},
		{"username", required_argument, NULL, 'U'},
		{"expanded", no_argument, NULL, 'x'},
		{"set", required_argument, NULL, 'v'},
		{"variable", required_argument, NULL, 'v'},
		{"version", no_argument, NULL, 'V'},
		{"password", no_argument, NULL, 'W'},
		{"help", no_argument, NULL, '?'},
	};

	int			optindex;

#endif

	extern char *optarg;
	extern int	optind;
	int			c;

	MemSet(options, 0, sizeof *options);

#ifdef HAVE_GETOPT_LONG
	while ((c = getopt_long(argc, argv, "Ac:d:eEf:F:lh:Hno:p:P:qsStT:uU:v:VWx?", long_options, &optindex)) != -1)
#else

	/*
	 * Be sure to leave the '-' in here, so we can catch accidental long
	 * options.
	 */
	while ((c = getopt(argc, argv, "Ac:d:eEf:F:lh:Hno:p:P:qsStT:uU:v:VWx?-")) != -1)
#endif
	{
		switch (c)
		{
			case 'A':
				pset->popt.topt.format = PRINT_UNALIGNED;
				break;
			case 'c':
				options->action_string = optarg;
				if (optarg[0] == '\\')
					options->action = ACT_SINGLE_SLASH;
				else
					options->action = ACT_SINGLE_QUERY;
				break;
			case 'd':
				options->dbname = optarg;
				break;
			case 'e':
				SetVariable(pset->vars, "echo", "");
				break;
			case 'E':
				SetVariable(pset->vars, "echo_secret", "");
				break;
			case 'f':
				options->action = ACT_FILE;
				options->action_string = optarg;
				break;
			case 'F':
				pset->popt.topt.fieldSep = strdup(optarg);
				break;
			case 'h':
				options->host = optarg;
				break;
			case 'H':
				pset->popt.topt.format = PRINT_HTML;
				break;
			case 'l':
				options->action = ACT_LIST_DB;
				break;
			case 'n':
				options->no_readline = true;
				break;
			case 'o':
				setQFout(optarg, pset);
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
						result = do_pset(value, NULL, &pset->popt, true);
					else
					{
						*equal_loc = '\0';
						result = do_pset(value, equal_loc + 1, &pset->popt, true);
					}

					if (!result)
					{
						fprintf(stderr, "Couldn't set printing paramter %s.\n", value);
						exit(EXIT_FAILURE);
					}

					free(value);
					break;
				}
			case 'q':
				SetVariable(pset->vars, "quiet", "");
				break;
			case 's':
				SetVariable(pset->vars, "singlestep", "");
				break;
			case 'S':
				SetVariable(pset->vars, "singleline", "");
				break;
			case 't':
				pset->popt.topt.tuples_only = true;
				break;
			case 'T':
				pset->popt.topt.tableAttr = xstrdup(optarg);
				break;
			case 'u':
				pset->getPassword = true;
				options->username = "?";
				break;
			case 'U':
				options->username = optarg;
				break;
			case 'x':
				pset->popt.topt.expanded = true;
				break;
			case 'v':
				{
					char	   *value;
					char	   *equal_loc;

					value = xstrdup(optarg);
					equal_loc = strchr(value, '=');
					if (!equal_loc)
					{
						if (!DeleteVariable(pset->vars, value))
						{
							fprintf(stderr, "Couldn't delete variable %s.\n", value);
							exit(EXIT_FAILURE);
						}
					}
					else
					{
						*equal_loc = '\0';
						if (!SetVariable(pset->vars, value, equal_loc + 1))
						{
							fprintf(stderr, "Couldn't set variable %s to %s.\n", value, equal_loc);
							exit(EXIT_FAILURE);
						}
					}

					free(value);
					break;
				}
			case 'V':
				options->action = ACT_SHOW_VER;
				break;
			case 'W':
				pset->getPassword = true;
				break;
			case '?':
				usage();
				exit(EXIT_SUCCESS);
				break;
#ifndef HAVE_GETOPT_LONG
			case '-':
				fprintf(stderr, "This version of psql was compiled without support for long options.\n"
						"Use -? for help on invocation options.\n");
				exit(EXIT_FAILURE);
				break;
#endif
			default:
				usage();
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
		else
			fprintf(stderr, "Warning: extra option %s ignored.\n", argv[optind]);

		optind++;
	}
}



/*
 * Load /etc/psqlrc or .psqlrc file, if found.
 */
static void
process_psqlrc(PsqlSettings *pset)
{
	char	   *psqlrc;
	char	   *home;

#ifdef WIN32
#define R_OK 0
#endif

	/* System-wide startup file */
	if (access("/etc/psqlrc-" PG_RELEASE "." PG_VERSION "." PG_SUBVERSION, R_OK) == 0)
		process_file("/etc/psqlrc-" PG_RELEASE "." PG_VERSION "." PG_SUBVERSION, pset);
	else if (access("/etc/psqlrc", R_OK) == 0)
		process_file("/etc/psqlrc", pset);

	/* Look for one in the home dir */
	home = getenv("HOME");

	if (home)
	{
		psqlrc = (char *) malloc(strlen(home) + 20);
		if (!psqlrc)
		{
			perror("malloc");
			exit(EXIT_FAILURE);
		}

		sprintf(psqlrc, "%s/.psqlrc-" PG_RELEASE "." PG_VERSION "." PG_SUBVERSION, home);
		if (access(psqlrc, R_OK) == 0)
			process_file(psqlrc, pset);
		else
		{
			sprintf(psqlrc, "%s/.psqlrc", home);
			if (access(psqlrc, R_OK) == 0)
				process_file(psqlrc, pset);
		}
		free(psqlrc);
	}
}



/* showVersion
 *
 * Displays the database backend version.
 * Also checks against the version psql was compiled for and makes
 * sure that there are no problems.
 *
 * Returns false if there was a problem retrieving the information
 * or a mismatch was detected.
 */
static void
showVersion(PsqlSettings *pset, bool verbose)
{
	PGresult   *res;
	char	   *versionstr = NULL;
	long int	release = 0,
				version = 0,
				subversion = 0;

	/* get backend version */
	res = PSQLexec(pset, "SELECT version()");
	if (PQresultStatus(res) == PGRES_TUPLES_OK)
		versionstr = PQgetvalue(res, 0, 0);

	if (!verbose)
	{
		if (versionstr)
			puts(versionstr);
		PQclear(res);
		return;
	}

	if (strncmp(versionstr, "PostgreSQL ", 11) == 0)
	{
		char	   *tmp;

		release = strtol(&versionstr[11], &tmp, 10);
		version = strtol(tmp + 1, &tmp, 10);
		subversion = strtol(tmp + 1, &tmp, 10);
	}

	printf("Server: %s\npsql", versionstr ? versionstr : "(could not connected)");

	if (strcmp(versionstr, PG_VERSION_STR) != 0)
		printf(&PG_VERSION_STR[strcspn(PG_VERSION_STR, " ")]);
	printf(" (" __DATE__ " " __TIME__ ")");

#ifdef MULTIBYTE
	printf(", multibyte");
#endif
#ifdef HAVE_GETOPT_LONG
	printf(", long options");
#endif
#ifdef USE_READLINE
	printf(", readline");
#endif
#ifdef USE_HISTORY
	printf(", history");
#endif
#ifdef USE_LOCALE
	printf(", locale");
#endif
#ifdef PSQL_ALWAYS_GET_PASSWORDS
	printf(", always password");
#endif
#ifdef USE_ASSERT_CHECKING
	printf(", assert checks");
#endif

	puts("");

	if (release < 6 || (release == 6 && version < 5))
		puts("\nWarning: The server you are connected to is potentially too old for this client\n"
			 "version. You should ideally be using clients and servers from the same\n"
			 "distribution.");

	PQclear(res);
}
