/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2024, PostgreSQL Global Development Group
 *
 * src/bin/psql/startup.c
 */
#include "postgres_fe.h"

#ifndef WIN32
#include <unistd.h>
#else							/* WIN32 */
#include <io.h>
#include <win32.h>
#endif							/* WIN32 */

#include "command.h"
#include "common.h"
#include "common/logging.h"
#include "common/string.h"
#include "describe.h"
#include "fe_utils/print.h"
#include "getopt_long.h"
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
	ACT_SINGLE_QUERY,
	ACT_SINGLE_SLASH,
	ACT_FILE,
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
							   struct adhoc_opts *options);
static void simple_action_list_append(SimpleActionList *list,
									  enum _actions action, const char *val);
static void process_psqlrc(char *argv0);
static void process_psqlrc_file(char *filename);
static void showVersion(void);
static void EstablishVariableSpace(void);

#define NOPAGER		0

static void
log_pre_callback(void)
{
	if (pset.queryFout && pset.queryFout != stdout)
		fflush(pset.queryFout);
}

static void
log_locus_callback(const char **filename, uint64 *lineno)
{
	if (pset.inputfile)
	{
		*filename = pset.inputfile;
		*lineno = pset.lineno;
	}
	else
	{
		*filename = NULL;
		*lineno = 0;
	}
}

#ifndef WIN32
static void
empty_signal_handler(SIGNAL_ARGS)
{
}
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
	char	   *password = NULL;
	bool		new_pass;

	pg_logging_init(argv[0]);
	pg_logging_set_pre_callback(log_pre_callback);
	pg_logging_set_locus_callback(log_locus_callback);
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

	pset.progname = get_progname(argv[0]);

	pset.db = NULL;
	pset.dead_conn = NULL;
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

	pset.popt.topt.csvFieldSep[0] = DEFAULT_CSV_FIELD_SEP;
	pset.popt.topt.csvFieldSep[1] = '\0';

	pset.popt.topt.unicode_border_linestyle = UNICODE_LINESTYLE_SINGLE;
	pset.popt.topt.unicode_column_linestyle = UNICODE_LINESTYLE_SINGLE;
	pset.popt.topt.unicode_header_linestyle = UNICODE_LINESTYLE_SINGLE;

	refresh_utf8format(&(pset.popt.topt));

	/* We must get COLUMNS here before readline() sets it */
	pset.popt.topt.env_columns = getenv("COLUMNS") ? atoi(getenv("COLUMNS")) : 0;

	pset.notty = (!isatty(fileno(stdin)) || !isatty(fileno(stdout)));

	pset.getPassword = TRI_DEFAULT;

	EstablishVariableSpace();

	/* Create variables showing psql version number */
	SetVariable(pset.vars, "VERSION", PG_VERSION_STR);
	SetVariable(pset.vars, "VERSION_NAME", PG_VERSION);
	SetVariable(pset.vars, "VERSION_NUM", CppAsString2(PG_VERSION_NUM));

	/* Initialize variables for last error */
	SetVariable(pset.vars, "LAST_ERROR_MESSAGE", "");
	SetVariable(pset.vars, "LAST_ERROR_SQLSTATE", "00000");

	/* Default values for variables (that don't match the result of \unset) */
	SetVariableBool(pset.vars, "AUTOCOMMIT");
	SetVariable(pset.vars, "PROMPT1", DEFAULT_PROMPT1);
	SetVariable(pset.vars, "PROMPT2", DEFAULT_PROMPT2);
	SetVariable(pset.vars, "PROMPT3", DEFAULT_PROMPT3);
	SetVariableBool(pset.vars, "SHOW_ALL_RESULTS");

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
		pg_fatal("-1 can only be used in non-interactive mode");

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

	if (pset.getPassword == TRI_YES)
	{
		/*
		 * We can't be sure yet of the username that will be used, so don't
		 * offer a potentially wrong one.  Typical uses of this option are
		 * noninteractive anyway.  (Note: since we've not yet set up our
		 * cancel handler, there's no need to use simple_prompt_extended.)
		 */
		password = simple_prompt("Password: ", false);
	}

	/* loop until we have a password if requested by backend */
	do
	{
#define PARAMS_ARRAY_SIZE	8
		const char **keywords = pg_malloc_array(const char *, PARAMS_ARRAY_SIZE);
		const char **values = pg_malloc_array(const char *, PARAMS_ARRAY_SIZE);

		keywords[0] = "host";
		values[0] = options.host;
		keywords[1] = "port";
		values[1] = options.port;
		keywords[2] = "user";
		values[2] = options.username;
		keywords[3] = "password";
		values[3] = password;
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
			!password &&
			pset.getPassword != TRI_NO)
		{
			/*
			 * Before closing the old PGconn, extract the user name that was
			 * actually connected with --- it might've come out of a URI or
			 * connstring "database name" rather than options.username.
			 */
			const char *realusername = PQuser(pset.db);
			char	   *password_prompt;

			if (realusername && realusername[0])
				password_prompt = psprintf(_("Password for user %s: "),
										   realusername);
			else
				password_prompt = pg_strdup(_("Password: "));
			PQfinish(pset.db);

			password = simple_prompt(password_prompt, false);
			free(password_prompt);
			new_pass = true;
		}
	} while (new_pass);

	if (PQstatus(pset.db) == CONNECTION_BAD)
	{
		pg_log_error("%s", PQerrorMessage(pset.db));
		PQfinish(pset.db);
		exit(EXIT_BADCONN);
	}

	psql_setup_cancel_handler();

