/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2016, PostgreSQL Global Development Group
 *
 * src/bin/psql/command.c
 */
#include "postgres_fe.h"
#include "command.h"

#ifdef __BORLANDC__				/* needed for BCC */
#undef mkdir
#endif

#include <ctype.h>
#include <time.h>
#ifdef HAVE_PWD_H
#include <pwd.h>
#endif
#ifndef WIN32
#include <sys/types.h>			/* for umask() */
#include <sys/stat.h>			/* for stat() */
#include <fcntl.h>				/* open() flags */
#include <unistd.h>				/* for geteuid(), getpid(), stat() */
#else
#include <win32.h>
#include <io.h>
#include <fcntl.h>
#include <direct.h>
#include <sys/types.h>			/* for umask() */
#include <sys/stat.h>			/* for stat() */
#endif

#include "portability/instr_time.h"

#include "libpq-fe.h"
#include "pqexpbuffer.h"
#include "dumputils.h"

#include "common.h"
#include "copy.h"
#include "describe.h"
#include "help.h"
#include "input.h"
#include "large_obj.h"
#include "mainloop.h"
#include "print.h"
#include "psqlscan.h"
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

/* functions for use in this file */
static backslashResult exec_command(const char *cmd,
			 PsqlScanState scan_state,
			 PQExpBuffer query_buf);
static bool do_edit(const char *filename_arg, PQExpBuffer query_buf,
		int lineno, bool *edited);
static bool do_connect(char *dbname, char *user, char *host, char *port);
static bool do_shell(const char *command);
static bool do_watch(PQExpBuffer query_buf, long sleep);
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
static bool printPsetInfo(const char *param, struct printQueryOpt *popt);
static char *pset_value_string(const char *param, struct printQueryOpt *popt);

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
 * 'query_buf' contains the query-so-far, which may be modified by
 * execution of the backslash command (for example, \r clears it).
 * query_buf can be NULL if there is no query so far.
 *
 * Returns a status code indicating what action is desired, see command.h.
 *----------
 */

