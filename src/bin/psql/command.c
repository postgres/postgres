#include <config.h>
#include <c.h>
#include "command.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#ifndef WIN32
#include <sys/types.h>			/* for umask() */
#include <sys/stat.h>			/* for umask(), stat() */
#include <unistd.h>				/* for geteuid(), getpid(), stat() */
#endif
#include <assert.h>

#include <libpq-fe.h>
#include <pqexpbuffer.h>

#include "stringutils.h"
#include "mainloop.h"
#include "copy.h"
#include "help.h"
#include "settings.h"
#include "common.h"
#include "large_obj.h"
#include "print.h"
#include "describe.h"
#include "input.h"

#ifdef WIN32
#define popen(x,y) _popen(x,y)
#define pclose(x) _pclose(x)
#endif


/* functions for use in this file */

static backslashResult exec_command(const char *cmd,
			 char *const * options,
			 const char *options_string,
			 PQExpBuffer query_buf,
			 PsqlSettings *pset);

static bool do_edit(const char *filename_arg, PQExpBuffer query_buf);

static char * unescape(const char *source, PsqlSettings *pset);

static bool do_connect(const char *new_dbname,
                       const char *new_user,
                       PsqlSettings *pset);


static bool do_shell(const char *command);

/*
 * Perhaps this should be changed to "infinity",
 * but there is no convincing reason to bother
 * at this point.
 */
#define NR_OPTIONS 16


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
 * query_buf can be NULL if there is no query-so-far.
 *
 * Returns a status code indicating what action is desired, see command.h.
 *----------
 */

backslashResult
HandleSlashCmds(PsqlSettings *pset,
				const char *line,
				PQExpBuffer query_buf,
				const char **end_of_cmd)
{
	backslashResult status = CMD_SKIP_LINE;
	char	   *my_line;
	char	   *options[NR_OPTIONS+1];
	char	   *token;
	const char *options_string = NULL;
	const char *cmd;
	size_t		blank_loc;
	int			i;
	const char *continue_parse = NULL;	/* tell the mainloop where the
										 * backslash command ended */

	my_line = xstrdup(line);

	/*
	 * Find the first whitespace. line[blank_loc] will now
	 * be the whitespace character or the \0 at the end
     *
     * Also look for a backslash, so stuff like \p\g works.
	 */
	blank_loc = strcspn(my_line, " \t\\");

    if (my_line[blank_loc] == '\\')
    {
        continue_parse = &my_line[blank_loc];
		my_line[blank_loc] = '\0';
    }
	/* do we have an option string? */
	else if (my_line[blank_loc] != '\0')
    {
        options_string = &my_line[blank_loc + 1];
		my_line[blank_loc] = '\0';
	}

    options[0] = NULL;

	if (options_string)
	{
		char		quote;
		unsigned int pos;

		options_string = &options_string[strspn(options_string, " \t")];		/* skip leading
																				 * whitespace */

		i = 0;
		token = strtokx(options_string, " \t", "\"'`", '\\', &quote, &pos);

		for (i = 0; token && i < NR_OPTIONS; i++)
		{
			switch (quote)
			{
				case '"':
					options[i] = unescape(token, pset);
					break;
				case '\'':
					options[i] = xstrdup(token);
					break;
				case '`':
					{
						bool		error = false;
						FILE	   *fd = NULL;
						char	   *file = unescape(token, pset);
						PQExpBufferData output;
						char		buf[512];
						size_t		result;

						fd = popen(file, "r");
						if (!fd)
						{
							perror(file);
							error = true;
						}

						if (!error)
						{
							initPQExpBuffer(&output);

							do
							{
								result = fread(buf, 1, 512, fd);
								if (ferror(fd))
								{
									perror(file);
									error = true;
									break;
								}
								appendBinaryPQExpBuffer(&output, buf, result);
							} while (!feof(fd));
							appendPQExpBufferChar(&output, '\0');

							if (pclose(fd) == -1)
							{
								perror(file);
								error = true;
							}
						}

						if (!error)
						{
							if (output.data[strlen(output.data) - 1] == '\n')
								output.data[strlen(output.data) - 1] = '\0';
						}

						free(file);
						if (!error)
							options[i] = output.data;
						else
						{
							options[i] = xstrdup("");
							termPQExpBuffer(&output);
						}
						break;
					}
				case 0:
				default:
					if (token[0] == '\\')
						continue_parse = options_string + pos;
					else if (token[0] == '$')
						options[i] = xstrdup(interpolate_var(token + 1, pset));
					else
						options[i] = xstrdup(token);
			}

			if (continue_parse)
				break;

			token = strtokx(NULL, " \t", "\"'`", '\\', &quote, &pos);
		} /* for */

        options[i] = NULL;
	}

	cmd = my_line;
	status = exec_command(cmd, options, options_string, query_buf, pset);

	if (status == CMD_UNKNOWN)
	{

		/*
		 * If the command was not recognized, try inserting a space after
		 * the first letter and call again. The one letter commands allow
		 * arguments to start immediately after the command, but that is
		 * no longer encouraged.
		 */
		const char *new_options[NR_OPTIONS+1];
		char		new_cmd[2];
		int			i;

		for (i = 1; i < NR_OPTIONS+1; i++)
			new_options[i] = options[i - 1];
		new_options[0] = cmd + 1;

		new_cmd[0] = cmd[0];
		new_cmd[1] = '\0';

		status = exec_command(new_cmd, (char *const *) new_options, my_line + 2, query_buf, pset);
	}

	if (status == CMD_UNKNOWN)
	{
        if (pset->cur_cmd_interactive)
            fprintf(stderr, "Invalid command \\%s. Try \\? for help.\n", cmd);
        else
            fprintf(stderr, "%s: invalid command \\%s", pset->progname, cmd);
		status = CMD_ERROR;
	}

	if (continue_parse && *(continue_parse + 1) == '\\')
		continue_parse += 2;


	if (end_of_cmd)
	{
		if (continue_parse)
			*end_of_cmd = line + (continue_parse - my_line);
		else
			*end_of_cmd = NULL;
	}

	/* clean up */
	for (i = 0; i < NR_OPTIONS && options[i]; i++)
		free(options[i]);

	free(my_line);

	return status;
}




