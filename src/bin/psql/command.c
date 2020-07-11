/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2020, PostgreSQL Global Development Group
 *
 * src/bin/psql/command.c
 */
#include "postgres_fe.h"

#include <ctype.h>
#include <time.h>
#include <pwd.h>
#ifndef WIN32
#include <sys/stat.h>			/* for stat() */
#include <fcntl.h>				/* open() flags */
#include <unistd.h>				/* for geteuid(), getpid(), stat() */
#else
#include <win32.h>
#include <io.h>
#include <fcntl.h>
#include <direct.h>
#include <sys/stat.h>			/* for stat() */
#endif

#include "catalog/pg_class_d.h"
#include "command.h"
#include "common.h"
#include "common/logging.h"
#include "copy.h"
#include "crosstabview.h"
#include "describe.h"
#include "fe_utils/cancel.h"
#include "fe_utils/print.h"
#include "fe_utils/string_utils.h"
#include "help.h"
#include "input.h"
#include "large_obj.h"
#include "libpq-fe.h"
#include "mainloop.h"
#include "portability/instr_time.h"
#include "pqexpbuffer.h"
#include "psqlscanslash.h"
#include "settings.h"
#include "variables.h"

/*
 * Editable database object types.
 */
typedef enum EditableObjectType
{
	EditableFunction,
	EditableView
} EditableObjectType;

/* local function declarations */
static backslashResult exec_command(const char *cmd,
									PsqlScanState scan_state,
									ConditionalStack cstack,
									PQExpBuffer query_buf,
									PQExpBuffer previous_buf);
