/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2016, PostgreSQL Global Development Group
 *
 * src/bin/psql/startup.c
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

#include <locale.h>

#include "command.h"
#include "common.h"
#include "describe.h"
#include "help.h"
#include "input.h"
#include "mainloop.h"
#include "fe_utils/print.h"
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
	ACT_SINGLE_QUERY,
	ACT_SINGLE_SLASH,
	ACT_FILE
};

typedef struct SimpleActionListCell
{
	struct SimpleActionListCell *next;
	enum _actions action;
	char	   *val;
} SimpleActionListCell;

typedef struct SimpleActionList
{
	SimpleActionListCell *head;
	SimpleActionListCell *tail;
} SimpleActionList;

struct adhoc_opts
{
	char	   *dbname;
	char	   *host;
	char	   *port;
	char	   *username;
	char	   *logfilename;
	bool		no_readline;
	bool		no_psqlrc;
	bool		single_txn;
	bool		list_dbs;
	SimpleActionList actions;
};

static void parse_psql_options(int argc, char *argv[],
				   struct adhoc_opts * options);
static void simple_action_list_append(SimpleActionList *list,
						  enum _actions action, const char *val);
static void process_psqlrc(char *argv0);
static void process_psqlrc_file(char *filename);
static void showVersion(void);
static void EstablishVariableSpace(void);

