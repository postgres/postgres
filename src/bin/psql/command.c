/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2003, PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/command.c,v 1.105 2003/10/11 18:04:26 momjian Exp $
 */
#include "postgres_fe.h"
#include "command.h"

#include <errno.h>
#include <assert.h>
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
#endif

#include "libpq-fe.h"
#include "pqexpbuffer.h"

#include "common.h"
#include "copy.h"
#include "describe.h"
#include "help.h"
#include "input.h"
#include "large_obj.h"
#include "mainloop.h"
#include "print.h"
#include "settings.h"
#include "variables.h"
#include "mb/pg_wchar.h"

/* functions for use in this file */

static backslashResult exec_command(const char *cmd,
			 const char *options_string,
			 const char **continue_parse,
			 PQExpBuffer query_buf,
			 volatile int *paren_level);

/* different ways for scan_option to handle parameter words */
enum option_type
{
	OT_NORMAL,					/* normal case */
	OT_SQLID,					/* treat as SQL identifier */
	OT_SQLIDHACK,				/* SQL identifier, but don't downcase */
	OT_FILEPIPE					/* it's a filename or pipe */
};

static char *scan_option(char **string, enum option_type type,
			char *quote, bool semicolon);
static char *unescape(const unsigned char *source, size_t len);

static bool do_edit(const char *filename_arg, PQExpBuffer query_buf);
static bool do_connect(const char *new_dbname, const char *new_user);
static bool do_shell(const char *command);

/*----------
 * HandleSlashCmds:
 *
 * Handles all the different commands that start with '\',
 * ordinarily called by MainLoop().
 *
 * 'line' is the current input line, which should not start with a '\'
 * but with the actual command name
 * (that is taken care of by MainLoop)
 *
 * 'query_buf' contains the query-so-far, which may be modified by
 * execution of the backslash command (for example, \r clears it)
 * query_buf can be NULL if there is no query so far.
 *
 * Returns a status code indicating what action is desired, see command.h.
 *----------
 */

backslashResult
HandleSlashCmds(const char *line,
				PQExpBuffer query_buf,
				const char **end_of_cmd,
				volatile int *paren_level)
{
	backslashResult status = CMD_SKIP_LINE;
	char	   *my_line;
	char	   *options_string = NULL;
	size_t		blank_loc;
	const char *continue_parse = NULL;	/* tell the mainloop where the
										 * backslash command ended */

#ifdef USE_ASSERT_CHECKING
	assert(line);
#endif

	my_line = xstrdup(line);

	/*
	 * Find the first whitespace. line[blank_loc] will now be the
	 * whitespace character or the \0 at the end
	 *
	 * Also look for a backslash, so stuff like \p\g works.
	 */
	blank_loc = strcspn(my_line, " \t\n\r\\");

	if (my_line[blank_loc] == '\\')
	{
		continue_parse = &my_line[blank_loc];
		my_line[blank_loc] = '\0';
		/* If it's a double backslash, we skip it. */
		if (my_line[blank_loc + 1] == '\\')
			continue_parse += 2;
	}
	/* do we have an option string? */
	else if (my_line[blank_loc] != '\0')
	{
		options_string = &my_line[blank_loc + 1];
		my_line[blank_loc] = '\0';
	}

	status = exec_command(my_line, options_string, &continue_parse, query_buf, paren_level);

	if (status == CMD_UNKNOWN)
	{
		/*
		 * If the command was not recognized, try to parse it as a
		 * one-letter command with immediately following argument (a
		 * still-supported, but no longer encouraged, syntax).
		 */
		char		new_cmd[2];

		new_cmd[0] = my_line[0];
		new_cmd[1] = '\0';

		/* use line for options, because my_line was clobbered above */
		status = exec_command(new_cmd, line + 1, &continue_parse, query_buf, paren_level);

		/*
		 * continue_parse must be relative to my_line for calculation
		 * below
		 */
		continue_parse += my_line - line;

#if 0							/* turned out to be too annoying */
		if (status != CMD_UNKNOWN && isalpha((unsigned char) new_cmd[0]))
			psql_error("Warning: This syntax is deprecated.\n");
#endif
	}

	if (status == CMD_UNKNOWN)
	{
		if (pset.cur_cmd_interactive)
			fprintf(stderr, gettext("Invalid command \\%s. Try \\? for help.\n"), my_line);
		else
			psql_error("invalid command \\%s\n", my_line);
		status = CMD_ERROR;
	}

	if (continue_parse && *continue_parse && *(continue_parse + 1) == '\\')
		continue_parse += 2;

	if (end_of_cmd)
	{
		if (continue_parse)
			*end_of_cmd = line + (continue_parse - my_line);
		else
			*end_of_cmd = line + strlen(line);
	}

	free(my_line);

	return status;
}