#ifndef WIN32

	/*
	 * do_watch() needs signal handlers installed (otherwise sigwait() will
	 * filter them out on some platforms), but doesn't need them to do
	 * anything, and they shouldn't ever run (unless perhaps a stray SIGALRM
	 * arrives due to a race when do_watch() cancels an itimer).
	 */
	pqsignal(SIGCHLD, empty_signal_handler);
	pqsignal(SIGALRM, empty_signal_handler);
#endif

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
			pg_fatal("could not open log file \"%s\": %m",
					 options.logfilename);
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
				pg_logging_config(PG_LOG_FLAG_TERSE);

				if (pset.echo == PSQL_ECHO_ALL)
					puts(cell->val);

				successResult = SendQuery(cell->val)
					? EXIT_SUCCESS : EXIT_FAILURE;
			}
			else if (cell->action == ACT_SINGLE_SLASH)
			{
				PsqlScanState scan_state;
				ConditionalStack cond_stack;

				pg_logging_config(PG_LOG_FLAG_TERSE);

				if (pset.echo == PSQL_ECHO_ALL)
					puts(cell->val);

				scan_state = psql_scan_create(&psqlscan_callbacks);
				psql_scan_setup(scan_state,
								cell->val, strlen(cell->val),
								pset.encoding, standard_strings());
				cond_stack = conditional_stack_create();
				psql_scan_set_passthrough(scan_state, (void *) cond_stack);

				successResult = HandleSlashCmds(scan_state,
												cond_stack,
												NULL,
												NULL) != PSQL_CMD_ERROR
					? EXIT_SUCCESS : EXIT_FAILURE;

				psql_scan_destroy(scan_state);
				conditional_stack_destroy(cond_stack);
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
			/*
			 * Rollback the contents of the single transaction if the caller
			 * has set ON_ERROR_STOP and one of the steps has failed.  This
			 * check needs to match the one done a couple of lines above.
			 */
			res = PSQLexec((successResult != EXIT_SUCCESS && pset.on_error_stop) ?
						   "ROLLBACK" : "COMMIT");
			if (res == NULL)
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
		pg_logging_config(PG_LOG_FLAG_TERSE);
		connection_warnings(true);
		if (!pset.quiet)
			printf(_("Type \"help\" for help.\n\n"));
		initializeInput(options.no_readline ? 0 : 1);
		successResult = MainLoop(stdin);
	}

	/* clean up */
	if (pset.logfile)
		fclose(pset.logfile);
	if (pset.db)
		PQfinish(pset.db);
	if (pset.dead_conn)
		PQfinish(pset.dead_conn);
	setQFout(NULL);

	return successResult;
}


/*
 * Parse command line options
 */