static backslashResult
exec_command(const char *cmd,
			 char *const * options,
			 const char *options_string,
			 PQExpBuffer query_buf,
			 PsqlSettings *pset)
{
	bool		success = true; /* indicate here if the command ran ok or
								 * failed */
	bool		quiet = GetVariableBool(pset->vars, "quiet");

	backslashResult status = CMD_SKIP_LINE;


	/* \a -- toggle field alignment This makes little sense but we keep it around. */
	if (strcmp(cmd, "a") == 0)
	{
		if (pset->popt.topt.format != PRINT_ALIGNED)
			success = do_pset("format", "aligned", &pset->popt, quiet);
		else
			success = do_pset("format", "unaligned", &pset->popt, quiet);
	}


	/* \C -- override table title (formerly change HTML caption) */
	else if (strcmp(cmd, "C") == 0)
		success = do_pset("title", options[0], &pset->popt, quiet);


	/*----------
	 * \c or \connect -- connect to new database or as different user
	 *
	 * \c foo bar: connect to db "foo" as user "bar"
     * \c foo [-]: connect to db "foo" as current user
     * \c - bar:   connect to current db as user "bar"
     * \c:          connect to default db as default user
     *----------
	 */
	else if (strcmp(cmd, "c") == 0 || strcmp(cmd, "connect") == 0)
	{
		if (options[1])
			/* gave username */
			success = do_connect(options[0], options[1], pset);
		else
		{
			if (options[0])
				/* gave database name */
				success = do_connect(options[0], "", pset);		/* empty string is same
																 * username as before,
																 * NULL would mean libpq
																 * default */
			else
				/* connect to default db as default user */
				success = do_connect(NULL, NULL, pset);
		}
	}


	/* \copy */
	else if (strcmp(cmd, "copy") == 0)
		success = do_copy(options_string, pset);

	/* \copyright */
	else if (strcmp(cmd, "copyright") == 0)
		print_copyright();

	/* \d* commands */
	else if (cmd[0] == 'd')
	{
        bool show_verbose = strchr(cmd, '+') ? true : false;

		switch (cmd[1])
		{
			case '\0':
            case '?':
				if (options[0])
					success = describeTableDetails(options[0], pset, show_verbose);
				else
                    /* standard listing of interesting things */
					success = listTables("tvs", NULL, pset, show_verbose);
				break;
			case 'a':
				success = describeAggregates(options[0], pset);
				break;
			case 'd':
				success = objectDescription(options[0], pset);
				break;
			case 'f':
				success = describeFunctions(options[0], pset, show_verbose);
				break;
			case 'l':
				success = do_lo_list(pset);
				break;
			case 'o':
				success = describeOperators(options[0], pset);
				break;
			case 'p':
				success = permissionsList(options[0], pset);
				break;
			case 'T':
				success = describeTypes(options[0], pset, show_verbose);
				break;
			case 't':
			case 'v':
			case 'i':
			case 's':
			case 'S':
				if (cmd[1] == 'S' && cmd[2] == '\0')
					success = listTables("Stvs", NULL, pset, show_verbose);
				else
					success = listTables(&cmd[1], options[0], pset, show_verbose);
				break;
			default:
				status = CMD_UNKNOWN;
		}
	}


	/*
	 * \e or \edit -- edit the current query buffer (or a file and make it
	 * the query buffer
	 */
	else if (strcmp(cmd, "e") == 0 || strcmp(cmd, "edit") == 0)
		status = do_edit(options[0], query_buf) ? CMD_NEWEDIT : CMD_ERROR;


	/* \echo */
	else if (strcmp(cmd, "echo") == 0)
	{
		int			i;

		for (i = 0; i < 16 && options[i]; i++)
			fputs(options[i], stdout);
		fputs("\n", stdout);
	}

	/* \f -- change field separator */
	else if (strcmp(cmd, "f") == 0)
		success = do_pset("fieldsep", options[0], &pset->popt, quiet);

	/* \g means send query */
	else if (strcmp(cmd, "g") == 0)
	{
		if (!options[0])
			pset->gfname = NULL;
		else
			pset->gfname = xstrdup(options[0]);
		status = CMD_SEND;
	}

	/* help */
	else if (strcmp(cmd, "h") == 0 || strcmp(cmd, "help") == 0)
    {
        char buf[256] = "";
        int i;
        for (i=0; options && options[i] && strlen(buf)<255; i++)
        {
            strncat(buf, options[i], 255 - strlen(buf));
            if (strlen(buf)<255 && options[i+1])
                strcat(buf, " ");
        }
        buf[255] = '\0';
		helpSQL(buf);
    }

	/* HTML mode */
	else if (strcmp(cmd, "H") == 0 || strcmp(cmd, "html") == 0)
    {
		if (pset->popt.topt.format != PRINT_HTML)
			success = do_pset("format", "html", &pset->popt, quiet);
		else
			success = do_pset("format", "aligned", &pset->popt, quiet);
    }


	/* \i is include file */
	else if (strcmp(cmd, "i") == 0 || strcmp(cmd, "include") == 0)
	{
		if (!options[0])
		{
            if (pset->cur_cmd_interactive)
                fprintf(stderr, "\\%s: missing required argument\n", cmd);
            else
                fprintf(stderr, "%s: \\%s: missing required argument", pset->progname, cmd);
			success = false;
		}
		else
			success = process_file(options[0], pset);
	}


	/* \l is list databases */
	else if (strcmp(cmd, "l") == 0 || strcmp(cmd, "list") == 0)
		success = listAllDbs(pset, false);
	else if (strcmp(cmd, "l+") == 0 || strcmp(cmd, "list+") == 0)
		success = listAllDbs(pset, true);


	/* large object things */
	else if (strncmp(cmd, "lo_", 3) == 0)
	{
		if (strcmp(cmd + 3, "export") == 0)
		{
			if (!options[1])
			{
                if (pset->cur_cmd_interactive)
                    fprintf(stderr, "\\%s: missing required argument", cmd);
                else
                    fprintf(stderr, "%s: \\%s: missing required argument", pset->progname, cmd);
				success = false;
			}
			else
				success = do_lo_export(pset, options[0], options[1]);
		}

		else if (strcmp(cmd + 3, "import") == 0)
		{
			if (!options[0])
			{
                if (pset->cur_cmd_interactive)
                    fprintf(stderr, "\\%s: missing required argument", cmd);
                else
                    fprintf(stderr, "%s: \\%s: missing required argument", pset->progname, cmd);
				success = false;
			}
			else
				success = do_lo_import(pset, options[0], options[1]);
		}

		else if (strcmp(cmd + 3, "list") == 0)
			success = do_lo_list(pset);

		else if (strcmp(cmd + 3, "unlink") == 0)
		{
			if (!options[0])
			{
                if (pset->cur_cmd_interactive)
                    fprintf(stderr, "\\%s: missing required argument", cmd);
                else
                    fprintf(stderr, "%s: \\%s: missing required argument", pset->progname, cmd);
				success = false;
			}
			else
				success = do_lo_unlink(pset, options[0]);
		}

		else
			status = CMD_UNKNOWN;
	}

	/* \o -- set query output */
	else if (strcmp(cmd, "o") == 0 || strcmp(cmd, "out") == 0)
		success = setQFout(options[0], pset);


	/* \p prints the current query buffer */
	else if (strcmp(cmd, "p") == 0 || strcmp(cmd, "print") == 0)
	{
		if (query_buf && query_buf->len > 0)
			puts(query_buf->data);
		else if (!quiet)
			puts("Query buffer is empty.");
		fflush(stdout);
	}

	/* \pset -- set printing parameters */
	else if (strcmp(cmd, "pset") == 0)
	{
		if (!options[0])
		{
            if (pset->cur_cmd_interactive)
                fprintf(stderr, "\\%s: missing required argument", cmd);
            else
                fprintf(stderr, "%s: \\%s: missing required argument", pset->progname, cmd);
			success = false;
		}
		else
			success = do_pset(options[0], options[1], &pset->popt, quiet);
	}

	/* \q or \quit */
	else if (strcmp(cmd, "q") == 0 || strcmp(cmd, "quit") == 0)
		status = CMD_TERMINATE;

	/* \qecho */
	else if (strcmp(cmd, "qecho") == 0)
	{
		int			i;

		for (i = 0; i < 16 && options[i]; i++)
			fputs(options[i], pset->queryFout);
		fputs("\n", pset->queryFout);
	}

	/* reset(clear) the buffer */
	else if (strcmp(cmd, "r") == 0 || strcmp(cmd, "reset") == 0)
	{
		resetPQExpBuffer(query_buf);
		if (!quiet)
			puts("Query buffer reset (cleared).");
	}


	/* \s save history in a file or show it on the screen */
	else if (strcmp(cmd, "s") == 0)
	{
		const char *fname;

		if (!options[0])
			fname = "/dev/tty";
		else
			fname = options[0];

		success = saveHistory(fname);

		if (success && !quiet && options[0])
			printf("Wrote history to %s.\n", fname);
	}


	/* \set -- generalized set option command */
	else if (strcmp(cmd, "set") == 0)
	{
		if (!options[0])
		{
			/* list all variables */

			/*
			 * (This is in utter violation of the GetVariable abstraction,
			 * but I have not dreamt up a better way.)
			 */
			struct _variable *ptr;

			for (ptr = pset->vars; ptr->next; ptr = ptr->next)
				fprintf(stdout, "%s = '%s'\n", ptr->next->name, ptr->next->value);
			success = true;
		}
		else
		{
			if (!SetVariable(pset->vars, options[0], options[1]))
			{
                if (pset->cur_cmd_interactive)
                    fprintf(stderr, "\\%s: failed\n", cmd);
                else
                    fprintf(stderr, "%s: \\%s: failed\n", pset->progname, cmd);

				success = false;
			}
		}
	}

	/* \t -- turn off headers and row count */
	else if (strcmp(cmd, "t") == 0)
		success = do_pset("tuples_only", NULL, &pset->popt, quiet);


	/* \T -- define html <table ...> attributes */
	else if (strcmp(cmd, "T") == 0)
		success = do_pset("tableattr", options[0], &pset->popt, quiet);


	/* \w -- write query buffer to file */
	else if (strcmp(cmd, "w") == 0 || strcmp(cmd, "write") == 0)
	{
		FILE	   *fd = NULL;
		bool		pipe = false;

		if (!options[0])
		{
            if (pset->cur_cmd_interactive)
                fprintf(stderr, "\\%s: missing required argument", cmd);
            else
                fprintf(stderr, "%s: \\%s: missing required argument", pset->progname, cmd);
			success = false;
		}
		else
		{
			if (options[0][0] == '|')
			{
				pipe = true;
#ifndef __CYGWIN32__
				fd = popen(&options[0][1], "w");
#else
				fd = popen(&options[0][1], "wb");
#endif
			}
			else
			{
#ifndef __CYGWIN32__
				fd = fopen(options[0], "w");
#else
				fd = fopen(options[0], "wb");
#endif
			}

			if (!fd)
			{
				perror(options[0]);
				success = false;
			}
		}

		if (fd)
		{
			int			result;

			if (query_buf && query_buf->len > 0)
				fprintf(fd, "%s\n", query_buf->data);

			if (pipe)
				result = pclose(fd);
			else
				result = fclose(fd);

			if (result == EOF)
			{
				perror("close");
				success = false;
			}
		}
	}

	/* \x -- toggle expanded table representation */
	else if (strcmp(cmd, "x") == 0)
		success = do_pset("expanded", NULL, &pset->popt, quiet);


	/* list table rights (grant/revoke) */
	else if (strcmp(cmd, "z") == 0)
		success = permissionsList(options[0], pset);


	else if (strcmp(cmd, "!") == 0)
		success = do_shell(options_string);

	else if (strcmp(cmd, "?") == 0)
		slashUsage(pset);


#ifdef NOT_USED

	/*
	 * These commands don't do anything. I just use them to test the
	 * parser.
	 */
	else if (strcmp(cmd, "void") == 0 || strcmp(cmd, "#") == 0)
	{
		int			i;

		fprintf(stderr, "+ optline = |%s|\n", options_string);
		for (i = 0; options[i]; i++)
			fprintf(stderr, "+ opt%d = |%s|\n", i, options[i]);
	}
#endif

	else
		status = CMD_UNKNOWN;

	if (!success)
		status = CMD_ERROR;
	return status;
}



