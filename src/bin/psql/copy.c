/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2003, PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/copy.c,v 1.33.4.1 2004/08/14 22:24:49 tgl Exp $
 */
#include "postgres_fe.h"
#include "copy.h"

#include <errno.h>
#include <assert.h>
#include <signal.h>
#include <sys/stat.h>
#ifndef WIN32
#include <unistd.h>				/* for isatty */
#else
#include <io.h>					/* I think */
#endif

#include "libpq-fe.h"
#include "pqexpbuffer.h"
#include "pqsignal.h"

#include "settings.h"
#include "common.h"
#include "stringutils.h"

#ifdef WIN32
#define strcasecmp(x,y) stricmp(x,y)
#define __S_ISTYPE(mode, mask)	(((mode) & S_IFMT) == (mask))
#define S_ISDIR(mode)	 __S_ISTYPE((mode), S_IFDIR)
#endif

/*
 * parse_slash_copy
 * -- parses \copy command line
 *
 * Accepted syntax: \copy table [(columnlist)] [with oids] from|to filename [with ] [ oids ] [ delimiter char] [ null as string ]
 * (binary is not here yet)
 *
 * Old syntax for backward compatibility: (2002-06-19):
 * \copy table [(columnlist)] [with oids] from|to filename [ using delimiters char] [ with null as string ]
 *
 * table name can be double-quoted and can have a schema part.
 * column names can be double-quoted.
 * filename, char, and string can be single-quoted like SQL literals.
 *
 * returns a malloc'ed structure with the options, or NULL on parsing error
 */

struct copy_options
{
	char	   *table;
	char	   *column_list;
	char	   *file;			/* NULL = stdin/stdout */
	bool		from;
	bool		binary;
	bool		oids;
	char	   *delim;
	char	   *null;
};


static void
free_copy_options(struct copy_options * ptr)
{
	if (!ptr)
		return;
	free(ptr->table);
	free(ptr->column_list);
	free(ptr->file);
	free(ptr->delim);
	free(ptr->null);
	free(ptr);
}


/* catenate "more" onto "var", freeing the original value of *var */
static void
xstrcat(char **var, const char *more)
{
	char	   *newvar;

	newvar = (char *) malloc(strlen(*var) + strlen(more) + 1);
	if (!newvar)
	{
		psql_error("out of memory\n");
		exit(EXIT_FAILURE);
	}
	strcpy(newvar, *var);
	strcat(newvar, more);
	free(*var);
	*var = newvar;
}