static backslashResult
exec_command(const char *cmd,
			 const char *options_string,
			 const char **continue_parse,
			 PQExpBuffer query_buf,
			 volatile int *paren_level)
{
	bool		success = true; /* indicate here if the command ran ok or
								 * failed */
	bool		quiet = QUIET();
	backslashResult status = CMD_SKIP_LINE;
	char	   *string,
			   *string_cpy,
			   *val;

	/*
	 * The 'string' variable will be overwritten to point to the next
	 * token, hence we need an extra pointer so we can free this at the
	 * end.
	 */
	if (options_string)
		string = string_cpy = xstrdup(options_string);
	else
		string = string_cpy = NULL;

	/*
	 * \a -- toggle field alignment This makes little sense but we keep it
	 * around.
	 */
	if (strcmp(cmd, "a") == 0)
	{
		if (pset.popt.topt.format != PRINT_ALIGNED)
			success = do_pset("format", "aligned", &pset.popt, quiet);
		else
			success = do_pset("format", "unaligned", &pset.popt, quiet);
	}

	/* \C -- override table title (formerly change HTML caption) */
	else if (strcmp(cmd, "C") == 0)
	{
		char	   *opt = scan_option(&string, OT_NORMAL, NULL, true);

		success = do_pset("title", opt, &pset.popt, quiet);
		free(opt);
	}

	/*----------
	 * \c or \connect -- connect to new database or as different user
	 *
	 * \c foo bar  connect to db "foo" as user "bar"
	 * \c foo [-]  connect to db "foo" as current user
	 * \c - bar    connect to current db as user "bar"
	 * \c		   connect to default db as default user
	 *----------
	 */
	else if (strcmp(cmd, "c") == 0 || strcmp(cmd, "connect") == 0)
	{
		char	   *opt1,
				   *opt2;
		char		opt1q,
					opt2q;

		/*
		 * Ideally we should treat the arguments as SQL identifiers.  But
		 * for backwards compatibility with 7.2 and older pg_dump files,
		 * we have to take unquoted arguments verbatim (don't downcase
		 * them). For now, double-quoted arguments may be stripped of
		 * double quotes (as if SQL identifiers).  By 7.4 or so, pg_dump
		 * files can be expected to double-quote all mixed-case \connect
		 * arguments, and then we can get rid of OT_SQLIDHACK.
		 */
		opt1 = scan_option(&string, OT_SQLIDHACK, &opt1q, true);
		opt2 = scan_option(&string, OT_SQLIDHACK, &opt2q, true);

		if (opt2)
			/* gave username */
			success = do_connect(!opt1q && (strcmp(opt1, "-") == 0 || strcmp(opt1, "") == 0) ? "" : opt1,
								 !opt2q && (strcmp(opt2, "-") == 0 || strcmp(opt2, "") == 0) ? "" : opt2);
		else if (opt1)
			/* gave database name */
			success = do_connect(!opt1q && (strcmp(opt1, "-") == 0 || strcmp(opt1, "") == 0) ? "" : opt1, "");
		else
			/* connect to default db as default user */
			success = do_connect(NULL, NULL);

		free(opt1);
		free(opt2);
	}

	/* \cd */
	else if (strcmp(cmd, "cd") == 0)
	{
		char	   *opt = scan_option(&string, OT_NORMAL, NULL, true);
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

		if (opt)
			free(opt);
	}

	/* \copy */
	else if (strcasecmp(cmd, "copy") == 0)
	{
		success = do_copy(options_string);
		if (options_string)
			string += strlen(string);
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
		pattern = scan_option(&string, OT_NORMAL, NULL, true);

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
			case 'l':
				success = do_lo_list();
				break;
			case 'n':
				success = listSchemas(pattern);
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
				success = describeUsers(pattern);
				break;

			default:
				status = CMD_UNKNOWN;
		}

		if (pattern)
			free(pattern);
	}


	/*
	 * \e or \edit -- edit the current query buffer (or a file and make it
	 * the query buffer
	 */
	else if (strcmp(cmd, "e") == 0 || strcmp(cmd, "edit") == 0)
	{
		char	   *fname;

		if (!query_buf)
		{
			psql_error("no query buffer\n");
			status = CMD_ERROR;
		}
		else
		{
			fname = scan_option(&string, OT_NORMAL, NULL, true);
			status = do_edit(fname, query_buf) ? CMD_NEWEDIT : CMD_ERROR;
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

		while ((value = scan_option(&string, OT_NORMAL, &quoted, false)))
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
		char	   *encoding = scan_option(&string, OT_NORMAL, NULL, false);

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
		char	   *fname = scan_option(&string, OT_NORMAL, NULL, false);

		success = do_pset("fieldsep", fname, &pset.popt, quiet);
		free(fname);
	}

	/* \g means send query */
	else if (strcmp(cmd, "g") == 0)
	{
		char	   *fname = scan_option(&string, OT_FILEPIPE, NULL, false);

		if (!fname)
			pset.gfname = NULL;
		else
			pset.gfname = xstrdup(fname);
		free(fname);
		status = CMD_SEND;
	}

	/* help */
	else if (strcmp(cmd, "h") == 0 || strcmp(cmd, "help") == 0)
	{
		helpSQL(options_string ? &options_string[strspn(options_string, " \t\n\r")] : NULL,
				pset.popt.topt.pager);
		/* set pointer to end of line */
		if (string)
			string += strlen(string);
	}

	/* HTML mode */
	else if (strcmp(cmd, "H") == 0 || strcmp(cmd, "html") == 0)
	{
		if (pset.popt.topt.format != PRINT_HTML)
			success = do_pset("format", "html", &pset.popt, quiet);
		else
			success = do_pset("format", "aligned", &pset.popt, quiet);
	}


	/* \i is include file */
	else if (strcmp(cmd, "i") == 0 || strcmp(cmd, "include") == 0)
	{
		char	   *fname = scan_option(&string, OT_NORMAL, NULL, true);

		if (!fname)
		{
			psql_error("\\%s: missing required argument\n", cmd);
			success = false;
		}
		else
		{
			success = (process_file(fname) == EXIT_SUCCESS);
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

		opt1 = scan_option(&string, OT_NORMAL, NULL, true);
		opt2 = scan_option(&string, OT_NORMAL, NULL, true);

		if (strcmp(cmd + 3, "export") == 0)
		{
			if (!opt2)
			{
				psql_error("\\%s: missing required argument\n", cmd);
				success = false;
			}
			else
				success = do_lo_export(opt1, opt2);
		}

		else if (strcmp(cmd + 3, "import") == 0)
		{
			if (!opt1)
			{
				psql_error("\\%s: missing required argument\n", cmd);
				success = false;
			}
			else
				success = do_lo_import(opt1, opt2);
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
			status = CMD_UNKNOWN;

		free(opt1);
		free(opt2);
	}


	/* \o -- set query output */
	else if (strcmp(cmd, "o") == 0 || strcmp(cmd, "out") == 0)
	{
		char	   *fname = scan_option(&string, OT_FILEPIPE, NULL, true);

		success = setQFout(fname);
		free(fname);
	}

	/* \p prints the current query buffer */
	else if (strcmp(cmd, "p") == 0 || strcmp(cmd, "print") == 0)
	{
		if (query_buf && query_buf->len > 0)
			puts(query_buf->data);
		else if (!quiet)
			puts(gettext("Query buffer is empty."));
		fflush(stdout);
	}

	/* \pset -- set printing parameters */
	else if (strcmp(cmd, "pset") == 0)
	{
		char	   *opt0 = scan_option(&string, OT_NORMAL, NULL, false);
		char	   *opt1 = scan_option(&string, OT_NORMAL, NULL, false);

		if (!opt0)
		{
			psql_error("\\%s: missing required argument\n", cmd);
			success = false;
		}
		else
			success = do_pset(opt0, opt1, &pset.popt, quiet);

		free(opt0);
		free(opt1);
	}

	/* \q or \quit */
	else if (strcmp(cmd, "q") == 0 || strcmp(cmd, "quit") == 0)
		status = CMD_TERMINATE;

	/* reset(clear) the buffer */
	else if (strcmp(cmd, "r") == 0 || strcmp(cmd, "reset") == 0)
	{
		resetPQExpBuffer(query_buf);
		if (paren_level)
			*paren_level = 0;
		if (!quiet)
			puts(gettext("Query buffer reset (cleared)."));
	}

	/* \s save history in a file or show it on the screen */
	else if (strcmp(cmd, "s") == 0)
	{
		char	   *fname = scan_option(&string, OT_NORMAL, NULL, true);

		success = saveHistory(fname ? fname : "/dev/tty");

		if (success && !quiet && fname)
			printf(gettext("Wrote history to file \"%s\".\n"), fname);
		free(fname);
	}

	/* \set -- generalized set variable/option command */
	else if (strcmp(cmd, "set") == 0)
	{
		char	   *opt0 = scan_option(&string, OT_NORMAL, NULL, false);

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
			char	   *newval = NULL;
			char	   *opt;

			opt = scan_option(&string, OT_NORMAL, NULL, false);
			newval = xstrdup(opt ? opt : "");
			free(opt);

			while ((opt = scan_option(&string, OT_NORMAL, NULL, false)))
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

			if (SetVariable(pset.vars, opt0, newval))
			{
				/* Check for special variables */
				if (strcmp(opt0, "VERBOSITY") == 0)
					SyncVerbosityVariable();
			}
			else
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
		success = do_pset("tuples_only", NULL, &pset.popt, quiet);


	/* \T -- define html <table ...> attributes */
	else if (strcmp(cmd, "T") == 0)
	{
		char	   *value = scan_option(&string, OT_NORMAL, NULL, false);

		success = do_pset("tableattr", value, &pset.popt, quiet);
		free(value);
	}

	/* \timing -- toggle timing of queries */
	else if (strcmp(cmd, "timing") == 0)
	{
		pset.timing = !pset.timing;
		if (!quiet)
		{
			if (pset.timing)
				puts(gettext("Timing is on."));
			else
				puts(gettext("Timing is off."));
		}
	}

	/* \unset */
	else if (strcmp(cmd, "unset") == 0)
	{
		char	   *opt = scan_option(&string, OT_NORMAL, NULL, false);

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
			status = CMD_ERROR;
		}
		else
		{
			fname = scan_option(&string, OT_FILEPIPE, NULL, true);

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
					fd = fopen(fname, "w");

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
		success = do_pset("expanded", NULL, &pset.popt, quiet);


	/* \z -- list table rights (equivalent to \dp) */
	else if (strcmp(cmd, "z") == 0)
	{
		char	   *pattern = scan_option(&string, OT_NORMAL, NULL, true);

		success = permissionsList(pattern);
		if (pattern)
			free(pattern);
	}

	/* \! -- shell escape */
	else if (strcmp(cmd, "!") == 0)
	{
		success = do_shell(options_string);
		/* wind pointer to end of line */
		if (string)
			string += strlen(string);
	}

	/* \? -- slash command help */
	else if (strcmp(cmd, "?") == 0)
		slashUsage(pset.popt.topt.pager);

#if 0

	/*
	 * These commands don't do anything. I just use them to test the
	 * parser.
	 */
	else if (strcmp(cmd, "void") == 0 || strcmp(cmd, "#") == 0)
	{
		int			i = 0;
		char	   *value;

		fprintf(stderr, "+ optstr = |%s|\n", options_string);
		while ((value = scan_option(&string, OT_NORMAL, NULL, true)))
		{
			fprintf(stderr, "+ opt(%d) = |%s|\n", i++, value);
			free(value);
		}
	}
#endif

	else
		status = CMD_UNKNOWN;

	if (!success)
		status = CMD_ERROR;

	/* eat the rest of the options string */
	while ((val = scan_option(&string, OT_NORMAL, NULL, false)))
	{
		if (status != CMD_UNKNOWN)
			psql_error("\\%s: extra argument \"%s\" ignored\n", cmd, val);
		if (val)
			free(val);
	}

	if (options_string && continue_parse)
		*continue_parse = options_string + (string - string_cpy);
	free(string_cpy);

	return status;
}



/*
 * scan_option()
 *
 * *string points to possible option string on entry; on exit, it's updated
 * to point past the option string (if any).
 *
 * type tells what processing, if any, to perform on the option string;
 * for example, if it's a SQL identifier, we want to downcase any unquoted
 * letters.
 *
 * if quote is not NULL, *quote is set to 0 if no quoting was found, else
 * the quote symbol.
 *
 * if semicolon is true, trailing semicolon(s) that would otherwise be taken
 * as part of the option string will be stripped.
 *
 * Return value is NULL if no option found, else a malloc'd copy of the
 * processed option value.
 */
static char *
scan_option(char **string, enum option_type type, char *quote, bool semicolon)
{
	unsigned int pos;
	char	   *options_string;
	char	   *return_val;

	if (quote)
		*quote = 0;

	if (!string || !(*string))
		return NULL;

	options_string = *string;
	/* skip leading whitespace */
	pos = strspn(options_string, " \t\n\r");

	switch (options_string[pos])
	{
			/*
			 * End of line: no option present
			 */
		case '\0':
			*string = &options_string[pos];
			return NULL;

			/*
			 * Next command: treat like end of line
			 *
			 * XXX this means we can't conveniently accept options that start
			 * with a backslash; therefore, option processing that
			 * encourages use of backslashes is rather broken.
			 */
		case '\\':
			*string = &options_string[pos];
			return NULL;

			/*
			 * A single quote has a psql internal meaning, such as for
			 * delimiting file names, and it also allows for such escape
			 * sequences as \t.
			 */
		case '\'':
			{
				unsigned int jj;
				unsigned short int bslash_count = 0;

				for (jj = pos + 1; options_string[jj]; jj += PQmblen(&options_string[jj], pset.encoding))
				{
					if (options_string[jj] == '\'' && bslash_count % 2 == 0)
						break;

					if (options_string[jj] == '\\')
						bslash_count++;
					else
						bslash_count = 0;
				}

				if (options_string[jj] == 0)
				{
					psql_error("parse error at the end of line\n");
					*string = &options_string[jj];
					return NULL;
				}

				return_val = unescape(&options_string[pos + 1], jj - pos - 1);
				*string = &options_string[jj + 1];
				if (quote)
					*quote = '\'';
				return return_val;
			}

			/*
			 * Backticks are for command substitution, like in shells
			 */
		case '`':
			{
				bool		error = false;
				FILE	   *fd;
				char	   *file;
				PQExpBufferData output;
				char		buf[512];
				size_t		result,
							len;

				len = strcspn(options_string + pos + 1, "`");
				if (options_string[pos + 1 + len] == 0)
				{
					psql_error("parse error at the end of line\n");
					*string = &options_string[pos + 1 + len];
					return NULL;
				}

				options_string[pos + 1 + len] = '\0';
				file = options_string + pos + 1;

				fd = popen(file, "r");
				if (!fd)
				{
					psql_error("%s: %s\n", file, strerror(errno));
					error = true;
				}

				initPQExpBuffer(&output);

				if (!error)
				{
					do
					{
						result = fread(buf, 1, 512, fd);
						if (ferror(fd))
						{
							psql_error("%s: %s\n", file, strerror(errno));
							error = true;
							break;
						}
						appendBinaryPQExpBuffer(&output, buf, result);
					} while (!feof(fd));
					appendPQExpBufferChar(&output, '\0');
				}

				if (fd && pclose(fd) == -1)
				{
					psql_error("%s: %s\n", file, strerror(errno));
					error = true;
				}

				if (!error)
				{
					if (output.data[strlen(output.data) - 1] == '\n')
						output.data[strlen(output.data) - 1] = '\0';
					return_val = output.data;
				}
				else
				{
					return_val = xstrdup("");
					termPQExpBuffer(&output);
				}

				options_string[pos + 1 + len] = '`';
				*string = options_string + pos + len + 2;
				if (quote)
					*quote = '`';
				return return_val;
			}

			/*
			 * Variable substitution
			 */
		case ':':
			{
				size_t		token_end;
				const char *value;
				char		save_char;

				token_end = strcspn(&options_string[pos + 1], " \t\n\r");
				save_char = options_string[pos + token_end + 1];
				options_string[pos + token_end + 1] = '\0';
				value = GetVariable(pset.vars, options_string + pos + 1);
				return_val = xstrdup(value ? value : "");
				options_string[pos + token_end + 1] = save_char;
				*string = &options_string[pos + token_end + 1];
				/* XXX should we set *quote to ':' here? */
				return return_val;
			}

			/*
			 * | could be the beginning of a pipe if so, take rest of line
			 * as command
			 */
		case '|':
			if (type == OT_FILEPIPE)
			{
				*string += strlen(*string);
				return xstrdup(options_string + pos);
			}
			/* fallthrough for other option types */

			/*
			 * Default case: token extends to next whitespace, except that
			 * whitespace within double quotes doesn't end the token.
			 *
			 * If we are processing the option as a SQL identifier, then
			 * downcase unquoted letters and remove double-quotes --- but
			 * doubled double-quotes become output double-quotes, per
			 * spec.
			 *
			 * Note that a string like FOO"BAR"BAZ will be converted to
			 * fooBARbaz; this is somewhat inconsistent with the SQL spec,
			 * which would have us parse it as several identifiers.  But
			 * for psql's purposes, we want a string like "foo"."bar" to
			 * be treated as one option, so there's little choice.
			 */
		default:
			{
				bool		inquotes = false;
				size_t		token_len;
				char	   *cp;

				/* Find end of option */

				cp = &options_string[pos];
				for (;;)
				{
					/* Find next quote, whitespace, or end of string */
					cp += strcspn(cp, "\" \t\n\r");
					if (inquotes)
					{
						if (*cp == '\0')
						{
							psql_error("parse error at the end of line\n");
							*string = cp;
							return NULL;
						}
						if (*cp == '"')
							inquotes = false;
						cp++;
					}
					else
					{
						if (*cp != '"')
							break;		/* whitespace or end of string */
						if (quote)
							*quote = '"';
						inquotes = true;
						cp++;
					}
				}

				*string = cp;

				/* Copy the option */
				token_len = cp - &options_string[pos];

				return_val = malloc(token_len + 1);
				if (!return_val)
				{
					psql_error("out of memory\n");
					exit(EXIT_FAILURE);
				}

				memcpy(return_val, &options_string[pos], token_len);
				return_val[token_len] = '\0';

				/* Strip any trailing semi-colons if requested */
				if (semicolon)
				{
					int			i;

					for (i = token_len - 1;
						 i >= 0 && return_val[i] == ';';
						 i--)
						 /* skip */ ;

					if (i < 0)
					{
						/* nothing left after stripping the semicolon... */
						free(return_val);
						return NULL;
					}

					if (i < (int) token_len - 1)
						return_val[i + 1] = '\0';
				}

				/*
				 * If SQL identifier processing was requested, then we
				 * strip out excess double quotes and downcase unquoted
				 * letters.
				 */
				if (type == OT_SQLID || type == OT_SQLIDHACK)
				{
					inquotes = false;
					cp = return_val;

					while (*cp)
					{
						if (*cp == '"')
						{
							if (inquotes && cp[1] == '"')
							{
								/* Keep the first quote, remove the second */
								cp++;
							}
							inquotes = !inquotes;
							/* Collapse out quote at *cp */
							memmove(cp, cp + 1, strlen(cp));
							/* do not advance cp */
						}
						else
						{
							if (!inquotes && type == OT_SQLID)
							{
								if (isupper((unsigned char) *cp))
									*cp = tolower((unsigned char) *cp);
							}
							cp += PQmblen(cp, pset.encoding);
						}
					}
				}

				return return_val;
			}
	}
}



/*
 * unescape
 *
 * Replaces \n, \t, and the like.
 *
 * The return value is malloc()'ed.
 */
static char *
unescape(const unsigned char *source, size_t len)
{
	const unsigned char *p;
	bool		esc = false;	/* Last character we saw was the escape
								 * character */
	char	   *destination,
			   *tmp;
	size_t		length;

#ifdef USE_ASSERT_CHECKING
	assert(source);
#endif

	length = Min(len, strlen(source)) + 1;

	tmp = destination = malloc(length);
	if (!tmp)
	{
		psql_error("out of memory\n");
		exit(EXIT_FAILURE);
	}

	for (p = source; p - source < (int) len && *p; p += PQmblen(p, pset.encoding))
	{
		if (esc)
		{
			char		c;

			switch (*p)
			{
				case 'n':
					c = '\n';
					break;
				case 't':
					c = '\t';
					break;
				case 'b':
					c = '\b';
					break;
				case 'r':
					c = '\r';
					break;
				case 'f':
					c = '\f';
					break;
				case '0':
				case '1':
				case '2':
				case '3':
				case '4':
				case '5':
				case '6':
				case '7':
				case '8':
				case '9':
					c = parse_char((char **) &p);
					break;

				default:
					c = *p;
			}
			*tmp++ = c;
			esc = false;
		}

		else if (*p == '\\')
			esc = true;

		else
		{
			int			i;
			const unsigned char *mp = p;

			for (i = 0; i < PQmblen(p, pset.encoding); i++)
				*tmp++ = *mp++;
			esc = false;
		}
	}

	*tmp = '\0';
	return destination;
}



/* do_connect
 * -- handler for \connect
 *
 * Connects to a database (new_dbname) as a certain user (new_user).
 * The new user can be NULL. A db name of "-" is the same as the old one.
 * (That is, the one currently in pset. But pset.db can also be NULL. A NULL
 * dbname is handled by libpq.)
 * Returns true if all ok, false if the new connection couldn't be established.
 * The old connection will be kept if the session is interactive.
 */
static bool
do_connect(const char *new_dbname, const char *new_user)
{
	PGconn	   *oldconn = pset.db;
	const char *dbparam = NULL;
	const char *userparam = NULL;
	const char *pwparam = NULL;
	char	   *prompted_password = NULL;
	bool		need_pass;
	bool		success = false;

	/* Delete variables (in case we fail before setting them anew) */
	UnsyncVariables();

	/* If dbname is "" then use old name, else new one (even if NULL) */
	if (oldconn && new_dbname && PQdb(oldconn) && strcmp(new_dbname, "") == 0)
		dbparam = PQdb(oldconn);
	else
		dbparam = new_dbname;

	/* If user is "" then use the old one */
	if (new_user && PQuser(oldconn) && strcmp(new_user, "") == 0)
		userparam = PQuser(oldconn);
	else
		userparam = new_user;

	/* need to prompt for password? */
	if (pset.getPassword)
		pwparam = prompted_password = simple_prompt("Password: ", 100, false);

	/*
	 * Use old password (if any) if no new one given and we are
	 * reconnecting as same user
	 */
	if (!pwparam && oldconn && PQuser(oldconn) && userparam &&
		strcmp(PQuser(oldconn), userparam) == 0)
		pwparam = PQpass(oldconn);

	do
	{
		need_pass = false;
		pset.db = PQsetdbLogin(PQhost(oldconn), PQport(oldconn),
							   NULL, NULL, dbparam, userparam, pwparam);

		if (PQstatus(pset.db) == CONNECTION_BAD &&
			strcmp(PQerrorMessage(pset.db), "fe_sendauth: no password supplied\n") == 0 &&
			!feof(stdin))
		{
			PQfinish(pset.db);
			need_pass = true;
			free(prompted_password);
			prompted_password = NULL;
			pwparam = prompted_password = simple_prompt("Password: ", 100, false);
		}
	} while (need_pass);

	free(prompted_password);

	/*
	 * If connection failed, try at least keep the old one. That's
	 * probably more convenient than just kicking you out of the program.
	 */
	if (!pset.db || PQstatus(pset.db) == CONNECTION_BAD)
	{
		if (pset.cur_cmd_interactive)
		{
			psql_error("%s", PQerrorMessage(pset.db));
			PQfinish(pset.db);
			if (oldconn)
			{
				fputs(gettext("Previous connection kept\n"), stderr);
				pset.db = oldconn;
			}
			else
				pset.db = NULL;
		}
		else
		{
			/*
			 * we don't want unpredictable things to happen in scripting
			 * mode
			 */
			psql_error("\\connect: %s", PQerrorMessage(pset.db));
			PQfinish(pset.db);
			if (oldconn)
				PQfinish(oldconn);
			pset.db = NULL;
		}
	}
	else
	{
		if (!QUIET())
		{
			if (userparam != new_user)	/* no new user */
				printf(gettext("You are now connected to database \"%s\".\n"), dbparam);
			else if (dbparam != new_dbname)		/* no new db */
				printf(gettext("You are now connected as new user \"%s\".\n"), new_user);
			else
/* both new */
				printf(gettext("You are now connected to database \"%s\" as user \"%s\".\n"),
					   PQdb(pset.db), PQuser(pset.db));
		}

		if (oldconn)
			PQfinish(oldconn);

		success = true;
	}

	PQsetNoticeProcessor(pset.db, NoticeProcessor, NULL);

	/* Update variables */
	SyncVariables();

	return success;
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

	SetVariable(pset.vars, "DBNAME", PQdb(pset.db));
	SetVariable(pset.vars, "USER", PQuser(pset.db));
	SetVariable(pset.vars, "HOST", PQhost(pset.db));
	SetVariable(pset.vars, "PORT", PQport(pset.db));
	SetVariable(pset.vars, "ENCODING", pg_encoding_to_char(pset.encoding));

	/* send stuff to it, too */
	SyncVerbosityVariable();
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
 * Update connection state from VERBOSITY variable
 */
void
SyncVerbosityVariable(void)
{
	switch (SwitchVariable(pset.vars, "VERBOSITY",
						   "default", "terse", "verbose", NULL))
	{
		case 1:			/* default */
			PQsetErrorVerbosity(pset.db, PQERRORS_DEFAULT);
			break;
		case 2:			/* terse */
			PQsetErrorVerbosity(pset.db, PQERRORS_TERSE);
			break;
		case 3:			/* verbose */
			PQsetErrorVerbosity(pset.db, PQERRORS_VERBOSE);
			break;
		default:				/* not set or unrecognized value */
			PQsetErrorVerbosity(pset.db, PQERRORS_DEFAULT);
			break;
	}
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

#ifdef USE_ASSERT_CHECKING
	assert(fname);
#else
	if (!fname)
		return false;
#endif

	/* Find an editor to use */
	editorName = getenv("PSQL_EDITOR");
	if (!editorName)
		editorName = getenv("EDITOR");
	if (!editorName)
		editorName = getenv("VISUAL");
	if (!editorName)
		editorName = DEFAULT_EDITOR;

	sys = malloc(strlen(editorName) + strlen(fname) + 10 + 1);
	if (!sys)
		return false;
	sprintf(sys,
#ifndef WIN32
			"exec "
#endif
			"%s '%s'", editorName, fname);
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

#ifndef WIN32
	struct stat before,
				after;
#endif

	if (filename_arg)
		fname = filename_arg;

	else
	{
		/* make a temp file to edit */
#ifndef WIN32
		const char *tmpdirenv = getenv("TMPDIR");

		snprintf(fnametmp, sizeof(fnametmp), "%s/psql.edit.%ld.%ld",
				 tmpdirenv ? tmpdirenv : "/tmp",
				 (long) geteuid(), (long) getpid());
#else
		GetTempFileName(".", "psql", 0, fnametmp);
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
			else
				fclose(stream);
		}
	}

#ifndef WIN32
	if (!error && stat(fname, &before) != 0)
	{
		psql_error("%s: %s\n", fname, strerror(errno));
		error = true;
	}
#endif

	/* call editor */
	if (!error)
		error = !editFile(fname);

#ifndef WIN32
	if (!error && stat(fname, &after) != 0)
	{
		psql_error("%s: %s\n", fname, strerror(errno));
		error = true;
	}

	if (!error && before.st_mtime != after.st_mtime)
	{
#else
	if (!error)
	{
#endif
		stream = fopen(fname, "r");
		if (!stream)
		{
			psql_error("%s: %s\n", fname, strerror(errno));
			error = true;
		}
		else
		{
			/* read file back in */
			char		line[1024];

			resetPQExpBuffer(query_buf);
			while (fgets(line, sizeof(line), stream) != NULL)
				appendPQExpBufferStr(query_buf, line);

			if (ferror(stream))
			{
				psql_error("%s: %s\n", fname, strerror(errno));
				error = true;
			}

#ifdef USE_READLINE
#ifdef HAVE_REPLACE_HISTORY_ENTRY

			replace_history_entry(where_history(), query_buf->data, NULL);
#else
			add_history(query_buf->data);
#endif
#endif
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
 * Handler for \i, but can be used for other things as well.
 */
int
process_file(char *filename)
{
	FILE	   *fd;
	int			result;
	char	   *oldfilename;

	if (!filename)
		return false;

	fd = fopen(filename, "r");

	if (!fd)
	{
		psql_error("%s: %s\n", filename, strerror(errno));
		return false;
	}

	oldfilename = pset.inputfile;
	pset.inputfile = filename;
	result = MainLoop(fd);
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
	}
	return "unknown";
}


bool
do_pset(const char *param, const char *value, printQueryOpt *popt, bool quiet)
{
	size_t		vallen = 0;

#ifdef USE_ASSERT_CHECKING
	assert(param);
#else
	if (!param)
		return false;
#endif

	if (value)
		vallen = strlen(value);

	/* set format */
	if (strcmp(param, "format") == 0)
	{
		if (!value)
			;
		else if (strncasecmp("unaligned", value, vallen) == 0)
			popt->topt.format = PRINT_UNALIGNED;
		else if (strncasecmp("aligned", value, vallen) == 0)
			popt->topt.format = PRINT_ALIGNED;
		else if (strncasecmp("html", value, vallen) == 0)
			popt->topt.format = PRINT_HTML;
		else if (strncasecmp("latex", value, vallen) == 0)
			popt->topt.format = PRINT_LATEX;
		else
		{
			psql_error("\\pset: allowed formats are unaligned, aligned, html, latex\n");
			return false;
		}

		if (!quiet)
			printf(gettext("Output format is %s.\n"), _align2string(popt->topt.format));
	}

	/* set border style/width */
	else if (strcmp(param, "border") == 0)
	{
		if (value)
			popt->topt.border = atoi(value);

		if (!quiet)
			printf(gettext("Border style is %d.\n"), popt->topt.border);
	}

	/* set expanded/vertical mode */
	else if (strcmp(param, "x") == 0 || strcmp(param, "expanded") == 0 || strcmp(param, "vertical") == 0)
	{
		popt->topt.expanded = !popt->topt.expanded;
		if (!quiet)
			printf(popt->topt.expanded
				   ? gettext("Expanded display is on.\n")
				   : gettext("Expanded display is off.\n"));
	}

	/* null display */
	else if (strcmp(param, "null") == 0)
	{
		if (value)
		{
			free(popt->nullPrint);
			popt->nullPrint = xstrdup(value);
		}
		if (!quiet)
			printf(gettext("Null display is \"%s\".\n"), popt->nullPrint ? popt->nullPrint : "");
	}

	/* field separator for unaligned text */
	else if (strcmp(param, "fieldsep") == 0)
	{
		if (value)
		{
			free(popt->topt.fieldSep);
			popt->topt.fieldSep = xstrdup(value);
		}
		if (!quiet)
			printf(gettext("Field separator is \"%s\".\n"), popt->topt.fieldSep);
	}

	/* record separator for unaligned text */
	else if (strcmp(param, "recordsep") == 0)
	{
		if (value)
		{
			free(popt->topt.recordSep);
			popt->topt.recordSep = xstrdup(value);
		}
		if (!quiet)
		{
			if (strcmp(popt->topt.recordSep, "\n") == 0)
				printf(gettext("Record separator is <newline>."));
			else
				printf(gettext("Record separator is \"%s\".\n"), popt->topt.recordSep);
		}
	}

	/* toggle between full and barebones format */
	else if (strcmp(param, "t") == 0 || strcmp(param, "tuples_only") == 0)
	{
		popt->topt.tuples_only = !popt->topt.tuples_only;
		if (!quiet)
		{
			if (popt->topt.tuples_only)
				puts(gettext("Showing only tuples."));
			else
				puts(gettext("Tuples only is off."));
		}
	}

	/* set title override */
	else if (strcmp(param, "title") == 0)
	{
		free(popt->title);
		if (!value)
			popt->title = NULL;
		else
			popt->title = xstrdup(value);

		if (!quiet)
		{
			if (popt->title)
				printf(gettext("Title is \"%s\".\n"), popt->title);
			else
				printf(gettext("Title is unset.\n"));
		}
	}

	/* set HTML table tag options */
	else if (strcmp(param, "T") == 0 || strcmp(param, "tableattr") == 0)
	{
		free(popt->topt.tableAttr);
		if (!value)
			popt->topt.tableAttr = NULL;
		else
			popt->topt.tableAttr = xstrdup(value);

		if (!quiet)
		{
			if (popt->topt.tableAttr)
				printf(gettext("Table attribute is \"%s\".\n"), popt->topt.tableAttr);
			else
				printf(gettext("Table attributes unset.\n"));
		}
	}

	/* toggle use of pager */
	else if (strcmp(param, "pager") == 0)
	{
		if (value && strcasecmp(value, "always") == 0)
			popt->topt.pager = 2;
		else if (popt->topt.pager == 1)
			popt->topt.pager = 0;
		else
			popt->topt.pager = 1;
		if (!quiet)
		{
			if (popt->topt.pager == 1)
				puts(gettext("Pager is used for long output."));
			else if (popt->topt.pager == 2)
				puts(gettext("Pager is always used."));
			else
				puts(gettext("Pager usage is off."));
		}
	}

	/* disable "(x rows)" footer */
	else if (strcmp(param, "footer") == 0)
	{
		popt->default_footer = !popt->default_footer;
		if (!quiet)
		{
			if (popt->default_footer)
				puts(gettext("Default footer is on."));
			else
				puts(gettext("Default footer is off."));
		}
	}

	else
	{
		psql_error("\\pset: unknown option: %s\n", param);
		return false;
	}

	return true;
}



#define DEFAULT_SHELL "/bin/sh"

static bool
do_shell(const char *command)
{
	int			result;

	if (!command)
	{
		char	   *sys;
		const char *shellName;

		shellName = getenv("SHELL");
		if (shellName == NULL)
			shellName = DEFAULT_SHELL;

		sys = malloc(strlen(shellName) + 16);
		if (!sys)
		{
			psql_error("out of memory\n");
			if (pset.cur_cmd_interactive)
				return false;
			else
				exit(EXIT_FAILURE);
		}
		sprintf(sys,
#ifndef WIN32
				"exec "
#endif
				"%s", shellName);
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