static backslashResult exec_command_a(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_C(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_connect(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_cd(PsqlScanState scan_state, bool active_branch,
									   const char *cmd);
static backslashResult exec_command_conninfo(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_copy(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_copyright(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_crosstabview(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_d(PsqlScanState scan_state, bool active_branch,
									  const char *cmd);
static backslashResult exec_command_edit(PsqlScanState scan_state, bool active_branch,
										 PQExpBuffer query_buf, PQExpBuffer previous_buf);
static backslashResult exec_command_ef_ev(PsqlScanState scan_state, bool active_branch,
										  PQExpBuffer query_buf, bool is_func);
static backslashResult exec_command_echo(PsqlScanState scan_state, bool active_branch,
										 const char *cmd);
static backslashResult exec_command_elif(PsqlScanState scan_state, ConditionalStack cstack,
										 PQExpBuffer query_buf);
static backslashResult exec_command_else(PsqlScanState scan_state, ConditionalStack cstack,
										 PQExpBuffer query_buf);
static backslashResult exec_command_endif(PsqlScanState scan_state, ConditionalStack cstack,
										  PQExpBuffer query_buf);
static backslashResult exec_command_encoding(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_errverbose(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_f(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_g(PsqlScanState scan_state, bool active_branch,
									  const char *cmd);
static backslashResult process_command_g_options(char *first_option,
												 PsqlScanState scan_state,
												 bool active_branch,
												 const char *cmd);
static backslashResult exec_command_gdesc(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_gexec(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_gset(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_help(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_html(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_include(PsqlScanState scan_state, bool active_branch,
											const char *cmd);
static backslashResult exec_command_if(PsqlScanState scan_state, ConditionalStack cstack,
									   PQExpBuffer query_buf);
static backslashResult exec_command_list(PsqlScanState scan_state, bool active_branch,
										 const char *cmd);
static backslashResult exec_command_lo(PsqlScanState scan_state, bool active_branch,
									   const char *cmd);
static backslashResult exec_command_out(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_print(PsqlScanState scan_state, bool active_branch,
										  PQExpBuffer query_buf, PQExpBuffer previous_buf);
static backslashResult exec_command_password(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_prompt(PsqlScanState scan_state, bool active_branch,
										   const char *cmd);
static backslashResult exec_command_pset(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_quit(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_reset(PsqlScanState scan_state, bool active_branch,
										  PQExpBuffer query_buf);
static backslashResult exec_command_s(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_set(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_setenv(PsqlScanState scan_state, bool active_branch,
										   const char *cmd);
static backslashResult exec_command_sf_sv(PsqlScanState scan_state, bool active_branch,
										  const char *cmd, bool is_func);
static backslashResult exec_command_t(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_T(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_timing(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_unset(PsqlScanState scan_state, bool active_branch,
										  const char *cmd);
static backslashResult exec_command_write(PsqlScanState scan_state, bool active_branch,
										  const char *cmd,
										  PQExpBuffer query_buf, PQExpBuffer previous_buf);
static backslashResult exec_command_watch(PsqlScanState scan_state, bool active_branch,
										  PQExpBuffer query_buf, PQExpBuffer previous_buf);
static backslashResult exec_command_x(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_z(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_shell_escape(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_slash_command_help(PsqlScanState scan_state, bool active_branch);
static char *read_connect_arg(PsqlScanState scan_state);
static PQExpBuffer gather_boolean_expression(PsqlScanState scan_state);
static bool is_true_boolean_expression(PsqlScanState scan_state, const char *name);
static void ignore_boolean_expression(PsqlScanState scan_state);
static void ignore_slash_options(PsqlScanState scan_state);
static void ignore_slash_filepipe(PsqlScanState scan_state);
static void ignore_slash_whole_line(PsqlScanState scan_state);
static bool is_branching_command(const char *cmd);
static void save_query_text_state(PsqlScanState scan_state, ConditionalStack cstack,
								  PQExpBuffer query_buf);
static void discard_query_text(PsqlScanState scan_state, ConditionalStack cstack,
							   PQExpBuffer query_buf);
static void copy_previous_query(PQExpBuffer query_buf, PQExpBuffer previous_buf);
static bool do_connect(enum trivalue reuse_previous_specification,
					   char *dbname, char *user, char *host, char *port);
static bool do_edit(const char *filename_arg, PQExpBuffer query_buf,
					int lineno, bool *edited);
static bool do_shell(const char *command);
static bool do_watch(PQExpBuffer query_buf, double sleep);
static bool lookup_object_oid(EditableObjectType obj_type, const char *desc,
							  Oid *obj_oid);
static bool get_create_object_cmd(EditableObjectType obj_type, Oid oid,
								  PQExpBuffer buf);
static int	strip_lineno_from_objdesc(char *obj);
static int	count_lines_in_buf(PQExpBuffer buf);
static void print_with_linenumbers(FILE *output, char *lines,
								   const char *header_keyword);
static void minimal_error_message(PGresult *res);

static void printSSLInfo(void);
static void printGSSInfo(void);
static bool printPsetInfo(const char *param, printQueryOpt *popt);
static char *pset_value_string(const char *param, printQueryOpt *popt);

#ifdef WIN32
static void checkWin32Codepage(void);
#endif



/*----------
 * HandleSlashCmds:
 *
 * Handles all the different commands that start with '\'.
 * Ordinarily called by MainLoop().
 *
 * scan_state is a lexer working state that is set to continue scanning
 * just after the '\'.  The lexer is advanced past the command and all
 * arguments on return.
 *
 * cstack is the current \if stack state.  This will be examined, and
 * possibly modified by conditional commands.
 *
 * query_buf contains the query-so-far, which may be modified by
 * execution of the backslash command (for example, \r clears it).
 *
 * previous_buf contains the query most recently sent to the server
 * (empty if none yet).  This should not be modified here, but some
 * commands copy its content into query_buf.
 *
 * query_buf and previous_buf will be NULL when executing a "-c"
 * command-line option.
 *
 * Returns a status code indicating what action is desired, see command.h.
 *----------
 */

backslashResult
HandleSlashCmds(PsqlScanState scan_state,
				ConditionalStack cstack,
				PQExpBuffer query_buf,
				PQExpBuffer previous_buf)
{
	backslashResult status;
	char	   *cmd;
	char	   *arg;

	Assert(scan_state != NULL);
	Assert(cstack != NULL);

	/* Parse off the command name */
	cmd = psql_scan_slash_command(scan_state);

	/* And try to execute it */
	status = exec_command(cmd, scan_state, cstack, query_buf, previous_buf);

	if (status == PSQL_CMD_UNKNOWN)
	{
		pg_log_error("invalid command \\%s", cmd);
		if (pset.cur_cmd_interactive)
			pg_log_info("Try \\? for help.");
		status = PSQL_CMD_ERROR;
	}

	if (status != PSQL_CMD_ERROR)
	{
		/*
		 * Eat any remaining arguments after a valid command.  We want to
		 * suppress evaluation of backticks in this situation, so transiently
		 * push an inactive conditional-stack entry.
		 */
		bool		active_branch = conditional_active(cstack);

		conditional_stack_push(cstack, IFSTATE_IGNORED);
		while ((arg = psql_scan_slash_option(scan_state,
											 OT_NORMAL, NULL, false)))
		{
			if (active_branch)
				pg_log_warning("\\%s: extra argument \"%s\" ignored", cmd, arg);
			free(arg);
		}
		conditional_stack_pop(cstack);
	}
	else
	{
		/* silently throw away rest of line after an erroneous command */
		while ((arg = psql_scan_slash_option(scan_state,
											 OT_WHOLE_LINE, NULL, false)))
			free(arg);
	}

	/* if there is a trailing \\, swallow it */
	psql_scan_slash_command_end(scan_state);

	free(cmd);

	/* some commands write to queryFout, so make sure output is sent */
	fflush(pset.queryFout);

	return status;
}


/*
 * Subroutine to actually try to execute a backslash command.
 *
 * The typical "success" result code is PSQL_CMD_SKIP_LINE, although some
 * commands return something else.  Failure results are PSQL_CMD_ERROR,
 * unless PSQL_CMD_UNKNOWN is more appropriate.
 */
static backslashResult
exec_command(const char *cmd,
			 PsqlScanState scan_state,
			 ConditionalStack cstack,
			 PQExpBuffer query_buf,
			 PQExpBuffer previous_buf)
{
	backslashResult status;
	bool		active_branch = conditional_active(cstack);

	/*
	 * In interactive mode, warn when we're ignoring a command within a false
	 * \if-branch.  But we continue on, so as to parse and discard the right
	 * amount of parameter text.  Each individual backslash command subroutine
	 * is responsible for doing nothing after discarding appropriate
	 * arguments, if !active_branch.
	 */
	if (pset.cur_cmd_interactive && !active_branch &&
		!is_branching_command(cmd))
	{
		pg_log_warning("\\%s command ignored; use \\endif or Ctrl-C to exit current \\if block",
					   cmd);
	}

	if (strcmp(cmd, "a") == 0)
		status = exec_command_a(scan_state, active_branch);
	else if (strcmp(cmd, "C") == 0)
		status = exec_command_C(scan_state, active_branch);
	else if (strcmp(cmd, "c") == 0 || strcmp(cmd, "connect") == 0)
		status = exec_command_connect(scan_state, active_branch);
	else if (strcmp(cmd, "cd") == 0)
		status = exec_command_cd(scan_state, active_branch, cmd);
	else if (strcmp(cmd, "conninfo") == 0)
		status = exec_command_conninfo(scan_state, active_branch);
	else if (pg_strcasecmp(cmd, "copy") == 0)
		status = exec_command_copy(scan_state, active_branch);
	else if (strcmp(cmd, "copyright") == 0)
		status = exec_command_copyright(scan_state, active_branch);
	else if (strcmp(cmd, "crosstabview") == 0)
		status = exec_command_crosstabview(scan_state, active_branch);
	else if (cmd[0] == 'd')
		status = exec_command_d(scan_state, active_branch, cmd);
	else if (strcmp(cmd, "e") == 0 || strcmp(cmd, "edit") == 0)
		status = exec_command_edit(scan_state, active_branch,
								   query_buf, previous_buf);
	else if (strcmp(cmd, "ef") == 0)
		status = exec_command_ef_ev(scan_state, active_branch, query_buf, true);
	else if (strcmp(cmd, "ev") == 0)
		status = exec_command_ef_ev(scan_state, active_branch, query_buf, false);
	else if (strcmp(cmd, "echo") == 0 || strcmp(cmd, "qecho") == 0 ||
			 strcmp(cmd, "warn") == 0)
		status = exec_command_echo(scan_state, active_branch, cmd);
	else if (strcmp(cmd, "elif") == 0)
		status = exec_command_elif(scan_state, cstack, query_buf);
	else if (strcmp(cmd, "else") == 0)
		status = exec_command_else(scan_state, cstack, query_buf);
	else if (strcmp(cmd, "endif") == 0)
		status = exec_command_endif(scan_state, cstack, query_buf);
	else if (strcmp(cmd, "encoding") == 0)
		status = exec_command_encoding(scan_state, active_branch);
	else if (strcmp(cmd, "errverbose") == 0)
		status = exec_command_errverbose(scan_state, active_branch);
	else if (strcmp(cmd, "f") == 0)
		status = exec_command_f(scan_state, active_branch);
	else if (strcmp(cmd, "g") == 0 || strcmp(cmd, "gx") == 0)
		status = exec_command_g(scan_state, active_branch, cmd);
	else if (strcmp(cmd, "gdesc") == 0)
		status = exec_command_gdesc(scan_state, active_branch);
	else if (strcmp(cmd, "gexec") == 0)
		status = exec_command_gexec(scan_state, active_branch);
	else if (strcmp(cmd, "gset") == 0)
		status = exec_command_gset(scan_state, active_branch);
	else if (strcmp(cmd, "h") == 0 || strcmp(cmd, "help") == 0)
		status = exec_command_help(scan_state, active_branch);
	else if (strcmp(cmd, "H") == 0 || strcmp(cmd, "html") == 0)
		status = exec_command_html(scan_state, active_branch);
	else if (strcmp(cmd, "i") == 0 || strcmp(cmd, "include") == 0 ||
			 strcmp(cmd, "ir") == 0 || strcmp(cmd, "include_relative") == 0)
		status = exec_command_include(scan_state, active_branch, cmd);
	else if (strcmp(cmd, "if") == 0)
		status = exec_command_if(scan_state, cstack, query_buf);
	else if (strcmp(cmd, "l") == 0 || strcmp(cmd, "list") == 0 ||
			 strcmp(cmd, "l+") == 0 || strcmp(cmd, "list+") == 0)
		status = exec_command_list(scan_state, active_branch, cmd);
	else if (strncmp(cmd, "lo_", 3) == 0)
		status = exec_command_lo(scan_state, active_branch, cmd);
	else if (strcmp(cmd, "o") == 0 || strcmp(cmd, "out") == 0)
		status = exec_command_out(scan_state, active_branch);
	else if (strcmp(cmd, "p") == 0 || strcmp(cmd, "print") == 0)
		status = exec_command_print(scan_state, active_branch,
									query_buf, previous_buf);
	else if (strcmp(cmd, "password") == 0)
		status = exec_command_password(scan_state, active_branch);
	else if (strcmp(cmd, "prompt") == 0)
		status = exec_command_prompt(scan_state, active_branch, cmd);
	else if (strcmp(cmd, "pset") == 0)
		status = exec_command_pset(scan_state, active_branch);
	else if (strcmp(cmd, "q") == 0 || strcmp(cmd, "quit") == 0)
		status = exec_command_quit(scan_state, active_branch);
	else if (strcmp(cmd, "r") == 0 || strcmp(cmd, "reset") == 0)
		status = exec_command_reset(scan_state, active_branch, query_buf);
	else if (strcmp(cmd, "s") == 0)
		status = exec_command_s(scan_state, active_branch);
	else if (strcmp(cmd, "set") == 0)
		status = exec_command_set(scan_state, active_branch);
	else if (strcmp(cmd, "setenv") == 0)
		status = exec_command_setenv(scan_state, active_branch, cmd);
	else if (strcmp(cmd, "sf") == 0 || strcmp(cmd, "sf+") == 0)
		status = exec_command_sf_sv(scan_state, active_branch, cmd, true);
	else if (strcmp(cmd, "sv") == 0 || strcmp(cmd, "sv+") == 0)
		status = exec_command_sf_sv(scan_state, active_branch, cmd, false);
	else if (strcmp(cmd, "t") == 0)
		status = exec_command_t(scan_state, active_branch);
	else if (strcmp(cmd, "T") == 0)
		status = exec_command_T(scan_state, active_branch);
	else if (strcmp(cmd, "timing") == 0)
		status = exec_command_timing(scan_state, active_branch);
	else if (strcmp(cmd, "unset") == 0)
		status = exec_command_unset(scan_state, active_branch, cmd);
	else if (strcmp(cmd, "w") == 0 || strcmp(cmd, "write") == 0)
		status = exec_command_write(scan_state, active_branch, cmd,
									query_buf, previous_buf);
	else if (strcmp(cmd, "watch") == 0)
		status = exec_command_watch(scan_state, active_branch,
									query_buf, previous_buf);
	else if (strcmp(cmd, "x") == 0)
		status = exec_command_x(scan_state, active_branch);
	else if (strcmp(cmd, "z") == 0)
		status = exec_command_z(scan_state, active_branch);
	else if (strcmp(cmd, "!") == 0)
		status = exec_command_shell_escape(scan_state, active_branch);
	else if (strcmp(cmd, "?") == 0)
		status = exec_command_slash_command_help(scan_state, active_branch);
	else
		status = PSQL_CMD_UNKNOWN;

	/*
	 * All the commands that return PSQL_CMD_SEND want to execute previous_buf
	 * if query_buf is empty.  For convenience we implement that here, not in
	 * the individual command subroutines.
	 */
	if (status == PSQL_CMD_SEND)
		copy_previous_query(query_buf, previous_buf);

	return status;
}


/*
 * \a -- toggle field alignment
 *
 * This makes little sense but we keep it around.
 */
static backslashResult
exec_command_a(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		if (pset.popt.topt.format != PRINT_ALIGNED)
			success = do_pset("format", "aligned", &pset.popt, pset.quiet);
		else
			success = do_pset("format", "unaligned", &pset.popt, pset.quiet);
	}

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \C -- override table title (formerly change HTML caption)
 */
static backslashResult
exec_command_C(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *opt = psql_scan_slash_option(scan_state,
												 OT_NORMAL, NULL, true);

		success = do_pset("title", opt, &pset.popt, pset.quiet);
		free(opt);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \c or \connect -- connect to database using the specified parameters.
 *
 * \c [-reuse-previous=BOOL] dbname user host port
 *
 * Specifying a parameter as '-' is equivalent to omitting it.  Examples:
 *
 * \c - - hst		Connect to current database on current port of
 *					host "hst" as current user.
 * \c - usr - prt	Connect to current database on port "prt" of current host
 *					as user "usr".
 * \c dbs			Connect to database "dbs" on current port of current host
 *					as current user.
 */
static backslashResult
exec_command_connect(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		static const char prefix[] = "-reuse-previous=";
		char	   *opt1,
				   *opt2,
				   *opt3,
				   *opt4;
		enum trivalue reuse_previous = TRI_DEFAULT;

		opt1 = read_connect_arg(scan_state);
		if (opt1 != NULL && strncmp(opt1, prefix, sizeof(prefix) - 1) == 0)
		{
			bool		on_off;

			success = ParseVariableBool(opt1 + sizeof(prefix) - 1,
										"-reuse-previous",
										&on_off);
			if (success)
			{
				reuse_previous = on_off ? TRI_YES : TRI_NO;
				free(opt1);
				opt1 = read_connect_arg(scan_state);
			}
		}

		if (success)			/* give up if reuse_previous was invalid */
		{
			opt2 = read_connect_arg(scan_state);
			opt3 = read_connect_arg(scan_state);
			opt4 = read_connect_arg(scan_state);

			success = do_connect(reuse_previous, opt1, opt2, opt3, opt4);

			free(opt2);
			free(opt3);
			free(opt4);
		}
		free(opt1);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \cd -- change directory
 */
static backslashResult
exec_command_cd(PsqlScanState scan_state, bool active_branch, const char *cmd)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *opt = psql_scan_slash_option(scan_state,
												 OT_NORMAL, NULL, true);
		char	   *dir;

		if (opt)
			dir = opt;
		else
		{
#ifndef WIN32
			struct passwd *pw;
			uid_t		user_id = geteuid();

			errno = 0;			/* clear errno before call */
			pw = getpwuid(user_id);
			if (!pw)
			{
				pg_log_error("could not get home directory for user ID %ld: %s",
							 (long) user_id,
							 errno ? strerror(errno) : _("user does not exist"));
				exit(EXIT_FAILURE);
			}
			dir = pw->pw_dir;
#else							/* WIN32 */

			/*
			 * On Windows, 'cd' without arguments prints the current
			 * directory, so if someone wants to code this here instead...
			 */
			dir = "/";
#endif							/* WIN32 */
		}

		if (chdir(dir) == -1)
		{
			pg_log_error("\\%s: could not change directory to \"%s\": %m",
						 cmd, dir);
			success = false;
		}

		if (opt)
			free(opt);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \conninfo -- display information about the current connection
 */
static backslashResult
exec_command_conninfo(PsqlScanState scan_state, bool active_branch)
{
	if (active_branch)
	{
		char	   *db = PQdb(pset.db);

		if (db == NULL)
			printf(_("You are currently not connected to a database.\n"));
		else
		{
			char	   *host = PQhost(pset.db);
			char	   *hostaddr = PQhostaddr(pset.db);

			/*
			 * If the host is an absolute path, the connection is via socket
			 * unless overridden by hostaddr
			 */
			if (is_absolute_path(host))
			{
				if (hostaddr && *hostaddr)
					printf(_("You are connected to database \"%s\" as user \"%s\" on address \"%s\" at port \"%s\".\n"),
						   db, PQuser(pset.db), hostaddr, PQport(pset.db));
				else
					printf(_("You are connected to database \"%s\" as user \"%s\" via socket in \"%s\" at port \"%s\".\n"),
						   db, PQuser(pset.db), host, PQport(pset.db));
			}
			else
			{
				if (hostaddr && *hostaddr && strcmp(host, hostaddr) != 0)
					printf(_("You are connected to database \"%s\" as user \"%s\" on host \"%s\" (address \"%s\") at port \"%s\".\n"),
						   db, PQuser(pset.db), host, hostaddr, PQport(pset.db));
				else
					printf(_("You are connected to database \"%s\" as user \"%s\" on host \"%s\" at port \"%s\".\n"),
						   db, PQuser(pset.db), host, PQport(pset.db));
			}
			printSSLInfo();
			printGSSInfo();
		}
	}

	return PSQL_CMD_SKIP_LINE;
}

/*
 * \copy -- run a COPY command
 */
static backslashResult
exec_command_copy(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *opt = psql_scan_slash_option(scan_state,
												 OT_WHOLE_LINE, NULL, false);

		success = do_copy(opt);
		free(opt);
	}
	else
		ignore_slash_whole_line(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \copyright -- print copyright notice
 */
static backslashResult
exec_command_copyright(PsqlScanState scan_state, bool active_branch)
{
	if (active_branch)
		print_copyright();

	return PSQL_CMD_SKIP_LINE;
}

/*
 * \crosstabview -- execute a query and display results in crosstab
 */
static backslashResult
exec_command_crosstabview(PsqlScanState scan_state, bool active_branch)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;

	if (active_branch)
	{
		int			i;

		for (i = 0; i < lengthof(pset.ctv_args); i++)
			pset.ctv_args[i] = psql_scan_slash_option(scan_state,
													  OT_NORMAL, NULL, true);
		pset.crosstab_flag = true;
		status = PSQL_CMD_SEND;
	}
	else
		ignore_slash_options(scan_state);

	return status;
}

/*
 * \d* commands
 */
static backslashResult
exec_command_d(PsqlScanState scan_state, bool active_branch, const char *cmd)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;
	bool		success = true;

	if (active_branch)
	{
		char	   *pattern;
		bool		show_verbose,
					show_system;

		/* We don't do SQLID reduction on the pattern yet */
		pattern = psql_scan_slash_option(scan_state,
										 OT_NORMAL, NULL, true);

		show_verbose = strchr(cmd, '+') ? true : false;
		show_system = strchr(cmd, 'S') ? true : false;

		switch (cmd[1])
		{
			case '\0':
			case '+':
			case 'S':
				if (pattern)
					success = describeTableDetails(pattern, show_verbose, show_system);
				else
					/* standard listing of interesting things */
					success = listTables("tvmsE", NULL, show_verbose, show_system);
				break;
			case 'A':
				{
					char	   *pattern2 = NULL;

					if (pattern && cmd[2] != '\0' && cmd[2] != '+')
						pattern2 = psql_scan_slash_option(scan_state, OT_NORMAL, NULL, true);

					switch (cmd[2])
					{
						case '\0':
						case '+':
							success = describeAccessMethods(pattern, show_verbose);
							break;
						case 'c':
							success = listOperatorClasses(pattern, pattern2, show_verbose);
							break;
						case 'f':
							success = listOperatorFamilies(pattern, pattern2, show_verbose);
							break;
						case 'o':
							success = listOpFamilyOperators(pattern, pattern2, show_verbose);
							break;
						case 'p':
							success = listOpFamilyFunctions(pattern, pattern2, show_verbose);
							break;
						default:
							status = PSQL_CMD_UNKNOWN;
							break;
					}

					if (pattern2)
						free(pattern2);
				}
				break;
			case 'a':
				success = describeAggregates(pattern, show_verbose, show_system);
				break;
			case 'b':
				success = describeTablespaces(pattern, show_verbose);
				break;
			case 'c':
				success = listConversions(pattern, show_verbose, show_system);
				break;
			case 'C':
				success = listCasts(pattern, show_verbose);
				break;
			case 'd':
				if (strncmp(cmd, "ddp", 3) == 0)
					success = listDefaultACLs(pattern);
				else
					success = objectDescription(pattern, show_system);
				break;
			case 'D':
				success = listDomains(pattern, show_verbose, show_system);
				break;
			case 'f':			/* function subsystem */
				switch (cmd[2])
				{
					case '\0':
					case '+':
					case 'S':
					case 'a':
					case 'n':
					case 'p':
					case 't':
					case 'w':
						success = describeFunctions(&cmd[2], pattern, show_verbose, show_system);
						break;
					default:
						status = PSQL_CMD_UNKNOWN;
						break;
				}
				break;
			case 'g':
				/* no longer distinct from \du */
				success = describeRoles(pattern, show_verbose, show_system);
				break;
			case 'l':
				success = do_lo_list();
				break;
			case 'L':
				success = listLanguages(pattern, show_verbose, show_system);
				break;
			case 'n':
				success = listSchemas(pattern, show_verbose, show_system);
				break;
			case 'o':
				success = describeOperators(pattern, show_verbose, show_system);
				break;
			case 'O':
				success = listCollations(pattern, show_verbose, show_system);
				break;
			case 'p':
				success = permissionsList(pattern);
				break;
			case 'P':
				{
					switch (cmd[2])
					{
						case '\0':
						case '+':
						case 't':
						case 'i':
						case 'n':
							success = listPartitionedTables(&cmd[2], pattern, show_verbose);
							break;
						default:
							status = PSQL_CMD_UNKNOWN;
							break;
					}
				}
				break;
			case 'T':
				success = describeTypes(pattern, show_verbose, show_system);
				break;
			case 't':
			case 'v':
			case 'm':
			case 'i':
			case 's':
			case 'E':
				success = listTables(&cmd[1], pattern, show_verbose, show_system);
				break;
			case 'r':
				if (cmd[2] == 'd' && cmd[3] == 's')
				{
					char	   *pattern2 = NULL;

					if (pattern)
						pattern2 = psql_scan_slash_option(scan_state,
														  OT_NORMAL, NULL, true);
					success = listDbRoleSettings(pattern, pattern2);

					if (pattern2)
						free(pattern2);
				}
				else
					status = PSQL_CMD_UNKNOWN;
				break;
			case 'R':
				switch (cmd[2])
				{
					case 'p':
						if (show_verbose)
							success = describePublications(pattern);
						else
							success = listPublications(pattern);
						break;
					case 's':
						success = describeSubscriptions(pattern, show_verbose);
						break;
					default:
						status = PSQL_CMD_UNKNOWN;
				}
				break;
			case 'u':
				success = describeRoles(pattern, show_verbose, show_system);
				break;
			case 'F':			/* text search subsystem */
				switch (cmd[2])
				{
					case '\0':
					case '+':
						success = listTSConfigs(pattern, show_verbose);
						break;
					case 'p':
						success = listTSParsers(pattern, show_verbose);
						break;
					case 'd':
						success = listTSDictionaries(pattern, show_verbose);
						break;
					case 't':
						success = listTSTemplates(pattern, show_verbose);
						break;
					default:
						status = PSQL_CMD_UNKNOWN;
						break;
				}
				break;
			case 'e':			/* SQL/MED subsystem */
				switch (cmd[2])
				{
					case 's':
						success = listForeignServers(pattern, show_verbose);
						break;
					case 'u':
						success = listUserMappings(pattern, show_verbose);
						break;
					case 'w':
						success = listForeignDataWrappers(pattern, show_verbose);
						break;
					case 't':
						success = listForeignTables(pattern, show_verbose);
						break;
					default:
						status = PSQL_CMD_UNKNOWN;
						break;
				}
				break;
			case 'x':			/* Extensions */
				if (show_verbose)
					success = listExtensionContents(pattern);
				else
					success = listExtensions(pattern);
				break;
			case 'y':			/* Event Triggers */
				success = listEventTriggers(pattern, show_verbose);
				break;
			default:
				status = PSQL_CMD_UNKNOWN;
		}

		if (pattern)
			free(pattern);
	}
	else
		ignore_slash_options(scan_state);

	if (!success)
		status = PSQL_CMD_ERROR;

	return status;
}

/*
 * \e or \edit -- edit the current query buffer, or edit a file and
 * make it the query buffer
 */
static backslashResult
exec_command_edit(PsqlScanState scan_state, bool active_branch,
				  PQExpBuffer query_buf, PQExpBuffer previous_buf)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;

	if (active_branch)
	{
		if (!query_buf)
		{
			pg_log_error("no query buffer");
			status = PSQL_CMD_ERROR;
		}
		else
		{
			char	   *fname;
			char	   *ln = NULL;
			int			lineno = -1;

			fname = psql_scan_slash_option(scan_state,
										   OT_NORMAL, NULL, true);
			if (fname)
			{
				/* try to get separate lineno arg */
				ln = psql_scan_slash_option(scan_state,
											OT_NORMAL, NULL, true);
				if (ln == NULL)
				{
					/* only one arg; maybe it is lineno not fname */
					if (fname[0] &&
						strspn(fname, "0123456789") == strlen(fname))
					{
						/* all digits, so assume it is lineno */
						ln = fname;
						fname = NULL;
					}
				}
			}
			if (ln)
			{
				lineno = atoi(ln);
				if (lineno < 1)
				{
					pg_log_error("invalid line number: %s", ln);
					status = PSQL_CMD_ERROR;
				}
			}
			if (status != PSQL_CMD_ERROR)
			{
				expand_tilde(&fname);
				if (fname)
					canonicalize_path(fname);

				/* If query_buf is empty, recall previous query for editing */
				copy_previous_query(query_buf, previous_buf);

				if (do_edit(fname, query_buf, lineno, NULL))
					status = PSQL_CMD_NEWEDIT;
				else
					status = PSQL_CMD_ERROR;
			}
			if (fname)
				free(fname);
			if (ln)
				free(ln);
		}
	}
	else
		ignore_slash_options(scan_state);

	return status;
}

/*
 * \ef/\ev -- edit the named function/view, or
 * present a blank CREATE FUNCTION/VIEW template if no argument is given
 */
static backslashResult
exec_command_ef_ev(PsqlScanState scan_state, bool active_branch,
				   PQExpBuffer query_buf, bool is_func)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;

	if (active_branch)
	{
		char	   *obj_desc = psql_scan_slash_option(scan_state,
													  OT_WHOLE_LINE,
													  NULL, true);
		int			lineno = -1;

		if (pset.sversion < (is_func ? 80400 : 70400))
		{
			char		sverbuf[32];

			formatPGVersionNumber(pset.sversion, false,
								  sverbuf, sizeof(sverbuf));
			if (is_func)
				pg_log_error("The server (version %s) does not support editing function source.",
							 sverbuf);
			else
				pg_log_error("The server (version %s) does not support editing view definitions.",
							 sverbuf);
			status = PSQL_CMD_ERROR;
		}
		else if (!query_buf)
		{
			pg_log_error("no query buffer");
			status = PSQL_CMD_ERROR;
		}
		else
		{
			Oid			obj_oid = InvalidOid;
			EditableObjectType eot = is_func ? EditableFunction : EditableView;

			lineno = strip_lineno_from_objdesc(obj_desc);
			if (lineno == 0)
			{
				/* error already reported */
				status = PSQL_CMD_ERROR;
			}
			else if (!obj_desc)
			{
				/* set up an empty command to fill in */
				resetPQExpBuffer(query_buf);
				if (is_func)
					appendPQExpBufferStr(query_buf,
										 "CREATE FUNCTION ( )\n"
										 " RETURNS \n"
										 " LANGUAGE \n"
										 " -- common options:  IMMUTABLE  STABLE  STRICT  SECURITY DEFINER\n"
										 "AS $function$\n"
										 "\n$function$\n");
				else
					appendPQExpBufferStr(query_buf,
										 "CREATE VIEW  AS\n"
										 " SELECT \n"
										 "  -- something...\n");
			}
			else if (!lookup_object_oid(eot, obj_desc, &obj_oid))
			{
				/* error already reported */
				status = PSQL_CMD_ERROR;
			}
			else if (!get_create_object_cmd(eot, obj_oid, query_buf))
			{
				/* error already reported */
				status = PSQL_CMD_ERROR;
			}
			else if (is_func && lineno > 0)
			{
				/*
				 * lineno "1" should correspond to the first line of the
				 * function body.  We expect that pg_get_functiondef() will
				 * emit that on a line beginning with "AS ", and that there
				 * can be no such line before the real start of the function
				 * body.  Increment lineno by the number of lines before that
				 * line, so that it becomes relative to the first line of the
				 * function definition.
				 */
				const char *lines = query_buf->data;

				while (*lines != '\0')
				{
					if (strncmp(lines, "AS ", 3) == 0)
						break;
					lineno++;
					/* find start of next line */
					lines = strchr(lines, '\n');
					if (!lines)
						break;
					lines++;
				}
			}
		}

		if (status != PSQL_CMD_ERROR)
		{
			bool		edited = false;

			if (!do_edit(NULL, query_buf, lineno, &edited))
				status = PSQL_CMD_ERROR;
			else if (!edited)
				puts(_("No changes"));
			else
				status = PSQL_CMD_NEWEDIT;
		}

		if (obj_desc)
			free(obj_desc);
	}
	else
		ignore_slash_whole_line(scan_state);

	return status;
}

/*
 * \echo, \qecho, and \warn -- echo arguments to stdout, query output, or stderr
 */
static backslashResult
exec_command_echo(PsqlScanState scan_state, bool active_branch, const char *cmd)
{
	if (active_branch)
	{
		char	   *value;
		char		quoted;
		bool		no_newline = false;
		bool		first = true;
		FILE	   *fout;

		if (strcmp(cmd, "qecho") == 0)
			fout = pset.queryFout;
		else if (strcmp(cmd, "warn") == 0)
			fout = stderr;
		else
			fout = stdout;

		while ((value = psql_scan_slash_option(scan_state,
											   OT_NORMAL, &quoted, false)))
		{
			if (first && !no_newline && !quoted && strcmp(value, "-n") == 0)
				no_newline = true;
			else
			{
				if (first)
					first = false;
				else
					fputc(' ', fout);
				fputs(value, fout);
			}
			free(value);
		}
		if (!no_newline)
			fputs("\n", fout);
	}
	else
		ignore_slash_options(scan_state);

	return PSQL_CMD_SKIP_LINE;
}

/*
 * \encoding -- set/show client side encoding
 */
static backslashResult
exec_command_encoding(PsqlScanState scan_state, bool active_branch)
{
	if (active_branch)
	{
		char	   *encoding = psql_scan_slash_option(scan_state,
													  OT_NORMAL, NULL, false);

		if (!encoding)
		{
			/* show encoding */
			puts(pg_encoding_to_char(pset.encoding));
		}
		else
		{
			/* set encoding */
			if (PQsetClientEncoding(pset.db, encoding) == -1)
				pg_log_error("%s: invalid encoding name or conversion procedure not found", encoding);
			else
			{
				/* save encoding info into psql internal data */
				pset.encoding = PQclientEncoding(pset.db);
				pset.popt.topt.encoding = pset.encoding;
				SetVariable(pset.vars, "ENCODING",
							pg_encoding_to_char(pset.encoding));
			}
			free(encoding);
		}
	}
	else
		ignore_slash_options(scan_state);

	return PSQL_CMD_SKIP_LINE;
}

/*
 * \errverbose -- display verbose message from last failed query
 */
static backslashResult
exec_command_errverbose(PsqlScanState scan_state, bool active_branch)
{
	if (active_branch)
	{
		if (pset.last_error_result)
		{
			char	   *msg;

			msg = PQresultVerboseErrorMessage(pset.last_error_result,
											  PQERRORS_VERBOSE,
											  PQSHOW_CONTEXT_ALWAYS);
			if (msg)
			{
				pg_log_error("%s", msg);
				PQfreemem(msg);
			}
			else
				puts(_("out of memory"));
		}
		else
			puts(_("There is no previous error."));
	}

	return PSQL_CMD_SKIP_LINE;
}

/*
 * \f -- change field separator
 */
static backslashResult
exec_command_f(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *fname = psql_scan_slash_option(scan_state,
												   OT_NORMAL, NULL, false);

		success = do_pset("fieldsep", fname, &pset.popt, pset.quiet);
		free(fname);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \g  [(pset-option[=pset-value] ...)] [filename/shell-command]
 * \gx [(pset-option[=pset-value] ...)] [filename/shell-command]
 *
 * Send the current query.  If pset options are specified, they are made
 * active just for this query.  If a filename or pipe command is given,
 * the query output goes there.  \gx implicitly sets "expanded=on" along
 * with any other pset options that are specified.
 */
static backslashResult
exec_command_g(PsqlScanState scan_state, bool active_branch, const char *cmd)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;
	char	   *fname;

	/*
	 * Because the option processing for this is fairly complicated, we do it
	 * and then decide whether the branch is active.
	 */
	fname = psql_scan_slash_option(scan_state,
								   OT_FILEPIPE, NULL, false);

	if (fname && fname[0] == '(')
	{
		/* Consume pset options through trailing ')' ... */
		status = process_command_g_options(fname + 1, scan_state,
										   active_branch, cmd);
		free(fname);
		/* ... and again attempt to scan the filename. */
		fname = psql_scan_slash_option(scan_state,
									   OT_FILEPIPE, NULL, false);
	}

	if (status == PSQL_CMD_SKIP_LINE && active_branch)
	{
		if (!fname)
			pset.gfname = NULL;
		else
		{
			expand_tilde(&fname);
			pset.gfname = pg_strdup(fname);
		}
		if (strcmp(cmd, "gx") == 0)
		{
			/* save settings if not done already, then force expanded=on */
			if (pset.gsavepopt == NULL)
				pset.gsavepopt = savePsetInfo(&pset.popt);
			pset.popt.topt.expanded = 1;
		}
		status = PSQL_CMD_SEND;
	}

	free(fname);

	return status;
}

/*
 * Process parenthesized pset options for \g
 *
 * Note: okay to modify first_option, but not to free it; caller does that
 */
static backslashResult
process_command_g_options(char *first_option, PsqlScanState scan_state,
						  bool active_branch, const char *cmd)
{
	bool		success = true;
	bool		found_r_paren = false;

	do
	{
		char	   *option;
		size_t		optlen;

		/* If not first time through, collect a new option */
		if (first_option)
			option = first_option;
		else
		{
			option = psql_scan_slash_option(scan_state,
											OT_NORMAL, NULL, false);
			if (!option)
			{
				if (active_branch)
				{
					pg_log_error("\\%s: missing right parenthesis", cmd);
					success = false;
				}
				break;
			}
		}

		/* Check for terminating right paren, and remove it from string */
		optlen = strlen(option);
		if (optlen > 0 && option[optlen - 1] == ')')
		{
			option[--optlen] = '\0';
			found_r_paren = true;
		}

		/* If there was anything besides parentheses, parse/execute it */
		if (optlen > 0)
		{
			/* We can have either "name" or "name=value" */
			char	   *valptr = strchr(option, '=');

			if (valptr)
				*valptr++ = '\0';
			if (active_branch)
			{
				/* save settings if not done already, then apply option */
				if (pset.gsavepopt == NULL)
					pset.gsavepopt = savePsetInfo(&pset.popt);
				success &= do_pset(option, valptr, &pset.popt, true);
			}
		}

		/* Clean up after this option.  We should not free first_option. */
		if (first_option)
			first_option = NULL;
		else
			free(option);
	} while (!found_r_paren);

	/* If we failed after already changing some options, undo side-effects */
	if (!success && active_branch && pset.gsavepopt)
	{
		restorePsetInfo(&pset.popt, pset.gsavepopt);
		pset.gsavepopt = NULL;
	}

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \gdesc -- describe query result
 */
static backslashResult
exec_command_gdesc(PsqlScanState scan_state, bool active_branch)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;

	if (active_branch)
	{
		pset.gdesc_flag = true;
		status = PSQL_CMD_SEND;
	}

	return status;
}

/*
 * \gexec -- send query and execute each field of result
 */
static backslashResult
exec_command_gexec(PsqlScanState scan_state, bool active_branch)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;

	if (active_branch)
	{
		pset.gexec_flag = true;
		status = PSQL_CMD_SEND;
	}

	return status;
}

/*
 * \gset [prefix] -- send query and store result into variables
 */
static backslashResult
exec_command_gset(PsqlScanState scan_state, bool active_branch)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;

	if (active_branch)
	{
		char	   *prefix = psql_scan_slash_option(scan_state,
													OT_NORMAL, NULL, false);

		if (prefix)
			pset.gset_prefix = prefix;
		else
		{
			/* we must set a non-NULL prefix to trigger storing */
			pset.gset_prefix = pg_strdup("");
		}
		/* gset_prefix is freed later */
		status = PSQL_CMD_SEND;
	}
	else
		ignore_slash_options(scan_state);

	return status;
}

/*
 * \help [topic] -- print help about SQL commands
 */
static backslashResult
exec_command_help(PsqlScanState scan_state, bool active_branch)
{
	if (active_branch)
	{
		char	   *opt = psql_scan_slash_option(scan_state,
												 OT_WHOLE_LINE, NULL, false);
		size_t		len;

		/* strip any trailing spaces and semicolons */
		if (opt)
		{
			len = strlen(opt);
			while (len > 0 &&
				   (isspace((unsigned char) opt[len - 1])
					|| opt[len - 1] == ';'))
				opt[--len] = '\0';
		}

		helpSQL(opt, pset.popt.topt.pager);
		free(opt);
	}
	else
		ignore_slash_whole_line(scan_state);

	return PSQL_CMD_SKIP_LINE;
}

/*
 * \H and \html -- toggle HTML formatting
 */
static backslashResult
exec_command_html(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		if (pset.popt.topt.format != PRINT_HTML)
			success = do_pset("format", "html", &pset.popt, pset.quiet);
		else
			success = do_pset("format", "aligned", &pset.popt, pset.quiet);
	}

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \i and \ir -- include a file
 */
static backslashResult
exec_command_include(PsqlScanState scan_state, bool active_branch, const char *cmd)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *fname = psql_scan_slash_option(scan_state,
												   OT_NORMAL, NULL, true);

		if (!fname)
		{
			pg_log_error("\\%s: missing required argument", cmd);
			success = false;
		}
		else
		{
			bool		include_relative;

			include_relative = (strcmp(cmd, "ir") == 0
								|| strcmp(cmd, "include_relative") == 0);
			expand_tilde(&fname);
			success = (process_file(fname, include_relative) == EXIT_SUCCESS);
			free(fname);
		}
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \if <expr> -- beginning of an \if..\endif block
 *
 * <expr> is parsed as a boolean expression.  Invalid expressions will emit a
 * warning and be treated as false.  Statements that follow a false expression
 * will be parsed but ignored.  Note that in the case where an \if statement
 * is itself within an inactive section of a block, then the entire inner
 * \if..\endif block will be parsed but ignored.
 */
static backslashResult
exec_command_if(PsqlScanState scan_state, ConditionalStack cstack,
				PQExpBuffer query_buf)
{
	if (conditional_active(cstack))
	{
		/*
		 * First, push a new active stack entry; this ensures that the lexer
		 * will perform variable substitution and backtick evaluation while
		 * scanning the expression.  (That should happen anyway, since we know
		 * we're in an active outer branch, but let's be sure.)
		 */
		conditional_stack_push(cstack, IFSTATE_TRUE);

		/* Remember current query state in case we need to restore later */
		save_query_text_state(scan_state, cstack, query_buf);

		/*
		 * Evaluate the expression; if it's false, change to inactive state.
		 */
		if (!is_true_boolean_expression(scan_state, "\\if expression"))
			conditional_stack_poke(cstack, IFSTATE_FALSE);
	}
	else
	{
		/*
		 * We're within an inactive outer branch, so this entire \if block
		 * will be ignored.  We don't want to evaluate the expression, so push
		 * the "ignored" stack state before scanning it.
		 */
		conditional_stack_push(cstack, IFSTATE_IGNORED);

		/* Remember current query state in case we need to restore later */
		save_query_text_state(scan_state, cstack, query_buf);

		ignore_boolean_expression(scan_state);
	}

	return PSQL_CMD_SKIP_LINE;
}

/*
 * \elif <expr> -- alternative branch in an \if..\endif block
 *
 * <expr> is evaluated the same as in \if <expr>.
 */
static backslashResult
exec_command_elif(PsqlScanState scan_state, ConditionalStack cstack,
				  PQExpBuffer query_buf)
{
	bool		success = true;

	switch (conditional_stack_peek(cstack))
	{
		case IFSTATE_TRUE:

			/*
			 * Just finished active branch of this \if block.  Update saved
			 * state so we will keep whatever data was put in query_buf by the
			 * active branch.
			 */
			save_query_text_state(scan_state, cstack, query_buf);

			/*
			 * Discard \elif expression and ignore the rest until \endif.
			 * Switch state before reading expression to ensure proper lexer
			 * behavior.
			 */
			conditional_stack_poke(cstack, IFSTATE_IGNORED);
			ignore_boolean_expression(scan_state);
			break;
		case IFSTATE_FALSE:

			/*
			 * Discard any query text added by the just-skipped branch.
			 */
			discard_query_text(scan_state, cstack, query_buf);

			/*
			 * Have not yet found a true expression in this \if block, so this
			 * might be the first.  We have to change state before examining
			 * the expression, or the lexer won't do the right thing.
			 */
			conditional_stack_poke(cstack, IFSTATE_TRUE);
			if (!is_true_boolean_expression(scan_state, "\\elif expression"))
				conditional_stack_poke(cstack, IFSTATE_FALSE);
			break;
		case IFSTATE_IGNORED:

			/*
			 * Discard any query text added by the just-skipped branch.
			 */
			discard_query_text(scan_state, cstack, query_buf);

			/*
			 * Skip expression and move on.  Either the \if block already had
			 * an active section, or whole block is being skipped.
			 */
			ignore_boolean_expression(scan_state);
			break;
		case IFSTATE_ELSE_TRUE:
		case IFSTATE_ELSE_FALSE:
			pg_log_error("\\elif: cannot occur after \\else");
			success = false;
			break;
		case IFSTATE_NONE:
			/* no \if to elif from */
			pg_log_error("\\elif: no matching \\if");
			success = false;
			break;
	}

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \else -- final alternative in an \if..\endif block
 *
 * Statements within an \else branch will only be executed if
 * all previous \if and \elif expressions evaluated to false
 * and the block was not itself being ignored.
 */
static backslashResult
exec_command_else(PsqlScanState scan_state, ConditionalStack cstack,
				  PQExpBuffer query_buf)
{
	bool		success = true;

	switch (conditional_stack_peek(cstack))
	{
		case IFSTATE_TRUE:

			/*
			 * Just finished active branch of this \if block.  Update saved
			 * state so we will keep whatever data was put in query_buf by the
			 * active branch.
			 */
			save_query_text_state(scan_state, cstack, query_buf);

			/* Now skip the \else branch */
			conditional_stack_poke(cstack, IFSTATE_ELSE_FALSE);
			break;
		case IFSTATE_FALSE:

			/*
			 * Discard any query text added by the just-skipped branch.
			 */
			discard_query_text(scan_state, cstack, query_buf);

			/*
			 * We've not found any true \if or \elif expression, so execute
			 * the \else branch.
			 */
			conditional_stack_poke(cstack, IFSTATE_ELSE_TRUE);
			break;
		case IFSTATE_IGNORED:

			/*
			 * Discard any query text added by the just-skipped branch.
			 */
			discard_query_text(scan_state, cstack, query_buf);

			/*
			 * Either we previously processed the active branch of this \if,
			 * or the whole \if block is being skipped.  Either way, skip the
			 * \else branch.
			 */
			conditional_stack_poke(cstack, IFSTATE_ELSE_FALSE);
			break;
		case IFSTATE_ELSE_TRUE:
		case IFSTATE_ELSE_FALSE:
			pg_log_error("\\else: cannot occur after \\else");
			success = false;
			break;
		case IFSTATE_NONE:
			/* no \if to else from */
			pg_log_error("\\else: no matching \\if");
			success = false;
			break;
	}

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \endif -- ends an \if...\endif block
 */
static backslashResult
exec_command_endif(PsqlScanState scan_state, ConditionalStack cstack,
				   PQExpBuffer query_buf)
{
	bool		success = true;

	switch (conditional_stack_peek(cstack))
	{
		case IFSTATE_TRUE:
		case IFSTATE_ELSE_TRUE:
			/* Close the \if block, keeping the query text */
			success = conditional_stack_pop(cstack);
			Assert(success);
			break;
		case IFSTATE_FALSE:
		case IFSTATE_IGNORED:
		case IFSTATE_ELSE_FALSE:

			/*
			 * Discard any query text added by the just-skipped branch.
			 */
			discard_query_text(scan_state, cstack, query_buf);

			/* Close the \if block */
			success = conditional_stack_pop(cstack);
			Assert(success);
			break;
		case IFSTATE_NONE:
			/* no \if to end */
			pg_log_error("\\endif: no matching \\if");
			success = false;
			break;
	}

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \l -- list databases
 */
static backslashResult
exec_command_list(PsqlScanState scan_state, bool active_branch, const char *cmd)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *pattern;
		bool		show_verbose;

		pattern = psql_scan_slash_option(scan_state,
										 OT_NORMAL, NULL, true);

		show_verbose = strchr(cmd, '+') ? true : false;

		success = listAllDbs(pattern, show_verbose);

		if (pattern)
			free(pattern);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \lo_* -- large object operations
 */
static backslashResult
exec_command_lo(PsqlScanState scan_state, bool active_branch, const char *cmd)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;
	bool		success = true;

	if (active_branch)
	{
		char	   *opt1,
				   *opt2;

		opt1 = psql_scan_slash_option(scan_state,
									  OT_NORMAL, NULL, true);
		opt2 = psql_scan_slash_option(scan_state,
									  OT_NORMAL, NULL, true);

		if (strcmp(cmd + 3, "export") == 0)
		{
			if (!opt2)
			{
				pg_log_error("\\%s: missing required argument", cmd);
				success = false;
			}
			else
			{
				expand_tilde(&opt2);
				success = do_lo_export(opt1, opt2);
			}
		}

		else if (strcmp(cmd + 3, "import") == 0)
		{
			if (!opt1)
			{
				pg_log_error("\\%s: missing required argument", cmd);
				success = false;
			}
			else
			{
				expand_tilde(&opt1);
				success = do_lo_import(opt1, opt2);
			}
		}

		else if (strcmp(cmd + 3, "list") == 0)
			success = do_lo_list();

		else if (strcmp(cmd + 3, "unlink") == 0)
		{
			if (!opt1)
			{
				pg_log_error("\\%s: missing required argument", cmd);
				success = false;
			}
			else
				success = do_lo_unlink(opt1);
		}

		else
			status = PSQL_CMD_UNKNOWN;

		free(opt1);
		free(opt2);
	}
	else
		ignore_slash_options(scan_state);

	if (!success)
		status = PSQL_CMD_ERROR;

	return status;
}

/*
 * \o -- set query output
 */
static backslashResult
exec_command_out(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *fname = psql_scan_slash_option(scan_state,
												   OT_FILEPIPE, NULL, true);

		expand_tilde(&fname);
		success = setQFout(fname);
		free(fname);
	}
	else
		ignore_slash_filepipe(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \p -- print the current query buffer
 */
static backslashResult
exec_command_print(PsqlScanState scan_state, bool active_branch,
				   PQExpBuffer query_buf, PQExpBuffer previous_buf)
{
	if (active_branch)
	{
		/*
		 * We want to print the same thing \g would execute, but not to change
		 * the query buffer state; so we can't use copy_previous_query().
		 * Also, beware of possibility that buffer pointers are NULL.
		 */
		if (query_buf && query_buf->len > 0)
			puts(query_buf->data);
		else if (previous_buf && previous_buf->len > 0)
			puts(previous_buf->data);
		else if (!pset.quiet)
			puts(_("Query buffer is empty."));
		fflush(stdout);
	}

	return PSQL_CMD_SKIP_LINE;
}

/*
 * \password -- set user password
 */
static backslashResult
exec_command_password(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *opt0 = psql_scan_slash_option(scan_state,
												  OT_SQLID, NULL, true);
		char		pw1[100];
		char		pw2[100];

		simple_prompt("Enter new password: ", pw1, sizeof(pw1), false);
		simple_prompt("Enter it again: ", pw2, sizeof(pw2), false);

		if (strcmp(pw1, pw2) != 0)
		{
			pg_log_error("Passwords didn't match.");
			success = false;
		}
		else
		{
			char	   *user;
			char	   *encrypted_password;

			if (opt0)
				user = opt0;
			else
				user = PQuser(pset.db);

			encrypted_password = PQencryptPasswordConn(pset.db, pw1, user, NULL);

			if (!encrypted_password)
			{
				pg_log_info("%s", PQerrorMessage(pset.db));
				success = false;
			}
			else
			{
				PQExpBufferData buf;
				PGresult   *res;

				initPQExpBuffer(&buf);
				printfPQExpBuffer(&buf, "ALTER USER %s PASSWORD ",
								  fmtId(user));
				appendStringLiteralConn(&buf, encrypted_password, pset.db);
				res = PSQLexec(buf.data);
				termPQExpBuffer(&buf);
				if (!res)
					success = false;
				else
					PQclear(res);
				PQfreemem(encrypted_password);
			}
		}

		if (opt0)
			free(opt0);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \prompt -- prompt and set variable
 */
static backslashResult
exec_command_prompt(PsqlScanState scan_state, bool active_branch,
					const char *cmd)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *opt,
				   *prompt_text = NULL;
		char	   *arg1,
				   *arg2;

		arg1 = psql_scan_slash_option(scan_state, OT_NORMAL, NULL, false);
		arg2 = psql_scan_slash_option(scan_state, OT_NORMAL, NULL, false);

		if (!arg1)
		{
			pg_log_error("\\%s: missing required argument", cmd);
			success = false;
		}
		else
		{
			char	   *result;

			if (arg2)
			{
				prompt_text = arg1;
				opt = arg2;
			}
			else
				opt = arg1;

			if (!pset.inputfile)
			{
				result = (char *) pg_malloc(4096);
				simple_prompt(prompt_text, result, 4096, true);
			}
			else
			{
				if (prompt_text)
				{
					fputs(prompt_text, stdout);
					fflush(stdout);
				}
				result = gets_fromFile(stdin);
				if (!result)
				{
					pg_log_error("\\%s: could not read value for variable",
								 cmd);
					success = false;
				}
			}

			if (result &&
				!SetVariable(pset.vars, opt, result))
				success = false;

			if (result)
				free(result);
			if (prompt_text)
				free(prompt_text);
			free(opt);
		}
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \pset -- set printing parameters
 */
static backslashResult
exec_command_pset(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *opt0 = psql_scan_slash_option(scan_state,
												  OT_NORMAL, NULL, false);
		char	   *opt1 = psql_scan_slash_option(scan_state,
												  OT_NORMAL, NULL, false);

		if (!opt0)
		{
			/* list all variables */

			int			i;
			static const char *const my_list[] = {
				"border", "columns", "csv_fieldsep", "expanded", "fieldsep",
				"fieldsep_zero", "footer", "format", "linestyle", "null",
				"numericlocale", "pager", "pager_min_lines",
				"recordsep", "recordsep_zero",
				"tableattr", "title", "tuples_only",
				"unicode_border_linestyle",
				"unicode_column_linestyle",
				"unicode_header_linestyle",
				NULL
			};

			for (i = 0; my_list[i] != NULL; i++)
			{
				char	   *val = pset_value_string(my_list[i], &pset.popt);

				printf("%-24s %s\n", my_list[i], val);
				free(val);
			}

			success = true;
		}
		else
			success = do_pset(opt0, opt1, &pset.popt, pset.quiet);

		free(opt0);
		free(opt1);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \q or \quit -- exit psql
 */
static backslashResult
exec_command_quit(PsqlScanState scan_state, bool active_branch)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;

	if (active_branch)
		status = PSQL_CMD_TERMINATE;

	return status;
}

/*
 * \r -- reset (clear) the query buffer
 */
static backslashResult
exec_command_reset(PsqlScanState scan_state, bool active_branch,
				   PQExpBuffer query_buf)
{
	if (active_branch)
	{
		resetPQExpBuffer(query_buf);
		psql_scan_reset(scan_state);
		if (!pset.quiet)
			puts(_("Query buffer reset (cleared)."));
	}

	return PSQL_CMD_SKIP_LINE;
}

/*
 * \s -- save history in a file or show it on the screen
 */
static backslashResult
exec_command_s(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *fname = psql_scan_slash_option(scan_state,
												   OT_NORMAL, NULL, true);

		expand_tilde(&fname);
		success = printHistory(fname, pset.popt.topt.pager);
		if (success && !pset.quiet && fname)
			printf(_("Wrote history to file \"%s\".\n"), fname);
		if (!fname)
			putchar('\n');
		free(fname);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \set -- set variable
 */
static backslashResult
exec_command_set(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *opt0 = psql_scan_slash_option(scan_state,
												  OT_NORMAL, NULL, false);

		if (!opt0)
		{
			/* list all variables */
			PrintVariables(pset.vars);
			success = true;
		}
		else
		{
			/*
			 * Set variable to the concatenation of the arguments.
			 */
			char	   *newval;
			char	   *opt;

			opt = psql_scan_slash_option(scan_state,
										 OT_NORMAL, NULL, false);
			newval = pg_strdup(opt ? opt : "");
			free(opt);

			while ((opt = psql_scan_slash_option(scan_state,
												 OT_NORMAL, NULL, false)))
			{
				newval = pg_realloc(newval, strlen(newval) + strlen(opt) + 1);
				strcat(newval, opt);
				free(opt);
			}

			if (!SetVariable(pset.vars, opt0, newval))
				success = false;

			free(newval);
		}
		free(opt0);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \setenv -- set environment variable
 */
static backslashResult
exec_command_setenv(PsqlScanState scan_state, bool active_branch,
					const char *cmd)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *envvar = psql_scan_slash_option(scan_state,
													OT_NORMAL, NULL, false);
		char	   *envval = psql_scan_slash_option(scan_state,
													OT_NORMAL, NULL, false);

		if (!envvar)
		{
			pg_log_error("\\%s: missing required argument", cmd);
			success = false;
		}
		else if (strchr(envvar, '=') != NULL)
		{
			pg_log_error("\\%s: environment variable name must not contain \"=\"",
						 cmd);
			success = false;
		}
		else if (!envval)
		{
			/* No argument - unset the environment variable */
			unsetenv(envvar);
			success = true;
		}
		else
		{
			/* Set variable to the value of the next argument */
			char	   *newval;

			newval = psprintf("%s=%s", envvar, envval);
			putenv(newval);
			success = true;

			/*
			 * Do not free newval here, it will screw up the environment if
			 * you do. See putenv man page for details. That means we leak a
			 * bit of memory here, but not enough to worry about.
			 */
		}
		free(envvar);
		free(envval);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \sf/\sv -- show a function/view's source code
 */
static backslashResult
exec_command_sf_sv(PsqlScanState scan_state, bool active_branch,
				   const char *cmd, bool is_func)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;

	if (active_branch)
	{
		bool		show_linenumbers = (strchr(cmd, '+') != NULL);
		PQExpBuffer buf;
		char	   *obj_desc;
		Oid			obj_oid = InvalidOid;
		EditableObjectType eot = is_func ? EditableFunction : EditableView;

		buf = createPQExpBuffer();
		obj_desc = psql_scan_slash_option(scan_state,
										  OT_WHOLE_LINE, NULL, true);
		if (pset.sversion < (is_func ? 80400 : 70400))
		{
			char		sverbuf[32];

			formatPGVersionNumber(pset.sversion, false,
								  sverbuf, sizeof(sverbuf));
			if (is_func)
				pg_log_error("The server (version %s) does not support showing function source.",
							 sverbuf);
			else
				pg_log_error("The server (version %s) does not support showing view definitions.",
							 sverbuf);
			status = PSQL_CMD_ERROR;
		}
		else if (!obj_desc)
		{
			if (is_func)
				pg_log_error("function name is required");
			else
				pg_log_error("view name is required");
			status = PSQL_CMD_ERROR;
		}
		else if (!lookup_object_oid(eot, obj_desc, &obj_oid))
		{
			/* error already reported */
			status = PSQL_CMD_ERROR;
		}
		else if (!get_create_object_cmd(eot, obj_oid, buf))
		{
			/* error already reported */
			status = PSQL_CMD_ERROR;
		}
		else
		{
			FILE	   *output;
			bool		is_pager;

			/* Select output stream: stdout, pager, or file */
			if (pset.queryFout == stdout)
			{
				/* count lines in function to see if pager is needed */
				int			lineno = count_lines_in_buf(buf);

				output = PageOutput(lineno, &(pset.popt.topt));
				is_pager = true;
			}
			else
			{
				/* use previously set output file, without pager */
				output = pset.queryFout;
				is_pager = false;
			}

			if (show_linenumbers)
			{
				/*
				 * For functions, lineno "1" should correspond to the first
				 * line of the function body.  We expect that
				 * pg_get_functiondef() will emit that on a line beginning
				 * with "AS ", and that there can be no such line before the
				 * real start of the function body.
				 */
				print_with_linenumbers(output, buf->data,
									   is_func ? "AS " : NULL);
			}
			else
			{
				/* just send the definition to output */
				fputs(buf->data, output);
			}

			if (is_pager)
				ClosePager(output);
		}

		if (obj_desc)
			free(obj_desc);
		destroyPQExpBuffer(buf);
	}
	else
		ignore_slash_whole_line(scan_state);

	return status;
}

/*
 * \t -- turn off table headers and row count
 */
static backslashResult
exec_command_t(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *opt = psql_scan_slash_option(scan_state,
												 OT_NORMAL, NULL, true);

		success = do_pset("tuples_only", opt, &pset.popt, pset.quiet);
		free(opt);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \T -- define html <table ...> attributes
 */
static backslashResult
exec_command_T(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *value = psql_scan_slash_option(scan_state,
												   OT_NORMAL, NULL, false);

		success = do_pset("tableattr", value, &pset.popt, pset.quiet);
		free(value);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \timing -- enable/disable timing of queries
 */
static backslashResult
exec_command_timing(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *opt = psql_scan_slash_option(scan_state,
												 OT_NORMAL, NULL, false);

		if (opt)
			success = ParseVariableBool(opt, "\\timing", &pset.timing);
		else
			pset.timing = !pset.timing;
		if (!pset.quiet)
		{
			if (pset.timing)
				puts(_("Timing is on."));
			else
				puts(_("Timing is off."));
		}
		free(opt);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \unset -- unset variable
 */
static backslashResult
exec_command_unset(PsqlScanState scan_state, bool active_branch,
				   const char *cmd)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *opt = psql_scan_slash_option(scan_state,
												 OT_NORMAL, NULL, false);

		if (!opt)
		{
			pg_log_error("\\%s: missing required argument", cmd);
			success = false;
		}
		else if (!SetVariable(pset.vars, opt, NULL))
			success = false;

		free(opt);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \w -- write query buffer to file
 */
static backslashResult
exec_command_write(PsqlScanState scan_state, bool active_branch,
				   const char *cmd,
				   PQExpBuffer query_buf, PQExpBuffer previous_buf)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;

	if (active_branch)
	{
		char	   *fname = psql_scan_slash_option(scan_state,
												   OT_FILEPIPE, NULL, true);
		FILE	   *fd = NULL;
		bool		is_pipe = false;

		if (!query_buf)
		{
			pg_log_error("no query buffer");
			status = PSQL_CMD_ERROR;
		}
		else
		{
			if (!fname)
			{
				pg_log_error("\\%s: missing required argument", cmd);
				status = PSQL_CMD_ERROR;
			}
			else
			{
				expand_tilde(&fname);
				if (fname[0] == '|')
				{
					is_pipe = true;
					disable_sigpipe_trap();
					fd = popen(&fname[1], "w");
				}
				else
				{
					canonicalize_path(fname);
					fd = fopen(fname, "w");
				}
				if (!fd)
				{
					pg_log_error("%s: %m", fname);
					status = PSQL_CMD_ERROR;
				}
			}
		}

		if (fd)
		{
			int			result;

			/*
			 * We want to print the same thing \g would execute, but not to
			 * change the query buffer state; so we can't use
			 * copy_previous_query().  Also, beware of possibility that buffer
			 * pointers are NULL.
			 */
			if (query_buf && query_buf->len > 0)
				fprintf(fd, "%s\n", query_buf->data);
			else if (previous_buf && previous_buf->len > 0)
				fprintf(fd, "%s\n", previous_buf->data);

			if (is_pipe)
				result = pclose(fd);
			else
				result = fclose(fd);

			if (result == EOF)
			{
				pg_log_error("%s: %m", fname);
				status = PSQL_CMD_ERROR;
			}
		}

		if (is_pipe)
			restore_sigpipe_trap();

		free(fname);
	}
	else
		ignore_slash_filepipe(scan_state);

	return status;
}

/*
 * \watch -- execute a query every N seconds
 */
static backslashResult
exec_command_watch(PsqlScanState scan_state, bool active_branch,
				   PQExpBuffer query_buf, PQExpBuffer previous_buf)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *opt = psql_scan_slash_option(scan_state,
												 OT_NORMAL, NULL, true);
		double		sleep = 2;

		/* Convert optional sleep-length argument */
		if (opt)
		{
			sleep = strtod(opt, NULL);
			if (sleep <= 0)
				sleep = 1;
			free(opt);
		}

		/* If query_buf is empty, recall and execute previous query */
		copy_previous_query(query_buf, previous_buf);

		success = do_watch(query_buf, sleep);

		/* Reset the query buffer as though for \r */
		resetPQExpBuffer(query_buf);
		psql_scan_reset(scan_state);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \x -- set or toggle expanded table representation
 */
static backslashResult
exec_command_x(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *opt = psql_scan_slash_option(scan_state,
												 OT_NORMAL, NULL, true);

		success = do_pset("expanded", opt, &pset.popt, pset.quiet);
		free(opt);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \z -- list table privileges (equivalent to \dp)
 */
static backslashResult
exec_command_z(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *pattern = psql_scan_slash_option(scan_state,
													 OT_NORMAL, NULL, true);

		success = permissionsList(pattern);
		if (pattern)
			free(pattern);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \! -- execute shell command
 */
static backslashResult
exec_command_shell_escape(PsqlScanState scan_state, bool active_branch)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *opt = psql_scan_slash_option(scan_state,
												 OT_WHOLE_LINE, NULL, false);

		success = do_shell(opt);
		free(opt);
	}
	else
		ignore_slash_whole_line(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \? -- print help about backslash commands
 */
static backslashResult
exec_command_slash_command_help(PsqlScanState scan_state, bool active_branch)
{
	if (active_branch)
	{
		char	   *opt0 = psql_scan_slash_option(scan_state,
												  OT_NORMAL, NULL, false);

		if (!opt0 || strcmp(opt0, "commands") == 0)
			slashUsage(pset.popt.topt.pager);
		else if (strcmp(opt0, "options") == 0)
			usage(pset.popt.topt.pager);
		else if (strcmp(opt0, "variables") == 0)
			helpVariables(pset.popt.topt.pager);
		else
			slashUsage(pset.popt.topt.pager);

		if (opt0)
			free(opt0);
	}
	else
		ignore_slash_options(scan_state);

	return PSQL_CMD_SKIP_LINE;
}


/*
 * Read and interpret an argument to the \connect slash command.
 *
 * Returns a malloc'd string, or NULL if no/empty argument.
 */
static char *
read_connect_arg(PsqlScanState scan_state)
{
	char	   *result;
	char		quote;

	/*
	 * Ideally we should treat the arguments as SQL identifiers.  But for
	 * backwards compatibility with 7.2 and older pg_dump files, we have to
	 * take unquoted arguments verbatim (don't downcase them). For now,
	 * double-quoted arguments may be stripped of double quotes (as if SQL
	 * identifiers).  By 7.4 or so, pg_dump files can be expected to
	 * double-quote all mixed-case \connect arguments, and then we can get rid
	 * of OT_SQLIDHACK.
	 */
	result = psql_scan_slash_option(scan_state, OT_SQLIDHACK, &quote, true);

	if (!result)
		return NULL;

	if (quote)
		return result;

	if (*result == '\0' || strcmp(result, "-") == 0)
	{
		free(result);
		return NULL;
	}

	return result;
}

/*
 * Read a boolean expression, return it as a PQExpBuffer string.
 *
 * Note: anything more or less than one token will certainly fail to be
 * parsed by ParseVariableBool, so we don't worry about complaining here.
 * This routine's return data structure will need to be rethought anyway
 * to support likely future extensions such as "\if defined VARNAME".
 */
static PQExpBuffer
gather_boolean_expression(PsqlScanState scan_state)
{
	PQExpBuffer exp_buf = createPQExpBuffer();
	int			num_options = 0;
	char	   *value;

	/* collect all arguments for the conditional command into exp_buf */
	while ((value = psql_scan_slash_option(scan_state,
										   OT_NORMAL, NULL, false)) != NULL)
	{
		/* add spaces between tokens */
		if (num_options > 0)
			appendPQExpBufferChar(exp_buf, ' ');
		appendPQExpBufferStr(exp_buf, value);
		num_options++;
		free(value);
	}

	return exp_buf;
}

/*
 * Read a boolean expression, return true if the expression
 * was a valid boolean expression that evaluated to true.
 * Otherwise return false.
 *
 * Note: conditional stack's top state must be active, else lexer will
 * fail to expand variables and backticks.
 */
static bool
is_true_boolean_expression(PsqlScanState scan_state, const char *name)
{
	PQExpBuffer buf = gather_boolean_expression(scan_state);
	bool		value = false;
	bool		success = ParseVariableBool(buf->data, name, &value);

	destroyPQExpBuffer(buf);
	return success && value;
}

/*
 * Read a boolean expression, but do nothing with it.
 *
 * Note: conditional stack's top state must be INACTIVE, else lexer will
 * expand variables and backticks, which we do not want here.
 */
static void
ignore_boolean_expression(PsqlScanState scan_state)
{
	PQExpBuffer buf = gather_boolean_expression(scan_state);

	destroyPQExpBuffer(buf);
}

/*
 * Read and discard "normal" slash command options.
 *
 * This should be used for inactive-branch processing of any slash command
 * that eats one or more OT_NORMAL, OT_SQLID, or OT_SQLIDHACK parameters.
 * We don't need to worry about exactly how many it would eat, since the
 * cleanup logic in HandleSlashCmds would silently discard any extras anyway.
 */
static void
ignore_slash_options(PsqlScanState scan_state)
{
	char	   *arg;

	while ((arg = psql_scan_slash_option(scan_state,
										 OT_NORMAL, NULL, false)) != NULL)
		free(arg);
}

/*
 * Read and discard FILEPIPE slash command argument.
 *
 * This *MUST* be used for inactive-branch processing of any slash command
 * that takes an OT_FILEPIPE option.  Otherwise we might consume a different
 * amount of option text in active and inactive cases.
 */
static void
ignore_slash_filepipe(PsqlScanState scan_state)
{
	char	   *arg = psql_scan_slash_option(scan_state,
											 OT_FILEPIPE, NULL, false);

	if (arg)
		free(arg);
}

/*
 * Read and discard whole-line slash command argument.
 *
 * This *MUST* be used for inactive-branch processing of any slash command
 * that takes an OT_WHOLE_LINE option.  Otherwise we might consume a different
 * amount of option text in active and inactive cases.
 */
static void
ignore_slash_whole_line(PsqlScanState scan_state)
{
	char	   *arg = psql_scan_slash_option(scan_state,
											 OT_WHOLE_LINE, NULL, false);

	if (arg)
		free(arg);
}

/*
 * Return true if the command given is a branching command.
 */
static bool
is_branching_command(const char *cmd)
{
	return (strcmp(cmd, "if") == 0 ||
			strcmp(cmd, "elif") == 0 ||
			strcmp(cmd, "else") == 0 ||
			strcmp(cmd, "endif") == 0);
}

/*
 * Prepare to possibly restore query buffer to its current state
 * (cf. discard_query_text).
 *
 * We need to remember the length of the query buffer, and the lexer's
 * notion of the parenthesis nesting depth.
 */
static void
save_query_text_state(PsqlScanState scan_state, ConditionalStack cstack,
					  PQExpBuffer query_buf)
{
	if (query_buf)
		conditional_stack_set_query_len(cstack, query_buf->len);
	conditional_stack_set_paren_depth(cstack,
									  psql_scan_get_paren_depth(scan_state));
}

/*
 * Discard any query text absorbed during an inactive conditional branch.
 *
 * We must discard data that was appended to query_buf during an inactive
 * \if branch.  We don't have to do anything there if there's no query_buf.
 *
 * Also, reset the lexer state to the same paren depth there was before.
 * (The rest of its state doesn't need attention, since we could not be
 * inside a comment or literal or partial token.)
 */
static void
discard_query_text(PsqlScanState scan_state, ConditionalStack cstack,
				   PQExpBuffer query_buf)
{
	if (query_buf)
	{
		int			new_len = conditional_stack_get_query_len(cstack);

		Assert(new_len >= 0 && new_len <= query_buf->len);
		query_buf->len = new_len;
		query_buf->data[new_len] = '\0';
	}
	psql_scan_set_paren_depth(scan_state,
							  conditional_stack_get_paren_depth(cstack));
}

/*
 * If query_buf is empty, copy previous_buf into it.
 *
 * This is used by various slash commands for which re-execution of a
 * previous query is a common usage.  For convenience, we allow the
 * case of query_buf == NULL (and do nothing).
 */
static void
copy_previous_query(PQExpBuffer query_buf, PQExpBuffer previous_buf)
{
	if (query_buf && query_buf->len == 0)
		appendPQExpBufferStr(query_buf, previous_buf->data);
}

/*
 * Ask the user for a password; 'username' is the username the
 * password is for, if one has been explicitly specified. Returns a
 * malloc'd string.
 */
static char *
prompt_for_password(const char *username)
{
	char		buf[100];

	if (username == NULL || username[0] == '\0')
		simple_prompt("Password: ", buf, sizeof(buf), false);
	else
	{
		char	   *prompt_text;

		prompt_text = psprintf(_("Password for user %s: "), username);
		simple_prompt(prompt_text, buf, sizeof(buf), false);
		free(prompt_text);
	}
	return pg_strdup(buf);
}

static bool
param_is_newly_set(const char *old_val, const char *new_val)
{
	if (new_val == NULL)
		return false;

	if (old_val == NULL || strcmp(old_val, new_val) != 0)
		return true;

	return false;
}

/* return whether the connection has 'hostaddr' in its conninfo */
static bool
has_hostaddr(PGconn *conn)
{
	bool		used = false;
	PQconninfoOption *ciopt = PQconninfo(conn);

	for (PQconninfoOption *p = ciopt; p->keyword != NULL; p++)
	{
		if (strcmp(p->keyword, "hostaddr") == 0 && p->val != NULL)
		{
			used = true;
			break;
		}
	}

	PQconninfoFree(ciopt);
	return used;
}

/*
 * do_connect -- handler for \connect
 *
 * Connects to a database with given parameters. Absent an established
 * connection, all parameters are required. Given -reuse-previous=off or a
 * connection string without -reuse-previous=on, NULL values will pass through
 * to PQconnectdbParams(), so the libpq defaults will be used. Otherwise, NULL
 * values will be replaced with the ones in the current connection.
 *
 * In interactive mode, if connection fails with the given parameters,
 * the old connection will be kept.
 */
static bool
do_connect(enum trivalue reuse_previous_specification,
		   char *dbname, char *user, char *host, char *port)
{
	PGconn	   *o_conn = pset.db,
			   *n_conn;
	char	   *password = NULL;
	char	   *hostaddr = NULL;
	bool		keep_password;
	bool		has_connection_string;
	bool		reuse_previous;
	PQExpBufferData connstr;

	if (!o_conn && (!dbname || !user || !host || !port))
	{
		/*
		 * We don't know the supplied connection parameters and don't want to
		 * connect to the wrong database by using defaults, so require all
		 * parameters to be specified.
		 */
		pg_log_error("All connection parameters must be supplied because no "
					 "database connection exists");
		return false;
	}

	has_connection_string = dbname ?
		recognized_connection_string(dbname) : false;
	switch (reuse_previous_specification)
	{
		case TRI_YES:
			reuse_previous = true;
			break;
		case TRI_NO:
			reuse_previous = false;
			break;
		default:
			reuse_previous = !has_connection_string;
			break;
	}

	/* If the old connection does not exist, there is nothing to reuse. */
	if (!o_conn)
		reuse_previous = false;

	/* Silently ignore arguments subsequent to a connection string. */
	if (has_connection_string)
	{
		user = NULL;
		host = NULL;
		port = NULL;
	}

	/*
	 * Grab missing values from the old connection.  If we grab host (or host
	 * is the same as before) and hostaddr was set, grab that too.
	 */
	if (reuse_previous)
	{
		if (!user)
			user = PQuser(o_conn);
		if (host && strcmp(host, PQhost(o_conn)) == 0 &&
			has_hostaddr(o_conn))
		{
			hostaddr = PQhostaddr(o_conn);
		}
		if (!host)
		{
			host = PQhost(o_conn);
			if (has_hostaddr(o_conn))
				hostaddr = PQhostaddr(o_conn);
		}
		if (!port)
			port = PQport(o_conn);
	}

	/*
	 * Any change in the parameters read above makes us discard the password.
	 * We also discard it if we're to use a conninfo rather than the
	 * positional syntax.
	 */
	if (has_connection_string)
		keep_password = false;
	else
		keep_password =
			(user && PQuser(o_conn) && strcmp(user, PQuser(o_conn)) == 0) &&
			(host && PQhost(o_conn) && strcmp(host, PQhost(o_conn)) == 0) &&
			(port && PQport(o_conn) && strcmp(port, PQport(o_conn)) == 0);

	/*
	 * Grab missing dbname from old connection.  No password discard if this
	 * changes: passwords aren't (usually) database-specific.
	 */
	if (!dbname && reuse_previous)
	{
		initPQExpBuffer(&connstr);
		appendPQExpBufferStr(&connstr, "dbname=");
		appendConnStrVal(&connstr, PQdb(o_conn));
		dbname = connstr.data;
		/* has_connection_string=true would be a dead store */
	}
	else
		connstr.data = NULL;

	/*
	 * If the user asked to be prompted for a password, ask for one now. If
	 * not, use the password from the old connection, provided the username
	 * etc have not changed. Otherwise, try to connect without a password
	 * first, and then ask for a password if needed.
	 *
	 * XXX: this behavior leads to spurious connection attempts recorded in
	 * the postmaster's log.  But libpq offers no API that would let us obtain
	 * a password and then continue with the first connection attempt.
	 */
	if (pset.getPassword == TRI_YES)
	{
		/*
		 * If a connstring or URI is provided, we can't be sure we know which
		 * username will be used, since we haven't parsed that argument yet.
		 * Don't risk issuing a misleading prompt.  As in startup.c, it does
		 * not seem worth working harder, since this getPassword option is
		 * normally only used in noninteractive cases.
		 */
		password = prompt_for_password(has_connection_string ? NULL : user);
	}
	else if (o_conn && keep_password)
	{
		password = PQpass(o_conn);
		if (password && *password)
			password = pg_strdup(password);
		else
			password = NULL;
	}

	while (true)
	{
#define PARAMS_ARRAY_SIZE	9
		const char **keywords = pg_malloc(PARAMS_ARRAY_SIZE * sizeof(*keywords));
		const char **values = pg_malloc(PARAMS_ARRAY_SIZE * sizeof(*values));
		int			paramnum = -1;

		keywords[++paramnum] = "host";
		values[paramnum] = host;
		if (hostaddr && *hostaddr)
		{
			keywords[++paramnum] = "hostaddr";
			values[paramnum] = hostaddr;
		}
		keywords[++paramnum] = "port";
		values[paramnum] = port;
		keywords[++paramnum] = "user";
		values[paramnum] = user;

		/*
		 * Position in the array matters when the dbname is a connection
		 * string, because settings in a connection string override earlier
		 * array entries only.  Thus, user= in the connection string always
		 * takes effect, but client_encoding= often will not.
		 *
		 * If you change this code, also change the initial-connection code in
		 * main().  For no good reason, a connection string password= takes
		 * precedence in main() but not here.
		 */
		keywords[++paramnum] = "dbname";
		values[paramnum] = dbname;
		keywords[++paramnum] = "password";
		values[paramnum] = password;
		keywords[++paramnum] = "fallback_application_name";
		values[paramnum] = pset.progname;
		keywords[++paramnum] = "client_encoding";
		values[paramnum] = (pset.notty || getenv("PGCLIENTENCODING")) ? NULL : "auto";

		/* add array terminator */
		keywords[++paramnum] = NULL;
		values[paramnum] = NULL;

		n_conn = PQconnectdbParams(keywords, values, true);

		pg_free(keywords);
		pg_free(values);

		/* We can immediately discard the password -- no longer needed */
		if (password)
			pg_free(password);

		if (PQstatus(n_conn) == CONNECTION_OK)
			break;

		/*
		 * Connection attempt failed; either retry the connection attempt with
		 * a new password, or give up.
		 */
		if (!password && PQconnectionNeedsPassword(n_conn) && pset.getPassword != TRI_NO)
		{
			/*
			 * Prompt for password using the username we actually connected
			 * with --- it might've come out of "dbname" rather than "user".
			 */
			password = prompt_for_password(PQuser(n_conn));
			PQfinish(n_conn);
			continue;
		}

		/*
		 * Failed to connect to the database. In interactive mode, keep the
		 * previous connection to the DB; in scripting mode, close our
		 * previous connection as well.
		 */
		if (pset.cur_cmd_interactive)
		{
			pg_log_info("%s", PQerrorMessage(n_conn));

			/* pset.db is left unmodified */
			if (o_conn)
				pg_log_info("Previous connection kept");
		}
		else
		{
			pg_log_error("\\connect: %s", PQerrorMessage(n_conn));
			if (o_conn)
			{
				/*
				 * Transition to having no connection.  Keep this bit in sync
				 * with CheckConnection().
				 */
				PQfinish(o_conn);
				pset.db = NULL;
				ResetCancelConn();
				UnsyncVariables();
			}
		}

		PQfinish(n_conn);
		if (connstr.data)
			termPQExpBuffer(&connstr);
		return false;
	}
	if (connstr.data)
		termPQExpBuffer(&connstr);

	/*
	 * Replace the old connection with the new one, and update
	 * connection-dependent variables.  Keep the resynchronization logic in
	 * sync with CheckConnection().
	 */
	PQsetNoticeProcessor(n_conn, NoticeProcessor, NULL);
	pset.db = n_conn;
	SyncVariables();
	connection_warnings(false); /* Must be after SyncVariables */

	/* Tell the user about the new connection */
	if (!pset.quiet)
	{
		if (!o_conn ||
			param_is_newly_set(PQhost(o_conn), PQhost(pset.db)) ||
			param_is_newly_set(PQport(o_conn), PQport(pset.db)))
		{
			char	   *host = PQhost(pset.db);
			char	   *hostaddr = PQhostaddr(pset.db);

			/*
			 * If the host is an absolute path, the connection is via socket
			 * unless overridden by hostaddr
			 */
			if (is_absolute_path(host))
			{
				if (hostaddr && *hostaddr)
					printf(_("You are now connected to database \"%s\" as user \"%s\" on address \"%s\" at port \"%s\".\n"),
						   PQdb(pset.db), PQuser(pset.db), hostaddr, PQport(pset.db));
				else
					printf(_("You are now connected to database \"%s\" as user \"%s\" via socket in \"%s\" at port \"%s\".\n"),
						   PQdb(pset.db), PQuser(pset.db), host, PQport(pset.db));
			}
			else
			{
				if (hostaddr && *hostaddr && strcmp(host, hostaddr) != 0)
					printf(_("You are now connected to database \"%s\" as user \"%s\" on host \"%s\" (address \"%s\") at port \"%s\".\n"),
						   PQdb(pset.db), PQuser(pset.db), host, hostaddr, PQport(pset.db));
				else
					printf(_("You are now connected to database \"%s\" as user \"%s\" on host \"%s\" at port \"%s\".\n"),
						   PQdb(pset.db), PQuser(pset.db), host, PQport(pset.db));
			}
		}
		else
			printf(_("You are now connected to database \"%s\" as user \"%s\".\n"),
				   PQdb(pset.db), PQuser(pset.db));
	}

	if (o_conn)
		PQfinish(o_conn);
	return true;
}


void
connection_warnings(bool in_startup)
{
	if (!pset.quiet && !pset.notty)
	{
		int			client_ver = PG_VERSION_NUM;
		char		cverbuf[32];
		char		sverbuf[32];

		if (pset.sversion != client_ver)
		{
			const char *server_version;

			/* Try to get full text form, might include "devel" etc */
			server_version = PQparameterStatus(pset.db, "server_version");
			/* Otherwise fall back on pset.sversion */
			if (!server_version)
			{
				formatPGVersionNumber(pset.sversion, true,
									  sverbuf, sizeof(sverbuf));
				server_version = sverbuf;
			}

			printf(_("%s (%s, server %s)\n"),
				   pset.progname, PG_VERSION, server_version);
		}
		/* For version match, only print psql banner on startup. */
		else if (in_startup)
			printf("%s (%s)\n", pset.progname, PG_VERSION);

		if (pset.sversion / 100 > client_ver / 100)
			printf(_("WARNING: %s major version %s, server major version %s.\n"
					 "         Some psql features might not work.\n"),
				   pset.progname,
				   formatPGVersionNumber(client_ver, false,
										 cverbuf, sizeof(cverbuf)),
				   formatPGVersionNumber(pset.sversion, false,
										 sverbuf, sizeof(sverbuf)));

#ifdef WIN32
		if (in_startup)
			checkWin32Codepage();
#endif
		printSSLInfo();
		printGSSInfo();
	}
}


/*
 * printSSLInfo
 *
 * Prints information about the current SSL connection, if SSL is in use
 */
static void
printSSLInfo(void)
{
	const char *protocol;
	const char *cipher;
	const char *bits;
	const char *compression;

	if (!PQsslInUse(pset.db))
		return;					/* no SSL */

	protocol = PQsslAttribute(pset.db, "protocol");
	cipher = PQsslAttribute(pset.db, "cipher");
	bits = PQsslAttribute(pset.db, "key_bits");
	compression = PQsslAttribute(pset.db, "compression");

	printf(_("SSL connection (protocol: %s, cipher: %s, bits: %s, compression: %s)\n"),
		   protocol ? protocol : _("unknown"),
		   cipher ? cipher : _("unknown"),
		   bits ? bits : _("unknown"),
		   (compression && strcmp(compression, "off") != 0) ? _("on") : _("off"));
}

/*
 * printGSSInfo
 *
 * Prints information about the current GSSAPI connection, if GSSAPI encryption is in use
 */
static void
printGSSInfo(void)
{
	if (!PQgssEncInUse(pset.db))
		return;					/* no GSSAPI encryption in use */

	printf(_("GSSAPI-encrypted connection\n"));
}


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
		printf(_("WARNING: Console code page (%u) differs from Windows code page (%u)\n"
				 "         8-bit characters might not work correctly. See psql reference\n"
				 "         page \"Notes for Windows users\" for details.\n"),
			   concp, wincp);
	}
}
#endif


/*
 * SyncVariables
 *
 * Make psql's internal variables agree with connection state upon
 * establishing a new connection.
 */
void
SyncVariables(void)
{
	char		vbuf[32];
	const char *server_version;

	/* get stuff from connection */
	pset.encoding = PQclientEncoding(pset.db);
	pset.popt.topt.encoding = pset.encoding;
	pset.sversion = PQserverVersion(pset.db);

	SetVariable(pset.vars, "DBNAME", PQdb(pset.db));
	SetVariable(pset.vars, "USER", PQuser(pset.db));
	SetVariable(pset.vars, "HOST", PQhost(pset.db));
	SetVariable(pset.vars, "PORT", PQport(pset.db));
	SetVariable(pset.vars, "ENCODING", pg_encoding_to_char(pset.encoding));

	/* this bit should match connection_warnings(): */
	/* Try to get full text form of version, might include "devel" etc */
	server_version = PQparameterStatus(pset.db, "server_version");
	/* Otherwise fall back on pset.sversion */
	if (!server_version)
	{
		formatPGVersionNumber(pset.sversion, true, vbuf, sizeof(vbuf));
		server_version = vbuf;
	}
	SetVariable(pset.vars, "SERVER_VERSION_NAME", server_version);

	snprintf(vbuf, sizeof(vbuf), "%d", pset.sversion);
	SetVariable(pset.vars, "SERVER_VERSION_NUM", vbuf);

	/* send stuff to it, too */
	PQsetErrorVerbosity(pset.db, pset.verbosity);
	PQsetErrorContextVisibility(pset.db, pset.show_context);
}

/*
 * UnsyncVariables
 *
 * Clear variables that should be not be set when there is no connection.
 */
void
UnsyncVariables(void)
{
	SetVariable(pset.vars, "DBNAME", NULL);
	SetVariable(pset.vars, "USER", NULL);
	SetVariable(pset.vars, "HOST", NULL);
	SetVariable(pset.vars, "PORT", NULL);
	SetVariable(pset.vars, "ENCODING", NULL);
	SetVariable(pset.vars, "SERVER_VERSION_NAME", NULL);
	SetVariable(pset.vars, "SERVER_VERSION_NUM", NULL);
}


/*
 * do_edit -- handler for \e
 *
 * If you do not specify a filename, the current query buffer will be copied
 * into a temporary one.
 */
static bool
editFile(const char *fname, int lineno)
{
	const char *editorName;
	const char *editor_lineno_arg = NULL;
	char	   *sys;
	int			result;

	Assert(fname != NULL);

	/* Find an editor to use */
	editorName = getenv("PSQL_EDITOR");
	if (!editorName)
		editorName = getenv("EDITOR");
	if (!editorName)
		editorName = getenv("VISUAL");
	if (!editorName)
		editorName = DEFAULT_EDITOR;

	/* Get line number argument, if we need it. */
	if (lineno > 0)
	{
		editor_lineno_arg = getenv("PSQL_EDITOR_LINENUMBER_ARG");
#ifdef DEFAULT_EDITOR_LINENUMBER_ARG
		if (!editor_lineno_arg)
			editor_lineno_arg = DEFAULT_EDITOR_LINENUMBER_ARG;
#endif
		if (!editor_lineno_arg)
		{
			pg_log_error("environment variable PSQL_EDITOR_LINENUMBER_ARG must be set to specify a line number");
			return false;
		}
	}

	/*
	 * On Unix the EDITOR value should *not* be quoted, since it might include
	 * switches, eg, EDITOR="pico -t"; it's up to the user to put quotes in it
	 * if necessary.  But this policy is not very workable on Windows, due to
	 * severe brain damage in their command shell plus the fact that standard
	 * program paths include spaces.
	 */
#ifndef WIN32
	if (lineno > 0)
		sys = psprintf("exec %s %s%d '%s'",
					   editorName, editor_lineno_arg, lineno, fname);
	else
		sys = psprintf("exec %s '%s'",
					   editorName, fname);
#else
	if (lineno > 0)
		sys = psprintf("\"%s\" %s%d \"%s\"",
					   editorName, editor_lineno_arg, lineno, fname);
	else
		sys = psprintf("\"%s\" \"%s\"",
					   editorName, fname);
#endif
	result = system(sys);
	if (result == -1)
		pg_log_error("could not start editor \"%s\"", editorName);
	else if (result == 127)
		pg_log_error("could not start /bin/sh");
	free(sys);

	return result == 0;
}


/* call this one */
static bool
do_edit(const char *filename_arg, PQExpBuffer query_buf,
		int lineno, bool *edited)
{
	char		fnametmp[MAXPGPATH];
	FILE	   *stream = NULL;
	const char *fname;
	bool		error = false;
	int			fd;

	struct stat before,
				after;

	if (filename_arg)
		fname = filename_arg;
	else
	{
		/* make a temp file to edit */
#ifndef WIN32
		const char *tmpdir = getenv("TMPDIR");

		if (!tmpdir)
			tmpdir = "/tmp";
#else
		char		tmpdir[MAXPGPATH];
		int			ret;

		ret = GetTempPath(MAXPGPATH, tmpdir);
		if (ret == 0 || ret > MAXPGPATH)
		{
			pg_log_error("could not locate temporary directory: %s",
						 !ret ? strerror(errno) : "");
			return false;
		}

		/*
		 * No canonicalize_path() here. EDIT.EXE run from CMD.EXE prepends the
		 * current directory to the supplied path unless we use only
		 * backslashes, so we do that.
		 */
#endif
#ifndef WIN32
		snprintf(fnametmp, sizeof(fnametmp), "%s%spsql.edit.%d.sql", tmpdir,
				 "/", (int) getpid());
#else
		snprintf(fnametmp, sizeof(fnametmp), "%s%spsql.edit.%d.sql", tmpdir,
				 "" /* trailing separator already present */ , (int) getpid());
#endif

		fname = (const char *) fnametmp;

		fd = open(fname, O_WRONLY | O_CREAT | O_EXCL, 0600);
		if (fd != -1)
			stream = fdopen(fd, "w");

		if (fd == -1 || !stream)
		{
			pg_log_error("could not open temporary file \"%s\": %m", fname);
			error = true;
		}
		else
		{
			unsigned int ql = query_buf->len;

			/* force newline-termination of what we send to editor */
			if (ql > 0 && query_buf->data[ql - 1] != '\n')
			{
				appendPQExpBufferChar(query_buf, '\n');
				ql++;
			}

			if (fwrite(query_buf->data, 1, ql, stream) != ql)
			{
				pg_log_error("%s: %m", fname);

				if (fclose(stream) != 0)
					pg_log_error("%s: %m", fname);

				if (remove(fname) != 0)
					pg_log_error("%s: %m", fname);

				error = true;
			}
			else if (fclose(stream) != 0)
			{
				pg_log_error("%s: %m", fname);
				if (remove(fname) != 0)
					pg_log_error("%s: %m", fname);
				error = true;
			}
		}
	}

	if (!error && stat(fname, &before) != 0)
	{
		pg_log_error("%s: %m", fname);
		error = true;
	}

	/* call editor */
	if (!error)
		error = !editFile(fname, lineno);

	if (!error && stat(fname, &after) != 0)
	{
		pg_log_error("%s: %m", fname);
		error = true;
	}

	if (!error && before.st_mtime != after.st_mtime)
	{
		stream = fopen(fname, PG_BINARY_R);
		if (!stream)
		{
			pg_log_error("%s: %m", fname);
			error = true;
		}
		else
		{
			/* read file back into query_buf */
			char		line[1024];

			resetPQExpBuffer(query_buf);
			while (fgets(line, sizeof(line), stream) != NULL)
				appendPQExpBufferStr(query_buf, line);

			if (ferror(stream))
			{
				pg_log_error("%s: %m", fname);
				error = true;
			}
			else if (edited)
			{
				*edited = true;
			}

			fclose(stream);
		}
	}

	/* remove temp file */
	if (!filename_arg)
	{
		if (remove(fname) == -1)
		{
			pg_log_error("%s: %m", fname);
			error = true;
		}
	}

	return !error;
}



/*
 * process_file
 *
 * Reads commands from filename and passes them to the main processing loop.
 * Handler for \i and \ir, but can be used for other things as well.  Returns
 * MainLoop() error code.
 *
 * If use_relative_path is true and filename is not an absolute path, then open
 * the file from where the currently processed file (if any) is located.
 */
int
process_file(char *filename, bool use_relative_path)
{
	FILE	   *fd;
	int			result;
	char	   *oldfilename;
	char		relpath[MAXPGPATH];

	if (!filename)
	{
		fd = stdin;
		filename = NULL;
	}
	else if (strcmp(filename, "-") != 0)
	{
		canonicalize_path(filename);

		/*
		 * If we were asked to resolve the pathname relative to the location
		 * of the currently executing script, and there is one, and this is a
		 * relative pathname, then prepend all but the last pathname component
		 * of the current script to this pathname.
		 */
		if (use_relative_path && pset.inputfile &&
			!is_absolute_path(filename) && !has_drive_prefix(filename))
		{
			strlcpy(relpath, pset.inputfile, sizeof(relpath));
			get_parent_directory(relpath);
			join_path_components(relpath, relpath, filename);
			canonicalize_path(relpath);

			filename = relpath;
		}

		fd = fopen(filename, PG_BINARY_R);

		if (!fd)
		{
			pg_log_error("%s: %m", filename);
			return EXIT_FAILURE;
		}
	}
	else
	{
		fd = stdin;
		filename = "<stdin>";	/* for future error messages */
	}

	oldfilename = pset.inputfile;
	pset.inputfile = filename;

	pg_logging_config(pset.inputfile ? 0 : PG_LOG_FLAG_TERSE);

	result = MainLoop(fd);

	if (fd != stdin)
		fclose(fd);

	pset.inputfile = oldfilename;

	pg_logging_config(pset.inputfile ? 0 : PG_LOG_FLAG_TERSE);

	return result;
}



static const char *
_align2string(enum printFormat in)
{
	switch (in)
	{
		case PRINT_NOTHING:
			return "nothing";
			break;
		case PRINT_ALIGNED:
			return "aligned";
			break;
		case PRINT_ASCIIDOC:
			return "asciidoc";
			break;
		case PRINT_CSV:
			return "csv";
			break;
		case PRINT_HTML:
			return "html";
			break;
		case PRINT_LATEX:
			return "latex";
			break;
		case PRINT_LATEX_LONGTABLE:
			return "latex-longtable";
			break;
		case PRINT_TROFF_MS:
			return "troff-ms";
			break;
		case PRINT_UNALIGNED:
			return "unaligned";
			break;
		case PRINT_WRAPPED:
			return "wrapped";
			break;
	}
	return "unknown";
}

/*
 * Parse entered Unicode linestyle.  If ok, update *linestyle and return
 * true, else return false.
 */
static bool
set_unicode_line_style(const char *value, size_t vallen,
					   unicode_linestyle *linestyle)
{
	if (pg_strncasecmp("single", value, vallen) == 0)
		*linestyle = UNICODE_LINESTYLE_SINGLE;
	else if (pg_strncasecmp("double", value, vallen) == 0)
		*linestyle = UNICODE_LINESTYLE_DOUBLE;
	else
		return false;
	return true;
}

static const char *
_unicode_linestyle2string(int linestyle)
{
	switch (linestyle)
	{
		case UNICODE_LINESTYLE_SINGLE:
			return "single";
			break;
		case UNICODE_LINESTYLE_DOUBLE:
			return "double";
			break;
	}
	return "unknown";
}

/*
 * do_pset
 *
 * Performs the assignment "param = value", where value could be NULL;
 * for some params that has an effect such as inversion, for others
 * it does nothing.
 *
 * Adjusts the state of the formatting options at *popt.  (In practice that
 * is always pset.popt, but maybe someday it could be different.)
 *
 * If successful and quiet is false, then invokes printPsetInfo() to report
 * the change.
 *
 * Returns true if successful, else false (eg for invalid param or value).
 */
bool
do_pset(const char *param, const char *value, printQueryOpt *popt, bool quiet)
{
	size_t		vallen = 0;

	Assert(param != NULL);

	if (value)
		vallen = strlen(value);

	/* set format */
	if (strcmp(param, "format") == 0)
	{
		static const struct fmt
		{
			const char *name;
			enum printFormat number;
		}			formats[] =
		{
			/* remember to update error message below when adding more */
			{"aligned", PRINT_ALIGNED},
			{"asciidoc", PRINT_ASCIIDOC},
			{"csv", PRINT_CSV},
			{"html", PRINT_HTML},
			{"latex", PRINT_LATEX},
			{"troff-ms", PRINT_TROFF_MS},
			{"unaligned", PRINT_UNALIGNED},
			{"wrapped", PRINT_WRAPPED}
		};

		if (!value)
			;
		else
		{
			int			match_pos = -1;

			for (int i = 0; i < lengthof(formats); i++)
			{
				if (pg_strncasecmp(formats[i].name, value, vallen) == 0)
				{
					if (match_pos < 0)
						match_pos = i;
					else
					{
						pg_log_error("\\pset: ambiguous abbreviation \"%s\" matches both \"%s\" and \"%s\"",
									 value,
									 formats[match_pos].name, formats[i].name);
						return false;
					}
				}
			}
			if (match_pos >= 0)
				popt->topt.format = formats[match_pos].number;
			else if (pg_strncasecmp("latex-longtable", value, vallen) == 0)
			{
				/*
				 * We must treat latex-longtable specially because latex is a
				 * prefix of it; if both were in the table above, we'd think
				 * "latex" is ambiguous.
				 */
				popt->topt.format = PRINT_LATEX_LONGTABLE;
			}
			else
			{
				pg_log_error("\\pset: allowed formats are aligned, asciidoc, csv, html, latex, latex-longtable, troff-ms, unaligned, wrapped");
				return false;
			}
		}
	}

	/* set table line style */
	else if (strcmp(param, "linestyle") == 0)
	{
		if (!value)
			;
		else if (pg_strncasecmp("ascii", value, vallen) == 0)
			popt->topt.line_style = &pg_asciiformat;
		else if (pg_strncasecmp("old-ascii", value, vallen) == 0)
			popt->topt.line_style = &pg_asciiformat_old;
		else if (pg_strncasecmp("unicode", value, vallen) == 0)
			popt->topt.line_style = &pg_utf8format;
		else
		{
			pg_log_error("\\pset: allowed line styles are ascii, old-ascii, unicode");
			return false;
		}
	}

	/* set unicode border line style */
	else if (strcmp(param, "unicode_border_linestyle") == 0)
	{
		if (!value)
			;
		else if (set_unicode_line_style(value, vallen,
										&popt->topt.unicode_border_linestyle))
			refresh_utf8format(&(popt->topt));
		else
		{
			pg_log_error("\\pset: allowed Unicode border line styles are single, double");
			return false;
		}
	}

	/* set unicode column line style */
	else if (strcmp(param, "unicode_column_linestyle") == 0)
	{
		if (!value)
			;
		else if (set_unicode_line_style(value, vallen,
										&popt->topt.unicode_column_linestyle))
			refresh_utf8format(&(popt->topt));
		else
		{
			pg_log_error("\\pset: allowed Unicode column line styles are single, double");
			return false;
		}
	}

	/* set unicode header line style */
	else if (strcmp(param, "unicode_header_linestyle") == 0)
	{
		if (!value)
			;
		else if (set_unicode_line_style(value, vallen,
										&popt->topt.unicode_header_linestyle))
			refresh_utf8format(&(popt->topt));
		else
		{
			pg_log_error("\\pset: allowed Unicode header line styles are single, double");
			return false;
		}
	}

	/* set border style/width */
	else if (strcmp(param, "border") == 0)
	{
		if (value)
			popt->topt.border = atoi(value);
	}

	/* set expanded/vertical mode */
	else if (strcmp(param, "x") == 0 ||
			 strcmp(param, "expanded") == 0 ||
			 strcmp(param, "vertical") == 0)
	{
		if (value && pg_strcasecmp(value, "auto") == 0)
			popt->topt.expanded = 2;
		else if (value)
		{
			bool		on_off;

			if (ParseVariableBool(value, NULL, &on_off))
				popt->topt.expanded = on_off ? 1 : 0;
			else
			{
				PsqlVarEnumError(param, value, "on, off, auto");
				return false;
			}
		}
		else
			popt->topt.expanded = !popt->topt.expanded;
	}

	/* field separator for CSV format */
	else if (strcmp(param, "csv_fieldsep") == 0)
	{
		if (value)
		{
			/* CSV separator has to be a one-byte character */
			if (strlen(value) != 1)
			{
				pg_log_error("\\pset: csv_fieldsep must be a single one-byte character");
				return false;
			}
			if (value[0] == '"' || value[0] == '\n' || value[0] == '\r')
			{
				pg_log_error("\\pset: csv_fieldsep cannot be a double quote, a newline, or a carriage return");
				return false;
			}
			popt->topt.csvFieldSep[0] = value[0];
		}
	}

	/* locale-aware numeric output */
	else if (strcmp(param, "numericlocale") == 0)
	{
		if (value)
			return ParseVariableBool(value, param, &popt->topt.numericLocale);
		else
			popt->topt.numericLocale = !popt->topt.numericLocale;
	}

	/* null display */
	else if (strcmp(param, "null") == 0)
	{
		if (value)
		{
			free(popt->nullPrint);
			popt->nullPrint = pg_strdup(value);
		}
	}

	/* field separator for unaligned text */
	else if (strcmp(param, "fieldsep") == 0)
	{
		if (value)
		{
			free(popt->topt.fieldSep.separator);
			popt->topt.fieldSep.separator = pg_strdup(value);
			popt->topt.fieldSep.separator_zero = false;
		}
	}

	else if (strcmp(param, "fieldsep_zero") == 0)
	{
		free(popt->topt.fieldSep.separator);
		popt->topt.fieldSep.separator = NULL;
		popt->topt.fieldSep.separator_zero = true;
	}

	/* record separator for unaligned text */
	else if (strcmp(param, "recordsep") == 0)
	{
		if (value)
		{
			free(popt->topt.recordSep.separator);
			popt->topt.recordSep.separator = pg_strdup(value);
			popt->topt.recordSep.separator_zero = false;
		}
	}

	else if (strcmp(param, "recordsep_zero") == 0)
	{
		free(popt->topt.recordSep.separator);
		popt->topt.recordSep.separator = NULL;
		popt->topt.recordSep.separator_zero = true;
	}

	/* toggle between full and tuples-only format */
	else if (strcmp(param, "t") == 0 || strcmp(param, "tuples_only") == 0)
	{
		if (value)
			return ParseVariableBool(value, param, &popt->topt.tuples_only);
		else
			popt->topt.tuples_only = !popt->topt.tuples_only;
	}

	/* set title override */
	else if (strcmp(param, "C") == 0 || strcmp(param, "title") == 0)
	{
		free(popt->title);
		if (!value)
			popt->title = NULL;
		else
			popt->title = pg_strdup(value);
	}

	/* set HTML table tag options */
	else if (strcmp(param, "T") == 0 || strcmp(param, "tableattr") == 0)
	{
		free(popt->topt.tableAttr);
		if (!value)
			popt->topt.tableAttr = NULL;
		else
			popt->topt.tableAttr = pg_strdup(value);
	}

	/* toggle use of pager */
	else if (strcmp(param, "pager") == 0)
	{
		if (value && pg_strcasecmp(value, "always") == 0)
			popt->topt.pager = 2;
		else if (value)
		{
			bool		on_off;

			if (!ParseVariableBool(value, NULL, &on_off))
			{
				PsqlVarEnumError(param, value, "on, off, always");
				return false;
			}
			popt->topt.pager = on_off ? 1 : 0;
		}
		else if (popt->topt.pager == 1)
			popt->topt.pager = 0;
		else
			popt->topt.pager = 1;
	}

	/* set minimum lines for pager use */
	else if (strcmp(param, "pager_min_lines") == 0)
	{
		if (value)
			popt->topt.pager_min_lines = atoi(value);
	}

	/* disable "(x rows)" footer */
	else if (strcmp(param, "footer") == 0)
	{
		if (value)
			return ParseVariableBool(value, param, &popt->topt.default_footer);
		else
			popt->topt.default_footer = !popt->topt.default_footer;
	}

	/* set border style/width */
	else if (strcmp(param, "columns") == 0)
	{
		if (value)
			popt->topt.columns = atoi(value);
	}
	else
	{
		pg_log_error("\\pset: unknown option: %s", param);
		return false;
	}

	if (!quiet)
		printPsetInfo(param, &pset.popt);

	return true;
}

/*
 * printPsetInfo: print the state of the "param" formatting parameter in popt.
 */
static bool
printPsetInfo(const char *param, printQueryOpt *popt)
{
	Assert(param != NULL);

	/* show border style/width */
	if (strcmp(param, "border") == 0)
		printf(_("Border style is %d.\n"), popt->topt.border);

	/* show the target width for the wrapped format */
	else if (strcmp(param, "columns") == 0)
	{
		if (!popt->topt.columns)
			printf(_("Target width is unset.\n"));
		else
			printf(_("Target width is %d.\n"), popt->topt.columns);
	}

	/* show expanded/vertical mode */
	else if (strcmp(param, "x") == 0 || strcmp(param, "expanded") == 0 || strcmp(param, "vertical") == 0)
	{
		if (popt->topt.expanded == 1)
			printf(_("Expanded display is on.\n"));
		else if (popt->topt.expanded == 2)
			printf(_("Expanded display is used automatically.\n"));
		else
			printf(_("Expanded display is off.\n"));
	}

	/* show field separator for CSV format */
	else if (strcmp(param, "csv_fieldsep") == 0)
	{
		printf(_("Field separator for CSV is \"%s\".\n"),
			   popt->topt.csvFieldSep);
	}

	/* show field separator for unaligned text */
	else if (strcmp(param, "fieldsep") == 0)
	{
		if (popt->topt.fieldSep.separator_zero)
			printf(_("Field separator is zero byte.\n"));
		else
			printf(_("Field separator is \"%s\".\n"),
				   popt->topt.fieldSep.separator);
	}

	else if (strcmp(param, "fieldsep_zero") == 0)
	{
		printf(_("Field separator is zero byte.\n"));
	}

	/* show disable "(x rows)" footer */
	else if (strcmp(param, "footer") == 0)
	{
		if (popt->topt.default_footer)
			printf(_("Default footer is on.\n"));
		else
			printf(_("Default footer is off.\n"));
	}

	/* show format */
	else if (strcmp(param, "format") == 0)
	{
		printf(_("Output format is %s.\n"), _align2string(popt->topt.format));
	}

	/* show table line style */
	else if (strcmp(param, "linestyle") == 0)
	{
		printf(_("Line style is %s.\n"),
			   get_line_style(&popt->topt)->name);
	}

	/* show null display */
	else if (strcmp(param, "null") == 0)
	{
		printf(_("Null display is \"%s\".\n"),
			   popt->nullPrint ? popt->nullPrint : "");
	}

	/* show locale-aware numeric output */
	else if (strcmp(param, "numericlocale") == 0)
	{
		if (popt->topt.numericLocale)
			printf(_("Locale-adjusted numeric output is on.\n"));
		else
			printf(_("Locale-adjusted numeric output is off.\n"));
	}

	/* show toggle use of pager */
	else if (strcmp(param, "pager") == 0)
	{
		if (popt->topt.pager == 1)
			printf(_("Pager is used for long output.\n"));
		else if (popt->topt.pager == 2)
			printf(_("Pager is always used.\n"));
		else
			printf(_("Pager usage is off.\n"));
	}

	/* show minimum lines for pager use */
	else if (strcmp(param, "pager_min_lines") == 0)
	{
		printf(ngettext("Pager won't be used for less than %d line.\n",
						"Pager won't be used for less than %d lines.\n",
						popt->topt.pager_min_lines),
			   popt->topt.pager_min_lines);
	}

	/* show record separator for unaligned text */
	else if (strcmp(param, "recordsep") == 0)
	{
		if (popt->topt.recordSep.separator_zero)
			printf(_("Record separator is zero byte.\n"));
		else if (strcmp(popt->topt.recordSep.separator, "\n") == 0)
			printf(_("Record separator is <newline>.\n"));
		else
			printf(_("Record separator is \"%s\".\n"),
				   popt->topt.recordSep.separator);
	}

	else if (strcmp(param, "recordsep_zero") == 0)
	{
		printf(_("Record separator is zero byte.\n"));
	}

	/* show HTML table tag options */
	else if (strcmp(param, "T") == 0 || strcmp(param, "tableattr") == 0)
	{
		if (popt->topt.tableAttr)
			printf(_("Table attributes are \"%s\".\n"),
				   popt->topt.tableAttr);
		else
			printf(_("Table attributes unset.\n"));
	}

	/* show title override */
	else if (strcmp(param, "C") == 0 || strcmp(param, "title") == 0)
	{
		if (popt->title)
			printf(_("Title is \"%s\".\n"), popt->title);
		else
			printf(_("Title is unset.\n"));
	}

	/* show toggle between full and tuples-only format */
	else if (strcmp(param, "t") == 0 || strcmp(param, "tuples_only") == 0)
	{
		if (popt->topt.tuples_only)
			printf(_("Tuples only is on.\n"));
		else
			printf(_("Tuples only is off.\n"));
	}

	/* Unicode style formatting */
	else if (strcmp(param, "unicode_border_linestyle") == 0)
	{
		printf(_("Unicode border line style is \"%s\".\n"),
			   _unicode_linestyle2string(popt->topt.unicode_border_linestyle));
	}

	else if (strcmp(param, "unicode_column_linestyle") == 0)
	{
		printf(_("Unicode column line style is \"%s\".\n"),
			   _unicode_linestyle2string(popt->topt.unicode_column_linestyle));
	}

	else if (strcmp(param, "unicode_header_linestyle") == 0)
	{
		printf(_("Unicode header line style is \"%s\".\n"),
			   _unicode_linestyle2string(popt->topt.unicode_header_linestyle));
	}

	else
	{
		pg_log_error("\\pset: unknown option: %s", param);
		return false;
	}

	return true;
}

/*
 * savePsetInfo: make a malloc'd copy of the data in *popt.
 *
 * Possibly this should be somewhere else, but it's a bit specific to psql.
 */
printQueryOpt *
savePsetInfo(const printQueryOpt *popt)
{
	printQueryOpt *save;

	save = (printQueryOpt *) pg_malloc(sizeof(printQueryOpt));

	/* Flat-copy all the scalar fields, then duplicate sub-structures. */
	memcpy(save, popt, sizeof(printQueryOpt));

	/* topt.line_style points to const data that need not be duplicated */
	if (popt->topt.fieldSep.separator)
		save->topt.fieldSep.separator = pg_strdup(popt->topt.fieldSep.separator);
	if (popt->topt.recordSep.separator)
		save->topt.recordSep.separator = pg_strdup(popt->topt.recordSep.separator);
	if (popt->topt.tableAttr)
		save->topt.tableAttr = pg_strdup(popt->topt.tableAttr);
	if (popt->nullPrint)
		save->nullPrint = pg_strdup(popt->nullPrint);
	if (popt->title)
		save->title = pg_strdup(popt->title);

	/*
	 * footers and translate_columns are never set in psql's print settings,
	 * so we needn't write code to duplicate them.
	 */
	Assert(popt->footers == NULL);
	Assert(popt->translate_columns == NULL);

	return save;
}

/*
 * restorePsetInfo: restore *popt from the previously-saved copy *save,
 * then free *save.
 */
void
restorePsetInfo(printQueryOpt *popt, printQueryOpt *save)
{
	/* Free all the old data we're about to overwrite the pointers to. */

	/* topt.line_style points to const data that need not be duplicated */
	if (popt->topt.fieldSep.separator)
		free(popt->topt.fieldSep.separator);
	if (popt->topt.recordSep.separator)
		free(popt->topt.recordSep.separator);
	if (popt->topt.tableAttr)
		free(popt->topt.tableAttr);
	if (popt->nullPrint)
		free(popt->nullPrint);
	if (popt->title)
		free(popt->title);

	/*
	 * footers and translate_columns are never set in psql's print settings,
	 * so we needn't write code to duplicate them.
	 */
	Assert(popt->footers == NULL);
	Assert(popt->translate_columns == NULL);

	/* Now we may flat-copy all the fields, including pointers. */
	memcpy(popt, save, sizeof(printQueryOpt));

	/* Lastly, free "save" ... but its sub-structures now belong to popt. */
	free(save);
}

static const char *
pset_bool_string(bool val)
{
	return val ? "on" : "off";
}


static char *
pset_quoted_string(const char *str)
{
	char	   *ret = pg_malloc(strlen(str) * 2 + 3);
	char	   *r = ret;

	*r++ = '\'';

	for (; *str; str++)
	{
		if (*str == '\n')
		{
			*r++ = '\\';
			*r++ = 'n';
		}
		else if (*str == '\'')
		{
			*r++ = '\\';
			*r++ = '\'';
		}
		else
			*r++ = *str;
	}

	*r++ = '\'';
	*r = '\0';

	return ret;
}


/*
 * Return a malloc'ed string for the \pset value.
 *
 * Note that for some string parameters, print.c distinguishes between unset
 * and empty string, but for others it doesn't.  This function should produce
 * output that produces the correct setting when fed back into \pset.
 */
static char *
pset_value_string(const char *param, printQueryOpt *popt)
{
	Assert(param != NULL);

	if (strcmp(param, "border") == 0)
		return psprintf("%d", popt->topt.border);
	else if (strcmp(param, "columns") == 0)
		return psprintf("%d", popt->topt.columns);
	else if (strcmp(param, "csv_fieldsep") == 0)
		return pset_quoted_string(popt->topt.csvFieldSep);
	else if (strcmp(param, "expanded") == 0)
		return pstrdup(popt->topt.expanded == 2
					   ? "auto"
					   : pset_bool_string(popt->topt.expanded));
	else if (strcmp(param, "fieldsep") == 0)
		return pset_quoted_string(popt->topt.fieldSep.separator
								  ? popt->topt.fieldSep.separator
								  : "");
	else if (strcmp(param, "fieldsep_zero") == 0)
		return pstrdup(pset_bool_string(popt->topt.fieldSep.separator_zero));
	else if (strcmp(param, "footer") == 0)
		return pstrdup(pset_bool_string(popt->topt.default_footer));
	else if (strcmp(param, "format") == 0)
		return psprintf("%s", _align2string(popt->topt.format));
	else if (strcmp(param, "linestyle") == 0)
		return psprintf("%s", get_line_style(&popt->topt)->name);
	else if (strcmp(param, "null") == 0)
		return pset_quoted_string(popt->nullPrint
								  ? popt->nullPrint
								  : "");
	else if (strcmp(param, "numericlocale") == 0)
		return pstrdup(pset_bool_string(popt->topt.numericLocale));
	else if (strcmp(param, "pager") == 0)
		return psprintf("%d", popt->topt.pager);
	else if (strcmp(param, "pager_min_lines") == 0)
		return psprintf("%d", popt->topt.pager_min_lines);
	else if (strcmp(param, "recordsep") == 0)
		return pset_quoted_string(popt->topt.recordSep.separator
								  ? popt->topt.recordSep.separator
								  : "");
	else if (strcmp(param, "recordsep_zero") == 0)
		return pstrdup(pset_bool_string(popt->topt.recordSep.separator_zero));
	else if (strcmp(param, "tableattr") == 0)
		return popt->topt.tableAttr ? pset_quoted_string(popt->topt.tableAttr) : pstrdup("");
	else if (strcmp(param, "title") == 0)
		return popt->title ? pset_quoted_string(popt->title) : pstrdup("");
	else if (strcmp(param, "tuples_only") == 0)
		return pstrdup(pset_bool_string(popt->topt.tuples_only));
	else if (strcmp(param, "unicode_border_linestyle") == 0)
		return pstrdup(_unicode_linestyle2string(popt->topt.unicode_border_linestyle));
	else if (strcmp(param, "unicode_column_linestyle") == 0)
		return pstrdup(_unicode_linestyle2string(popt->topt.unicode_column_linestyle));
	else if (strcmp(param, "unicode_header_linestyle") == 0)
		return pstrdup(_unicode_linestyle2string(popt->topt.unicode_header_linestyle));
	else
		return pstrdup("ERROR");
}



#ifndef WIN32
#define DEFAULT_SHELL "/bin/sh"
#else
/*
 *	CMD.EXE is in different places in different Win32 releases so we
 *	have to rely on the path to find it.
 */
#define DEFAULT_SHELL "cmd.exe"
#endif

static bool
do_shell(const char *command)
{
	int			result;

	if (!command)
	{
		char	   *sys;
		const char *shellName;

		shellName = getenv("SHELL");
#ifdef WIN32
		if (shellName == NULL)
			shellName = getenv("COMSPEC");
#endif
		if (shellName == NULL)
			shellName = DEFAULT_SHELL;

		/* See EDITOR handling comment for an explanation */
#ifndef WIN32
		sys = psprintf("exec %s", shellName);
#else
		sys = psprintf("\"%s\"", shellName);
#endif
		result = system(sys);
		free(sys);
	}
	else
		result = system(command);

	if (result == 127 || result == -1)
	{
		pg_log_error("\\!: failed");
		return false;
	}
	return true;
}

/*
 * do_watch -- handler for \watch
 *
 * We break this out of exec_command to avoid having to plaster "volatile"
 * onto a bunch of exec_command's variables to silence stupider compilers.
 */
static bool
do_watch(PQExpBuffer query_buf, double sleep)
{
	long		sleep_ms = (long) (sleep * 1000);
	printQueryOpt myopt = pset.popt;
	const char *strftime_fmt;
	const char *user_title;
	char	   *title;
	int			title_len;
	int			res = 0;

	if (!query_buf || query_buf->len <= 0)
	{
		pg_log_error("\\watch cannot be used with an empty query");
		return false;
	}

	/*
	 * Choose format for timestamps.  We might eventually make this a \pset
	 * option.  In the meantime, using a variable for the format suppresses
	 * overly-anal-retentive gcc warnings about %c being Y2K sensitive.
	 */
	strftime_fmt = "%c";

	/*
	 * Set up rendering options, in particular, disable the pager, because
	 * nobody wants to be prompted while watching the output of 'watch'.
	 */
	myopt.topt.pager = 0;

	/*
	 * If there's a title in the user configuration, make sure we have room
	 * for it in the title buffer.  Allow 128 bytes for the timestamp plus 128
	 * bytes for the rest.
	 */
	user_title = myopt.title;
	title_len = (user_title ? strlen(user_title) : 0) + 256;
	title = pg_malloc(title_len);

	for (;;)
	{
		time_t		timer;
		char		timebuf[128];
		long		i;

		/*
		 * Prepare title for output.  Note that we intentionally include a
		 * newline at the end of the title; this is somewhat historical but it
		 * makes for reasonably nicely formatted output in simple cases.
		 */
		timer = time(NULL);
		strftime(timebuf, sizeof(timebuf), strftime_fmt, localtime(&timer));

		if (user_title)
			snprintf(title, title_len, _("%s\t%s (every %gs)\n"),
					 user_title, timebuf, sleep);
		else
			snprintf(title, title_len, _("%s (every %gs)\n"),
					 timebuf, sleep);
		myopt.title = title;

		/* Run the query and print out the results */
		res = PSQLexecWatch(query_buf->data, &myopt);

		/*
		 * PSQLexecWatch handles the case where we can no longer repeat the
		 * query, and returns 0 or -1.
		 */
		if (res <= 0)
			break;

		/*
		 * Set up cancellation of 'watch' via SIGINT.  We redo this each time
		 * through the loop since it's conceivable something inside
		 * PSQLexecWatch could change sigint_interrupt_jmp.
		 */
		if (sigsetjmp(sigint_interrupt_jmp, 1) != 0)
			break;

		/*
		 * Enable 'watch' cancellations and wait a while before running the
		 * query again.  Break the sleep into short intervals (at most 1s)
		 * since pg_usleep isn't interruptible on some platforms.
		 */
		sigint_interrupt_enabled = true;
		i = sleep_ms;
		while (i > 0)
		{
			long		s = Min(i, 1000L);

			pg_usleep(s * 1000L);
			if (cancel_pressed)
				break;
			i -= s;
		}
		sigint_interrupt_enabled = false;
	}

	pg_free(title);
	return (res >= 0);
}

/*
 * a little code borrowed from PSQLexec() to manage ECHO_HIDDEN output.
 * returns true unless we have ECHO_HIDDEN_NOEXEC.
 */
static bool
echo_hidden_command(const char *query)
{
	if (pset.echo_hidden != PSQL_ECHO_HIDDEN_OFF)
	{
		printf(_("********* QUERY **********\n"
				 "%s\n"
				 "**************************\n\n"), query);
		fflush(stdout);
		if (pset.logfile)
		{
			fprintf(pset.logfile,
					_("********* QUERY **********\n"
					  "%s\n"
					  "**************************\n\n"), query);
			fflush(pset.logfile);
		}

		if (pset.echo_hidden == PSQL_ECHO_HIDDEN_NOEXEC)
			return false;
	}
	return true;
}

/*
 * Look up the object identified by obj_type and desc.  If successful,
 * store its OID in *obj_oid and return true, else return false.
 *
 * Note that we'll fail if the object doesn't exist OR if there are multiple
 * matching candidates OR if there's something syntactically wrong with the
 * object description; unfortunately it can be hard to tell the difference.
 */
static bool
lookup_object_oid(EditableObjectType obj_type, const char *desc,
				  Oid *obj_oid)
{
	bool		result = true;
	PQExpBuffer query = createPQExpBuffer();
	PGresult   *res;

	switch (obj_type)
	{
		case EditableFunction:

			/*
			 * We have a function description, e.g. "x" or "x(int)".  Issue a
			 * query to retrieve the function's OID using a cast to regproc or
			 * regprocedure (as appropriate).
			 */
			appendPQExpBufferStr(query, "SELECT ");
			appendStringLiteralConn(query, desc, pset.db);
			appendPQExpBuffer(query, "::pg_catalog.%s::pg_catalog.oid",
							  strchr(desc, '(') ? "regprocedure" : "regproc");
			break;

		case EditableView:

			/*
			 * Convert view name (possibly schema-qualified) to OID.  Note:
			 * this code doesn't check if the relation is actually a view.
			 * We'll detect that in get_create_object_cmd().
			 */
			appendPQExpBufferStr(query, "SELECT ");
			appendStringLiteralConn(query, desc, pset.db);
			appendPQExpBufferStr(query, "::pg_catalog.regclass::pg_catalog.oid");
			break;
	}

	if (!echo_hidden_command(query->data))
	{
		destroyPQExpBuffer(query);
		return false;
	}
	res = PQexec(pset.db, query->data);
	if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) == 1)
		*obj_oid = atooid(PQgetvalue(res, 0, 0));
	else
	{
		minimal_error_message(res);
		result = false;
	}

	PQclear(res);
	destroyPQExpBuffer(query);

	return result;
}

/*
 * Construct a "CREATE OR REPLACE ..." command that describes the specified
 * database object.  If successful, the result is stored in buf.
 */
static bool
get_create_object_cmd(EditableObjectType obj_type, Oid oid,
					  PQExpBuffer buf)
{
	bool		result = true;
	PQExpBuffer query = createPQExpBuffer();
	PGresult   *res;

	switch (obj_type)
	{
		case EditableFunction:
			printfPQExpBuffer(query,
							  "SELECT pg_catalog.pg_get_functiondef(%u)",
							  oid);
			break;

		case EditableView:

			/*
			 * pg_get_viewdef() just prints the query, so we must prepend
			 * CREATE for ourselves.  We must fully qualify the view name to
			 * ensure the right view gets replaced.  Also, check relation kind
			 * to be sure it's a view.
			 *
			 * Starting with 9.2, views may have reloptions (security_barrier)
			 * and from 9.4 onwards they may also have WITH [LOCAL|CASCADED]
			 * CHECK OPTION.  These are not part of the view definition
			 * returned by pg_get_viewdef() and so need to be retrieved
			 * separately.  Materialized views (introduced in 9.3) may have
			 * arbitrary storage parameter reloptions.
			 */
			if (pset.sversion >= 90400)
			{
				printfPQExpBuffer(query,
								  "SELECT nspname, relname, relkind, "
								  "pg_catalog.pg_get_viewdef(c.oid, true), "
								  "pg_catalog.array_remove(pg_catalog.array_remove(c.reloptions,'check_option=local'),'check_option=cascaded') AS reloptions, "
								  "CASE WHEN 'check_option=local' = ANY (c.reloptions) THEN 'LOCAL'::text "
								  "WHEN 'check_option=cascaded' = ANY (c.reloptions) THEN 'CASCADED'::text ELSE NULL END AS checkoption "
								  "FROM pg_catalog.pg_class c "
								  "LEFT JOIN pg_catalog.pg_namespace n "
								  "ON c.relnamespace = n.oid WHERE c.oid = %u",
								  oid);
			}
			else if (pset.sversion >= 90200)
			{
				printfPQExpBuffer(query,
								  "SELECT nspname, relname, relkind, "
								  "pg_catalog.pg_get_viewdef(c.oid, true), "
								  "c.reloptions AS reloptions, "
								  "NULL AS checkoption "
								  "FROM pg_catalog.pg_class c "
								  "LEFT JOIN pg_catalog.pg_namespace n "
								  "ON c.relnamespace = n.oid WHERE c.oid = %u",
								  oid);
			}
			else
			{
				printfPQExpBuffer(query,
								  "SELECT nspname, relname, relkind, "
								  "pg_catalog.pg_get_viewdef(c.oid, true), "
								  "NULL AS reloptions, "
								  "NULL AS checkoption "
								  "FROM pg_catalog.pg_class c "
								  "LEFT JOIN pg_catalog.pg_namespace n "
								  "ON c.relnamespace = n.oid WHERE c.oid = %u",
								  oid);
			}
			break;
	}

	if (!echo_hidden_command(query->data))
	{
		destroyPQExpBuffer(query);
		return false;
	}
	res = PQexec(pset.db, query->data);
	if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) == 1)
	{
		resetPQExpBuffer(buf);
		switch (obj_type)
		{
			case EditableFunction:
				appendPQExpBufferStr(buf, PQgetvalue(res, 0, 0));
				break;

			case EditableView:
				{
					char	   *nspname = PQgetvalue(res, 0, 0);
					char	   *relname = PQgetvalue(res, 0, 1);
					char	   *relkind = PQgetvalue(res, 0, 2);
					char	   *viewdef = PQgetvalue(res, 0, 3);
					char	   *reloptions = PQgetvalue(res, 0, 4);
					char	   *checkoption = PQgetvalue(res, 0, 5);

					/*
					 * If the backend ever supports CREATE OR REPLACE
					 * MATERIALIZED VIEW, allow that here; but as of today it
					 * does not, so editing a matview definition in this way
					 * is impossible.
					 */
					switch (relkind[0])
					{
#ifdef NOT_USED
						case RELKIND_MATVIEW:
							appendPQExpBufferStr(buf, "CREATE OR REPLACE MATERIALIZED VIEW ");
							break;
#endif
						case RELKIND_VIEW:
							appendPQExpBufferStr(buf, "CREATE OR REPLACE VIEW ");
							break;
						default:
							pg_log_error("\"%s.%s\" is not a view",
										 nspname, relname);
							result = false;
							break;
					}
					appendPQExpBuffer(buf, "%s.", fmtId(nspname));
					appendPQExpBufferStr(buf, fmtId(relname));

					/* reloptions, if not an empty array "{}" */
					if (reloptions != NULL && strlen(reloptions) > 2)
					{
						appendPQExpBufferStr(buf, "\n WITH (");
						if (!appendReloptionsArray(buf, reloptions, "",
												   pset.encoding,
												   standard_strings()))
						{
							pg_log_error("could not parse reloptions array");
							result = false;
						}
						appendPQExpBufferChar(buf, ')');
					}

					/* View definition from pg_get_viewdef (a SELECT query) */
					appendPQExpBuffer(buf, " AS\n%s", viewdef);

					/* Get rid of the semicolon that pg_get_viewdef appends */
					if (buf->len > 0 && buf->data[buf->len - 1] == ';')
						buf->data[--(buf->len)] = '\0';

					/* WITH [LOCAL|CASCADED] CHECK OPTION */
					if (checkoption && checkoption[0] != '\0')
						appendPQExpBuffer(buf, "\n WITH %s CHECK OPTION",
										  checkoption);
				}
				break;
		}
		/* Make sure result ends with a newline */
		if (buf->len > 0 && buf->data[buf->len - 1] != '\n')
			appendPQExpBufferChar(buf, '\n');
	}
	else
	{
		minimal_error_message(res);
		result = false;
	}

	PQclear(res);
	destroyPQExpBuffer(query);

	return result;
}

/*
 * If the given argument of \ef or \ev ends with a line number, delete the line
 * number from the argument string and return it as an integer.  (We need
 * this kluge because we're too lazy to parse \ef's function or \ev's view
 * argument carefully --- we just slop it up in OT_WHOLE_LINE mode.)
 *
 * Returns -1 if no line number is present, 0 on error, or a positive value
 * on success.
 */
static int
strip_lineno_from_objdesc(char *obj)
{
	char	   *c;
	int			lineno;

	if (!obj || obj[0] == '\0')
		return -1;

	c = obj + strlen(obj) - 1;

	/*
	 * This business of parsing backwards is dangerous as can be in a
	 * multibyte environment: there is no reason to believe that we are
	 * looking at the first byte of a character, nor are we necessarily
	 * working in a "safe" encoding.  Fortunately the bitpatterns we are
	 * looking for are unlikely to occur as non-first bytes, but beware of
	 * trying to expand the set of cases that can be recognized.  We must
	 * guard the <ctype.h> macros by using isascii() first, too.
	 */

	/* skip trailing whitespace */
	while (c > obj && isascii((unsigned char) *c) && isspace((unsigned char) *c))
		c--;

	/* must have a digit as last non-space char */
	if (c == obj || !isascii((unsigned char) *c) || !isdigit((unsigned char) *c))
		return -1;

	/* find start of digit string */
	while (c > obj && isascii((unsigned char) *c) && isdigit((unsigned char) *c))
		c--;

	/* digits must be separated from object name by space or closing paren */
	/* notice also that we are not allowing an empty object name ... */
	if (c == obj || !isascii((unsigned char) *c) ||
		!(isspace((unsigned char) *c) || *c == ')'))
		return -1;

	/* parse digit string */
	c++;
	lineno = atoi(c);
	if (lineno < 1)
	{
		pg_log_error("invalid line number: %s", c);
		return 0;
	}

	/* strip digit string from object name */
	*c = '\0';

	return lineno;
}

/*
 * Count number of lines in the buffer.
 * This is used to test if pager is needed or not.
 */
static int
count_lines_in_buf(PQExpBuffer buf)
{
	int			lineno = 0;
	const char *lines = buf->data;

	while (*lines != '\0')
	{
		lineno++;
		/* find start of next line */
		lines = strchr(lines, '\n');
		if (!lines)
			break;
		lines++;
	}

	return lineno;
}

/*
 * Write text at *lines to output with line numbers.
 *
 * If header_keyword isn't NULL, then line 1 should be the first line beginning
 * with header_keyword; lines before that are unnumbered.
 *
 * Caution: this scribbles on *lines.
 */
static void
print_with_linenumbers(FILE *output, char *lines,
					   const char *header_keyword)
{
	bool		in_header = (header_keyword != NULL);
	size_t		header_sz = in_header ? strlen(header_keyword) : 0;
	int			lineno = 0;

	while (*lines != '\0')
	{
		char	   *eol;

		if (in_header && strncmp(lines, header_keyword, header_sz) == 0)
			in_header = false;

		/* increment lineno only for body's lines */
		if (!in_header)
			lineno++;

		/* find and mark end of current line */
		eol = strchr(lines, '\n');
		if (eol != NULL)
			*eol = '\0';

		/* show current line as appropriate */
		if (in_header)
			fprintf(output, "        %s\n", lines);
		else
			fprintf(output, "%-7d %s\n", lineno, lines);

		/* advance to next line, if any */
		if (eol == NULL)
			break;
		lines = ++eol;
	}
}

/*
 * Report just the primary error; this is to avoid cluttering the output
 * with, for instance, a redisplay of the internally generated query
 */
static void
minimal_error_message(PGresult *res)
{
	PQExpBuffer msg;
	const char *fld;

	msg = createPQExpBuffer();

	fld = PQresultErrorField(res, PG_DIAG_SEVERITY);
	if (fld)
		printfPQExpBuffer(msg, "%s:  ", fld);
	else
		printfPQExpBuffer(msg, "ERROR:  ");
	fld = PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY);
	if (fld)
		appendPQExpBufferStr(msg, fld);
	else
		appendPQExpBufferStr(msg, "(not available)");
	appendPQExpBufferChar(msg, '\n');

	pg_log_error("%s", msg->data);

	destroyPQExpBuffer(msg);
}