static struct copy_options *
parse_slash_copy(const char *args)
{
	struct copy_options *result;
	char	   *line;
	char	   *token;
	const char *whitespace = " \t\n\r";

	if (args)
		line = xstrdup(args);
	else
	{
		psql_error("\\copy: arguments required\n");
		return NULL;
	}

	if (!(result = calloc(1, sizeof(struct copy_options))))
	{
		psql_error("out of memory\n");
		exit(EXIT_FAILURE);
	}

	token = strtokx(line, whitespace, ".,()", "\"",
					0, false, pset.encoding);
	if (!token)
		goto error;

#ifdef NOT_USED
	/* this is not implemented yet */
	if (strcasecmp(token, "binary") == 0)
	{
		result->binary = true;
		token = strtokx(NULL, whitespace, ".,()", "\"",
						0, false, pset.encoding);
		if (!token)
			goto error;
	}
#endif

	result->table = xstrdup(token);

	token = strtokx(NULL, whitespace, ".,()", "\"",
					0, false, pset.encoding);
	if (!token)
		goto error;

	/*
	 * strtokx() will not have returned a multi-character token starting
	 * with '.', so we don't need strcmp() here.  Likewise for '(', etc,
	 * below.
	 */
	if (token[0] == '.')
	{
		/* handle schema . table */
		xstrcat(&result->table, token);
		token = strtokx(NULL, whitespace, ".,()", "\"",
						0, false, pset.encoding);
		if (!token)
			goto error;
		xstrcat(&result->table, token);
		token = strtokx(NULL, whitespace, ".,()", "\"",
						0, false, pset.encoding);
		if (!token)
			goto error;
	}

	if (token[0] == '(')
	{
		/* handle parenthesized column list */
		result->column_list = xstrdup(token);
		for (;;)
		{
			token = strtokx(NULL, whitespace, ".,()", "\"",
							0, false, pset.encoding);
			if (!token || strchr(".,()", token[0]))
				goto error;
			xstrcat(&result->column_list, token);
			token = strtokx(NULL, whitespace, ".,()", "\"",
							0, false, pset.encoding);
			if (!token)
				goto error;
			xstrcat(&result->column_list, token);
			if (token[0] == ')')
				break;
			if (token[0] != ',')
				goto error;
		}
		token = strtokx(NULL, whitespace, ".,()", "\"",
						0, false, pset.encoding);
		if (!token)
			goto error;
	}

	/*
	 * Allows old COPY syntax for backward compatibility 2002-06-19
	 */
	if (strcasecmp(token, "with") == 0)
	{
		token = strtokx(NULL, whitespace, NULL, NULL,
						0, false, pset.encoding);
		if (!token || strcasecmp(token, "oids") != 0)
			goto error;
		result->oids = true;

		token = strtokx(NULL, whitespace, NULL, NULL,
						0, false, pset.encoding);
		if (!token)
			goto error;
	}

	if (strcasecmp(token, "from") == 0)
		result->from = true;
	else if (strcasecmp(token, "to") == 0)
		result->from = false;
	else
		goto error;

	token = strtokx(NULL, whitespace, NULL, "'",
					'\\', true, pset.encoding);
	if (!token)
		goto error;

	if (strcasecmp(token, "stdin") == 0 ||
		strcasecmp(token, "stdout") == 0)
		result->file = NULL;
	else
		result->file = xstrdup(token);

	token = strtokx(NULL, whitespace, NULL, NULL,
					0, false, pset.encoding);

	/*
	 * Allows old COPY syntax for backward compatibility 2002-06-19
	 */
	if (token && strcasecmp(token, "using") == 0)
	{
		token = strtokx(NULL, whitespace, NULL, NULL,
						0, false, pset.encoding);
		if (!(token && strcasecmp(token, "delimiters") == 0))
			goto error;
		token = strtokx(NULL, whitespace, NULL, "'",
						'\\', false, pset.encoding);
		if (!token)
			goto error;
		result->delim = xstrdup(token);
		token = strtokx(NULL, whitespace, NULL, NULL,
						0, false, pset.encoding);
	}

	if (token)
	{
		if (strcasecmp(token, "with") != 0)
			goto error;
		while ((token = strtokx(NULL, whitespace, NULL, NULL,
								0, false, pset.encoding)) != NULL)
		{
			if (strcasecmp(token, "delimiter") == 0)
			{
				token = strtokx(NULL, whitespace, NULL, "'",
								'\\', false, pset.encoding);
				if (token && strcasecmp(token, "as") == 0)
					token = strtokx(NULL, whitespace, NULL, "'",
									'\\', false, pset.encoding);
				if (token)
					result->delim = xstrdup(token);
				else
					goto error;
			}
			else if (strcasecmp(token, "null") == 0)
			{
				token = strtokx(NULL, whitespace, NULL, "'",
								'\\', false, pset.encoding);
				if (token && strcasecmp(token, "as") == 0)
					token = strtokx(NULL, whitespace, NULL, "'",
									'\\', false, pset.encoding);
				if (token)
					result->null = xstrdup(token);
				else
					goto error;
			}
			else
				goto error;
		}
	}

	free(line);

	return result;

error:
	if (token)
		psql_error("\\copy: parse error at \"%s\"\n", token);
	else
		psql_error("\\copy: parse error at end of line\n");
	free_copy_options(result);
	free(line);

	return NULL;
}



/*
 * Execute a \copy command (frontend copy). We have to open a file, then
 * submit a COPY query to the backend and either feed it data from the
 * file or route its response into the file.
 */
bool
do_copy(const char *args)
{
	PQExpBufferData query;
	FILE	   *copystream;
	struct copy_options *options;
	PGresult   *result;
	bool		success;
	struct stat st;

	/* parse options */
	options = parse_slash_copy(args);

	if (!options)
		return false;

	initPQExpBuffer(&query);

	printfPQExpBuffer(&query, "COPY ");
	if (options->binary)
		appendPQExpBuffer(&query, "BINARY ");

	appendPQExpBuffer(&query, "%s ", options->table);

	if (options->column_list)
		appendPQExpBuffer(&query, "%s ", options->column_list);

	/* Uses old COPY syntax for backward compatibility 2002-06-19 */
	if (options->oids)
		appendPQExpBuffer(&query, "WITH OIDS ");

	if (options->from)
		appendPQExpBuffer(&query, "FROM STDIN");
	else
		appendPQExpBuffer(&query, "TO STDOUT");


	/* Uses old COPY syntax for backward compatibility 2002-06-19 */
	if (options->delim)
	{
		if (options->delim[0] == '\'')
			appendPQExpBuffer(&query, " USING DELIMITERS %s",
							  options->delim);
		else
			appendPQExpBuffer(&query, " USING DELIMITERS '%s'",
							  options->delim);
	}

	if (options->null)
	{
		if (options->null[0] == '\'')
			appendPQExpBuffer(&query, " WITH NULL AS %s", options->null);
		else
			appendPQExpBuffer(&query, " WITH NULL AS '%s'", options->null);
	}

	if (options->from)
	{
		if (options->file)
			copystream = fopen(options->file, "r");
		else
			copystream = stdin;
	}
	else
	{
		if (options->file)
			copystream = fopen(options->file, "w");
		else
			copystream = stdout;
	}

	if (!copystream)
	{
		psql_error("%s: %s\n",
				   options->file, strerror(errno));
		free_copy_options(options);
		return false;
	}

	/* make sure the specified file is not a directory */
	fstat(fileno(copystream), &st);
	if (S_ISDIR(st.st_mode))
	{
		fclose(copystream);
		psql_error("%s: cannot copy from/to a directory\n",
				   options->file);
		free_copy_options(options);
		return false;
	}

	result = PSQLexec(query.data, true);
	termPQExpBuffer(&query);

	switch (PQresultStatus(result))
	{
		case PGRES_COPY_OUT:
			success = handleCopyOut(pset.db, copystream);
			break;
		case PGRES_COPY_IN:
			success = handleCopyIn(pset.db, copystream, NULL);
			break;
		case PGRES_NONFATAL_ERROR:
		case PGRES_FATAL_ERROR:
		case PGRES_BAD_RESPONSE:
			success = false;
			psql_error("\\copy: %s", PQerrorMessage(pset.db));
			break;
		default:
			success = false;
			psql_error("\\copy: unexpected response (%d)\n", PQresultStatus(result));
	}

	PQclear(result);

	if (copystream != stdout && copystream != stdin)
		fclose(copystream);
	free_copy_options(options);
	return success;
}