/*
 * unescape
 *
 * Replaces \n, \t, and the like.
 * Also interpolates ${variables}.
 *
 * The return value is malloc()'ed.
 */
static char *
unescape(const char *source, PsqlSettings *pset)
{
	unsigned char *p;
	bool		esc = false;	/* Last character we saw was the escape
								 * character */
	char	   *destination,
			   *tmp;
	size_t		length;

#ifdef USE_ASSERT_CHECKING
	assert(source);
#endif

	length = strlen(source) + 1;

	tmp = destination = (char *) malloc(length);
	if (!tmp)
	{
		perror("malloc");
		exit(EXIT_FAILURE);
	}

	for (p = (char *) source; *p; p += PQmblen(p))
	{
		if (esc)
		{
			char		c;

			switch (*p)
			{
				case 'n':
					c = '\n';
					break;
				case 'r':
					c = '\r';
					break;
				case 't':
					c = '\t';
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
					{
						long int	l;
						char	   *end;

						l = strtol(p, &end, 0);
						c = l;
						p = end - 1;
						break;
					}
				default:
					c = *p;
			}
			*tmp++ = c;
			esc = false;
		}

		else if (*p == '\\')
			esc = true;

		else if (*p == '$')
		{
			if (*(p + 1) == '{')
			{
				unsigned int len;
				char	   *copy;
				const char *value;
				void	   *new;

				len = strcspn(p + 2, "}");
				copy = xstrdup(p + 2);
				copy[len] = '\0';
				value = interpolate_var(copy, pset);

				length += strlen(value) - (len + 3);
				new = realloc(destination, length);
				if (!new)
				{
					perror("realloc");
					exit(EXIT_FAILURE);
				}
				tmp = new + (tmp - destination);
				destination = new;

				strcpy(tmp, value);
				tmp += strlen(value);
				p += len + 2;
				free(copy);
			}
			else
				*tmp++ = '$';
		}

		else
		{
			*tmp++ = *p;
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
 * (That is, the one currently in pset. But pset->db can also be NULL. A NULL
 * dbname is handled by libpq.)
 * Returns true if all ok, false if the new connection couldn't be established
 * but the old one was set back. Otherwise it terminates the program.
 */
static bool
do_connect(const char *new_dbname, const char *new_user, PsqlSettings *pset)
{
	PGconn	   *oldconn = pset->db;
	const char *dbparam = NULL;
	const char *userparam = NULL;
	const char *pwparam = NULL;
	char	   *prompted_password = NULL;
	char	   *prompted_user = NULL;
	bool		need_pass;
	bool		success = false;

	/* If dbname is "-" then use old name, else new one (even if NULL) */
	if (new_dbname && PQdb(oldconn) && (strcmp(new_dbname, "-") == 0 || strcmp(new_dbname, PQdb(oldconn)) == 0))
		dbparam = PQdb(oldconn);
	else
		dbparam = new_dbname;

	/* If user is "" or "-" then use the old one */
	if (new_user && PQuser(oldconn) && (strcmp(new_user, "") == 0 || strcmp(new_user, "-") == 0 || strcmp(new_user, PQuser(oldconn)) == 0))
		userparam = PQuser(oldconn);
	/* If username is "?" then prompt */
	else if (new_user && strcmp(new_user, "?") == 0)
		userparam = prompted_user = simple_prompt("Username: ", 100, true);		/* save for free() */
	else
		userparam = new_user;

	/* need to prompt for password? */
	if (pset->getPassword)
		pwparam = prompted_password = simple_prompt("Password: ", 100, false);	/* need to save for
																				 * free() */

	/*
	 * Use old password if no new one given (if you didn't have an old
	 * one, fine)
	 */
	if (!pwparam)
		pwparam = PQpass(oldconn);


#ifdef MULTIBYTE

	/*
	 * PGCLIENTENCODING may be set by the previous connection. if a user
	 * does not explicitly set PGCLIENTENCODING, we should discard
	 * PGCLIENTENCODING so that libpq could get the backend encoding as
	 * the default PGCLIENTENCODING value. -- 1998/12/12 Tatsuo Ishii
	 */

	if (!pset->has_client_encoding)
		putenv("PGCLIENTENCODING=");
#endif

	do
	{
		need_pass = false;
		pset->db = PQsetdbLogin(PQhost(oldconn), PQport(oldconn),
								NULL, NULL, dbparam, userparam, pwparam);

		if (PQstatus(pset->db) == CONNECTION_BAD &&
			strcmp(PQerrorMessage(pset->db), "fe_sendauth: no password supplied\n") == 0)
		{
			need_pass = true;
			free(prompted_password);
			prompted_password = NULL;
			pwparam = prompted_password = simple_prompt("Password: ", 100, false);
		}
	} while (need_pass);

	free(prompted_password);
	free(prompted_user);

	/*
	 * If connection failed, try at least keep the old one. That's
	 * probably more convenient than just kicking you out of the program.
	 */
	if (!pset->db || PQstatus(pset->db) == CONNECTION_BAD)
	{
        if (pset->cur_cmd_interactive)
        {
            fprintf(stderr, "\\connect: %s", PQerrorMessage(pset->db));
            PQfinish(pset->db);
            if (oldconn)
            {
                fputs("Previous connection kept\n", stderr);
                pset->db = oldconn;
            }
            else
                pset->db = NULL;
        }
        else
        {
            /* we don't want unpredictable things to
             * happen in scripting mode */
            fprintf(stderr, "%s: \\connect: %s", pset->progname, PQerrorMessage(pset->db));
            PQfinish(pset->db);
			if (oldconn)
				PQfinish(oldconn);
            pset->db = NULL;
		}
	}
	else
	{
		if (!GetVariable(pset->vars, "quiet"))
		{
			if (userparam != new_user)	/* no new user */
				printf("You are now connected to database %s.\n", dbparam);
			else if (dbparam != new_dbname)		/* no new db */
				printf("You are now connected as new user %s.\n", new_user);
			else /* both new */
				printf("You are now connected to database %s as user %s.\n",
					   PQdb(pset->db), PQuser(pset->db));
		}

		if (oldconn)
			PQfinish(oldconn);

		success = true;
	}

	return success;
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
	char	   *editorName;
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

	sys = malloc(strlen(editorName) + strlen(fname) + 32 + 1);
	if (!sys)
		return false;
	sprintf(sys, "exec %s %s", editorName, fname);
	result = system(sys);
	if (result == -1 || result == 127)
		perror(sys);
	free(sys);

	return result == 0;
}


/* call this one */
static bool
do_edit(const char *filename_arg, PQExpBuffer query_buf)
{
	char		fnametmp[MAXPGPATH];
	FILE	   *stream;
	const char *fname;
	bool		error = false;

#ifndef WIN32
	struct stat before,
				after;

#endif

#ifdef USE_ASSERT_CHECKING
	assert(query_buf);
#else
	if (!query_buf)
		return false;
#endif


	if (filename_arg)
		fname = filename_arg;

	else
	{
		/* make a temp file to edit */
#ifndef WIN32
		mode_t		oldumask;
        const char *tmpdirenv = getenv("TMPDIR");

		sprintf(fnametmp, "%s/psql.edit.%ld.%ld",
                tmpdirenv ? tmpdirenv : "/tmp",
                (long) geteuid(), (long) getpid());
#else
		GetTempFileName(".", "psql", 0, fnametmp);
#endif
		fname = (const char *) fnametmp;

#ifndef WIN32
		oldumask = umask(0177);
#endif
		stream = fopen(fname, "w");
#ifndef WIN32
		umask(oldumask);
#endif

		if (!stream)
		{
			perror(fname);
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
				perror(fname);
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
		perror(fname);
		error = true;
	}
#endif

	/* call editor */
	if (!error)
		error = !editFile(fname);

#ifndef WIN32
	if (!error && stat(fname, &after) != 0)
	{
		perror(fname);
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
			perror(fname);
			error = true;
		}
		else
		{
			/* read file back in */
			char		line[1024];
			size_t		result;

			resetPQExpBuffer(query_buf);
			do
			{
				result = fread(line, 1, 1024, stream);
				if (ferror(stream))
				{
					perror(fname);
					error = true;
					break;
				}
				appendBinaryPQExpBuffer(query_buf, line, result);
			} while (!feof(stream));
			appendPQExpBufferChar(query_buf, '\0');

			fclose(stream);
		}

		/* remove temp file */
		if (!filename_arg)
			remove(fname);
	}

	return !error;
}



/*
 * process_file
 *
 * Read commands from filename and then them to the main processing loop
 * Handler for \i, but can be used for other things as well.
 */
bool
process_file(const char *filename, PsqlSettings *pset)
{
	FILE	   *fd;
	int			result;

	if (!filename)
		return false;

#ifdef __CYGWIN32__
	fd = fopen(filename, "rb");
#else
	fd = fopen(filename, "r");
#endif

	if (!fd)
	{
		perror(filename);
		return false;
	}

	result = MainLoop(pset, fd);
	fclose(fd);
	return (result == EXIT_SUCCESS);
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
do_pset(const char *param, const char *value, printQueryOpt * popt, bool quiet)
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
			fprintf(stderr, "Allowed formats are unaligned, aligned, html, latex.\n");
			return false;
		}

		if (!quiet)
			printf("Output format is %s.\n", _align2string(popt->topt.format));
	}

	/* set border style/width */
	else if (strcmp(param, "border") == 0)
	{
		if (value)
			popt->topt.border = atoi(value);

		if (!quiet)
			printf("Border style is %d.\n", popt->topt.border);
	}

	/* set expanded/vertical mode */
	else if (strcmp(param, "x") == 0 || strcmp(param, "expanded") == 0 || strcmp(param, "vertical") == 0)
	{
		popt->topt.expanded = !popt->topt.expanded;
		if (!quiet)
			printf("Expanded display is %s.\n", popt->topt.expanded ? "on" : "off");
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
			printf("Null display is \"%s\".\n", popt->nullPrint ? popt->nullPrint : "");
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
			printf("Field separator is \"%s\".\n", popt->topt.fieldSep);
	}

	/* toggle between full and barebones format */
	else if (strcmp(param, "t") == 0 || strcmp(param, "tuples_only") == 0)
	{
		popt->topt.tuples_only = !popt->topt.tuples_only;
		if (!quiet)
		{
			if (popt->topt.tuples_only)
				puts("Showing only tuples.");
			else
				puts("Tuples only is off.");
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
				printf("Title is \"%s\".\n", popt->title);
			else
				printf("Title is unset.\n");
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
				printf("Table attribute is \"%s\".\n", popt->topt.tableAttr);
			else
				printf("Table attributes unset.\n");
		}
	}

	/* toggle use of pager */
	else if (strcmp(param, "pager") == 0)
	{
		popt->topt.pager = !popt->topt.pager;
		if (!quiet)
		{
			if (popt->topt.pager)
				puts("Using pager is on.");
			else
				puts("Using pager is off.");
		}
	}


	else
	{
		fprintf(stderr, "Unknown option: %s\n", param);
		return false;
	}

	return true;
}



#define DEFAULT_SHELL  "/bin/sh"

static bool
do_shell(const char *command)
{
	int			result;

	if (!command)
	{
		char	   *sys;
		char	   *shellName;

		shellName = getenv("SHELL");
		if (shellName == NULL)
			shellName = DEFAULT_SHELL;

		sys = malloc(strlen(shellName) + 16);
		if (!sys)
			return false;
		sprintf(sys, "exec %s", shellName);
		result = system(sys);
		free(sys);
	}
	else
		result = system(command);

	if (result == 127 || result == -1)
	{
		perror("system");
		return false;
	}
	return true;
}