static void
parse_psql_options(int argc, char *argv[], struct adhoc_opts *options)
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
		{"csv", no_argument, NULL, 2},
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
						pg_fatal("could not set printing parameter \"%s\"", value);

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
							exit(EXIT_FAILURE); /* error already printed */
					}
					else
					{
						*equal_loc = '\0';
						if (!SetVariable(pset.vars, value, equal_loc + 1))
							exit(EXIT_FAILURE); /* error already printed */
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
				if (optind <= argc &&
					strcmp(argv[optind - 1], "-?") == 0)
				{
					/* actual help option given */
					usage(NOPAGER);
					exit(EXIT_SUCCESS);
				}
				else
				{
					/* getopt error (unknown option or missing argument) */
					goto unknown_option;
				}
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
			case 2:
				pset.popt.topt.format = PRINT_CSV;
				break;
			default:
		unknown_option:
				/* getopt_long already emitted a complaint */
				pg_log_error_hint("Try \"%s --help\" for more information.",
								  pset.progname);
				exit(EXIT_FAILURE);
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
			pg_log_warning("extra command-line argument \"%s\" ignored",
						   argv[optind]);

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

	cell = pg_malloc_object(SimpleActionListCell);

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
		pg_fatal("could not find own program executable");

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
 * Substitute hooks and assign hooks for psql variables.
 *
 * This isn't an amazingly good place for them, but neither is anywhere else.
 *
 * By policy, every special variable that controls any psql behavior should
 * have one or both hooks, even if they're just no-ops.  This ensures that
 * the variable will remain present in variables.c's list even when unset,
 * which ensures that it's known to tab completion.
 */

static char *
bool_substitute_hook(char *newval)
{
	if (newval == NULL)
	{
		/* "\unset FOO" becomes "\set FOO off" */
		newval = pg_strdup("off");
	}
	else if (newval[0] == '\0')
	{
		/* "\set FOO" becomes "\set FOO on" */
		pg_free(newval);
		newval = pg_strdup("on");
	}
	return newval;
}

static bool
autocommit_hook(const char *newval)
{
	return ParseVariableBool(newval, "AUTOCOMMIT", &pset.autocommit);
}

static bool
on_error_stop_hook(const char *newval)
{
	return ParseVariableBool(newval, "ON_ERROR_STOP", &pset.on_error_stop);
}

static bool
quiet_hook(const char *newval)
{
	return ParseVariableBool(newval, "QUIET", &pset.quiet);
}

static bool
singleline_hook(const char *newval)
{
	return ParseVariableBool(newval, "SINGLELINE", &pset.singleline);
}

static bool
singlestep_hook(const char *newval)
{
	return ParseVariableBool(newval, "SINGLESTEP", &pset.singlestep);
}

static char *
fetch_count_substitute_hook(char *newval)
{
	if (newval == NULL)
		newval = pg_strdup("0");
	return newval;
}

static bool
fetch_count_hook(const char *newval)
{
	return ParseVariableNum(newval, "FETCH_COUNT", &pset.fetch_count);
}

static bool
histfile_hook(const char *newval)
{
	/*
	 * Someday we might try to validate the filename, but for now, this is
	 * just a placeholder to ensure HISTFILE is known to tab completion.
	 */
	return true;
}

static char *
histsize_substitute_hook(char *newval)
{
	if (newval == NULL)
		newval = pg_strdup("500");
	return newval;
}

static bool
histsize_hook(const char *newval)
{
	return ParseVariableNum(newval, "HISTSIZE", &pset.histsize);
}

static char *
ignoreeof_substitute_hook(char *newval)
{
	int			dummy;

	/*
	 * This tries to mimic the behavior of bash, to wit "If set, the value is
	 * the number of consecutive EOF characters which must be typed as the
	 * first characters on an input line before bash exits.  If the variable
	 * exists but does not have a numeric value, or has no value, the default
	 * value is 10.  If it does not exist, EOF signifies the end of input to
	 * the shell."  Unlike bash, however, we insist on the stored value
	 * actually being a valid integer.
	 */
	if (newval == NULL)
		newval = pg_strdup("0");
	else if (!ParseVariableNum(newval, NULL, &dummy))
		newval = pg_strdup("10");
	return newval;
}

static bool
ignoreeof_hook(const char *newval)
{
	return ParseVariableNum(newval, "IGNOREEOF", &pset.ignoreeof);
}

static char *
echo_substitute_hook(char *newval)
{
	if (newval == NULL)
		newval = pg_strdup("none");
	return newval;
}

static bool
echo_hook(const char *newval)
{
	Assert(newval != NULL);		/* else substitute hook messed up */
	if (pg_strcasecmp(newval, "queries") == 0)
		pset.echo = PSQL_ECHO_QUERIES;
	else if (pg_strcasecmp(newval, "errors") == 0)
		pset.echo = PSQL_ECHO_ERRORS;
	else if (pg_strcasecmp(newval, "all") == 0)
		pset.echo = PSQL_ECHO_ALL;
	else if (pg_strcasecmp(newval, "none") == 0)
		pset.echo = PSQL_ECHO_NONE;
	else
	{
		PsqlVarEnumError("ECHO", newval, "none, errors, queries, all");
		return false;
	}
	return true;
}

static bool
echo_hidden_hook(const char *newval)
{
	Assert(newval != NULL);		/* else substitute hook messed up */
	if (pg_strcasecmp(newval, "noexec") == 0)
		pset.echo_hidden = PSQL_ECHO_HIDDEN_NOEXEC;
	else
	{
		bool		on_off;

		if (ParseVariableBool(newval, NULL, &on_off))
			pset.echo_hidden = on_off ? PSQL_ECHO_HIDDEN_ON : PSQL_ECHO_HIDDEN_OFF;
		else
		{
			PsqlVarEnumError("ECHO_HIDDEN", newval, "on, off, noexec");
			return false;
		}
	}
	return true;
}

static bool
on_error_rollback_hook(const char *newval)
{
	Assert(newval != NULL);		/* else substitute hook messed up */
	if (pg_strcasecmp(newval, "interactive") == 0)
		pset.on_error_rollback = PSQL_ERROR_ROLLBACK_INTERACTIVE;
	else
	{
		bool		on_off;

		if (ParseVariableBool(newval, NULL, &on_off))
			pset.on_error_rollback = on_off ? PSQL_ERROR_ROLLBACK_ON : PSQL_ERROR_ROLLBACK_OFF;
		else
		{
			PsqlVarEnumError("ON_ERROR_ROLLBACK", newval, "on, off, interactive");
			return false;
		}
	}
	return true;
}

static char *
comp_keyword_case_substitute_hook(char *newval)
{
	if (newval == NULL)
		newval = pg_strdup("preserve-upper");
	return newval;
}

static bool
comp_keyword_case_hook(const char *newval)
{
	Assert(newval != NULL);		/* else substitute hook messed up */
	if (pg_strcasecmp(newval, "preserve-upper") == 0)
		pset.comp_case = PSQL_COMP_CASE_PRESERVE_UPPER;
	else if (pg_strcasecmp(newval, "preserve-lower") == 0)
		pset.comp_case = PSQL_COMP_CASE_PRESERVE_LOWER;
	else if (pg_strcasecmp(newval, "upper") == 0)
		pset.comp_case = PSQL_COMP_CASE_UPPER;
	else if (pg_strcasecmp(newval, "lower") == 0)
		pset.comp_case = PSQL_COMP_CASE_LOWER;
	else
	{
		PsqlVarEnumError("COMP_KEYWORD_CASE", newval,
						 "lower, upper, preserve-lower, preserve-upper");
		return false;
	}
	return true;
}

static char *
histcontrol_substitute_hook(char *newval)
{
	if (newval == NULL)
		newval = pg_strdup("none");
	return newval;
}

static bool
histcontrol_hook(const char *newval)
{
	Assert(newval != NULL);		/* else substitute hook messed up */
	if (pg_strcasecmp(newval, "ignorespace") == 0)
		pset.histcontrol = hctl_ignorespace;
	else if (pg_strcasecmp(newval, "ignoredups") == 0)
		pset.histcontrol = hctl_ignoredups;
	else if (pg_strcasecmp(newval, "ignoreboth") == 0)
		pset.histcontrol = hctl_ignoreboth;
	else if (pg_strcasecmp(newval, "none") == 0)
		pset.histcontrol = hctl_none;
	else
	{
		PsqlVarEnumError("HISTCONTROL", newval,
						 "none, ignorespace, ignoredups, ignoreboth");
		return false;
	}
	return true;
}

static bool
prompt1_hook(const char *newval)
{
	pset.prompt1 = newval ? newval : "";
	return true;
}

static bool
prompt2_hook(const char *newval)
{
	pset.prompt2 = newval ? newval : "";
	return true;
}

static bool
prompt3_hook(const char *newval)
{
	pset.prompt3 = newval ? newval : "";
	return true;
}

static char *
verbosity_substitute_hook(char *newval)
{
	if (newval == NULL)
		newval = pg_strdup("default");
	return newval;
}

static bool
verbosity_hook(const char *newval)
{
	Assert(newval != NULL);		/* else substitute hook messed up */
	if (pg_strcasecmp(newval, "default") == 0)
		pset.verbosity = PQERRORS_DEFAULT;
	else if (pg_strcasecmp(newval, "verbose") == 0)
		pset.verbosity = PQERRORS_VERBOSE;
	else if (pg_strcasecmp(newval, "terse") == 0)
		pset.verbosity = PQERRORS_TERSE;
	else if (pg_strcasecmp(newval, "sqlstate") == 0)
		pset.verbosity = PQERRORS_SQLSTATE;
	else
	{
		PsqlVarEnumError("VERBOSITY", newval, "default, verbose, terse, sqlstate");
		return false;
	}

	if (pset.db)
		PQsetErrorVerbosity(pset.db, pset.verbosity);
	return true;
}

static bool
show_all_results_hook(const char *newval)
{
	return ParseVariableBool(newval, "SHOW_ALL_RESULTS", &pset.show_all_results);
}

static char *
show_context_substitute_hook(char *newval)
{
	if (newval == NULL)
		newval = pg_strdup("errors");
	return newval;
}

static bool
show_context_hook(const char *newval)
{
	Assert(newval != NULL);		/* else substitute hook messed up */
	if (pg_strcasecmp(newval, "never") == 0)
		pset.show_context = PQSHOW_CONTEXT_NEVER;
	else if (pg_strcasecmp(newval, "errors") == 0)
		pset.show_context = PQSHOW_CONTEXT_ERRORS;
	else if (pg_strcasecmp(newval, "always") == 0)
		pset.show_context = PQSHOW_CONTEXT_ALWAYS;
	else
	{
		PsqlVarEnumError("SHOW_CONTEXT", newval, "never, errors, always");
		return false;
	}

	if (pset.db)
		PQsetErrorContextVisibility(pset.db, pset.show_context);
	return true;
}

static bool
hide_compression_hook(const char *newval)
{
	return ParseVariableBool(newval, "HIDE_TOAST_COMPRESSION",
							 &pset.hide_compression);
}

static bool
hide_tableam_hook(const char *newval)
{
	return ParseVariableBool(newval, "HIDE_TABLEAM", &pset.hide_tableam);
}

static void
EstablishVariableSpace(void)
{
	pset.vars = CreateVariableSpace();

	SetVariableHooks(pset.vars, "AUTOCOMMIT",
					 bool_substitute_hook,
					 autocommit_hook);
	SetVariableHooks(pset.vars, "ON_ERROR_STOP",
					 bool_substitute_hook,
					 on_error_stop_hook);
	SetVariableHooks(pset.vars, "QUIET",
					 bool_substitute_hook,
					 quiet_hook);
	SetVariableHooks(pset.vars, "SINGLELINE",
					 bool_substitute_hook,
					 singleline_hook);
	SetVariableHooks(pset.vars, "SINGLESTEP",
					 bool_substitute_hook,
					 singlestep_hook);
	SetVariableHooks(pset.vars, "FETCH_COUNT",
					 fetch_count_substitute_hook,
					 fetch_count_hook);
	SetVariableHooks(pset.vars, "HISTFILE",
					 NULL,
					 histfile_hook);
	SetVariableHooks(pset.vars, "HISTSIZE",
					 histsize_substitute_hook,
					 histsize_hook);
	SetVariableHooks(pset.vars, "IGNOREEOF",
					 ignoreeof_substitute_hook,
					 ignoreeof_hook);
	SetVariableHooks(pset.vars, "ECHO",
					 echo_substitute_hook,
					 echo_hook);
	SetVariableHooks(pset.vars, "ECHO_HIDDEN",
					 bool_substitute_hook,
					 echo_hidden_hook);
	SetVariableHooks(pset.vars, "ON_ERROR_ROLLBACK",
					 bool_substitute_hook,
					 on_error_rollback_hook);
	SetVariableHooks(pset.vars, "COMP_KEYWORD_CASE",
					 comp_keyword_case_substitute_hook,
					 comp_keyword_case_hook);
	SetVariableHooks(pset.vars, "HISTCONTROL",
					 histcontrol_substitute_hook,
					 histcontrol_hook);
	SetVariableHooks(pset.vars, "PROMPT1",
					 NULL,
					 prompt1_hook);
	SetVariableHooks(pset.vars, "PROMPT2",
					 NULL,
					 prompt2_hook);
	SetVariableHooks(pset.vars, "PROMPT3",
					 NULL,
					 prompt3_hook);
	SetVariableHooks(pset.vars, "VERBOSITY",
					 verbosity_substitute_hook,
					 verbosity_hook);
	SetVariableHooks(pset.vars, "SHOW_ALL_RESULTS",
					 bool_substitute_hook,
					 show_all_results_hook);
	SetVariableHooks(pset.vars, "SHOW_CONTEXT",
					 show_context_substitute_hook,
					 show_context_hook);
	SetVariableHooks(pset.vars, "HIDE_TOAST_COMPRESSION",
					 bool_substitute_hook,
					 hide_compression_hook);
	SetVariableHooks(pset.vars, "HIDE_TABLEAM",
					 bool_substitute_hook,
					 hide_tableam_hook);
}
