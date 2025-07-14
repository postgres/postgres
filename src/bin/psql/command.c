/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2025, PostgreSQL Global Development Group
 *
 * src/bin/psql/command.c
 */
#include "postgres_fe.h"

#include <ctype.h>
#include <time.h>
#include <pwd.h>
#include <utime.h>
#ifndef WIN32
#include <sys/stat.h>			/* for stat() */
#include <sys/time.h>			/* for setitimer() */
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
#include "common/string.h"
#include "copy.h"
#include "describe.h"
#include "fe_utils/cancel.h"
#include "fe_utils/print.h"
#include "fe_utils/string_utils.h"
#include "help.h"
#include "input.h"
#include "large_obj.h"
#include "libpq/pqcomm.h"
#include "mainloop.h"
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
	EditableView,
} EditableObjectType;

/* local function declarations */
static backslashResult exec_command(const char *cmd,
									PsqlScanState scan_state,
									ConditionalStack cstack,
									PQExpBuffer query_buf,
									PQExpBuffer previous_buf);
static backslashResult exec_command_a(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_bind(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_bind_named(PsqlScanState scan_state, bool active_branch,
											   const char *cmd);
static backslashResult exec_command_C(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_connect(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_cd(PsqlScanState scan_state, bool active_branch,
									   const char *cmd);
static backslashResult exec_command_close_prepared(PsqlScanState scan_state,
												   bool active_branch, const char *cmd);
static backslashResult exec_command_conninfo(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_copy(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_copyright(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_crosstabview(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_d(PsqlScanState scan_state, bool active_branch,
									  const char *cmd);
static bool exec_command_dfo(PsqlScanState scan_state, const char *cmd,
							 const char *pattern,
							 bool show_verbose, bool show_system);
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
static backslashResult exec_command_endpipeline(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_encoding(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_errverbose(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_f(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_flush(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_flushrequest(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_g(PsqlScanState scan_state, bool active_branch,
									  const char *cmd);
static backslashResult process_command_g_options(char *first_option,
												 PsqlScanState scan_state,
												 bool active_branch,
												 const char *cmd);
static backslashResult exec_command_gdesc(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_getenv(PsqlScanState scan_state, bool active_branch,
										   const char *cmd);
static backslashResult exec_command_gexec(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_getresults(PsqlScanState scan_state, bool active_branch);
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
static backslashResult exec_command_parse(PsqlScanState scan_state, bool active_branch,
										  const char *cmd);
static backslashResult exec_command_password(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_prompt(PsqlScanState scan_state, bool active_branch,
										   const char *cmd);
static backslashResult exec_command_pset(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_quit(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_reset(PsqlScanState scan_state, bool active_branch,
										  PQExpBuffer query_buf);
static backslashResult exec_command_s(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_sendpipeline(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_set(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_setenv(PsqlScanState scan_state, bool active_branch,
										   const char *cmd);
static backslashResult exec_command_sf_sv(PsqlScanState scan_state, bool active_branch,
										  const char *cmd, bool is_func);
static backslashResult exec_command_startpipeline(PsqlScanState scan_state, bool active_branch);
static backslashResult exec_command_syncpipeline(PsqlScanState scan_state, bool active_branch);
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
static backslashResult exec_command_z(PsqlScanState scan_state, bool active_branch,
									  const char *cmd);
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
static bool copy_previous_query(PQExpBuffer query_buf, PQExpBuffer previous_buf);
static bool do_connect(enum trivalue reuse_previous_specification,
					   char *dbname, char *user, char *host, char *port);
static void wait_until_connected(PGconn *conn);
static bool do_edit(const char *filename_arg, PQExpBuffer query_buf,
					int lineno, bool discard_on_quit, bool *edited);
static bool do_shell(const char *command);
static bool do_watch(PQExpBuffer query_buf, double sleep, int iter, int min_rows);
static bool lookup_object_oid(EditableObjectType obj_type, const char *desc,
							  Oid *obj_oid);
static bool get_create_object_cmd(EditableObjectType obj_type, Oid oid,
								  PQExpBuffer buf);
static int	strip_lineno_from_objdesc(char *obj);
static int	count_lines_in_buf(PQExpBuffer buf);
static void print_with_linenumbers(FILE *output, char *lines, bool is_func);
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
			pg_log_error_hint("Try \\? for help.");
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
 * commands return something else.  Failure result code is PSQL_CMD_ERROR,
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
	else if (strcmp(cmd, "bind") == 0)
		status = exec_command_bind(scan_state, active_branch);
	else if (strcmp(cmd, "bind_named") == 0)
		status = exec_command_bind_named(scan_state, active_branch, cmd);
	else if (strcmp(cmd, "C") == 0)
		status = exec_command_C(scan_state, active_branch);
	else if (strcmp(cmd, "c") == 0 || strcmp(cmd, "connect") == 0)
		status = exec_command_connect(scan_state, active_branch);
	else if (strcmp(cmd, "cd") == 0)
		status = exec_command_cd(scan_state, active_branch, cmd);
	else if (strcmp(cmd, "close_prepared") == 0)
		status = exec_command_close_prepared(scan_state, active_branch, cmd);
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
	else if (strcmp(cmd, "endpipeline") == 0)
		status = exec_command_endpipeline(scan_state, active_branch);
	else if (strcmp(cmd, "encoding") == 0)
		status = exec_command_encoding(scan_state, active_branch);
	else if (strcmp(cmd, "errverbose") == 0)
		status = exec_command_errverbose(scan_state, active_branch);
	else if (strcmp(cmd, "f") == 0)
		status = exec_command_f(scan_state, active_branch);
	else if (strcmp(cmd, "flush") == 0)
		status = exec_command_flush(scan_state, active_branch);
	else if (strcmp(cmd, "flushrequest") == 0)
		status = exec_command_flushrequest(scan_state, active_branch);
	else if (strcmp(cmd, "g") == 0 || strcmp(cmd, "gx") == 0)
		status = exec_command_g(scan_state, active_branch, cmd);
	else if (strcmp(cmd, "gdesc") == 0)
		status = exec_command_gdesc(scan_state, active_branch);
	else if (strcmp(cmd, "getenv") == 0)
		status = exec_command_getenv(scan_state, active_branch, cmd);
	else if (strcmp(cmd, "getresults") == 0)
		status = exec_command_getresults(scan_state, active_branch);
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
			 strcmp(cmd, "lx") == 0 || strcmp(cmd, "listx") == 0 ||
			 strcmp(cmd, "l+") == 0 || strcmp(cmd, "list+") == 0 ||
			 strcmp(cmd, "lx+") == 0 || strcmp(cmd, "listx+") == 0 ||
			 strcmp(cmd, "l+x") == 0 || strcmp(cmd, "list+x") == 0)
		status = exec_command_list(scan_state, active_branch, cmd);
	else if (strncmp(cmd, "lo_", 3) == 0)
		status = exec_command_lo(scan_state, active_branch, cmd);
	else if (strcmp(cmd, "o") == 0 || strcmp(cmd, "out") == 0)
		status = exec_command_out(scan_state, active_branch);
	else if (strcmp(cmd, "p") == 0 || strcmp(cmd, "print") == 0)
		status = exec_command_print(scan_state, active_branch,
									query_buf, previous_buf);
	else if (strcmp(cmd, "parse") == 0)
		status = exec_command_parse(scan_state, active_branch, cmd);
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
	else if (strcmp(cmd, "sendpipeline") == 0)
		status = exec_command_sendpipeline(scan_state, active_branch);
	else if (strcmp(cmd, "set") == 0)
		status = exec_command_set(scan_state, active_branch);
	else if (strcmp(cmd, "setenv") == 0)
		status = exec_command_setenv(scan_state, active_branch, cmd);
	else if (strcmp(cmd, "sf") == 0 || strcmp(cmd, "sf+") == 0)
		status = exec_command_sf_sv(scan_state, active_branch, cmd, true);
	else if (strcmp(cmd, "sv") == 0 || strcmp(cmd, "sv+") == 0)
		status = exec_command_sf_sv(scan_state, active_branch, cmd, false);
	else if (strcmp(cmd, "startpipeline") == 0)
		status = exec_command_startpipeline(scan_state, active_branch);
	else if (strcmp(cmd, "syncpipeline") == 0)
		status = exec_command_syncpipeline(scan_state, active_branch);
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
	else if (strcmp(cmd, "z") == 0 ||
			 strcmp(cmd, "zS") == 0 || strcmp(cmd, "zx") == 0 ||
			 strcmp(cmd, "zSx") == 0 || strcmp(cmd, "zxS") == 0)
		status = exec_command_z(scan_state, active_branch, cmd);
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
		(void) copy_previous_query(query_buf, previous_buf);

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
 * \bind -- set query parameters
 */
static backslashResult
exec_command_bind(PsqlScanState scan_state, bool active_branch)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;

	if (active_branch)
	{
		char	   *opt;
		int			nparams = 0;
		int			nalloc = 0;

		clean_extended_state();

		while ((opt = psql_scan_slash_option(scan_state, OT_NORMAL, NULL, false)))
		{
			nparams++;
			if (nparams > nalloc)
			{
				nalloc = nalloc ? nalloc * 2 : 1;
				pset.bind_params = pg_realloc_array(pset.bind_params, char *, nalloc);
			}
			pset.bind_params[nparams - 1] = opt;
		}

		pset.bind_nparams = nparams;
		pset.send_mode = PSQL_SEND_EXTENDED_QUERY_PARAMS;
	}
	else
		ignore_slash_options(scan_state);

	return status;
}

/*
 * \bind_named -- set query parameters for an existing prepared statement
 */
static backslashResult
exec_command_bind_named(PsqlScanState scan_state, bool active_branch,
						const char *cmd)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;

	if (active_branch)
	{
		char	   *opt;
		int			nparams = 0;
		int			nalloc = 0;

		clean_extended_state();

		/* get the mandatory prepared statement name */
		opt = psql_scan_slash_option(scan_state, OT_NORMAL, NULL, false);
		if (!opt)
		{
			pg_log_error("\\%s: missing required argument", cmd);
			status = PSQL_CMD_ERROR;
		}
		else
		{
			pset.stmtName = opt;
			pset.send_mode = PSQL_SEND_EXTENDED_QUERY_PREPARED;

			/* set of parameters */
			while ((opt = psql_scan_slash_option(scan_state, OT_NORMAL, NULL, false)))
			{
				nparams++;
				if (nparams > nalloc)
				{
					nalloc = nalloc ? nalloc * 2 : 1;
					pset.bind_params = pg_realloc_array(pset.bind_params, char *, nalloc);
				}
				pset.bind_params[nparams - 1] = opt;
			}
			pset.bind_nparams = nparams;
		}
	}
	else
		ignore_slash_options(scan_state);

	return status;
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
			/* This should match get_home_path() */
			dir = getenv("HOME");
			if (dir == NULL || dir[0] == '\0')
			{
				uid_t		user_id = geteuid();
				struct passwd *pw;

				errno = 0;		/* clear errno before call */
				pw = getpwuid(user_id);
				if (pw)
					dir = pw->pw_dir;
				else
				{
					pg_log_error("could not get home directory for user ID %ld: %s",
								 (long) user_id,
								 errno ? strerror(errno) : _("user does not exist"));
					success = false;
				}
			}
#else							/* WIN32 */

			/*
			 * On Windows, 'cd' without arguments prints the current
			 * directory, so if someone wants to code this here instead...
			 */
			dir = "/";
#endif							/* WIN32 */
		}

		if (success &&
			chdir(dir) < 0)
		{
			pg_log_error("\\%s: could not change directory to \"%s\": %m",
						 cmd, dir);
			success = false;
		}

		free(opt);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \close_prepared -- close a previously prepared statement
 */
static backslashResult
exec_command_close_prepared(PsqlScanState scan_state, bool active_branch, const char *cmd)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;

	if (active_branch)
	{
		char	   *opt = psql_scan_slash_option(scan_state,
												 OT_NORMAL, NULL, false);

		clean_extended_state();

		if (!opt)
		{
			pg_log_error("\\%s: missing required argument", cmd);
			status = PSQL_CMD_ERROR;
		}
		else
		{
			pset.stmtName = opt;
			pset.send_mode = PSQL_SEND_EXTENDED_CLOSE;
			status = PSQL_CMD_SEND;
		}
	}
	else
		ignore_slash_options(scan_state);

	return status;
}

/*
 * \conninfo -- display information about the current connection
 */
static backslashResult
exec_command_conninfo(PsqlScanState scan_state, bool active_branch)
{
	printTableContent cont;
	int			rows,
				cols;
	char	   *db;
	char	   *host;
	bool		print_hostaddr;
	char	   *hostaddr;
	char	   *protocol_version,
			   *backend_pid;
	int			ssl_in_use,
				password_used,
				gssapi_used;
	int			version_num;
	char	   *paramval;

	if (!active_branch)
		return PSQL_CMD_SKIP_LINE;

	db = PQdb(pset.db);
	if (db == NULL)
	{
		printf(_("You are currently not connected to a database.\n"));
		return PSQL_CMD_SKIP_LINE;
	}

	/* Get values for the parameters */
	host = PQhost(pset.db);
	hostaddr = PQhostaddr(pset.db);
	version_num = PQfullProtocolVersion(pset.db);
	protocol_version = psprintf("%d.%d", version_num / 10000,
								version_num % 10000);
	ssl_in_use = PQsslInUse(pset.db);
	password_used = PQconnectionUsedPassword(pset.db);
	gssapi_used = PQconnectionUsedGSSAPI(pset.db);
	backend_pid = psprintf("%d", PQbackendPID(pset.db));

	/* Only print hostaddr if it differs from host, and not if unixsock */
	print_hostaddr = (!is_unixsock_path(host) &&
					  hostaddr && *hostaddr && strcmp(host, hostaddr) != 0);

	/* Determine the exact number of rows to print */
	rows = 12;
	cols = 2;
	if (ssl_in_use)
		rows += 6;
	if (print_hostaddr)
		rows++;

	/* Set it all up */
	printTableInit(&cont, &pset.popt.topt, _("Connection Information"), cols, rows);
	printTableAddHeader(&cont, _("Parameter"), true, 'l');
	printTableAddHeader(&cont, _("Value"), true, 'l');

	/* Database */
	printTableAddCell(&cont, _("Database"), false, false);
	printTableAddCell(&cont, db, false, false);

	/* Client User */
	printTableAddCell(&cont, _("Client User"), false, false);
	printTableAddCell(&cont, PQuser(pset.db), false, false);

	/* Host/hostaddr/socket */
	if (is_unixsock_path(host))
	{
		/* hostaddr if specified overrides socket, so suppress the latter */
		if (hostaddr && *hostaddr)
		{
			printTableAddCell(&cont, _("Host Address"), false, false);
			printTableAddCell(&cont, hostaddr, false, false);
		}
		else
		{
			printTableAddCell(&cont, _("Socket Directory"), false, false);
			printTableAddCell(&cont, host, false, false);
		}
	}
	else
	{
		printTableAddCell(&cont, _("Host"), false, false);
		printTableAddCell(&cont, host, false, false);
		if (print_hostaddr)
		{
			printTableAddCell(&cont, _("Host Address"), false, false);
			printTableAddCell(&cont, hostaddr, false, false);
		}
	}

	/* Server Port */
	printTableAddCell(&cont, _("Server Port"), false, false);
	printTableAddCell(&cont, PQport(pset.db), false, false);

	/* Options */
	printTableAddCell(&cont, _("Options"), false, false);
	printTableAddCell(&cont, PQoptions(pset.db), false, false);

	/* Protocol Version */
	printTableAddCell(&cont, _("Protocol Version"), false, false);
	printTableAddCell(&cont, protocol_version, false, false);

	/* Password Used */
	printTableAddCell(&cont, _("Password Used"), false, false);
	printTableAddCell(&cont, password_used ? _("true") : _("false"), false, false);

	/* GSSAPI Authenticated */
	printTableAddCell(&cont, _("GSSAPI Authenticated"), false, false);
	printTableAddCell(&cont, gssapi_used ? _("true") : _("false"), false, false);

	/* Backend PID */
	printTableAddCell(&cont, _("Backend PID"), false, false);
	printTableAddCell(&cont, backend_pid, false, false);

	/* SSL Connection */
	printTableAddCell(&cont, _("SSL Connection"), false, false);
	printTableAddCell(&cont, ssl_in_use ? _("true") : _("false"), false, false);

	/* SSL Information */
	if (ssl_in_use)
	{
		char	   *library,
				   *protocol,
				   *key_bits,
				   *cipher,
				   *compression,
				   *alpn;

		library = (char *) PQsslAttribute(pset.db, "library");
		protocol = (char *) PQsslAttribute(pset.db, "protocol");
		key_bits = (char *) PQsslAttribute(pset.db, "key_bits");
		cipher = (char *) PQsslAttribute(pset.db, "cipher");
		compression = (char *) PQsslAttribute(pset.db, "compression");
		alpn = (char *) PQsslAttribute(pset.db, "alpn");

		printTableAddCell(&cont, _("SSL Library"), false, false);
		printTableAddCell(&cont, library ? library : _("unknown"), false, false);

		printTableAddCell(&cont, _("SSL Protocol"), false, false);
		printTableAddCell(&cont, protocol ? protocol : _("unknown"), false, false);

		printTableAddCell(&cont, _("SSL Key Bits"), false, false);
		printTableAddCell(&cont, key_bits ? key_bits : _("unknown"), false, false);

		printTableAddCell(&cont, _("SSL Cipher"), false, false);
		printTableAddCell(&cont, cipher ? cipher : _("unknown"), false, false);

		printTableAddCell(&cont, _("SSL Compression"), false, false);
		printTableAddCell(&cont, (compression && strcmp(compression, "off") != 0) ?
						  _("true") : _("false"), false, false);

		printTableAddCell(&cont, _("ALPN"), false, false);
		printTableAddCell(&cont, (alpn && alpn[0] != '\0') ? alpn : _("none"), false, false);
	}

	paramval = (char *) PQparameterStatus(pset.db, "is_superuser");
	printTableAddCell(&cont, "Superuser", false, false);
	printTableAddCell(&cont, paramval ? paramval : _("unknown"), false, false);

	paramval = (char *) PQparameterStatus(pset.db, "in_hot_standby");
	printTableAddCell(&cont, "Hot Standby", false, false);
	printTableAddCell(&cont, paramval ? paramval : _("unknown"), false, false);

	printTable(&cont, pset.queryFout, false, pset.logfile);
	printTableCleanup(&cont);

	pfree(protocol_version);
	pfree(backend_pid);

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
 * \crosstabview -- execute a query and display result in crosstab
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
		unsigned short int save_expanded;

		/* We don't do SQLID reduction on the pattern yet */
		pattern = psql_scan_slash_option(scan_state,
										 OT_NORMAL, NULL, true);

		show_verbose = strchr(cmd, '+') ? true : false;
		show_system = strchr(cmd, 'S') ? true : false;

		/*
		 * The 'x' option turns expanded mode on for this command only. This
		 * is allowed in all \d* commands, except \d by itself, since \dx is a
		 * separate command. So the 'x' option cannot appear immediately after
		 * \d, but it can appear after \d followed by other options.
		 */
		save_expanded = pset.popt.topt.expanded;
		if (cmd[1] != '\0' && strchr(&cmd[2], 'x'))
			pset.popt.topt.expanded = 1;

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

					if (pattern && cmd[2] != '\0' && cmd[2] != '+' && cmd[2] != 'x')
						pattern2 = psql_scan_slash_option(scan_state, OT_NORMAL, NULL, true);

					switch (cmd[2])
					{
						case '\0':
						case '+':
						case 'x':
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
				if (strncmp(cmd, "dconfig", 7) == 0)
					success = describeConfigurationParameters(pattern,
															  show_verbose,
															  show_system);
				else
					success = listConversions(pattern,
											  show_verbose,
											  show_system);
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
					case 'x':
						success = exec_command_dfo(scan_state, cmd, pattern,
												   show_verbose, show_system);
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
				success = listLargeObjects(show_verbose);
				break;
			case 'L':
				success = listLanguages(pattern, show_verbose, show_system);
				break;
			case 'n':
				success = listSchemas(pattern, show_verbose, show_system);
				break;
			case 'o':
				success = exec_command_dfo(scan_state, cmd, pattern,
										   show_verbose, show_system);
				break;
			case 'O':
				success = listCollations(pattern, show_verbose, show_system);
				break;
			case 'p':
				success = permissionsList(pattern, show_system);
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
						case 'x':
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

					free(pattern2);
				}
				else if (cmd[2] == 'g')
					success = describeRoleGrants(pattern, show_system);
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
					case 'x':
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
			case 'X':			/* Extended Statistics */
				success = listExtendedStats(pattern);
				break;
			case 'y':			/* Event Triggers */
				success = listEventTriggers(pattern, show_verbose);
				break;
			default:
				status = PSQL_CMD_UNKNOWN;
		}

		/* Restore original expanded mode */
		pset.popt.topt.expanded = save_expanded;

		free(pattern);
	}
	else
		ignore_slash_options(scan_state);

	if (!success)
		status = PSQL_CMD_ERROR;

	return status;
}

/* \df and \do; messy enough to split out of exec_command_d */
static bool
exec_command_dfo(PsqlScanState scan_state, const char *cmd,
				 const char *pattern,
				 bool show_verbose, bool show_system)
{
	bool		success;
	char	   *arg_patterns[FUNC_MAX_ARGS];
	int			num_arg_patterns = 0;

	/* Collect argument-type patterns too */
	if (pattern)				/* otherwise it was just \df or \do */
	{
		char	   *ap;

		while ((ap = psql_scan_slash_option(scan_state,
											OT_NORMAL, NULL, true)) != NULL)
		{
			arg_patterns[num_arg_patterns++] = ap;
			if (num_arg_patterns >= FUNC_MAX_ARGS)
				break;			/* protect limited-size array */
		}
	}

	if (cmd[1] == 'f')
		success = describeFunctions(&cmd[2], pattern,
									arg_patterns, num_arg_patterns,
									show_verbose, show_system);
	else
		success = describeOperators(pattern,
									arg_patterns, num_arg_patterns,
									show_verbose, show_system);

	while (--num_arg_patterns >= 0)
		free(arg_patterns[num_arg_patterns]);

	return success;
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
				bool		discard_on_quit;

				expand_tilde(&fname);
				if (fname)
				{
					canonicalize_path_enc(fname, pset.encoding);
					/* Always clear buffer if the file isn't modified */
					discard_on_quit = true;
				}
				else
				{
					/*
					 * If query_buf is empty, recall previous query for
					 * editing.  But in that case, the query buffer should be
					 * emptied if editing doesn't modify the file.
					 */
					discard_on_quit = copy_previous_query(query_buf,
														  previous_buf);
				}

				if (do_edit(fname, query_buf, lineno, discard_on_quit, NULL))
					status = PSQL_CMD_NEWEDIT;
				else
					status = PSQL_CMD_ERROR;
			}

			/*
			 * On error while editing or if specifying an incorrect line
			 * number, reset the query buffer.
			 */
			if (status == PSQL_CMD_ERROR)
				resetPQExpBuffer(query_buf);

			free(fname);
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

		if (!query_buf)
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
				 * emit that on a line beginning with "AS ", "BEGIN ", or
				 * "RETURN ", and that there can be no such line before the
				 * real start of the function body.  Increment lineno by the
				 * number of lines before that line, so that it becomes
				 * relative to the first line of the function definition.
				 */
				const char *lines = query_buf->data;

				while (*lines != '\0')
				{
					if (strncmp(lines, "AS ", 3) == 0 ||
						strncmp(lines, "BEGIN ", 6) == 0 ||
						strncmp(lines, "RETURN ", 7) == 0)
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

			if (!do_edit(NULL, query_buf, lineno, true, &edited))
				status = PSQL_CMD_ERROR;
			else if (!edited)
				puts(_("No changes"));
			else
				status = PSQL_CMD_NEWEDIT;
		}

		/*
		 * On error while doing object lookup or while editing, or if
		 * specifying an incorrect line number, reset the query buffer.
		 */
		if (status == PSQL_CMD_ERROR)
			resetPQExpBuffer(query_buf);

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
				setFmtEncoding(pset.encoding);
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
 * \flush -- call PQflush() on the connection
 */
static backslashResult
exec_command_flush(PsqlScanState scan_state, bool active_branch)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;

	if (active_branch)
	{
		pset.send_mode = PSQL_SEND_FLUSH;
		status = PSQL_CMD_SEND;
	}
	else
		ignore_slash_options(scan_state);

	return status;
}

/*
 * \flushrequest -- call PQsendFlushRequest() on the connection
 */
static backslashResult
exec_command_flushrequest(PsqlScanState scan_state, bool active_branch)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;

	if (active_branch)
	{
		pset.send_mode = PSQL_SEND_FLUSH_REQUEST;
		status = PSQL_CMD_SEND;
	}
	else
		ignore_slash_options(scan_state);

	return status;
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
		if (PQpipelineStatus(pset.db) != PQ_PIPELINE_OFF)
		{
			pg_log_error("\\%s not allowed in pipeline mode", cmd);
			clean_extended_state();
			free(fname);
			return PSQL_CMD_ERROR;
		}

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
 * \getenv -- set variable from environment variable
 */
static backslashResult
exec_command_getenv(PsqlScanState scan_state, bool active_branch,
					const char *cmd)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *myvar = psql_scan_slash_option(scan_state,
												   OT_NORMAL, NULL, false);
		char	   *envvar = psql_scan_slash_option(scan_state,
													OT_NORMAL, NULL, false);

		if (!myvar || !envvar)
		{
			pg_log_error("\\%s: missing required argument", cmd);
			success = false;
		}
		else
		{
			char	   *envval = getenv(envvar);

			if (envval && !SetVariable(pset.vars, myvar, envval))
				success = false;
		}
		free(myvar);
		free(envvar);
	}
	else
		ignore_slash_options(scan_state);

	return success ? PSQL_CMD_SKIP_LINE : PSQL_CMD_ERROR;
}

/*
 * \getresults -- read results
 */
static backslashResult
exec_command_getresults(PsqlScanState scan_state, bool active_branch)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;

	if (active_branch)
	{
		char	   *opt;
		int			num_results;

		pset.send_mode = PSQL_SEND_GET_RESULTS;
		status = PSQL_CMD_SEND;
		opt = psql_scan_slash_option(scan_state, OT_NORMAL, NULL, false);

		pset.requested_results = 0;
		if (opt != NULL)
		{
			num_results = atoi(opt);
			if (num_results < 0)
			{
				pg_log_error("\\getresults: invalid number of requested results");
				return PSQL_CMD_ERROR;
			}
			pset.requested_results = num_results;
		}
	}
	else
		ignore_slash_options(scan_state);

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
		if (PQpipelineStatus(pset.db) != PQ_PIPELINE_OFF)
		{
			pg_log_error("\\%s not allowed in pipeline mode", "gexec");
			clean_extended_state();
			return PSQL_CMD_ERROR;
		}
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

		if (PQpipelineStatus(pset.db) != PQ_PIPELINE_OFF)
		{
			pg_log_error("\\%s not allowed in pipeline mode", "gset");
			clean_extended_state();
			return PSQL_CMD_ERROR;
		}

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
												 OT_WHOLE_LINE, NULL, true);

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
		unsigned short int save_expanded;

		pattern = psql_scan_slash_option(scan_state,
										 OT_NORMAL, NULL, true);

		show_verbose = strchr(cmd, '+') ? true : false;

		/* if 'x' option specified, force expanded mode */
		save_expanded = pset.popt.topt.expanded;
		if (strchr(cmd, 'x'))
			pset.popt.topt.expanded = 1;

		success = listAllDbs(pattern, show_verbose);

		/* restore original expanded mode */
		pset.popt.topt.expanded = save_expanded;

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

		else if (strncmp(cmd + 3, "list", 4) == 0)
		{
			bool		show_verbose;
			unsigned short int save_expanded;

			show_verbose = strchr(cmd, '+') ? true : false;

			/* if 'x' option specified, force expanded mode */
			save_expanded = pset.popt.topt.expanded;
			if (strchr(cmd, 'x'))
				pset.popt.topt.expanded = 1;

			success = listLargeObjects(show_verbose);

			/* restore original expanded mode */
			pset.popt.topt.expanded = save_expanded;
		}

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
 * \parse -- parse query
 */
static backslashResult
exec_command_parse(PsqlScanState scan_state, bool active_branch,
				   const char *cmd)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;

	if (active_branch)
	{
		char	   *opt = psql_scan_slash_option(scan_state,
												 OT_NORMAL, NULL, false);

		clean_extended_state();

		if (!opt)
		{
			pg_log_error("\\%s: missing required argument", cmd);
			status = PSQL_CMD_ERROR;
		}
		else
		{
			pset.stmtName = opt;
			pset.send_mode = PSQL_SEND_EXTENDED_PARSE;
			status = PSQL_CMD_SEND;
		}
	}
	else
		ignore_slash_options(scan_state);

	return status;
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
		char	   *user = psql_scan_slash_option(scan_state,
												  OT_SQLID, NULL, true);
		char	   *pw1 = NULL;
		char	   *pw2 = NULL;
		PQExpBufferData buf;
		PromptInterruptContext prompt_ctx;

		if (user == NULL)
		{
			/* By default, the command applies to CURRENT_USER */
			PGresult   *res;

			res = PSQLexec("SELECT CURRENT_USER");
			if (!res)
				return PSQL_CMD_ERROR;

			user = pg_strdup(PQgetvalue(res, 0, 0));
			PQclear(res);
		}

		/* Set up to let SIGINT cancel simple_prompt_extended() */
		prompt_ctx.jmpbuf = sigint_interrupt_jmp;
		prompt_ctx.enabled = &sigint_interrupt_enabled;
		prompt_ctx.canceled = false;

		initPQExpBuffer(&buf);
		printfPQExpBuffer(&buf, _("Enter new password for user \"%s\": "), user);

		pw1 = simple_prompt_extended(buf.data, false, &prompt_ctx);
		if (!prompt_ctx.canceled)
			pw2 = simple_prompt_extended("Enter it again: ", false, &prompt_ctx);

		if (prompt_ctx.canceled)
		{
			/* fail silently */
			success = false;
		}
		else if (strcmp(pw1, pw2) != 0)
		{
			pg_log_error("Passwords didn't match.");
			success = false;
		}
		else
		{
			PGresult   *res = PQchangePassword(pset.db, user, pw1);

			if (PQresultStatus(res) != PGRES_COMMAND_OK)
			{
				pg_log_info("%s", PQerrorMessage(pset.db));
				success = false;
			}

			PQclear(res);
		}

		free(user);
		free(pw1);
		free(pw2);
		termPQExpBuffer(&buf);
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
			PromptInterruptContext prompt_ctx;

			/* Set up to let SIGINT cancel simple_prompt_extended() */
			prompt_ctx.jmpbuf = sigint_interrupt_jmp;
			prompt_ctx.enabled = &sigint_interrupt_enabled;
			prompt_ctx.canceled = false;

			if (arg2)
			{
				prompt_text = arg1;
				opt = arg2;
			}
			else
				opt = arg1;

			if (!pset.inputfile)
			{
				result = simple_prompt_extended(prompt_text, true, &prompt_ctx);
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

			if (prompt_ctx.canceled ||
				(result && !SetVariable(pset.vars, opt, result)))
				success = false;

			free(result);
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
				"xheader_width",
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
 * \sendpipeline -- send an extended query to an ongoing pipeline
 */
static backslashResult
exec_command_sendpipeline(PsqlScanState scan_state, bool active_branch)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;

	if (active_branch)
	{
		if (PQpipelineStatus(pset.db) != PQ_PIPELINE_OFF)
		{
			if (pset.send_mode == PSQL_SEND_EXTENDED_QUERY_PREPARED ||
				pset.send_mode == PSQL_SEND_EXTENDED_QUERY_PARAMS)
			{
				status = PSQL_CMD_SEND;
			}
			else
			{
				pg_log_error("\\sendpipeline must be used after \\bind or \\bind_named");
				clean_extended_state();
				return PSQL_CMD_ERROR;
			}
		}
		else
		{
			pg_log_error("\\sendpipeline not allowed outside of pipeline mode");
			clean_extended_state();
			return PSQL_CMD_ERROR;
		}
	}
	else
		ignore_slash_options(scan_state);

	return status;
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
			setenv(envvar, envval, 1);
			success = true;
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
		if (!obj_desc)
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
				/* add line numbers */
				print_with_linenumbers(output, buf->data, is_func);
			}
			else
			{
				/* just send the definition to output */
				fputs(buf->data, output);
			}

			if (is_pager)
				ClosePager(output);
		}

		free(obj_desc);
		destroyPQExpBuffer(buf);
	}
	else
		ignore_slash_whole_line(scan_state);

	return status;
}

/*
 * \startpipeline -- enter pipeline mode
 */
static backslashResult
exec_command_startpipeline(PsqlScanState scan_state, bool active_branch)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;

	if (active_branch)
	{
		pset.send_mode = PSQL_SEND_START_PIPELINE_MODE;
		status = PSQL_CMD_SEND;
	}
	else
		ignore_slash_options(scan_state);

	return status;
}

/*
 * \syncpipeline -- send a sync message to an active pipeline
 */
static backslashResult
exec_command_syncpipeline(PsqlScanState scan_state, bool active_branch)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;

	if (active_branch)
	{
		pset.send_mode = PSQL_SEND_PIPELINE_SYNC;
		status = PSQL_CMD_SEND;
	}
	else
		ignore_slash_options(scan_state);

	return status;
}

/*
 * \endpipeline -- end pipeline mode
 */
static backslashResult
exec_command_endpipeline(PsqlScanState scan_state, bool active_branch)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;

	if (active_branch)
	{
		pset.send_mode = PSQL_SEND_END_PIPELINE_MODE;
		status = PSQL_CMD_SEND;
	}
	else
		ignore_slash_options(scan_state);

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
					fflush(NULL);
					disable_sigpipe_trap();
					fd = popen(&fname[1], "w");
				}
				else
				{
					canonicalize_path_enc(fname, pset.encoding);
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
			{
				result = pclose(fd);

				if (result != 0)
				{
					pg_log_error("%s: %s", fname, wait_result_to_str(result));
					status = PSQL_CMD_ERROR;
				}
				SetShellResultVariables(result);
			}
			else
			{
				result = fclose(fd);

				if (result == EOF)
				{
					pg_log_error("%s: %m", fname);
					status = PSQL_CMD_ERROR;
				}
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
 * \watch -- execute a query every N seconds.
 * Optionally, stop after M iterations.
 */
static backslashResult
exec_command_watch(PsqlScanState scan_state, bool active_branch,
				   PQExpBuffer query_buf, PQExpBuffer previous_buf)
{
	bool		success = true;

	if (active_branch)
	{
		bool		have_sleep = false;
		bool		have_iter = false;
		bool		have_min_rows = false;
		double		sleep = pset.watch_interval;
		int			iter = 0;
		int			min_rows = 0;

		if (PQpipelineStatus(pset.db) != PQ_PIPELINE_OFF)
		{
			pg_log_error("\\%s not allowed in pipeline mode", "watch");
			clean_extended_state();
			success = false;
		}

		/*
		 * Parse arguments.  We allow either an unlabeled interval or
		 * "name=value", where name is from the set ('i', 'interval', 'c',
		 * 'count', 'm', 'min_rows').  The parsing of interval value should be
		 * kept in sync with ParseVariableDouble which is used for setting the
		 * default interval value.
		 */
		while (success)
		{
			char	   *opt = psql_scan_slash_option(scan_state,
													 OT_NORMAL, NULL, true);
			char	   *valptr;
			char	   *opt_end;

			if (!opt)
				break;			/* no more arguments */

			valptr = strchr(opt, '=');
			if (valptr)
			{
				/* Labeled argument */
				valptr++;
				if (strncmp("i=", opt, strlen("i=")) == 0 ||
					strncmp("interval=", opt, strlen("interval=")) == 0)
				{
					if (have_sleep)
					{
						pg_log_error("\\watch: interval value is specified more than once");
						success = false;
					}
					else
					{
						have_sleep = true;
						errno = 0;
						sleep = strtod(valptr, &opt_end);
						if (sleep < 0 || *opt_end || errno == ERANGE)
						{
							pg_log_error("\\watch: incorrect interval value \"%s\"", valptr);
							success = false;
						}
					}
				}
				else if (strncmp("c=", opt, strlen("c=")) == 0 ||
						 strncmp("count=", opt, strlen("count=")) == 0)
				{
					if (have_iter)
					{
						pg_log_error("\\watch: iteration count is specified more than once");
						success = false;
					}
					else
					{
						have_iter = true;
						errno = 0;
						iter = strtoint(valptr, &opt_end, 10);
						if (iter <= 0 || *opt_end || errno == ERANGE)
						{
							pg_log_error("\\watch: incorrect iteration count \"%s\"", valptr);
							success = false;
						}
					}
				}
				else if (strncmp("m=", opt, strlen("m=")) == 0 ||
						 strncmp("min_rows=", opt, strlen("min_rows=")) == 0)
				{
					if (have_min_rows)
					{
						pg_log_error("\\watch: minimum row count specified more than once");
						success = false;
					}
					else
					{
						have_min_rows = true;
						errno = 0;
						min_rows = strtoint(valptr, &opt_end, 10);
						if (min_rows <= 0 || *opt_end || errno == ERANGE)
						{
							pg_log_error("\\watch: incorrect minimum row count \"%s\"", valptr);
							success = false;
						}
					}
				}
				else
				{
					pg_log_error("\\watch: unrecognized parameter \"%s\"", opt);
					success = false;
				}
			}
			else
			{
				/* Unlabeled argument: take it as interval */
				if (have_sleep)
				{
					pg_log_error("\\watch: interval value is specified more than once");
					success = false;
				}
				else
				{
					have_sleep = true;
					errno = 0;
					sleep = strtod(opt, &opt_end);
					if (sleep < 0 || *opt_end || errno == ERANGE)
					{
						pg_log_error("\\watch: incorrect interval value \"%s\"", opt);
						success = false;
					}
				}
			}

			free(opt);
		}

		/* If we parsed arguments successfully, do the command */
		if (success)
		{
			/* If query_buf is empty, recall and execute previous query */
			(void) copy_previous_query(query_buf, previous_buf);

			success = do_watch(query_buf, sleep, iter, min_rows);
		}

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
exec_command_z(PsqlScanState scan_state, bool active_branch, const char *cmd)
{
	bool		success = true;

	if (active_branch)
	{
		char	   *pattern;
		bool		show_system;
		unsigned short int save_expanded;

		pattern = psql_scan_slash_option(scan_state,
										 OT_NORMAL, NULL, true);

		show_system = strchr(cmd, 'S') ? true : false;

		/* if 'x' option specified, force expanded mode */
		save_expanded = pset.popt.topt.expanded;
		if (strchr(cmd, 'x'))
			pset.popt.topt.expanded = 1;

		success = permissionsList(pattern, show_system);

		/* restore original expanded mode */
		pset.popt.topt.expanded = save_expanded;

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

	free(arg);
}

/*
 * Read and discard whole-line slash command argument.
 *
 * This *MUST* be used for inactive-branch processing of any slash command
 * that takes an OT_WHOLE_LINE option.  Otherwise we might consume a different
 * amount of option text in active and inactive cases.
 *
 * Note: although callers might pass "semicolon" as either true or false,
 * we need not duplicate that here, since it doesn't affect the amount of
 * input text consumed.
 */
static void
ignore_slash_whole_line(PsqlScanState scan_state)
{
	char	   *arg = psql_scan_slash_option(scan_state,
											 OT_WHOLE_LINE, NULL, false);

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
 *
 * Returns "true" if the previous query was copied into the query
 * buffer, else "false".
 */
static bool
copy_previous_query(PQExpBuffer query_buf, PQExpBuffer previous_buf)
{
	if (query_buf && query_buf->len == 0)
	{
		appendPQExpBufferStr(query_buf, previous_buf->data);
		return true;
	}
	return false;
}

/*
 * Ask the user for a password; 'username' is the username the
 * password is for, if one has been explicitly specified.
 * Returns a malloc'd string.
 * If 'canceled' is provided, *canceled will be set to true if the prompt
 * is canceled via SIGINT, and to false otherwise.
 */
static char *
prompt_for_password(const char *username, bool *canceled)
{
	char	   *result;
	PromptInterruptContext prompt_ctx;

	/* Set up to let SIGINT cancel simple_prompt_extended() */
	prompt_ctx.jmpbuf = sigint_interrupt_jmp;
	prompt_ctx.enabled = &sigint_interrupt_enabled;
	prompt_ctx.canceled = false;

	if (username == NULL || username[0] == '\0')
		result = simple_prompt_extended("Password: ", false, &prompt_ctx);
	else
	{
		char	   *prompt_text;

		prompt_text = psprintf(_("Password for user %s: "), username);
		result = simple_prompt_extended(prompt_text, false, &prompt_ctx);
		free(prompt_text);
	}

	if (canceled)
		*canceled = prompt_ctx.canceled;

	return result;
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

/*
 * do_connect -- handler for \connect
 *
 * Connects to a database with given parameters.  If we are told to re-use
 * parameters, parameters from the previous connection are used where the
 * command's own options do not supply a value.  Otherwise, libpq defaults
 * are used.
 *
 * In interactive mode, if connection fails with the given parameters,
 * the old connection will be kept.
 */
static bool
do_connect(enum trivalue reuse_previous_specification,
		   char *dbname, char *user, char *host, char *port)
{
	PGconn	   *o_conn = pset.db,
			   *n_conn = NULL;
	PQconninfoOption *cinfo;
	int			nconnopts = 0;
	bool		same_host = false;
	char	   *password = NULL;
	char	   *client_encoding;
	bool		success = true;
	bool		keep_password = true;
	bool		has_connection_string;
	bool		reuse_previous;

	has_connection_string = dbname ?
		recognized_connection_string(dbname) : false;

	/* Complain if we have additional arguments after a connection string. */
	if (has_connection_string && (user || host || port))
	{
		pg_log_error("Do not give user, host, or port separately when using a connection string");
		return false;
	}

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

	/*
	 * If we intend to re-use connection parameters, collect them out of the
	 * old connection, then replace individual values as necessary.  (We may
	 * need to resort to looking at pset.dead_conn, if the connection died
	 * previously.)  Otherwise, obtain a PQconninfoOption array containing
	 * libpq's defaults, and modify that.  Note this function assumes that
	 * PQconninfo, PQconndefaults, and PQconninfoParse will all produce arrays
	 * containing the same options in the same order.
	 */
	if (reuse_previous)
	{
		if (o_conn)
			cinfo = PQconninfo(o_conn);
		else if (pset.dead_conn)
			cinfo = PQconninfo(pset.dead_conn);
		else
		{
			/* This is reachable after a non-interactive \connect failure */
			pg_log_error("No database connection exists to re-use parameters from");
			return false;
		}
	}
	else
		cinfo = PQconndefaults();

	if (cinfo)
	{
		if (has_connection_string)
		{
			/* Parse the connstring and insert values into cinfo */
			PQconninfoOption *replcinfo;
			char	   *errmsg;

			replcinfo = PQconninfoParse(dbname, &errmsg);
			if (replcinfo)
			{
				PQconninfoOption *ci;
				PQconninfoOption *replci;
				bool		have_password = false;

				for (ci = cinfo, replci = replcinfo;
					 ci->keyword && replci->keyword;
					 ci++, replci++)
				{
					Assert(strcmp(ci->keyword, replci->keyword) == 0);
					/* Insert value from connstring if one was provided */
					if (replci->val)
					{
						/*
						 * We know that both val strings were allocated by
						 * libpq, so the least messy way to avoid memory leaks
						 * is to swap them.
						 */
						char	   *swap = replci->val;

						replci->val = ci->val;
						ci->val = swap;

						/*
						 * Check whether connstring provides options affecting
						 * password re-use.  While any change in user, host,
						 * hostaddr, or port causes us to ignore the old
						 * connection's password, we don't force that for
						 * dbname, since passwords aren't database-specific.
						 */
						if (replci->val == NULL ||
							strcmp(ci->val, replci->val) != 0)
						{
							if (strcmp(replci->keyword, "user") == 0 ||
								strcmp(replci->keyword, "host") == 0 ||
								strcmp(replci->keyword, "hostaddr") == 0 ||
								strcmp(replci->keyword, "port") == 0)
								keep_password = false;
						}
						/* Also note whether connstring contains a password. */
						if (strcmp(replci->keyword, "password") == 0)
							have_password = true;
					}
					else if (!reuse_previous)
					{
						/*
						 * When we have a connstring and are not re-using
						 * parameters, swap *all* entries, even those not set
						 * by the connstring.  This avoids absorbing
						 * environment-dependent defaults from the result of
						 * PQconndefaults().  We don't want to do that because
						 * they'd override service-file entries if the
						 * connstring specifies a service parameter, whereas
						 * the priority should be the other way around.  libpq
						 * can certainly recompute any defaults we don't pass
						 * here.  (In this situation, it's a bit wasteful to
						 * have called PQconndefaults() at all, but not doing
						 * so would require yet another major code path here.)
						 */
						replci->val = ci->val;
						ci->val = NULL;
					}
				}
				Assert(ci->keyword == NULL && replci->keyword == NULL);

				/* While here, determine how many option slots there are */
				nconnopts = ci - cinfo;

				PQconninfoFree(replcinfo);

				/*
				 * If the connstring contains a password, tell the loop below
				 * that we may use it, regardless of other settings (i.e.,
				 * cinfo's password is no longer an "old" password).
				 */
				if (have_password)
					keep_password = true;

				/* Don't let code below try to inject dbname into params. */
				dbname = NULL;
			}
			else
			{
				/* PQconninfoParse failed */
				if (errmsg)
				{
					pg_log_error("%s", errmsg);
					PQfreemem(errmsg);
				}
				else
					pg_log_error("out of memory");
				success = false;
			}
		}
		else
		{
			/*
			 * If dbname isn't a connection string, then we'll inject it and
			 * the other parameters into the keyword array below.  (We can't
			 * easily insert them into the cinfo array because of memory
			 * management issues: PQconninfoFree would misbehave on Windows.)
			 * However, to avoid dependencies on the order in which parameters
			 * appear in the array, make a preliminary scan to set
			 * keep_password and same_host correctly.
			 *
			 * While any change in user, host, or port causes us to ignore the
			 * old connection's password, we don't force that for dbname,
			 * since passwords aren't database-specific.
			 */
			PQconninfoOption *ci;

			for (ci = cinfo; ci->keyword; ci++)
			{
				if (user && strcmp(ci->keyword, "user") == 0)
				{
					if (!(ci->val && strcmp(user, ci->val) == 0))
						keep_password = false;
				}
				else if (host && strcmp(ci->keyword, "host") == 0)
				{
					if (ci->val && strcmp(host, ci->val) == 0)
						same_host = true;
					else
						keep_password = false;
				}
				else if (port && strcmp(ci->keyword, "port") == 0)
				{
					if (!(ci->val && strcmp(port, ci->val) == 0))
						keep_password = false;
				}
			}

			/* While here, determine how many option slots there are */
			nconnopts = ci - cinfo;
		}
	}
	else
	{
		/* We failed to create the cinfo structure */
		pg_log_error("out of memory");
		success = false;
	}

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
	if (pset.getPassword == TRI_YES && success)
	{
		bool		canceled = false;

		/*
		 * If a connstring or URI is provided, we don't know which username
		 * will be used, since we haven't dug that out of the connstring.
		 * Don't risk issuing a misleading prompt.  As in startup.c, it does
		 * not seem worth working harder, since this getPassword setting is
		 * normally only used in noninteractive cases.
		 */
		password = prompt_for_password(has_connection_string ? NULL : user,
									   &canceled);
		success = !canceled;
	}

	/*
	 * Consider whether to force client_encoding to "auto" (overriding
	 * anything in the connection string).  We do so if we have a terminal
	 * connection and there is no PGCLIENTENCODING environment setting.
	 */
	if (pset.notty || getenv("PGCLIENTENCODING"))
		client_encoding = NULL;
	else
		client_encoding = "auto";

	/* Loop till we have a connection or fail, which we might've already */
	while (success)
	{
		const char **keywords = pg_malloc((nconnopts + 1) * sizeof(*keywords));
		const char **values = pg_malloc((nconnopts + 1) * sizeof(*values));
		int			paramnum = 0;
		PQconninfoOption *ci;

		/*
		 * Copy non-default settings into the PQconnectdbParams parameter
		 * arrays; but inject any values specified old-style, as well as any
		 * interactively-obtained password, and a couple of fields we want to
		 * set forcibly.
		 *
		 * If you change this code, see also the initial-connection code in
		 * main().
		 */
		for (ci = cinfo; ci->keyword; ci++)
		{
			keywords[paramnum] = ci->keyword;

			if (dbname && strcmp(ci->keyword, "dbname") == 0)
				values[paramnum++] = dbname;
			else if (user && strcmp(ci->keyword, "user") == 0)
				values[paramnum++] = user;
			else if (host && strcmp(ci->keyword, "host") == 0)
				values[paramnum++] = host;
			else if (host && !same_host && strcmp(ci->keyword, "hostaddr") == 0)
			{
				/* If we're changing the host value, drop any old hostaddr */
				values[paramnum++] = NULL;
			}
			else if (port && strcmp(ci->keyword, "port") == 0)
				values[paramnum++] = port;
			/* If !keep_password, we unconditionally drop old password */
			else if ((password || !keep_password) &&
					 strcmp(ci->keyword, "password") == 0)
				values[paramnum++] = password;
			else if (strcmp(ci->keyword, "fallback_application_name") == 0)
				values[paramnum++] = pset.progname;
			else if (client_encoding &&
					 strcmp(ci->keyword, "client_encoding") == 0)
				values[paramnum++] = client_encoding;
			else if (ci->val)
				values[paramnum++] = ci->val;
			/* else, don't bother making libpq parse this keyword */
		}
		/* add array terminator */
		keywords[paramnum] = NULL;
		values[paramnum] = NULL;

		/* Note we do not want libpq to re-expand the dbname parameter */
		n_conn = PQconnectStartParams(keywords, values, false);

		pg_free(keywords);
		pg_free(values);

		wait_until_connected(n_conn);
		if (PQstatus(n_conn) == CONNECTION_OK)
			break;

		/*
		 * Connection attempt failed; either retry the connection attempt with
		 * a new password, or give up.
		 */
		if (!password && PQconnectionNeedsPassword(n_conn) && pset.getPassword != TRI_NO)
		{
			bool		canceled = false;

			/*
			 * Prompt for password using the username we actually connected
			 * with --- it might've come out of "dbname" rather than "user".
			 */
			password = prompt_for_password(PQuser(n_conn), &canceled);
			PQfinish(n_conn);
			n_conn = NULL;
			success = !canceled;
			continue;
		}

		/*
		 * We'll report the error below ... unless n_conn is NULL, indicating
		 * that libpq didn't have enough memory to make a PGconn.
		 */
		if (n_conn == NULL)
			pg_log_error("out of memory");

		success = false;
	}							/* end retry loop */

	/* Release locally allocated data, whether we succeeded or not */
	pg_free(password);
	PQconninfoFree(cinfo);

	if (!success)
	{
		/*
		 * Failed to connect to the database. In interactive mode, keep the
		 * previous connection to the DB; in scripting mode, close our
		 * previous connection as well.
		 */
		if (pset.cur_cmd_interactive)
		{
			if (n_conn)
			{
				pg_log_info("%s", PQerrorMessage(n_conn));
				PQfinish(n_conn);
			}

			/* pset.db is left unmodified */
			if (o_conn)
				pg_log_info("Previous connection kept");
		}
		else
		{
			if (n_conn)
			{
				pg_log_error("\\connect: %s", PQerrorMessage(n_conn));
				PQfinish(n_conn);
			}

			if (o_conn)
			{
				/*
				 * Transition to having no connection.
				 *
				 * Unlike CheckConnection(), we close the old connection
				 * immediately to prevent its parameters from being re-used.
				 * This is so that a script cannot accidentally reuse
				 * parameters it did not expect to.  Otherwise, the state
				 * cleanup should be the same as in CheckConnection().
				 */
				PQfinish(o_conn);
				pset.db = NULL;
				ResetCancelConn();
				UnsyncVariables();
			}

			/* On the same reasoning, release any dead_conn to prevent reuse */
			if (pset.dead_conn)
			{
				PQfinish(pset.dead_conn);
				pset.dead_conn = NULL;
			}
		}

		return false;
	}

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
			char	   *connhost = PQhost(pset.db);
			char	   *hostaddr = PQhostaddr(pset.db);

			if (is_unixsock_path(connhost))
			{
				/* hostaddr overrides connhost */
				if (hostaddr && *hostaddr)
					printf(_("You are now connected to database \"%s\" as user \"%s\" on address \"%s\" at port \"%s\".\n"),
						   PQdb(pset.db), PQuser(pset.db), hostaddr, PQport(pset.db));
				else
					printf(_("You are now connected to database \"%s\" as user \"%s\" via socket in \"%s\" at port \"%s\".\n"),
						   PQdb(pset.db), PQuser(pset.db), connhost, PQport(pset.db));
			}
			else
			{
				if (hostaddr && *hostaddr && strcmp(connhost, hostaddr) != 0)
					printf(_("You are now connected to database \"%s\" as user \"%s\" on host \"%s\" (address \"%s\") at port \"%s\".\n"),
						   PQdb(pset.db), PQuser(pset.db), connhost, hostaddr, PQport(pset.db));
				else
					printf(_("You are now connected to database \"%s\" as user \"%s\" on host \"%s\" at port \"%s\".\n"),
						   PQdb(pset.db), PQuser(pset.db), connhost, PQport(pset.db));
			}
		}
		else
			printf(_("You are now connected to database \"%s\" as user \"%s\".\n"),
				   PQdb(pset.db), PQuser(pset.db));
	}

	/* Drop no-longer-needed connection(s) */
	if (o_conn)
		PQfinish(o_conn);
	if (pset.dead_conn)
	{
		PQfinish(pset.dead_conn);
		pset.dead_conn = NULL;
	}

	return true;
}

/*
 * Processes the connection sequence described by PQconnectStartParams(). Don't
 * worry about reporting errors in this function. Our caller will check the
 * connection's status, and report appropriately.
 */
static void
wait_until_connected(PGconn *conn)
{
	bool		forRead = false;

	while (true)
	{
		int			rc;
		int			sock;
		pg_usec_time_t end_time;

		/*
		 * On every iteration of the connection sequence, let's check if the
		 * user has requested a cancellation.
		 */
		if (cancel_pressed)
			break;

		/*
		 * Do not assume that the socket remains the same across
		 * PQconnectPoll() calls.
		 */
		sock = PQsocket(conn);
		if (sock == -1)
			break;

		/*
		 * If the user sends SIGINT between the cancel_pressed check, and
		 * polling of the socket, it will not be recognized. Instead, we will
		 * just wait until the next step in the connection sequence or
		 * forever, which might require users to send SIGTERM or SIGQUIT.
		 *
		 * Some solutions would include the "self-pipe trick," using
		 * pselect(2) and ppoll(2), or using a timeout.
		 *
		 * The self-pipe trick requires a bit of code to setup. pselect(2) and
		 * ppoll(2) are not on all the platforms we support. The simplest
		 * solution happens to just be adding a timeout, so let's wait for 1
		 * second and check cancel_pressed again.
		 */
		end_time = PQgetCurrentTimeUSec() + 1000000;
		rc = PQsocketPoll(sock, forRead, !forRead, end_time);
		if (rc == -1)
			return;

		switch (PQconnectPoll(conn))
		{
			case PGRES_POLLING_OK:
			case PGRES_POLLING_FAILED:
				return;
			case PGRES_POLLING_READING:
				forRead = true;
				continue;
			case PGRES_POLLING_WRITING:
				forRead = false;
				continue;
			case PGRES_POLLING_ACTIVE:
				pg_unreachable();
		}
	}
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

		/*
		 * Warn if server's major version is newer than ours, or if server
		 * predates our support cutoff (currently 9.2).
		 */
		if (pset.sversion / 100 > client_ver / 100 ||
			pset.sversion < 90200)
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
	const char *compression;
	const char *alpn;

	if (!PQsslInUse(pset.db))
		return;					/* no SSL */

	protocol = PQsslAttribute(pset.db, "protocol");
	cipher = PQsslAttribute(pset.db, "cipher");
	compression = PQsslAttribute(pset.db, "compression");
	alpn = PQsslAttribute(pset.db, "alpn");

	printf(_("SSL connection (protocol: %s, cipher: %s, compression: %s, ALPN: %s)\n"),
		   protocol ? protocol : _("unknown"),
		   cipher ? cipher : _("unknown"),
		   (compression && strcmp(compression, "off") != 0) ? _("on") : _("off"),
		   (alpn && alpn[0] != '\0') ? alpn : _("none"));
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
	char	   *service_name;
	char	   *service_file;

	/* get stuff from connection */
	pset.encoding = PQclientEncoding(pset.db);
	pset.popt.topt.encoding = pset.encoding;
	pset.sversion = PQserverVersion(pset.db);

	setFmtEncoding(pset.encoding);

	SetVariable(pset.vars, "DBNAME", PQdb(pset.db));
	SetVariable(pset.vars, "USER", PQuser(pset.db));
	SetVariable(pset.vars, "HOST", PQhost(pset.db));
	SetVariable(pset.vars, "PORT", PQport(pset.db));
	SetVariable(pset.vars, "ENCODING", pg_encoding_to_char(pset.encoding));

	service_name = get_conninfo_value("service");
	SetVariable(pset.vars, "SERVICE", service_name);
	if (service_name)
		pg_free(service_name);

	service_file = get_conninfo_value("servicefile");
	SetVariable(pset.vars, "SERVICEFILE", service_file);
	if (service_file)
		pg_free(service_file);

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
	SetVariable(pset.vars, "SERVICE", NULL);
	SetVariable(pset.vars, "SERVICEFILE", NULL);
	SetVariable(pset.vars, "USER", NULL);
	SetVariable(pset.vars, "HOST", NULL);
	SetVariable(pset.vars, "PORT", NULL);
	SetVariable(pset.vars, "ENCODING", NULL);
	SetVariable(pset.vars, "SERVER_VERSION_NAME", NULL);
	SetVariable(pset.vars, "SERVER_VERSION_NUM", NULL);
}


/*
 * helper for do_edit(): actually invoke the editor
 *
 * Returns true on success, false if we failed to invoke the editor or
 * it returned nonzero status.  (An error message is printed for failed-
 * to-invoke cases, but not if the editor returns nonzero status.)
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
	fflush(NULL);
	result = system(sys);
	if (result == -1)
		pg_log_error("could not start editor \"%s\"", editorName);
	else if (result == 127)
		pg_log_error("could not start /bin/sh");
	free(sys);

	return result == 0;
}


/*
 * do_edit -- handler for \e
 *
 * If you do not specify a filename, the current query buffer will be copied
 * into a temporary file.
 *
 * After this function is done, the resulting file will be copied back into the
 * query buffer.  As an exception to this, the query buffer will be emptied
 * if the file was not modified (or the editor failed) and the caller passes
 * "discard_on_quit" = true.
 *
 * If "edited" isn't NULL, *edited will be set to true if the query buffer
 * is successfully replaced.
 */
static bool
do_edit(const char *filename_arg, PQExpBuffer query_buf,
		int lineno, bool discard_on_quit, bool *edited)
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
#endif

		/*
		 * No canonicalize_path() here. EDIT.EXE run from CMD.EXE prepends the
		 * current directory to the supplied path unless we use only
		 * backslashes, so we do that.
		 */
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
			else
			{
				struct utimbuf ut;

				/*
				 * Try to set the file modification time of the temporary file
				 * a few seconds in the past.  Otherwise, the low granularity
				 * (one second, or even worse on some filesystems) that we can
				 * portably measure with stat(2) could lead us to not
				 * recognize a modification, if the user typed very quickly.
				 *
				 * This is a rather unlikely race condition, so don't error
				 * out if the utime(2) call fails --- that would make the cure
				 * worse than the disease.
				 */
				ut.modtime = ut.actime = time(NULL) - 2;
				(void) utime(fname, &ut);
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

	/* file was edited if the size or modification time has changed */
	if (!error &&
		(before.st_size != after.st_size ||
		 before.st_mtime != after.st_mtime))
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
				resetPQExpBuffer(query_buf);
			}
			else if (edited)
			{
				*edited = true;
			}

			fclose(stream);
		}
	}
	else
	{
		/*
		 * If the file was not modified, and the caller requested it, discard
		 * the query buffer.
		 */
		if (discard_on_quit)
			resetPQExpBuffer(query_buf);
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
		canonicalize_path_enc(filename, pset.encoding);

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
			canonicalize_path_enc(relpath, pset.encoding);

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

	/* header line width in expanded mode */
	else if (strcmp(param, "xheader_width") == 0)
	{
		if (!value)
			;
		else if (pg_strcasecmp(value, "full") == 0)
			popt->topt.expanded_header_width_type = PRINT_XHEADER_FULL;
		else if (pg_strcasecmp(value, "column") == 0)
			popt->topt.expanded_header_width_type = PRINT_XHEADER_COLUMN;
		else if (pg_strcasecmp(value, "page") == 0)
			popt->topt.expanded_header_width_type = PRINT_XHEADER_PAGE;
		else
		{
			int			intval = atoi(value);

			if (intval == 0)
			{
				pg_log_error("\\pset: allowed xheader_width values are \"%s\" (default), \"%s\", \"%s\", or a number specifying the exact width", "full", "column", "page");
				return false;
			}

			popt->topt.expanded_header_width_type = PRINT_XHEADER_EXACT_WIDTH;
			popt->topt.expanded_header_exact_width = intval;
		}
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
		if (value &&
			!ParseVariableNum(value, "pager_min_lines", &popt->topt.pager_min_lines))
			return false;
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

	/* show xheader width value */
	else if (strcmp(param, "xheader_width") == 0)
	{
		if (popt->topt.expanded_header_width_type == PRINT_XHEADER_FULL)
			printf(_("Expanded header width is \"%s\".\n"), "full");
		else if (popt->topt.expanded_header_width_type == PRINT_XHEADER_COLUMN)
			printf(_("Expanded header width is \"%s\".\n"), "column");
		else if (popt->topt.expanded_header_width_type == PRINT_XHEADER_PAGE)
			printf(_("Expanded header width is \"%s\".\n"), "page");
		else if (popt->topt.expanded_header_width_type == PRINT_XHEADER_EXACT_WIDTH)
			printf(_("Expanded header width is %d.\n"), popt->topt.expanded_header_exact_width);
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
	free(popt->topt.fieldSep.separator);
	free(popt->topt.recordSep.separator);
	free(popt->topt.tableAttr);
	free(popt->nullPrint);
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
		return pstrdup(_align2string(popt->topt.format));
	else if (strcmp(param, "linestyle") == 0)
		return pstrdup(get_line_style(&popt->topt)->name);
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
	else if (strcmp(param, "xheader_width") == 0)
	{
		if (popt->topt.expanded_header_width_type == PRINT_XHEADER_FULL)
			return pstrdup("full");
		else if (popt->topt.expanded_header_width_type == PRINT_XHEADER_COLUMN)
			return pstrdup("column");
		else if (popt->topt.expanded_header_width_type == PRINT_XHEADER_PAGE)
			return pstrdup("page");
		else
		{
			/* must be PRINT_XHEADER_EXACT_WIDTH */
			char		wbuff[32];

			snprintf(wbuff, sizeof(wbuff), "%d",
					 popt->topt.expanded_header_exact_width);
			return pstrdup(wbuff);
		}
	}
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

	fflush(NULL);
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

	SetShellResultVariables(result);

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
 *
 * "sleep" is the amount of time to sleep during each loop, measured in
 * seconds.  The internals of this function should use "sleep_ms" for
 * precise sleep time calculations.
 */
static bool
do_watch(PQExpBuffer query_buf, double sleep, int iter, int min_rows)
{
	long		sleep_ms = (long) (sleep * 1000);
	printQueryOpt myopt = pset.popt;
	const char *strftime_fmt;
	const char *user_title;
	char	   *title;
	const char *pagerprog = NULL;
	FILE	   *pagerpipe = NULL;
	int			title_len;
	int			res = 0;
	bool		done = false;
#ifndef WIN32
	sigset_t	sigalrm_sigchld_sigint;
	sigset_t	sigalrm_sigchld;
	sigset_t	sigint;
	struct itimerval interval;
#endif

	if (!query_buf || query_buf->len <= 0)
	{
		pg_log_error("\\watch cannot be used with an empty query");
		return false;
	}

#ifndef WIN32
	sigemptyset(&sigalrm_sigchld_sigint);
	sigaddset(&sigalrm_sigchld_sigint, SIGCHLD);
	sigaddset(&sigalrm_sigchld_sigint, SIGALRM);
	sigaddset(&sigalrm_sigchld_sigint, SIGINT);

	sigemptyset(&sigalrm_sigchld);
	sigaddset(&sigalrm_sigchld, SIGCHLD);
	sigaddset(&sigalrm_sigchld, SIGALRM);

	sigemptyset(&sigint);
	sigaddset(&sigint, SIGINT);

	/*
	 * Block SIGALRM and SIGCHLD before we start the timer and the pager (if
	 * configured), to avoid races.  sigwait() will receive them.
	 */
	sigprocmask(SIG_BLOCK, &sigalrm_sigchld, NULL);

	/*
	 * Set a timer to interrupt sigwait() so we can run the query at the
	 * requested intervals.
	 */
	interval.it_value.tv_sec = sleep_ms / 1000;
	interval.it_value.tv_usec = (sleep_ms % 1000) * 1000;
	interval.it_interval = interval.it_value;
	if (setitimer(ITIMER_REAL, &interval, NULL) < 0)
	{
		pg_log_error("could not set timer: %m");
		done = true;
	}
#endif

	/*
	 * For \watch, we ignore the size of the result and always use the pager
	 * as long as we're talking to a terminal and "\pset pager" is enabled.
	 * However, we'll only use the pager identified by PSQL_WATCH_PAGER.  We
	 * ignore the regular PSQL_PAGER or PAGER environment variables, because
	 * traditional pagers probably won't be very useful for showing a stream
	 * of results.
	 */
#ifndef WIN32
	pagerprog = getenv("PSQL_WATCH_PAGER");
	/* if variable is empty or all-white-space, don't use pager */
	if (pagerprog && strspn(pagerprog, " \t\r\n") == strlen(pagerprog))
		pagerprog = NULL;
#endif
	if (pagerprog && myopt.topt.pager &&
		isatty(fileno(stdin)) && isatty(fileno(stdout)))
	{
		fflush(NULL);
		disable_sigpipe_trap();
		pagerpipe = popen(pagerprog, "w");

		if (!pagerpipe)
			/* silently proceed without pager */
			restore_sigpipe_trap();
	}

	/*
	 * Choose format for timestamps.  We might eventually make this a \pset
	 * option.  In the meantime, using a variable for the format suppresses
	 * overly-anal-retentive gcc warnings about %c being Y2K sensitive.
	 */
	strftime_fmt = "%c";

	/*
	 * Set up rendering options, in particular, disable the pager unless
	 * PSQL_WATCH_PAGER was successfully launched.
	 */
	if (!pagerpipe)
		myopt.topt.pager = 0;

	/*
	 * If there's a title in the user configuration, make sure we have room
	 * for it in the title buffer.  Allow 128 bytes for the timestamp plus 128
	 * bytes for the rest.
	 */
	user_title = myopt.title;
	title_len = (user_title ? strlen(user_title) : 0) + 256;
	title = pg_malloc(title_len);

	/* Loop to run query and then sleep awhile */
	while (!done)
	{
		time_t		timer;
		char		timebuf[128];

		/*
		 * Prepare title for output.  Note that we intentionally include a
		 * newline at the end of the title; this is somewhat historical but it
		 * makes for reasonably nicely formatted output in simple cases.
		 */
		timer = time(NULL);
		strftime(timebuf, sizeof(timebuf), strftime_fmt, localtime(&timer));

		if (user_title)
			snprintf(title, title_len, _("%s\t%s (every %gs)\n"),
					 user_title, timebuf, sleep_ms / 1000.0);
		else
			snprintf(title, title_len, _("%s (every %gs)\n"),
					 timebuf, sleep_ms / 1000.0);
		myopt.title = title;

		/* Run the query and print out the result */
		res = PSQLexecWatch(query_buf->data, &myopt, pagerpipe, min_rows);

		/*
		 * PSQLexecWatch handles the case where we can no longer repeat the
		 * query, and returns 0 or -1.
		 */
		if (res <= 0)
			break;

		/* If we have iteration count, check that it's not exceeded yet */
		if (iter && (--iter <= 0))
			break;

		/* Quit if error on pager pipe (probably pager has quit) */
		if (pagerpipe && ferror(pagerpipe))
			break;

		/* Tight loop, no wait needed */
		if (sleep_ms == 0)
			continue;

#ifdef WIN32

		/*
		 * Wait a while before running the query again.  Break the sleep into
		 * short intervals (at most 1s); that's probably unnecessary since
		 * pg_usleep is interruptible on Windows, but it's cheap insurance.
		 */
		for (long i = sleep_ms; i > 0;)
		{
			long		s = Min(i, 1000L);

			pg_usleep(s * 1000L);
			if (cancel_pressed)
			{
				done = true;
				break;
			}
			i -= s;
		}
#else
		/* sigwait() will handle SIGINT. */
		sigprocmask(SIG_BLOCK, &sigint, NULL);
		if (cancel_pressed)
			done = true;

		/* Wait for SIGINT, SIGCHLD or SIGALRM. */
		while (!done)
		{
			int			signal_received;

			errno = sigwait(&sigalrm_sigchld_sigint, &signal_received);
			if (errno != 0)
			{
				/* Some other signal arrived? */
				if (errno == EINTR)
					continue;
				else
				{
					pg_log_error("could not wait for signals: %m");
					done = true;
					break;
				}
			}
			/* On ^C or pager exit, it's time to stop running the query. */
			if (signal_received == SIGINT || signal_received == SIGCHLD)
				done = true;
			/* Otherwise, we must have SIGALRM.  Time to run the query again. */
			break;
		}

		/* Unblock SIGINT so that slow queries can be interrupted. */
		sigprocmask(SIG_UNBLOCK, &sigint, NULL);
#endif
	}

	if (pagerpipe)
	{
		pclose(pagerpipe);
		restore_sigpipe_trap();
	}
	else
	{
		/*
		 * If the terminal driver echoed "^C", libedit/libreadline might be
		 * confused about the cursor position.  Therefore, inject a newline
		 * before the next prompt is displayed.  We only do this when not
		 * using a pager, because pagers are expected to restore the screen to
		 * a sane state on exit.
		 */
		fprintf(stdout, "\n");
		fflush(stdout);
	}

#ifndef WIN32
	/* Disable the interval timer. */
	memset(&interval, 0, sizeof(interval));
	setitimer(ITIMER_REAL, &interval, NULL);
	/* Unblock SIGINT, SIGCHLD and SIGALRM. */
	sigprocmask(SIG_UNBLOCK, &sigalrm_sigchld_sigint, NULL);
#endif

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
		printf(_("/******** QUERY *********/\n"
				 "%s\n"
				 "/************************/\n\n"), query);
		fflush(stdout);
		if (pset.logfile)
		{
			fprintf(pset.logfile,
					_("/******** QUERY *********/\n"
					  "%s\n"
					  "/************************/\n\n"), query);
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
			 * Starting with PG 9.4, views may have WITH [LOCAL|CASCADED]
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
			else
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
 * For functions, lineno "1" should correspond to the first line of the
 * function body; lines before that are unnumbered.  We expect that
 * pg_get_functiondef() will emit that on a line beginning with "AS ",
 * "BEGIN ", or "RETURN ", and that there can be no such line before
 * the real start of the function body.
 *
 * Caution: this scribbles on *lines.
 */
static void
print_with_linenumbers(FILE *output, char *lines, bool is_func)
{
	bool		in_header = is_func;
	int			lineno = 0;

	while (*lines != '\0')
	{
		char	   *eol;

		if (in_header &&
			(strncmp(lines, "AS ", 3) == 0 ||
			 strncmp(lines, "BEGIN ", 6) == 0 ||
			 strncmp(lines, "RETURN ", 7) == 0))
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