#define COPYBUFSIZ 8192			/* size doesn't matter */


/*
 * handleCopyOut
 * receives data as a result of a COPY ... TO stdout command
 *
 * If you want to use COPY TO in your application, this is the code to steal :)
 *
 * conn should be a database connection that you just called COPY TO on
 * (and which gave you PGRES_COPY_OUT back);
 * copystream is the file stream you want the output to go to
 */
bool
handleCopyOut(PGconn *conn, FILE *copystream)
{
	bool		copydone = false;		/* haven't started yet */
	char		copybuf[COPYBUFSIZ];
	int			ret;

	while (!copydone)
	{
		ret = PQgetline(conn, copybuf, COPYBUFSIZ);

		if (copybuf[0] == '\\' &&
			copybuf[1] == '.' &&
			copybuf[2] == '\0')
		{
			copydone = true;	/* we're at the end */
		}
		else
		{
			fputs(copybuf, copystream);
			switch (ret)
			{
				case EOF:
					copydone = true;
					/* FALLTHROUGH */
				case 0:
					fputc('\n', copystream);
					break;
				case 1:
					break;
			}
		}
	}
	fflush(copystream);
	ret = !PQendcopy(conn);
	ResetCancelConn();
	return ret;
}



/*
 * handleCopyIn
 * receives data as a result of a COPY ... FROM stdin command
 *
 * Again, if you want to use COPY FROM in your application, copy this.
 *
 * conn should be a database connection that you just called COPY FROM on
 * (and which gave you PGRES_COPY_IN back);
 * copystream is the file stream you want the input to come from
 * prompt is something to display to request user input (only makes sense
 *	 if stdin is an interactive tty)
 */

bool
handleCopyIn(PGconn *conn, FILE *copystream, const char *prompt)
{
	bool		copydone = false;
	bool		firstload;
	bool		linedone;
	bool		saw_cr = false;
	char		copybuf[COPYBUFSIZ];
	char	   *s;
	int			bufleft;
	int			c = 0;
	int			ret;
	unsigned int linecount = 0;

	if (prompt)					/* disable prompt if not interactive */
	{
		if (!isatty(fileno(copystream)))
			prompt = NULL;
	}

	while (!copydone)
	{							/* for each input line ... */
		if (prompt)
		{
			fputs(prompt, stdout);
			fflush(stdout);
		}
		firstload = true;
		linedone = false;

		while (!linedone)
		{						/* for each bufferload in line ... */
			/* Fetch string until \n, EOF, or buffer full */
			s = copybuf;
			for (bufleft = COPYBUFSIZ - 1; bufleft > 0; bufleft--)
			{
				c = getc(copystream);
				if (c == EOF)
				{
					linedone = true;
					break;
				}
				*s++ = c;
				if (c == '\n')
				{
					linedone = true;
					break;
				}
				if (c == '\r')
					saw_cr = true;
			}
			*s = '\0';
			/* EOF with empty line-so-far? */
			if (c == EOF && s == copybuf && firstload)
			{
				/*
				 * We are guessing a little bit as to the right line-ending
				 * here...
				 */
				if (saw_cr)
					PQputline(conn, "\\.\r\n");
				else
					PQputline(conn, "\\.\n");
				copydone = true;
				if (pset.cur_cmd_interactive)
					puts("\\.");
				break;
			}
			/* No, so pass the data to the backend */
			PQputline(conn, copybuf);
			/* Check for line consisting only of \. */
			if (firstload)
			{
				if (strcmp(copybuf, "\\.\n") == 0 ||
					strcmp(copybuf, "\\.\r\n") == 0)
				{
					copydone = true;
					break;
				}
				firstload = false;
			}
		}
		linecount++;
	}
	ret = !PQendcopy(conn);
	pset.lineno += linecount;
	return ret;
}
