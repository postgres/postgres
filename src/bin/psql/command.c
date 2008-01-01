/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2008, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/bin/psql/command.c,v 1.186 2008/01/01 19:45:55 momjian Exp $
 */
#include "postgres_fe.h"
#include "command.h"

#ifdef __BORLANDC__				/* needed for BCC */
#undef mkdir
#endif

#include <ctype.h>
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


/* functions for use in this file */
static backslashResult exec_command(const char *cmd,
			 PsqlScanState scan_state,
			 PQExpBuffer query_buf);
static bool do_edit(const char *filename_arg, PQExpBuffer query_buf);
static bool do_connect(char *dbname, char *user, char *host, char *port);
static bool do_shell(const char *command);


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

	psql_assert(scan_state);

	/* Parse off the command name */
	cmd = psql_scan_slash_command(scan_state);

	/* And try to execute it */
	status = exec_command(cmd, scan_state, query_buf);

	if (status == PSQL_CMD_UNKNOWN && strlen(cmd) > 1)
	{
		/*
		 * If the command was not recognized, try to parse it as a one-letter
		 * command with immediately following argument (a still-supported, but
		 * no longer encouraged, syntax).
		 */
		char		new_cmd[2];

		/* don't change cmd until we know it's okay */
		new_cmd[0] = cmd[0];
		new_cmd[1] = '\0';

		psql_scan_slash_pushback(scan_state, cmd + 1);

		status = exec_command(new_cmd, scan_state, query_buf);

		if (status != PSQL_CMD_UNKNOWN)
		{
			/* adjust cmd for possible messages below */
			cmd[1] = '\0';
		}
	}

	if (status == PSQL_CMD_UNKNOWN)
	{
		if (pset.cur_cmd_interactive)
			fprintf(stderr, _("Invalid command \\%s. Try \\? for help.\n"), cmd);
		else
			psql_error("invalid command \\%s\n", cmd);
		status = PSQL_CMD_ERROR;
	}

	if (status != PSQL_CMD_ERROR)
	{
		/* eat any remaining arguments after a valid command */
		/* note we suppress evaluation of backticks here */
		while ((arg = psql_scan_slash_option(scan_state,
											 OT_VERBATIM, NULL, false)))
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

			pw = getpwuid(geteuid());
			if (!pw)
			{
				psql_error("could not get home directory: %s\n", strerror(errno));
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

		if (pset.dirname)
			free(pset.dirname);
		pset.dirname = pg_strdup(dir);
		canonicalize_path(pset.dirname);

		if (opt)
			free(opt);
	}

	/* \copy */
	else if (pg_strcasecmp(cmd, "copy") == 0)
	{
		/* Default fetch-it-all-and-print mode */
		TimevalStruct before,
					after;
		double		elapsed_msec = 0;

		char	   *opt = psql_scan_slash_option(scan_state,
												 OT_WHOLE_LINE, NULL, false);

		if (pset.timing)
			GETTIMEOFDAY(&before);

		success = do_copy(opt);

		if (pset.timing && success)
		{
			GETTIMEOFDAY(&after);
			elapsed_msec = DIFF_MSEC(&after, &before);
			printf(_("Time: %.3f ms\n"), elapsed_msec);

		}

		free(opt);
	}

	/* \copyright */
	else if (strcmp(cmd, "copyright") == 0)
		print_copyright();

	/* \d* commands */
	else if (cmd[0] == 'd')
	{
		char	   *pattern;
		bool		show_verbose;

		/* We don't do SQLID reduction on the pattern yet */
		pattern = psql_scan_slash_option(scan_state,
										 OT_NORMAL, NULL, true);

		show_verbose = strchr(cmd, '+') ? true : false;

		switch (cmd[1])
		{
			case '\0':
			case '+':
				if (pattern)
					success = describeTableDetails(pattern, show_verbose);
				else
					/* standard listing of interesting things */
					success = listTables("tvs", NULL, show_verbose);
				break;
			case 'a':
				success = describeAggregates(pattern, show_verbose);
				break;
			case 'b':
				success = describeTablespaces(pattern, show_verbose);
				break;
			case 'c':
				success = listConversions(pattern);
				break;
			case 'C':
				success = listCasts(pattern);
				break;
			case 'd':
				success = objectDescription(pattern);
				break;
			case 'D':
				success = listDomains(pattern);
				break;
			case 'f':
				success = describeFunctions(pattern, show_verbose);
				break;
			case 'g':
				/* no longer distinct from \du */
				success = describeRoles(pattern, show_verbose);
				break;
			case 'l':
				success = do_lo_list();
				break;
			case 'n':
				success = listSchemas(pattern, show_verbose);
				break;
			case 'o':
				success = describeOperators(pattern);
				break;
			case 'p':
				success = permissionsList(pattern);
				break;
			case 'T':
				success = describeTypes(pattern, show_verbose);
				break;
			case 't':
			case 'v':
			case 'i':
			case 's':
			case 'S':
				success = listTables(&cmd[1], pattern, show_verbose);
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

			default:
				status = PSQL_CMD_UNKNOWN;
		}

		if (pattern)
			free(pattern);
	}


	/*
	 * \e or \edit -- edit the current query buffer (or a file and make it the
	 * query buffer
	 */
	else if (strcmp(cmd, "e") == 0 || strcmp(cmd, "edit") == 0)
	{
		char	   *fname;

		if (!query_buf)
		{
			psql_error("no query buffer\n");
			status = PSQL_CMD_ERROR;
		}
		else
		{
			fname = psql_scan_slash_option(scan_state,
										   OT_NORMAL, NULL, true);
			expand_tilde(&fname);
			if (fname)
				canonicalize_path(fname);
			status = do_edit(fname, query_buf) ? PSQL_CMD_NEWEDIT : PSQL_CMD_ERROR;
			free(fname);
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

	/* \g means send query */
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

	/* help */
	else if (strcmp(cmd, "h") == 0 || strcmp(cmd, "help") == 0)
	{
		char	   *opt = psql_scan_slash_option(scan_state,
												 OT_WHOLE_LINE, NULL, false);

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


	/* \i is include file */
	else if (strcmp(cmd, "i") == 0 || strcmp(cmd, "include") == 0)
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
			expand_tilde(&fname);
			success = (process_file(fname, false) == EXIT_SUCCESS);
			free(fname);
		}
	}

	/* \l is list databases */
	else if (strcmp(cmd, "l") == 0 || strcmp(cmd, "list") == 0)
		success = listAllDbs(false);
	else if (strcmp(cmd, "l+") == 0 || strcmp(cmd, "list+") == 0)
		success = listAllDbs(true);

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
			fprintf(stderr, _("Passwords didn't match.\n"));
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
				fprintf(stderr, _("Password encryption failed.\n"));
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
				res = PSQLexec(buf.data, false);
				termPQExpBuffer(&buf);
				if (!res)
					success = false;
				else
					PQclear(res);
				PQfreemem(encrypted_password);
			}
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
				psql_error("\\%s: error\n", cmd);
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
			psql_error("\\%s: missing required argument\n", cmd);
			success = false;
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
		/* This scrolls off the screen when using /dev/tty */
		success = saveHistory(fname ? fname : DEVTTY, false);
		if (success && !pset.quiet && fname)
			printf(gettext("Wrote history to file \"%s/%s\".\n"),
				   pset.dirname ? pset.dirname : ".", fname);
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
				newval = realloc(newval, strlen(newval) + strlen(opt) + 1);
				if (!newval)
				{
					psql_error("out of memory\n");
					exit(EXIT_FAILURE);
				}
				strcat(newval, opt);
				free(opt);
			}

			if (!SetVariable(pset.vars, opt0, newval))
			{
				psql_error("\\%s: error\n", cmd);
				success = false;
			}
			free(newval);
		}
		free(opt0);
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
		pset.timing = !pset.timing;
		if (!pset.quiet)
		{
			if (pset.timing)
				puts(_("Timing is on."));
			else
				puts(_("Timing is off."));
		}
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
			psql_error("\\%s: error\n", cmd);
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

		free(fname);
	}

	/* \x -- toggle expanded table representation */
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
		slashUsage(pset.popt.topt.pager);

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
			fprintf(stderr, "+ opt(%d) = |%s|\n", i++, value);
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

		prompt_text = malloc(strlen(username) + 100);
		snprintf(prompt_text, strlen(username) + 100,
				 _("Password for user %s: "), username);
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
 * parameter to PQsetdbLogin(), so the libpq defaults will be used.
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

	if (!dbname)
		dbname = PQdb(o_conn);
	if (!user)
		user = PQuser(o_conn);
	if (!host)
		host = PQhost(o_conn);
	if (!port)
		port = PQport(o_conn);

	/*
	 * If the user asked to be prompted for a password, ask for one now. If
	 * not, use the password from the old connection, provided the username
	 * has not changed. Otherwise, try to connect without a password first,
	 * and then ask for a password if needed.
	 *
	 * XXX: this behavior leads to spurious connection attempts recorded in
	 * the postmaster's log.  But libpq offers no API that would let us obtain
	 * a password and then continue with the first connection attempt.
	 */
	if (pset.getPassword)
	{
		password = prompt_for_password(user);
	}
	else if (o_conn && user && strcmp(PQuser(o_conn), user) == 0)
	{
		password = strdup(PQpass(o_conn));
	}

	while (true)
	{
		n_conn = PQsetdbLogin(host, port, NULL, NULL,
							  dbname, user, password);

		/* We can immediately discard the password -- no longer needed */
		if (password)
			free(password);

		if (PQstatus(n_conn) == CONNECTION_OK)
			break;

		/*
		 * Connection attempt failed; either retry the connection attempt with
		 * a new password, or give up.
		 */
		if (!password && PQconnectionNeedsPassword(n_conn))
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
				fputs(_("Previous connection kept\n"), stderr);
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

	/* Tell the user about the new connection */
	if (!pset.quiet)
	{
		printf(_("You are now connected to database \"%s\""), PQdb(pset.db));

		if (param_is_newly_set(PQhost(o_conn), PQhost(pset.db)))
			printf(_(" on host \"%s\""), PQhost(pset.db));

		if (param_is_newly_set(PQport(o_conn), PQport(pset.db)))
			printf(_(" at port \"%s\""), PQport(pset.db));

		if (param_is_newly_set(PQuser(o_conn), PQuser(pset.db)))
			printf(_(" as user \"%s\""), PQuser(pset.db));

		printf(".\n");
	}

	if (o_conn)
		PQfinish(o_conn);
	return true;
}


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
editFile(const char *fname)
{
	const char *editorName;
	char	   *sys;
	int			result;

	psql_assert(fname);

	/* Find an editor to use */
	editorName = getenv("PSQL_EDITOR");
	if (!editorName)
		editorName = getenv("EDITOR");
	if (!editorName)
		editorName = getenv("VISUAL");
	if (!editorName)
		editorName = DEFAULT_EDITOR;

	/*
	 * On Unix the EDITOR value should *not* be quoted, since it might include
	 * switches, eg, EDITOR="pico -t"; it's up to the user to put quotes in it
	 * if necessary.  But this policy is not very workable on Windows, due to
	 * severe brain damage in their command shell plus the fact that standard
	 * program paths include spaces.
	 */
	sys = pg_malloc(strlen(editorName) + strlen(fname) + 10 + 1);
#ifndef WIN32
	sprintf(sys, "exec %s '%s'", editorName, fname);
#else
	sprintf(sys, "%s\"%s\" \"%s\"%s",
			SYSTEMQUOTE, editorName, fname, SYSTEMQUOTE);
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
do_edit(const char *filename_arg, PQExpBuffer query_buf)
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
			psql_error("cannot locate temporary directory: %s",
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
		snprintf(fnametmp, sizeof(fnametmp), "%s%spsql.edit.%d", tmpdir,
				 "/", (int) getpid());
#else
		snprintf(fnametmp, sizeof(fnametmp), "%s%spsql.edit.%d", tmpdir,
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
				fclose(stream);
				remove(fname);
				error = true;
			}
			else if (fclose(stream) != 0)
			{
				psql_error("%s: %s\n", fname, strerror(errno));
				remove(fname);
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
		error = !editFile(fname);

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
 * Read commands from filename and then them to the main processing loop
 * Handler for \i, but can be used for other things as well.  Returns
 * MainLoop() error code.
 */
int
process_file(char *filename, bool single_txn)
{
	FILE	   *fd;
	int			result;
	char	   *oldfilename;
	PGresult   *res;

	if (!filename)
		return EXIT_FAILURE;

	canonicalize_path(filename);
	fd = fopen(filename, PG_BINARY_R);

	if (!fd)
	{
		psql_error("%s: %s\n", filename, strerror(errno));
		return EXIT_FAILURE;
	}

	oldfilename = pset.inputfile;
	pset.inputfile = filename;

	if (single_txn)
		res = PSQLexec("BEGIN", false);
	result = MainLoop(fd);
	if (single_txn)
		res = PSQLexec("COMMIT", false);

	fclose(fd);
	pset.inputfile = oldfilename;
	return result;
}



/*
 * do_pset
 *
 */
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
		case PRINT_HTML:
			return "html";
			break;
		case PRINT_LATEX:
			return "latex";
			break;
		case PRINT_TROFF_MS:
			return "troff-ms";
			break;
	}
	return "unknown";
}


bool
do_pset(const char *param, const char *value, printQueryOpt *popt, bool quiet)
{
	size_t		vallen = 0;

	psql_assert(param);

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
		else if (pg_strncasecmp("html", value, vallen) == 0)
			popt->topt.format = PRINT_HTML;
		else if (pg_strncasecmp("latex", value, vallen) == 0)
			popt->topt.format = PRINT_LATEX;
		else if (pg_strncasecmp("troff-ms", value, vallen) == 0)
			popt->topt.format = PRINT_TROFF_MS;
		else
		{
			psql_error("\\pset: allowed formats are unaligned, aligned, html, latex, troff-ms\n");
			return false;
		}

		if (!quiet)
			printf(_("Output format is %s.\n"), _align2string(popt->topt.format));
	}

	/* set border style/width */
	else if (strcmp(param, "border") == 0)
	{
		if (value)
			popt->topt.border = atoi(value);

		if (!quiet)
			printf(_("Border style is %d.\n"), popt->topt.border);
	}

	/* set expanded/vertical mode */
	else if (strcmp(param, "x") == 0 || strcmp(param, "expanded") == 0 || strcmp(param, "vertical") == 0)
	{
		if (value)
			popt->topt.expanded = ParseVariableBool(value);
		else
			popt->topt.expanded = !popt->topt.expanded;
		if (!quiet)
			printf(popt->topt.expanded
				   ? _("Expanded display is on.\n")
				   : _("Expanded display is off.\n"));
	}

	/* locale-aware numeric output */
	else if (strcmp(param, "numericlocale") == 0)
	{
		if (value)
			popt->topt.numericLocale = ParseVariableBool(value);
		else
			popt->topt.numericLocale = !popt->topt.numericLocale;
		if (!quiet)
		{
			if (popt->topt.numericLocale)
				puts(_("Showing locale-adjusted numeric output."));
			else
				puts(_("Locale-adjusted numeric output is off."));
		}
	}

	/* null display */
	else if (strcmp(param, "null") == 0)
	{
		if (value)
		{
			free(popt->nullPrint);
			popt->nullPrint = pg_strdup(value);
		}
		if (!quiet)
			printf(_("Null display is \"%s\".\n"), popt->nullPrint ? popt->nullPrint : "");
	}

	/* field separator for unaligned text */
	else if (strcmp(param, "fieldsep") == 0)
	{
		if (value)
		{
			free(popt->topt.fieldSep);
			popt->topt.fieldSep = pg_strdup(value);
		}
		if (!quiet)
			printf(_("Field separator is \"%s\".\n"), popt->topt.fieldSep);
	}

	/* record separator for unaligned text */
	else if (strcmp(param, "recordsep") == 0)
	{
		if (value)
		{
			free(popt->topt.recordSep);
			popt->topt.recordSep = pg_strdup(value);
		}
		if (!quiet)
		{
			if (strcmp(popt->topt.recordSep, "\n") == 0)
				printf(_("Record separator is <newline>."));
			else
				printf(_("Record separator is \"%s\".\n"), popt->topt.recordSep);
		}
	}

	/* toggle between full and tuples-only format */
	else if (strcmp(param, "t") == 0 || strcmp(param, "tuples_only") == 0)
	{
		if (value)
			popt->topt.tuples_only = ParseVariableBool(value);
		else
			popt->topt.tuples_only = !popt->topt.tuples_only;
		if (!quiet)
		{
			if (popt->topt.tuples_only)
				puts(_("Showing only tuples."));
			else
				puts(_("Tuples only is off."));
		}
	}

	/* set title override */
	else if (strcmp(param, "title") == 0)
	{
		free(popt->title);
		if (!value)
			popt->title = NULL;
		else
			popt->title = pg_strdup(value);

		if (!quiet)
		{
			if (popt->title)
				printf(_("Title is \"%s\".\n"), popt->title);
			else
				printf(_("Title is unset.\n"));
		}
	}

	/* set HTML table tag options */
	else if (strcmp(param, "T") == 0 || strcmp(param, "tableattr") == 0)
	{
		free(popt->topt.tableAttr);
		if (!value)
			popt->topt.tableAttr = NULL;
		else
			popt->topt.tableAttr = pg_strdup(value);

		if (!quiet)
		{
			if (popt->topt.tableAttr)
				printf(_("Table attribute is \"%s\".\n"), popt->topt.tableAttr);
			else
				printf(_("Table attributes unset.\n"));
		}
	}

	/* toggle use of pager */
	else if (strcmp(param, "pager") == 0)
	{
		if (value && pg_strcasecmp(value, "always") == 0)
			popt->topt.pager = 2;
		else if (value)
			if (ParseVariableBool(value))
				popt->topt.pager = 1;
			else
				popt->topt.pager = 0;
		else if (popt->topt.pager == 1)
			popt->topt.pager = 0;
		else
			popt->topt.pager = 1;
		if (!quiet)
		{
			if (popt->topt.pager == 1)
				puts(_("Pager is used for long output."));
			else if (popt->topt.pager == 2)
				puts(_("Pager is always used."));
			else
				puts(_("Pager usage is off."));
		}
	}

	/* disable "(x rows)" footer */
	else if (strcmp(param, "footer") == 0)
	{
		if (value)
			popt->default_footer = ParseVariableBool(value);
		else
			popt->default_footer = !popt->default_footer;
		if (!quiet)
		{
			if (popt->default_footer)
				puts(_("Default footer is on."));
			else
				puts(_("Default footer is off."));
		}
	}

	else
	{
		psql_error("\\pset: unknown option: %s\n", param);
		return false;
	}

	return true;
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

		sys = pg_malloc(strlen(shellName) + 16);
#ifndef WIN32
		sprintf(sys,
		/* See EDITOR handling comment for an explaination */
				"exec %s", shellName);
#else
		sprintf(sys,
		/* See EDITOR handling comment for an explaination */
				"%s\"%s\"%s", SYSTEMQUOTE, shellName, SYSTEMQUOTE);
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