#define NOPAGER		0

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
	bool		have_password = false;
	char		password[100];
	char	   *password_prompt = NULL;
	bool		new_pass;

	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("psql"));

	if (argc > 1)
	{
		if ((strcmp(argv[1], "-?") == 0) || (argc == 2 && (strcmp(argv[1], "--help") == 0)))
		{
			usage(NOPAGER);
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

	pset.progname = get_progname(argv[0]);

	pset.db = NULL;
	setDecimalLocale();
	pset.encoding = PQenv2encoding();
	pset.queryFout = stdout;
	pset.queryFoutPipe = false;
	pset.copyStream = NULL;
	pset.last_error_result = NULL;
	pset.cur_cmd_source = stdin;
	pset.cur_cmd_interactive = false;

	/* We rely on unmentioned fields of pset.popt to start out 0/false/NULL */
	pset.popt.topt.format = PRINT_ALIGNED;
	pset.popt.topt.border = 1;
	pset.popt.topt.pager = 1;
	pset.popt.topt.pager_min_lines = 0;
	pset.popt.topt.start_table = true;
	pset.popt.topt.stop_table = true;
	pset.popt.topt.default_footer = true;

	pset.popt.topt.unicode_border_linestyle = UNICODE_LINESTYLE_SINGLE;
	pset.popt.topt.unicode_column_linestyle = UNICODE_LINESTYLE_SINGLE;
	pset.popt.topt.unicode_header_linestyle = UNICODE_LINESTYLE_SINGLE;

	refresh_utf8format(&(pset.popt.topt));

	/* We must get COLUMNS here before readline() sets it */
	pset.popt.topt.env_columns = getenv("COLUMNS") ? atoi(getenv("COLUMNS")) : 0;

	pset.notty = (!isatty(fileno(stdin)) || !isatty(fileno(stdout)));

	pset.getPassword = TRI_DEFAULT;

	EstablishVariableSpace();

	SetVariable(pset.vars, "VERSION", PG_VERSION_STR);

	/* Default values for variables */
	SetVariableBool(pset.vars, "AUTOCOMMIT");
	SetVariable(pset.vars, "VERBOSITY", "default");
	SetVariable(pset.vars, "SHOW_CONTEXT", "errors");
	SetVariable(pset.vars, "PROMPT1", DEFAULT_PROMPT1);
	SetVariable(pset.vars, "PROMPT2", DEFAULT_PROMPT2);
	SetVariable(pset.vars, "PROMPT3", DEFAULT_PROMPT3);

	parse_psql_options(argc, argv, &options);

	/*
	 * If no action was specified and we're in non-interactive mode, treat it
	 * as if the user had specified "-f -".  This lets single-transaction mode
	 * work in this case.
	 */
	if (options.actions.head == NULL && pset.notty)
		simple_action_list_append(&options.actions, ACT_FILE, NULL);

	/* Bail out if -1 was specified but will be ignored. */
	if (options.single_txn && options.actions.head == NULL)
	{
		fprintf(stderr, _("%s: -1 can only be used in non-interactive mode\n"), pset.progname);
		exit(EXIT_FAILURE);
	}

	if (!pset.popt.topt.fieldSep.separator &&
		!pset.popt.topt.fieldSep.separator_zero)
	{
		pset.popt.topt.fieldSep.separator = pg_strdup(DEFAULT_FIELD_SEP);
		pset.popt.topt.fieldSep.separator_zero = false;
	}
	if (!pset.popt.topt.recordSep.separator &&
		!pset.popt.topt.recordSep.separator_zero)
	{
		pset.popt.topt.recordSep.separator = pg_strdup(DEFAULT_RECORD_SEP);
		pset.popt.topt.recordSep.separator_zero = false;
	}

	if (options.username == NULL)
		password_prompt = pg_strdup(_("Password: "));
	else
		password_prompt = psprintf(_("Password for user %s: "),
								   options.username);

	if (pset.getPassword == TRI_YES)
	{
		simple_prompt(password_prompt, password, sizeof(password), false);
		have_password = true;
	}

	/* loop until we have a password if requested by backend */
	do
	{
#define PARAMS_ARRAY_SIZE	8
		const char **keywords = pg_malloc(PARAMS_ARRAY_SIZE * sizeof(*keywords));
		const char **values = pg_malloc(PARAMS_ARRAY_SIZE * sizeof(*values));

		keywords[0] = "host";
		values[0] = options.host;
		keywords[1] = "port";
		values[1] = options.port;
		keywords[2] = "user";
		values[2] = options.username;
		keywords[3] = "password";
		values[3] = have_password ? password : NULL;
		keywords[4] = "dbname"; /* see do_connect() */
		values[4] = (options.list_dbs && options.dbname == NULL) ?
			"postgres" : options.dbname;
		keywords[5] = "fallback_application_name";
		values[5] = pset.progname;
		keywords[6] = "client_encoding";
		values[6] = (pset.notty || getenv("PGCLIENTENCODING")) ? NULL : "auto";
		keywords[7] = NULL;
		values[7] = NULL;

		new_pass = false;
		pset.db = PQconnectdbParams(keywords, values, true);
		free(keywords);
		free(values);

		if (PQstatus(pset.db) == CONNECTION_BAD &&
			PQconnectionNeedsPassword(pset.db) &&
			!have_password &&
			pset.getPassword != TRI_NO)
		{
			PQfinish(pset.db);
			simple_prompt(password_prompt, password, sizeof(password), false);
			have_password = true;
			new_pass = true;
		}
	} while (new_pass);

	free(password_prompt);

	if (PQstatus(pset.db) == CONNECTION_BAD)
	{
		fprintf(stderr, "%s: %s", pset.progname, PQerrorMessage(pset.db));
		PQfinish(pset.db);
		exit(EXIT_BADCONN);
	}

	setup_cancel_handler();

	PQsetNoticeProcessor(pset.db, NoticeProcessor, NULL);

	SyncVariables();

	if (options.list_dbs)
	{
		int			success;

		if (!options.no_psqlrc)
			process_psqlrc(argv[0]);

		success = listAllDbs(NULL, false);
		PQfinish(pset.db);
		exit(success ? EXIT_SUCCESS : EXIT_FAILURE);
	}

	if (options.logfilename)
	{
		pset.logfile = fopen(options.logfilename, "a");
		if (!pset.logfile)
		{
			fprintf(stderr, _("%s: could not open log file \"%s\": %s\n"),
					pset.progname, options.logfilename, strerror(errno));
			exit(EXIT_FAILURE);
		}
	}

	if (!options.no_psqlrc)
		process_psqlrc(argv[0]);

	/*
	 * If any actions were given by user, process them in the order in which
	 * they were specified.  Note single_txn is only effective in this mode.
	 */
	if (options.actions.head != NULL)
	{
		PGresult   *res;
		SimpleActionListCell *cell;

		successResult = EXIT_SUCCESS;	/* silence compiler */

		if (options.single_txn)
		{
			if ((res = PSQLexec("BEGIN")) == NULL)
			{
				if (pset.on_error_stop)
				{
					successResult = EXIT_USER;
					goto error;
				}
			}
			else
				PQclear(res);
		}

		for (cell = options.actions.head; cell; cell = cell->next)
		{
			if (cell->action == ACT_SINGLE_QUERY)
			{
				if (pset.echo == PSQL_ECHO_ALL)
					puts(cell->val);

				successResult = SendQuery(cell->val)
					? EXIT_SUCCESS : EXIT_FAILURE;
			}
			else if (cell->action == ACT_SINGLE_SLASH)
			{
				PsqlScanState scan_state;

				if (pset.echo == PSQL_ECHO_ALL)
					puts(cell->val);

				scan_state = psql_scan_create(&psqlscan_callbacks);
				psql_scan_setup(scan_state,
								cell->val, strlen(cell->val),
								pset.encoding, standard_strings());

				successResult = HandleSlashCmds(scan_state, NULL) != PSQL_CMD_ERROR
					? EXIT_SUCCESS : EXIT_FAILURE;

				psql_scan_destroy(scan_state);
			}
			else if (cell->action == ACT_FILE)
			{
				successResult = process_file(cell->val, false);
			}
			else
			{
				/* should never come here */
				Assert(false);
			}

			if (successResult != EXIT_SUCCESS && pset.on_error_stop)
				break;
		}

		if (options.single_txn)
		{
			if ((res = PSQLexec("COMMIT")) == NULL)
			{
				if (pset.on_error_stop)
				{
					successResult = EXIT_USER;
					goto error;
				}
			}
			else
				PQclear(res);
		}

error:
		;
	}

	/*
	 * or otherwise enter interactive main loop
	 */
	else
	{
		connection_warnings(true);
		if (!pset.quiet)
			printf(_("Type \"help\" for help.\n\n"));
		initializeInput(options.no_readline ? 0 : 1);
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
		{"echo-errors", no_argument, NULL, 'b'},
		{"echo-hidden", no_argument, NULL, 'E'},
		{"file", required_argument, NULL, 'f'},
		{"field-separator", required_argument, NULL, 'F'},
		{"field-separator-zero", no_argument, NULL, 'z'},
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
		{"record-separator-zero", no_argument, NULL, '0'},
		{"single-step", no_argument, NULL, 's'},
		{"single-line", no_argument, NULL, 'S'},
		{"tuples-only", no_argument, NULL, 't'},
		{"table-attr", required_argument, NULL, 'T'},
		{"username", required_argument, NULL, 'U'},
		{"set", required_argument, NULL, 'v'},
		{"variable", required_argument, NULL, 'v'},
		{"version", no_argument, NULL, 'V'},
		{"no-password", no_argument, NULL, 'w'},
		{"password", no_argument, NULL, 'W'},
		{"expanded", no_argument, NULL, 'x'},
		{"no-psqlrc", no_argument, NULL, 'X'},
		{"help", optional_argument, NULL, 1},
		{NULL, 0, NULL, 0}
	};

	int			optindex;
	int			c;

	memset(options, 0, sizeof *options);

	while ((c = getopt_long(argc, argv, "aAbc:d:eEf:F:h:HlL:no:p:P:qR:sStT:U:v:VwWxXz?01",
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
			case 'b':
				SetVariable(pset.vars, "ECHO", "errors");
				break;
			case 'c':
				if (optarg[0] == '\\')
					simple_action_list_append(&options->actions,
											  ACT_SINGLE_SLASH,
											  optarg + 1);
				else
					simple_action_list_append(&options->actions,
											  ACT_SINGLE_QUERY,
											  optarg);
				break;
			case 'd':
				options->dbname = pg_strdup(optarg);
				break;
			case 'e':
				SetVariable(pset.vars, "ECHO", "queries");
				break;
			case 'E':
				SetVariableBool(pset.vars, "ECHO_HIDDEN");
				break;
			case 'f':
				simple_action_list_append(&options->actions,
										  ACT_FILE,
										  optarg);
				break;
			case 'F':
				pset.popt.topt.fieldSep.separator = pg_strdup(optarg);
				pset.popt.topt.fieldSep.separator_zero = false;
				break;
			case 'h':
				options->host = pg_strdup(optarg);
				break;
			case 'H':
				pset.popt.topt.format = PRINT_HTML;
				break;
			case 'l':
				options->list_dbs = true;
				break;
			case 'L':
				options->logfilename = pg_strdup(optarg);
				break;
			case 'n':
				options->no_readline = true;
				break;
			case 'o':
				if (!setQFout(optarg))
					exit(EXIT_FAILURE);
				break;
			case 'p':
				options->port = pg_strdup(optarg);
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
				pset.popt.topt.recordSep.separator = pg_strdup(optarg);
				pset.popt.topt.recordSep.separator_zero = false;
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
			case 'U':
				options->username = pg_strdup(optarg);
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
			case 'w':
				pset.getPassword = TRI_NO;
				break;
			case 'W':
				pset.getPassword = TRI_YES;
				break;
			case 'x':
				pset.popt.topt.expanded = true;
				break;
			case 'X':
				options->no_psqlrc = true;
				break;
			case 'z':
				pset.popt.topt.fieldSep.separator_zero = true;
				break;
			case '0':
				pset.popt.topt.recordSep.separator_zero = true;
				break;
			case '1':
				options->single_txn = true;
				break;
			case '?':
				/* Actual help option given */
				if (strcmp(argv[optind - 1], "-?") == 0)
				{
					usage(NOPAGER);
					exit(EXIT_SUCCESS);
				}
				/* unknown option reported by getopt */
				else
					goto unknown_option;
				break;
			case 1:
				{
					if (!optarg || strcmp(optarg, "options") == 0)
						usage(NOPAGER);
					else if (optarg && strcmp(optarg, "commands") == 0)
						slashUsage(NOPAGER);
					else if (optarg && strcmp(optarg, "variables") == 0)
						helpVariables(NOPAGER);
					else
						goto unknown_option;

					exit(EXIT_SUCCESS);
				}
				break;
			default:
		unknown_option:
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
}


/*
 * Append a new item to the end of the SimpleActionList.
 * Note that "val" is copied if it's not NULL.
 */
static void
simple_action_list_append(SimpleActionList *list,
						  enum _actions action, const char *val)
{
	SimpleActionListCell *cell;

	cell = (SimpleActionListCell *) pg_malloc(sizeof(SimpleActionListCell));

	cell->next = NULL;
	cell->action = action;
	if (val)
		cell->val = pg_strdup(val);
	else
		cell->val = NULL;

	if (list->tail)
		list->tail->next = cell;
	else
		list->head = cell;
	list->tail = cell;
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
	char	   *envrc = getenv("PSQLRC");

	if (find_my_exec(argv0, my_exec_path) < 0)
	{
		fprintf(stderr, _("%s: could not find own program executable\n"), argv0);
		exit(EXIT_FAILURE);
	}

	get_etc_path(my_exec_path, etc_path);

	snprintf(rc_file, MAXPGPATH, "%s/%s", etc_path, SYSPSQLRC);
	process_psqlrc_file(rc_file);

	if (envrc != NULL && strlen(envrc) > 0)
	{
		/* might need to free() this */
		char	   *envrc_alloc = pstrdup(envrc);

		expand_tilde(&envrc_alloc);
		process_psqlrc_file(envrc_alloc);
	}
	else if (get_home_path(home))
	{
		snprintf(rc_file, MAXPGPATH, "%s/%s", home, PSQLRC);
		process_psqlrc_file(rc_file);
	}
}



static void
process_psqlrc_file(char *filename)
{
	char	   *psqlrc_minor,
			   *psqlrc_major;

#if defined(WIN32) && (!defined(__MINGW32__))
#define R_OK 4
#endif

	psqlrc_minor = psprintf("%s-%s", filename, PG_VERSION);
	psqlrc_major = psprintf("%s-%s", filename, PG_MAJORVERSION);

	/* check for minor version first, then major, then no version */
	if (access(psqlrc_minor, R_OK) == 0)
		(void) process_file(psqlrc_minor, false);
	else if (access(psqlrc_major, R_OK) == 0)
		(void) process_file(psqlrc_major, false);
	else if (access(filename, R_OK) == 0)
		(void) process_file(filename, false);

	free(psqlrc_minor);
	free(psqlrc_major);
}



/* showVersion
 *
 * This output format is intended to match GNU standards.
 */
static void
showVersion(void)
{
	puts("psql (PostgreSQL) " PG_VERSION);
}



/*
 * Assign hooks for psql variables.
 *
 * This isn't an amazingly good place for them, but neither is anywhere else.
 */

static void
autocommit_hook(const char *newval)
{
	pset.autocommit = ParseVariableBool(newval, "AUTOCOMMIT");
}

static void
on_error_stop_hook(const char *newval)
{
	pset.on_error_stop = ParseVariableBool(newval, "ON_ERROR_STOP");
}

static void
quiet_hook(const char *newval)
{
	pset.quiet = ParseVariableBool(newval, "QUIET");
}

static void
singleline_hook(const char *newval)
{
	pset.singleline = ParseVariableBool(newval, "SINGLELINE");
}

static void
singlestep_hook(const char *newval)
{
	pset.singlestep = ParseVariableBool(newval, "SINGLESTEP");
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
	else if (pg_strcasecmp(newval, "queries") == 0)
		pset.echo = PSQL_ECHO_QUERIES;
	else if (pg_strcasecmp(newval, "errors") == 0)
		pset.echo = PSQL_ECHO_ERRORS;
	else if (pg_strcasecmp(newval, "all") == 0)
		pset.echo = PSQL_ECHO_ALL;
	else if (pg_strcasecmp(newval, "none") == 0)
		pset.echo = PSQL_ECHO_NONE;
	else
	{
		psql_error("unrecognized value \"%s\" for \"%s\"; assuming \"%s\"\n",
				   newval, "ECHO", "none");
		pset.echo = PSQL_ECHO_NONE;
	}
}

static void
echo_hidden_hook(const char *newval)
{
	if (newval == NULL)
		pset.echo_hidden = PSQL_ECHO_HIDDEN_OFF;
	else if (pg_strcasecmp(newval, "noexec") == 0)
		pset.echo_hidden = PSQL_ECHO_HIDDEN_NOEXEC;
	else if (ParseVariableBool(newval, "ECHO_HIDDEN"))
		pset.echo_hidden = PSQL_ECHO_HIDDEN_ON;
	else	/* ParseVariableBool printed msg if needed */
		pset.echo_hidden = PSQL_ECHO_HIDDEN_OFF;
}

static void
on_error_rollback_hook(const char *newval)
{
	if (newval == NULL)
		pset.on_error_rollback = PSQL_ERROR_ROLLBACK_OFF;
	else if (pg_strcasecmp(newval, "interactive") == 0)
		pset.on_error_rollback = PSQL_ERROR_ROLLBACK_INTERACTIVE;
	else if (ParseVariableBool(newval, "ON_ERROR_ROLLBACK"))
		pset.on_error_rollback = PSQL_ERROR_ROLLBACK_ON;
	else	/* ParseVariableBool printed msg if needed */
		pset.on_error_rollback = PSQL_ERROR_ROLLBACK_OFF;
}

static void
comp_keyword_case_hook(const char *newval)
{
	if (newval == NULL)
		pset.comp_case = PSQL_COMP_CASE_PRESERVE_UPPER;
	else if (pg_strcasecmp(newval, "preserve-upper") == 0)
		pset.comp_case = PSQL_COMP_CASE_PRESERVE_UPPER;
	else if (pg_strcasecmp(newval, "preserve-lower") == 0)
		pset.comp_case = PSQL_COMP_CASE_PRESERVE_LOWER;
	else if (pg_strcasecmp(newval, "upper") == 0)
		pset.comp_case = PSQL_COMP_CASE_UPPER;
	else if (pg_strcasecmp(newval, "lower") == 0)
		pset.comp_case = PSQL_COMP_CASE_LOWER;
	else
	{
		psql_error("unrecognized value \"%s\" for \"%s\"; assuming \"%s\"\n",
				   newval, "COMP_KEYWORD_CASE", "preserve-upper");
		pset.comp_case = PSQL_COMP_CASE_PRESERVE_UPPER;
	}
}

static void
histcontrol_hook(const char *newval)
{
	if (newval == NULL)
		pset.histcontrol = hctl_none;
	else if (pg_strcasecmp(newval, "ignorespace") == 0)
		pset.histcontrol = hctl_ignorespace;
	else if (pg_strcasecmp(newval, "ignoredups") == 0)
		pset.histcontrol = hctl_ignoredups;
	else if (pg_strcasecmp(newval, "ignoreboth") == 0)
		pset.histcontrol = hctl_ignoreboth;
	else if (pg_strcasecmp(newval, "none") == 0)
		pset.histcontrol = hctl_none;
	else
	{
		psql_error("unrecognized value \"%s\" for \"%s\"; assuming \"%s\"\n",
				   newval, "HISTCONTROL", "none");
		pset.histcontrol = hctl_none;
	}
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
	else if (pg_strcasecmp(newval, "default") == 0)
		pset.verbosity = PQERRORS_DEFAULT;
	else if (pg_strcasecmp(newval, "terse") == 0)
		pset.verbosity = PQERRORS_TERSE;
	else if (pg_strcasecmp(newval, "verbose") == 0)
		pset.verbosity = PQERRORS_VERBOSE;
	else
	{
		psql_error("unrecognized value \"%s\" for \"%s\"; assuming \"%s\"\n",
				   newval, "VERBOSITY", "default");
		pset.verbosity = PQERRORS_DEFAULT;
	}

	if (pset.db)
		PQsetErrorVerbosity(pset.db, pset.verbosity);
}

static void
show_context_hook(const char *newval)
{
	if (newval == NULL)
		pset.show_context = PQSHOW_CONTEXT_ERRORS;
	else if (pg_strcasecmp(newval, "never") == 0)
		pset.show_context = PQSHOW_CONTEXT_NEVER;
	else if (pg_strcasecmp(newval, "errors") == 0)
		pset.show_context = PQSHOW_CONTEXT_ERRORS;
	else if (pg_strcasecmp(newval, "always") == 0)
		pset.show_context = PQSHOW_CONTEXT_ALWAYS;
	else
	{
		psql_error("unrecognized value \"%s\" for \"%s\"; assuming \"%s\"\n",
				   newval, "SHOW_CONTEXT", "errors");
		pset.show_context = PQSHOW_CONTEXT_ERRORS;
	}

	if (pset.db)
		PQsetErrorContextVisibility(pset.db, pset.show_context);
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
	SetVariableAssignHook(pset.vars, "COMP_KEYWORD_CASE", comp_keyword_case_hook);
	SetVariableAssignHook(pset.vars, "HISTCONTROL", histcontrol_hook);
	SetVariableAssignHook(pset.vars, "PROMPT1", prompt1_hook);
	SetVariableAssignHook(pset.vars, "PROMPT2", prompt2_hook);
	SetVariableAssignHook(pset.vars, "PROMPT3", prompt3_hook);
	SetVariableAssignHook(pset.vars, "VERBOSITY", verbosity_hook);
	SetVariableAssignHook(pset.vars, "SHOW_CONTEXT", show_context_hook);
}