backslashResult
HandleSlashCmds(PsqlScanState scan_state,
				PQExpBuffer query_buf)
{
	backslashResult status = PSQL_CMD_SKIP_LINE;
	char	   *cmd;
	char	   *arg;

	Assert(scan_state != NULL);

	/* Parse off the command name */
	cmd = psql_scan_slash_command(scan_state);

	/* And try to execute it */
	status = exec_command(cmd, scan_state, query_buf);

	if (status == PSQL_CMD_UNKNOWN)
	{
		if (pset.cur_cmd_interactive)
			psql_error("Invalid command \\%s. Try \\? for help.\n", cmd);
		else
			psql_error("invalid command \\%s\n", cmd);
		status = PSQL_CMD_ERROR;
	}

	if (status != PSQL_CMD_ERROR)
	{
		/* eat any remaining arguments after a valid command */
		/* note we suppress evaluation of backticks here */
		while ((arg = psql_scan_slash_option(scan_state,
											 OT_NO_EVAL, NULL, false)))
		{
			psql_error("\\%s: extra argument \"%s\" ignored\n", cmd, arg);
			free(arg);
		}
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
 * Read and interpret an argument to the \connect slash command.
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
		return NULL;

	return result;
}


/*
 * Subroutine to actually try to execute a backslash command.
 */
static backslashResult
exec_command(const char *cmd,
			 PsqlScanState scan_state,
			 PQExpBuffer query_buf)
{
	bool		success = true; /* indicate here if the command ran ok or
								 * failed */
	backslashResult status = PSQL_CMD_SKIP_LINE;

	/*
	 * \a -- toggle field alignment This makes little sense but we keep it
	 * around.
	 */
	if (strcmp(cmd, "a") == 0)
	{
		if (pset.popt.topt.format != PRINT_ALIGNED)
			success = do_pset("format", "aligned", &pset.popt, pset.quiet);
		else
			success = do_pset("format", "unaligned", &pset.popt, pset.quiet);
	}

	/* \C -- override table title (formerly change HTML caption) */
	else if (strcmp(cmd, "C") == 0)
	{
		char	   *opt = psql_scan_slash_option(scan_state,
												 OT_NORMAL, NULL, true);

		success = do_pset("title", opt, &pset.popt, pset.quiet);
		free(opt);
	}

	/*
	 * \c or \connect -- connect to database using the specified parameters.
	 *
	 * \c dbname user host port
	 *
	 * If any of these parameters are omitted or specified as '-', the current
	 * value of the parameter will be used instead. If the parameter has no
	 * current value, the default value for that parameter will be used. Some
	 * examples:
	 *
	 * \c - - hst		Connect to current database on current port of host
	 * "hst" as current user. \c - usr - prt   Connect to current database on
	 * "prt" port of current host as user "usr". \c dbs			  Connect to
	 * "dbs" database on current port of current host as current user.
	 */
	else if (strcmp(cmd, "c") == 0 || strcmp(cmd, "connect") == 0)
	{
		char	   *opt1,
				   *opt2,
				   *opt3,
				   *opt4;

		opt1 = read_connect_arg(scan_state);
		opt2 = read_connect_arg(scan_state);
		opt3 = read_connect_arg(scan_state);
		opt4 = read_connect_arg(scan_state);

		success = do_connect(opt1, opt2, opt3, opt4);

		free(opt1);
		free(opt2);
		free(opt3);
		free(opt4);
	}

	/* \cd */
	else if (strcmp(cmd, "cd") == 0)
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
				psql_error("could not get home directory for user ID %ld: %s\n",
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
#endif   /* WIN32 */
		}

		if (chdir(dir) == -1)
		{
			psql_error("\\%s: could not change directory to \"%s\": %s\n",
					   cmd, dir, strerror(errno));
			success = false;
		}

		if (opt)
			free(opt);
	}

	/* \conninfo -- display information about the current connection */
	else if (strcmp(cmd, "conninfo") == 0)
	{
		char	   *db = PQdb(pset.db);

		if (db == NULL)
			printf(_("You are currently not connected to a database.\n"));
		else
		{
			char	   *host;
			PQconninfoOption *connOptions;
			PQconninfoOption *option;

			host = PQhost(pset.db);
			/* A usable "hostaddr" overrides the basic sense of host. */
			connOptions = PQconninfo(pset.db);
			if (connOptions == NULL)
			{
				psql_error("out of memory\n");
				exit(EXIT_FAILURE);
			}
			for (option = connOptions; option && option->keyword; option++)
				if (strcmp(option->keyword, "hostaddr") == 0)
				{
					if (option->val != NULL && option->val[0] != '\0')
						host = option->val;
					break;
				}

			/* If the host is an absolute path, the connection is via socket */
			if (is_absolute_path(host))
				printf(_("You are connected to database \"%s\" as user \"%s\" via socket in \"%s\" at port \"%s\".\n"),
					   db, PQuser(pset.db), host, PQport(pset.db));
			else
				printf(_("You are connected to database \"%s\" as user \"%s\" on host \"%s\" at port \"%s\".\n"),
					   db, PQuser(pset.db), host, PQport(pset.db));
			printSSLInfo();

			PQconninfoFree(connOptions);
		}
	}

	/* \copy */
	else if (pg_strcasecmp(cmd, "copy") == 0)
	{
		char	   *opt = psql_scan_slash_option(scan_state,
												 OT_WHOLE_LINE, NULL, false);

		success = do_copy(opt);
		free(opt);
	}

	/* \copyright */
	else if (strcmp(cmd, "copyright") == 0)
		print_copyright();

	/* \d* commands */
	else if (cmd[0] == 'd')
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
				success = describeRoles(pattern, show_verbose);
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
				}
				else
					success = PSQL_CMD_UNKNOWN;
				break;
			case 'u':
				success = describeRoles(pattern, show_verbose);
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


	/*
	 * \e or \edit -- edit the current query buffer, or edit a file and make
	 * it the query buffer
	 */
	else if (strcmp(cmd, "e") == 0 || strcmp(cmd, "edit") == 0)
	{
		if (!query_buf)
		{
			psql_error("no query buffer\n");
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
					psql_error("invalid line number: %s\n", ln);
					status = PSQL_CMD_ERROR;
				}
			}
			if (status != PSQL_CMD_ERROR)
			{
				expand_tilde(&fname);
				if (fname)
					canonicalize_path(fname);
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

	/*
	 * \ef -- edit the named function, or present a blank CREATE FUNCTION
	 * template if no argument is given
	 */
	else if (strcmp(cmd, "ef") == 0)
	{
		int			lineno = -1;

		if (pset.sversion < 80400)
		{
			psql_error("The server (version %d.%d) does not support editing function source.\n",
					   pset.sversion / 10000, (pset.sversion / 100) % 100);
			status = PSQL_CMD_ERROR;
		}
		else if (!query_buf)
		{
			psql_error("no query buffer\n");
			status = PSQL_CMD_ERROR;
		}
		else
		{
			char	   *func;
			Oid			foid = InvalidOid;

			func = psql_scan_slash_option(scan_state,
										  OT_WHOLE_LINE, NULL, true);
			lineno = strip_lineno_from_objdesc(func);
			if (lineno == 0)
			{
				/* error already reported */
				status = PSQL_CMD_ERROR;
			}
			else if (!func)
			{
				/* set up an empty command to fill in */
				printfPQExpBuffer(query_buf,
								  "CREATE FUNCTION ( )\n"
								  " RETURNS \n"
								  " LANGUAGE \n"
								  " -- common options:  IMMUTABLE  STABLE  STRICT  SECURITY DEFINER\n"
								  "AS $function$\n"
								  "\n$function$\n");
			}
			else if (!lookup_object_oid(EditableFunction, func, &foid))
			{
				/* error already reported */
				status = PSQL_CMD_ERROR;
			}
			else if (!get_create_object_cmd(EditableFunction, foid, query_buf))
			{
				/* error already reported */
				status = PSQL_CMD_ERROR;
			}
			else if (lineno > 0)
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

			if (func)
				free(func);
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
	}

	/*
	 * \ev -- edit the named view, or present a blank CREATE VIEW template if
	 * no argument is given
	 */
	else if (strcmp(cmd, "ev") == 0)
	{
		int			lineno = -1;

		if (pset.sversion < 70400)
		{
			psql_error("The server (version %d.%d) does not support editing view definitions.\n",
					   pset.sversion / 10000, (pset.sversion / 100) % 100);
			status = PSQL_CMD_ERROR;
		}
		else if (!query_buf)
		{
			psql_error("no query buffer\n");
			status = PSQL_CMD_ERROR;
		}
		else
		{
			char	   *view;
			Oid			view_oid = InvalidOid;

			view = psql_scan_slash_option(scan_state,
										  OT_WHOLE_LINE, NULL, true);
			lineno = strip_lineno_from_objdesc(view);
			if (lineno == 0)
			{
				/* error already reported */
				status = PSQL_CMD_ERROR;
			}
			else if (!view)
			{
				/* set up an empty command to fill in */
				printfPQExpBuffer(query_buf,
								  "CREATE VIEW  AS\n"
								  " SELECT \n"
								  "  -- something...\n");
			}
			else if (!lookup_object_oid(EditableView, view, &view_oid))
			{
				/* error already reported */
				status = PSQL_CMD_ERROR;
			}
			else if (!get_create_object_cmd(EditableView, view_oid, query_buf))
			{
				/* error already reported */
				status = PSQL_CMD_ERROR;
			}

			if (view)
				free(view);
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
	}

	/* \echo and \qecho */
	else if (strcmp(cmd, "echo") == 0 || strcmp(cmd, "qecho") == 0)
	{
		char	   *value;
		char		quoted;
		bool		no_newline = false;
		bool		first = true;
		FILE	   *fout;

		if (strcmp(cmd, "qecho") == 0)
			fout = pset.queryFout;
		else
			fout = stdout;

		while ((value = psql_scan_slash_option(scan_state,
											   OT_NORMAL, &quoted, false)))
		{
			if (!quoted && strcmp(value, "-n") == 0)
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

	/* \encoding -- set/show client side encoding */
	else if (strcmp(cmd, "encoding") == 0)
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
				psql_error("%s: invalid encoding name or conversion procedure not found\n", encoding);
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

	/* \f -- change field separator */
	else if (strcmp(cmd, "f") == 0)
	{
		char	   *fname = psql_scan_slash_option(scan_state,
												   OT_NORMAL, NULL, false);

		success = do_pset("fieldsep", fname, &pset.popt, pset.quiet);
		free(fname);
	}

	/* \g [filename] -- send query, optionally with output to file/pipe */
	else if (strcmp(cmd, "g") == 0)
	{
		char	   *fname = psql_scan_slash_option(scan_state,
												   OT_FILEPIPE, NULL, false);

		if (!fname)
			pset.gfname = NULL;
		else
		{
			expand_tilde(&fname);
			pset.gfname = pg_strdup(fname);
		}
		free(fname);
		status = PSQL_CMD_SEND;
	}

	/* \gset [prefix] -- send query and store result into variables */
	else if (strcmp(cmd, "gset") == 0)
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

	/* help */
	else if (strcmp(cmd, "h") == 0 || strcmp(cmd, "help") == 0)
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

	/* HTML mode */
	else if (strcmp(cmd, "H") == 0 || strcmp(cmd, "html") == 0)
	{
		if (pset.popt.topt.format != PRINT_HTML)
			success = do_pset("format", "html", &pset.popt, pset.quiet);
		else
			success = do_pset("format", "aligned", &pset.popt, pset.quiet);
	}


	/* \i and \ir include files */
	else if (strcmp(cmd, "i") == 0 || strcmp(cmd, "include") == 0
		   || strcmp(cmd, "ir") == 0 || strcmp(cmd, "include_relative") == 0)
	{
		char	   *fname = psql_scan_slash_option(scan_state,
												   OT_NORMAL, NULL, true);

		if (!fname)
		{
			psql_error("\\%s: missing required argument\n", cmd);
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

	/* \l is list databases */
	else if (strcmp(cmd, "l") == 0 || strcmp(cmd, "list") == 0 ||
			 strcmp(cmd, "l+") == 0 || strcmp(cmd, "list+") == 0)
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

	/*
	 * large object things
	 */
	else if (strncmp(cmd, "lo_", 3) == 0)
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
				psql_error("\\%s: missing required argument\n", cmd);
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
				psql_error("\\%s: missing required argument\n", cmd);
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
				psql_error("\\%s: missing required argument\n", cmd);
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


	/* \o -- set query output */
	else if (strcmp(cmd, "o") == 0 || strcmp(cmd, "out") == 0)
	{
		char	   *fname = psql_scan_slash_option(scan_state,
												   OT_FILEPIPE, NULL, true);

		expand_tilde(&fname);
		success = setQFout(fname);
		free(fname);
	}

	/* \p prints the current query buffer */
	else if (strcmp(cmd, "p") == 0 || strcmp(cmd, "print") == 0)
	{
		if (query_buf && query_buf->len > 0)
			puts(query_buf->data);
		else if (!pset.quiet)
			puts(_("Query buffer is empty."));
		fflush(stdout);
	}

	/* \password -- set user password */
	else if (strcmp(cmd, "password") == 0)
	{
		char	   *pw1;
		char	   *pw2;

		pw1 = simple_prompt("Enter new password: ", 100, false);
		pw2 = simple_prompt("Enter it again: ", 100, false);

		if (strcmp(pw1, pw2) != 0)
		{
			psql_error("Passwords didn't match.\n");
			success = false;
		}
		else
		{
			char	   *opt0 = psql_scan_slash_option(scan_state, OT_SQLID, NULL, true);
			char	   *user;
			char	   *encrypted_password;

			if (opt0)
				user = opt0;
			else
				user = PQuser(pset.db);

			encrypted_password = PQencryptPassword(pw1, user);

			if (!encrypted_password)
			{
				psql_error("Password encryption failed.\n");
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

			if (opt0)
				free(opt0);
		}

		free(pw1);
		free(pw2);
	}

	/* \prompt -- prompt and set variable */
	else if (strcmp(cmd, "prompt") == 0)
	{
		char	   *opt,
				   *prompt_text = NULL;
		char	   *arg1,
				   *arg2;

		arg1 = psql_scan_slash_option(scan_state, OT_NORMAL, NULL, false);
		arg2 = psql_scan_slash_option(scan_state, OT_NORMAL, NULL, false);

		if (!arg1)
		{
			psql_error("\\%s: missing required argument\n", cmd);
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
				result = simple_prompt(prompt_text, 4096, true);
			else
			{
				if (prompt_text)
				{
					fputs(prompt_text, stdout);
					fflush(stdout);
				}
				result = gets_fromFile(stdin);
			}

			if (!SetVariable(pset.vars, opt, result))
			{
				psql_error("\\%s: error while setting variable\n", cmd);
				success = false;
			}

			free(result);
			if (prompt_text)
				free(prompt_text);
			free(opt);
		}
	}

	/* \pset -- set printing parameters */
	else if (strcmp(cmd, "pset") == 0)
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
				"border", "columns", "expanded", "fieldsep", "fieldsep_zero",
				"footer", "format", "linestyle", "null",
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

	/* \q or \quit */
	else if (strcmp(cmd, "q") == 0 || strcmp(cmd, "quit") == 0)
		status = PSQL_CMD_TERMINATE;

	/* reset(clear) the buffer */
	else if (strcmp(cmd, "r") == 0 || strcmp(cmd, "reset") == 0)
	{
		resetPQExpBuffer(query_buf);
		psql_scan_reset(scan_state);
		if (!pset.quiet)
			puts(_("Query buffer reset (cleared)."));
	}

	/* \s save history in a file or show it on the screen */
	else if (strcmp(cmd, "s") == 0)
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

	/* \set -- generalized set variable/option command */
	else if (strcmp(cmd, "set") == 0)
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
			{
				psql_error("\\%s: error while setting variable\n", cmd);
				success = false;
			}
			free(newval);
		}
		free(opt0);
	}


	/* \setenv -- set environment command */
	else if (strcmp(cmd, "setenv") == 0)
	{
		char	   *envvar = psql_scan_slash_option(scan_state,
													OT_NORMAL, NULL, false);
		char	   *envval = psql_scan_slash_option(scan_state,
													OT_NORMAL, NULL, false);

		if (!envvar)
		{
			psql_error("\\%s: missing required argument\n", cmd);
			success = false;
		}
		else if (strchr(envvar, '=') != NULL)
		{
			psql_error("\\%s: environment variable name must not contain \"=\"\n",
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

	/* \sf -- show a function's source code */
	else if (strcmp(cmd, "sf") == 0 || strcmp(cmd, "sf+") == 0)
	{
		bool		show_linenumbers = (strcmp(cmd, "sf+") == 0);
		PQExpBuffer func_buf;
		char	   *func;
		Oid			foid = InvalidOid;

		func_buf = createPQExpBuffer();
		func = psql_scan_slash_option(scan_state,
									  OT_WHOLE_LINE, NULL, true);
		if (pset.sversion < 80400)
		{
			psql_error("The server (version %d.%d) does not support showing function source.\n",
					   pset.sversion / 10000, (pset.sversion / 100) % 100);
			status = PSQL_CMD_ERROR;
		}
		else if (!func)
		{
			psql_error("function name is required\n");
			status = PSQL_CMD_ERROR;
		}
		else if (!lookup_object_oid(EditableFunction, func, &foid))
		{
			/* error already reported */
			status = PSQL_CMD_ERROR;
		}
		else if (!get_create_object_cmd(EditableFunction, foid, func_buf))
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
				int			lineno = count_lines_in_buf(func_buf);

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
				 * lineno "1" should correspond to the first line of the
				 * function body.  We expect that pg_get_functiondef() will
				 * emit that on a line beginning with "AS ", and that there
				 * can be no such line before the real start of the function
				 * body.
				 */
				print_with_linenumbers(output, func_buf->data, "AS ");
			}
			else
			{
				/* just send the function definition to output */
				fputs(func_buf->data, output);
			}

			if (is_pager)
				ClosePager(output);
		}

		if (func)
			free(func);
		destroyPQExpBuffer(func_buf);
	}

	/* \sv -- show a view's source code */
	else if (strcmp(cmd, "sv") == 0 || strcmp(cmd, "sv+") == 0)
	{
		bool		show_linenumbers = (strcmp(cmd, "sv+") == 0);
		PQExpBuffer view_buf;
		char	   *view;
		Oid			view_oid = InvalidOid;

		view_buf = createPQExpBuffer();
		view = psql_scan_slash_option(scan_state,
									  OT_WHOLE_LINE, NULL, true);
		if (pset.sversion < 70400)
		{
			psql_error("The server (version %d.%d) does not support showing view definitions.\n",
					   pset.sversion / 10000, (pset.sversion / 100) % 100);
			status = PSQL_CMD_ERROR;
		}
		else if (!view)
		{
			psql_error("view name is required\n");
			status = PSQL_CMD_ERROR;
		}
		else if (!lookup_object_oid(EditableView, view, &view_oid))
		{
			/* error already reported */
			status = PSQL_CMD_ERROR;
		}
		else if (!get_create_object_cmd(EditableView, view_oid, view_buf))
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
				/* count lines in view to see if pager is needed */
				int			lineno = count_lines_in_buf(view_buf);

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
				/* add line numbers, numbering all lines */
				print_with_linenumbers(output, view_buf->data, NULL);
			}
			else
			{
				/* just send the view definition to output */
				fputs(view_buf->data, output);
			}

			if (is_pager)
				ClosePager(output);
		}

		if (view)
			free(view);
		destroyPQExpBuffer(view_buf);
	}

	/* \t -- turn off headers and row count */
	else if (strcmp(cmd, "t") == 0)
	{
		char	   *opt = psql_scan_slash_option(scan_state,
												 OT_NORMAL, NULL, true);

		success = do_pset("tuples_only", opt, &pset.popt, pset.quiet);
		free(opt);
	}

	/* \T -- define html <table ...> attributes */
	else if (strcmp(cmd, "T") == 0)
	{
		char	   *value = psql_scan_slash_option(scan_state,
												   OT_NORMAL, NULL, false);

		success = do_pset("tableattr", value, &pset.popt, pset.quiet);
		free(value);
	}

	/* \timing -- toggle timing of queries */
	else if (strcmp(cmd, "timing") == 0)
	{
		char	   *opt = psql_scan_slash_option(scan_state,
												 OT_NORMAL, NULL, false);

		if (opt)
			pset.timing = ParseVariableBool(opt, "\\timing");
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

	/* \unset */
	else if (strcmp(cmd, "unset") == 0)
	{
		char	   *opt = psql_scan_slash_option(scan_state,
												 OT_NORMAL, NULL, false);

		if (!opt)
		{
			psql_error("\\%s: missing required argument\n", cmd);
			success = false;
		}
		else if (!SetVariable(pset.vars, opt, NULL))
		{
			psql_error("\\%s: error while setting variable\n", cmd);
			success = false;
		}
		free(opt);
	}

	/* \w -- write query buffer to file */
	else if (strcmp(cmd, "w") == 0 || strcmp(cmd, "write") == 0)
	{
		FILE	   *fd = NULL;
		bool		is_pipe = false;
		char	   *fname = NULL;

		if (!query_buf)
		{
			psql_error("no query buffer\n");
			status = PSQL_CMD_ERROR;
		}
		else
		{
			fname = psql_scan_slash_option(scan_state,
										   OT_FILEPIPE, NULL, true);
			expand_tilde(&fname);

			if (!fname)
			{
				psql_error("\\%s: missing required argument\n", cmd);
				success = false;
			}
			else
			{
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
					psql_error("%s: %s\n", fname, strerror(errno));
					success = false;
				}
			}
		}

		if (fd)
		{
			int			result;

			if (query_buf && query_buf->len > 0)
				fprintf(fd, "%s\n", query_buf->data);

			if (is_pipe)
				result = pclose(fd);
			else
				result = fclose(fd);

			if (result == EOF)
			{
				psql_error("%s: %s\n", fname, strerror(errno));
				success = false;
			}
		}

		if (is_pipe)
			restore_sigpipe_trap();

		free(fname);
	}

	/* \watch -- execute a query every N seconds */
	else if (strcmp(cmd, "watch") == 0)
	{
		char	   *opt = psql_scan_slash_option(scan_state,
												 OT_NORMAL, NULL, true);
		long		sleep = 2;

		/* Convert optional sleep-length argument */
		if (opt)
		{
			sleep = strtol(opt, NULL, 10);
			if (sleep <= 0)
				sleep = 1;
			free(opt);
		}

		success = do_watch(query_buf, sleep);

		/* Reset the query buffer as though for \r */
		resetPQExpBuffer(query_buf);
		psql_scan_reset(scan_state);
	}

	/* \x -- set or toggle expanded table representation */
	else if (strcmp(cmd, "x") == 0)
	{
		char	   *opt = psql_scan_slash_option(scan_state,
												 OT_NORMAL, NULL, true);

		success = do_pset("expanded", opt, &pset.popt, pset.quiet);
		free(opt);
	}

	/* \z -- list table rights (equivalent to \dp) */
	else if (strcmp(cmd, "z") == 0)
	{
		char	   *pattern = psql_scan_slash_option(scan_state,
													 OT_NORMAL, NULL, true);

		success = permissionsList(pattern);
		if (pattern)
			free(pattern);
	}

	/* \! -- shell escape */
	else if (strcmp(cmd, "!") == 0)
	{
		char	   *opt = psql_scan_slash_option(scan_state,
												 OT_WHOLE_LINE, NULL, false);

		success = do_shell(opt);
		free(opt);
	}

	/* \? -- slash command help */
	else if (strcmp(cmd, "?") == 0)
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
	}

#if 0

	/*
	 * These commands don't do anything. I just use them to test the parser.
	 */
	else if (strcmp(cmd, "void") == 0 || strcmp(cmd, "#") == 0)
	{
		int			i = 0;
		char	   *value;

		while ((value = psql_scan_slash_option(scan_state,
											   OT_NORMAL, NULL, true)))
		{
			psql_error("+ opt(%d) = |%s|\n", i++, value);
			free(value);
		}
	}
#endif

	else
		status = PSQL_CMD_UNKNOWN;

	if (!success)
		status = PSQL_CMD_ERROR;

	return status;
}

/*
 * Ask the user for a password; 'username' is the username the
 * password is for, if one has been explicitly specified. Returns a
 * malloc'd string.
 */
static char *
prompt_for_password(const char *username)
{
	char	   *result;

	if (username == NULL)
		result = simple_prompt("Password: ", 100, false);
	else
	{
		char	   *prompt_text;

		prompt_text = psprintf(_("Password for user %s: "), username);
		result = simple_prompt(prompt_text, 100, false);
		free(prompt_text);
	}

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
 * Connects to a database with given parameters. If there exists an
 * established connection, NULL values will be replaced with the ones
 * in the current connection. Otherwise NULL will be passed for that
 * parameter to PQconnectdbParams(), so the libpq defaults will be used.
 *
 * In interactive mode, if connection fails with the given parameters,
 * the old connection will be kept.
 */
static bool
do_connect(char *dbname, char *user, char *host, char *port)
{
	PGconn	   *o_conn = pset.db,
			   *n_conn;
	char	   *password = NULL;
	bool		keep_password;
	bool		has_connection_string;

	if (!o_conn && (!dbname || !user || !host || !port))
	{
		/*
		 * We don't know the supplied connection parameters and don't want to
		 * connect to the wrong database by using defaults, so require all
		 * parameters to be specified.
		 */
		psql_error("All connection parameters must be supplied because no "
				   "database connection exists\n");
		return false;
	}

	/* grab values from the old connection, unless supplied by caller */
	if (!user)
		user = PQuser(o_conn);
	if (!host)
		host = PQhost(o_conn);
	if (!port)
		port = PQport(o_conn);

	has_connection_string =
		dbname ? recognized_connection_string(dbname) : false;

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
	 * Grab dbname from old connection unless supplied by caller.  No password
	 * discard if this changes: passwords aren't (usually) database-specific.
	 */
	if (!dbname)
		dbname = PQdb(o_conn);

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
		password = prompt_for_password(user);
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
#define PARAMS_ARRAY_SIZE	8
		const char **keywords = pg_malloc(PARAMS_ARRAY_SIZE * sizeof(*keywords));
		const char **values = pg_malloc(PARAMS_ARRAY_SIZE * sizeof(*values));
		int			paramnum = 0;

		keywords[0] = "dbname";
		values[0] = dbname;

		if (!has_connection_string)
		{
			keywords[++paramnum] = "host";
			values[paramnum] = host;
			keywords[++paramnum] = "port";
			values[paramnum] = port;
			keywords[++paramnum] = "user";
			values[paramnum] = user;
		}
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
			PQfinish(n_conn);
			password = prompt_for_password(user);
			continue;
		}

		/*
		 * Failed to connect to the database. In interactive mode, keep the
		 * previous connection to the DB; in scripting mode, close our
		 * previous connection as well.
		 */
		if (pset.cur_cmd_interactive)
		{
			psql_error("%s", PQerrorMessage(n_conn));

			/* pset.db is left unmodified */
			if (o_conn)
				psql_error("Previous connection kept\n");
		}
		else
		{
			psql_error("\\connect: %s", PQerrorMessage(n_conn));
			if (o_conn)
			{
				PQfinish(o_conn);
				pset.db = NULL;
			}
		}

		PQfinish(n_conn);
		return false;
	}

	/*
	 * Replace the old connection with the new one, and update
	 * connection-dependent variables.
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

			/* If the host is an absolute path, the connection is via socket */
			if (is_absolute_path(host))
				printf(_("You are now connected to database \"%s\" as user \"%s\" via socket in \"%s\" at port \"%s\".\n"),
					   PQdb(pset.db), PQuser(pset.db), host, PQport(pset.db));
			else
				printf(_("You are now connected to database \"%s\" as user \"%s\" on host \"%s\" at port \"%s\".\n"),
					   PQdb(pset.db), PQuser(pset.db), host, PQport(pset.db));
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

			printf(_("%s (%s, server %s)\n"),
				   pset.progname, PG_VERSION, server_version);
		}
		/* For version match, only print psql banner on startup. */
		else if (in_startup)
			printf("%s (%s)\n", pset.progname, PG_VERSION);

		if (pset.sversion / 100 > client_ver / 100)
			printf(_("WARNING: %s major version %d.%d, server major version %d.%d.\n"
					 "         Some psql features might not work.\n"),
				 pset.progname, client_ver / 10000, (client_ver / 100) % 100,
				   pset.sversion / 10000, (pset.sversion / 100) % 100);

#ifdef WIN32
		checkWin32Codepage();
#endif
		printSSLInfo();
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
	/* get stuff from connection */
	pset.encoding = PQclientEncoding(pset.db);
	pset.popt.topt.encoding = pset.encoding;
	pset.sversion = PQserverVersion(pset.db);

	SetVariable(pset.vars, "DBNAME", PQdb(pset.db));
	SetVariable(pset.vars, "USER", PQuser(pset.db));
	SetVariable(pset.vars, "HOST", PQhost(pset.db));
	SetVariable(pset.vars, "PORT", PQport(pset.db));
	SetVariable(pset.vars, "ENCODING", pg_encoding_to_char(pset.encoding));

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
			psql_error("environment variable PSQL_EDITOR_LINENUMBER_ARG must be set to specify a line number\n");
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
		psql_error("could not start editor \"%s\"\n", editorName);
	else if (result == 127)
		psql_error("could not start /bin/sh\n");
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
			psql_error("could not locate temporary directory: %s\n",
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
			psql_error("could not open temporary file \"%s\": %s\n", fname, strerror(errno));
			error = true;
		}
		else
		{
			unsigned int ql = query_buf->len;

			if (ql == 0 || query_buf->data[ql - 1] != '\n')
			{
				appendPQExpBufferChar(query_buf, '\n');
				ql++;
			}

			if (fwrite(query_buf->data, 1, ql, stream) != ql)
			{
				psql_error("%s: %s\n", fname, strerror(errno));

				if (fclose(stream) != 0)
					psql_error("%s: %s\n", fname, strerror(errno));

				if (remove(fname) != 0)
					psql_error("%s: %s\n", fname, strerror(errno));

				error = true;
			}
			else if (fclose(stream) != 0)
			{
				psql_error("%s: %s\n", fname, strerror(errno));
				if (remove(fname) != 0)
					psql_error("%s: %s\n", fname, strerror(errno));
				error = true;
			}
		}
	}

	if (!error && stat(fname, &before) != 0)
	{
		psql_error("%s: %s\n", fname, strerror(errno));
		error = true;
	}

	/* call editor */
	if (!error)
		error = !editFile(fname, lineno);

	if (!error && stat(fname, &after) != 0)
	{
		psql_error("%s: %s\n", fname, strerror(errno));
		error = true;
	}

	if (!error && before.st_mtime != after.st_mtime)
	{
		stream = fopen(fname, PG_BINARY_R);
		if (!stream)
		{
			psql_error("%s: %s\n", fname, strerror(errno));
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
				psql_error("%s: %s\n", fname, strerror(errno));
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
			psql_error("%s: %s\n", fname, strerror(errno));
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
			psql_error("%s: %s\n", filename, strerror(errno));
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

	result = MainLoop(fd);

	if (fd != stdin)
		fclose(fd);

	pset.inputfile = oldfilename;
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
		case PRINT_UNALIGNED:
			return "unaligned";
			break;
		case PRINT_ALIGNED:
			return "aligned";
			break;
		case PRINT_WRAPPED:
			return "wrapped";
			break;
		case PRINT_HTML:
			return "html";
			break;
		case PRINT_ASCIIDOC:
			return "asciidoc";
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
		if (!value)
			;
		else if (pg_strncasecmp("unaligned", value, vallen) == 0)
			popt->topt.format = PRINT_UNALIGNED;
		else if (pg_strncasecmp("aligned", value, vallen) == 0)
			popt->topt.format = PRINT_ALIGNED;
		else if (pg_strncasecmp("wrapped", value, vallen) == 0)
			popt->topt.format = PRINT_WRAPPED;
		else if (pg_strncasecmp("html", value, vallen) == 0)
			popt->topt.format = PRINT_HTML;
		else if (pg_strncasecmp("asciidoc", value, vallen) == 0)
			popt->topt.format = PRINT_ASCIIDOC;
		else if (pg_strncasecmp("latex", value, vallen) == 0)
			popt->topt.format = PRINT_LATEX;
		else if (pg_strncasecmp("latex-longtable", value, vallen) == 0)
			popt->topt.format = PRINT_LATEX_LONGTABLE;
		else if (pg_strncasecmp("troff-ms", value, vallen) == 0)
			popt->topt.format = PRINT_TROFF_MS;
		else
		{
			psql_error("\\pset: allowed formats are unaligned, aligned, wrapped, html, asciidoc, latex, latex-longtable, troff-ms\n");
			return false;
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
			psql_error("\\pset: allowed line styles are ascii, old-ascii, unicode\n");
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
			psql_error("\\pset: allowed Unicode border line styles are single, double\n");
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
			psql_error("\\pset: allowed Unicode column line styles are single, double\n");
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
			psql_error("\\pset: allowed Unicode header line styles are single, double\n");
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
			popt->topt.expanded = ParseVariableBool(value, param);
		else
			popt->topt.expanded = !popt->topt.expanded;
	}

	/* locale-aware numeric output */
	else if (strcmp(param, "numericlocale") == 0)
	{
		if (value)
			popt->topt.numericLocale = ParseVariableBool(value, param);
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
			popt->topt.tuples_only = ParseVariableBool(value, param);
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
			if (ParseVariableBool(value, param))
				popt->topt.pager = 1;
			else
				popt->topt.pager = 0;
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
			popt->topt.default_footer = ParseVariableBool(value, param);
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
		psql_error("\\pset: unknown option: %s\n", param);
		return false;
	}

	if (!quiet)
		printPsetInfo(param, &pset.popt);

	return true;
}


static bool
printPsetInfo(const char *param, struct printQueryOpt *popt)
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
		psql_error("\\pset: unknown option: %s\n", param);
		return false;
	}

	return true;
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
pset_value_string(const char *param, struct printQueryOpt *popt)
{
	Assert(param != NULL);

	if (strcmp(param, "border") == 0)
		return psprintf("%d", popt->topt.border);
	else if (strcmp(param, "columns") == 0)
		return psprintf("%d", popt->topt.columns);
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
		psql_error("\\!: failed\n");
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
do_watch(PQExpBuffer query_buf, long sleep)
{
	printQueryOpt myopt = pset.popt;
	char		title[50];

	if (!query_buf || query_buf->len <= 0)
	{
		psql_error(_("\\watch cannot be used with an empty query\n"));
		return false;
	}

	/*
	 * Set up rendering options, in particular, disable the pager, because
	 * nobody wants to be prompted while watching the output of 'watch'.
	 */
	myopt.topt.pager = 0;

	for (;;)
	{
		int			res;
		time_t		timer;
		long		i;

		/*
		 * Prepare title for output.  XXX would it be better to use the time
		 * of completion of the command?
		 */
		timer = time(NULL);
		snprintf(title, sizeof(title), _("Watch every %lds\t%s"),
				 sleep, asctime(localtime(&timer)));
		myopt.title = title;

		/* Run the query and print out the results */
		res = PSQLexecWatch(query_buf->data, &myopt);

		/*
		 * PSQLexecWatch handles the case where we can no longer repeat the
		 * query, and returns 0 or -1.
		 */
		if (res == 0)
			break;
		if (res == -1)
			return false;

		/*
		 * Set up cancellation of 'watch' via SIGINT.  We redo this each time
		 * through the loop since it's conceivable something inside
		 * PSQLexecWatch could change sigint_interrupt_jmp.
		 */
		if (sigsetjmp(sigint_interrupt_jmp, 1) != 0)
			break;

		/*
		 * Enable 'watch' cancellations and wait a while before running the
		 * query again.  Break the sleep into short intervals since pg_usleep
		 * isn't interruptible on some platforms.
		 */
		sigint_interrupt_enabled = true;
		for (i = 0; i < sleep; i++)
		{
			pg_usleep(1000000L);
			if (cancel_pressed)
				break;
		}
		sigint_interrupt_enabled = false;
	}

	return true;
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
 * store its OID in *obj_oid and return TRUE, else return FALSE.
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
			appendPQExpBuffer(query, "::pg_catalog.regclass::pg_catalog.oid");
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
			 */
			printfPQExpBuffer(query,
							  "SELECT nspname, relname, relkind, pg_catalog.pg_get_viewdef(c.oid, true) FROM "
				 "pg_catalog.pg_class c LEFT JOIN pg_catalog.pg_namespace n "
							  "ON c.relnamespace = n.oid WHERE c.oid = %u",
							  oid);
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

					/*
					 * If the backend ever supports CREATE OR REPLACE
					 * MATERIALIZED VIEW, allow that here; but as of today it
					 * does not, so editing a matview definition in this way
					 * is impossible.
					 */
					switch (relkind[0])
					{
#ifdef NOT_USED
						case 'm':
							appendPQExpBufferStr(buf, "CREATE OR REPLACE MATERIALIZED VIEW ");
							break;
#endif
						case 'v':
							appendPQExpBufferStr(buf, "CREATE OR REPLACE VIEW ");
							break;
						default:
							psql_error("%s.%s is not a view\n",
									   nspname, relname);
							result = false;
							break;
					}
					appendPQExpBuffer(buf, "%s.", fmtId(nspname));
					appendPQExpBuffer(buf, "%s AS\n", fmtId(relname));
					appendPQExpBufferStr(buf, viewdef);
					/* Get rid of the semicolon that pg_get_viewdef appends */
					if (buf->len > 0 && buf->data[buf->len - 1] == ';')
						buf->data[--(buf->len)] = '\0';
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
		psql_error("invalid line number: %s\n", c);
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
	appendPQExpBufferStr(msg, "\n");

	psql_error("%s", msg->data);

	destroyPQExpBuffer(msg);
}
