/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2008, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/bin/psql/copy.c,v 1.77 2008/01/01 19:45:55 momjian Exp $
 */
#include "postgres_fe.h"
#include "copy.h"

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
#include "dumputils.h"

#include "settings.h"
#include "common.h"
#include "prompt.h"
#include "stringutils.h"

#if defined(WIN32) && !defined(S_ISDIR)
#define __S_ISTYPE(mode, mask)	(((mode) & S_IFMT) == (mask))
#define S_ISDIR(mode)	 __S_ISTYPE((mode), S_IFDIR)
#endif

/*
 * parse_slash_copy
 * -- parses \copy command line
 *
 * The documented syntax is:
 *	\copy tablename [(columnlist)] from|to filename
 *	  [ with ] [ binary ] [ oids ] [ delimiter [as] char ] [ null [as] string ]
 *	  [ csv  [ header ] [ quote [ AS ] string ]  escape [as] string
 *		[ force not null column [, ...] | force quote column [, ...] ] ]
 *
 *	\copy ( select stmt ) to filename
 *	  [ with ] [ binary ] [ delimiter [as] char ] [ null [as] string ]
 *	  [ csv  [ header ] [ quote [ AS ] string ]  escape [as] string
 *		[ force quote column [, ...] ] ]
 *
 * Force quote only applies for copy to; force not null only applies for
 * copy from.
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
	bool		psql_inout;		/* true = use psql stdin/stdout */
	bool		from;
	bool		binary;
	bool		oids;
	bool		csv_mode;
	bool		header;
	char	   *delim;
	char	   *null;
	char	   *quote;
	char	   *escape;
	char	   *force_quote_list;
	char	   *force_notnull_list;
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
	free(ptr->quote);
	free(ptr->escape);
	free(ptr->force_quote_list);
	free(ptr->force_notnull_list);
	free(ptr);
}


/* concatenate "more" onto "var", freeing the original value of *var */
static void
xstrcat(char **var, const char *more)
{
	char	   *newvar;

	newvar = pg_malloc(strlen(*var) + strlen(more) + 1);
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
	char		nonstd_backslash = standard_strings() ? 0 : '\\';

	if (args)
		line = pg_strdup(args);
	else
	{
		psql_error("\\copy: arguments required\n");
		return NULL;
	}

	result = pg_calloc(1, sizeof(struct copy_options));

	token = strtokx(line, whitespace, ".,()", "\"",
					0, false, false, pset.encoding);
	if (!token)
		goto error;

	if (pg_strcasecmp(token, "binary") == 0)
	{
		result->binary = true;
		token = strtokx(NULL, whitespace, ".,()", "\"",
						0, false, false, pset.encoding);
		if (!token)
			goto error;
	}

	result->table = pg_strdup(token);

	/* Handle COPY (SELECT) case */
	if (token[0] == '(')
	{
		int			parens = 1;

		while (parens > 0)
		{
			token = strtokx(NULL, whitespace, ".,()", "\"'",
							nonstd_backslash, true, false, pset.encoding);
			if (!token)
				goto error;
			if (token[0] == '(')
				parens++;
			else if (token[0] == ')')
				parens--;
			xstrcat(&result->table, " ");
			xstrcat(&result->table, token);
		}
	}

	token = strtokx(NULL, whitespace, ".,()", "\"",
					0, false, false, pset.encoding);
	if (!token)
		goto error;

	/*
	 * strtokx() will not have returned a multi-character token starting with
	 * '.', so we don't need strcmp() here.  Likewise for '(', etc, below.
	 */
	if (token[0] == '.')
	{
		/* handle schema . table */
		xstrcat(&result->table, token);
		token = strtokx(NULL, whitespace, ".,()", "\"",
						0, false, false, pset.encoding);
		if (!token)
			goto error;
		xstrcat(&result->table, token);
		token = strtokx(NULL, whitespace, ".,()", "\"",
						0, false, false, pset.encoding);
		if (!token)
			goto error;
	}

	if (token[0] == '(')
	{
		/* handle parenthesized column list */
		result->column_list = pg_strdup(token);
		for (;;)
		{
			token = strtokx(NULL, whitespace, ".,()", "\"",
							0, false, false, pset.encoding);
			if (!token || strchr(".,()", token[0]))
				goto error;
			xstrcat(&result->column_list, token);
			token = strtokx(NULL, whitespace, ".,()", "\"",
							0, false, false, pset.encoding);
			if (!token)
				goto error;
			xstrcat(&result->column_list, token);
			if (token[0] == ')')
				break;
			if (token[0] != ',')
				goto error;
		}
		token = strtokx(NULL, whitespace, ".,()", "\"",
						0, false, false, pset.encoding);
		if (!token)
			goto error;
	}

	if (pg_strcasecmp(token, "from") == 0)
		result->from = true;
	else if (pg_strcasecmp(token, "to") == 0)
		result->from = false;
	else
		goto error;

	token = strtokx(NULL, whitespace, NULL, "'",
					0, false, true, pset.encoding);
	if (!token)
		goto error;

	if (pg_strcasecmp(token, "stdin") == 0 ||
		pg_strcasecmp(token, "stdout") == 0)
	{
		result->psql_inout = false;
		result->file = NULL;
	}
	else if (pg_strcasecmp(token, "pstdin") == 0 ||
			 pg_strcasecmp(token, "pstdout") == 0)
	{
		result->psql_inout = true;
		result->file = NULL;
	}
	else
	{
		result->psql_inout = false;
		result->file = pg_strdup(token);
		expand_tilde(&result->file);
	}

	token = strtokx(NULL, whitespace, NULL, NULL,
					0, false, false, pset.encoding);

	if (token)
	{
		/*
		 * WITH is optional.  Also, the backend will allow WITH followed by
		 * nothing, so we do too.
		 */
		if (pg_strcasecmp(token, "with") == 0)
			token = strtokx(NULL, whitespace, NULL, NULL,
							0, false, false, pset.encoding);

		while (token)
		{
			bool		fetch_next;

			fetch_next = true;

			if (pg_strcasecmp(token, "oids") == 0)
				result->oids = true;
			else if (pg_strcasecmp(token, "binary") == 0)
				result->binary = true;
			else if (pg_strcasecmp(token, "csv") == 0)
				result->csv_mode = true;
			else if (pg_strcasecmp(token, "header") == 0)
				result->header = true;
			else if (pg_strcasecmp(token, "delimiter") == 0)
			{
				token = strtokx(NULL, whitespace, NULL, "'",
								nonstd_backslash, true, false, pset.encoding);
				if (token && pg_strcasecmp(token, "as") == 0)
					token = strtokx(NULL, whitespace, NULL, "'",
							   nonstd_backslash, true, false, pset.encoding);
				if (token)
					result->delim = pg_strdup(token);
				else
					goto error;
			}
			else if (pg_strcasecmp(token, "null") == 0)
			{
				token = strtokx(NULL, whitespace, NULL, "'",
								nonstd_backslash, true, false, pset.encoding);
				if (token && pg_strcasecmp(token, "as") == 0)
					token = strtokx(NULL, whitespace, NULL, "'",
							   nonstd_backslash, true, false, pset.encoding);
				if (token)
					result->null = pg_strdup(token);
				else
					goto error;
			}
			else if (pg_strcasecmp(token, "quote") == 0)
			{
				token = strtokx(NULL, whitespace, NULL, "'",
								nonstd_backslash, true, false, pset.encoding);
				if (token && pg_strcasecmp(token, "as") == 0)
					token = strtokx(NULL, whitespace, NULL, "'",
							   nonstd_backslash, true, false, pset.encoding);
				if (token)
					result->quote = pg_strdup(token);
				else
					goto error;
			}
			else if (pg_strcasecmp(token, "escape") == 0)
			{
				token = strtokx(NULL, whitespace, NULL, "'",
								nonstd_backslash, true, false, pset.encoding);
				if (token && pg_strcasecmp(token, "as") == 0)
					token = strtokx(NULL, whitespace, NULL, "'",
							   nonstd_backslash, true, false, pset.encoding);
				if (token)
					result->escape = pg_strdup(token);
				else
					goto error;
			}
			else if (pg_strcasecmp(token, "force") == 0)
			{
				token = strtokx(NULL, whitespace, ",", "\"",
								0, false, false, pset.encoding);
				if (pg_strcasecmp(token, "quote") == 0)
				{
					/* handle column list */
					fetch_next = false;
					for (;;)
					{
						token = strtokx(NULL, whitespace, ",", "\"",
										0, false, false, pset.encoding);
						if (!token || strchr(",", token[0]))
							goto error;
						if (!result->force_quote_list)
							result->force_quote_list = pg_strdup(token);
						else
							xstrcat(&result->force_quote_list, token);
						token = strtokx(NULL, whitespace, ",", "\"",
										0, false, false, pset.encoding);
						if (!token || token[0] != ',')
							break;
						xstrcat(&result->force_quote_list, token);
					}
				}
				else if (pg_strcasecmp(token, "not") == 0)
				{
					token = strtokx(NULL, whitespace, ",", "\"",
									0, false, false, pset.encoding);
					if (pg_strcasecmp(token, "null") != 0)
						goto error;
					/* handle column list */
					fetch_next = false;
					for (;;)
					{
						token = strtokx(NULL, whitespace, ",", "\"",
										0, false, false, pset.encoding);
						if (!token || strchr(",", token[0]))
							goto error;
						if (!result->force_notnull_list)
							result->force_notnull_list = pg_strdup(token);
						else
							xstrcat(&result->force_notnull_list, token);
						token = strtokx(NULL, whitespace, ",", "\"",
										0, false, false, pset.encoding);
						if (!token || token[0] != ',')
							break;
						xstrcat(&result->force_notnull_list, token);
					}
				}
				else
					goto error;
			}
			else
				goto error;

			if (fetch_next)
				token = strtokx(NULL, whitespace, NULL, NULL,
								0, false, false, pset.encoding);
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
 * Handle one of the "string" options of COPY.	If the user gave a quoted
 * string, pass it to the backend as-is; if it wasn't quoted then quote
 * and escape it.
 */
static void
emit_copy_option(PQExpBuffer query, const char *keyword, const char *option)
{
	appendPQExpBufferStr(query, keyword);
	if (option[0] == '\'' ||
		((option[0] == 'E' || option[0] == 'e') && option[1] == '\''))
		appendPQExpBufferStr(query, option);
	else
		appendStringLiteralConn(query, option, pset.db);
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

	appendPQExpBuffer(&query, "%s ", options->table);

	if (options->column_list)
		appendPQExpBuffer(&query, "%s ", options->column_list);

	if (options->from)
		appendPQExpBuffer(&query, "FROM STDIN");
	else
		appendPQExpBuffer(&query, "TO STDOUT");


	if (options->binary)
		appendPQExpBuffer(&query, " BINARY ");

	if (options->oids)
		appendPQExpBuffer(&query, " OIDS ");

	if (options->delim)
		emit_copy_option(&query, " DELIMITER ", options->delim);

	if (options->null)
		emit_copy_option(&query, " NULL AS ", options->null);

	if (options->csv_mode)
		appendPQExpBuffer(&query, " CSV");

	if (options->header)
		appendPQExpBuffer(&query, " HEADER");

	if (options->quote)
		emit_copy_option(&query, " QUOTE AS ", options->quote);

	if (options->escape)
		emit_copy_option(&query, " ESCAPE AS ", options->escape);

	if (options->force_quote_list)
		appendPQExpBuffer(&query, " FORCE QUOTE %s", options->force_quote_list);

	if (options->force_notnull_list)
		appendPQExpBuffer(&query, " FORCE NOT NULL %s", options->force_notnull_list);

	if (options->file)
		canonicalize_path(options->file);

	if (options->from)
	{
		if (options->file)
			copystream = fopen(options->file, PG_BINARY_R);
		else if (!options->psql_inout)
			copystream = pset.cur_cmd_source;
		else
			copystream = stdin;
	}
	else
	{
		if (options->file)
			copystream = fopen(options->file,
							   options->binary ? PG_BINARY_W : "w");
		else if (!options->psql_inout)
			copystream = pset.queryFout;
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
			SetCancelConn();
			success = handleCopyOut(pset.db, copystream);
			ResetCancelConn();
			break;
		case PGRES_COPY_IN:
			SetCancelConn();
			success = handleCopyIn(pset.db, copystream,
								   PQbinaryTuples(result));
			ResetCancelConn();
			break;
		case PGRES_NONFATAL_ERROR:
		case PGRES_FATAL_ERROR:
		case PGRES_BAD_RESPONSE:
			success = false;
			psql_error("\\copy: %s", PQerrorMessage(pset.db));
			break;
		default:
			success = false;
			psql_error("\\copy: unexpected response (%d)\n",
					   PQresultStatus(result));
			break;
	}

	PQclear(result);

	/*
	 * Make sure we have pumped libpq dry of results; else it may still be in
	 * ASYNC_BUSY state, leading to false readings in, eg, get_prompt().
	 */
	while ((result = PQgetResult(pset.db)) != NULL)
	{
		success = false;
		psql_error("\\copy: unexpected response (%d)\n",
				   PQresultStatus(result));
		PQclear(result);
	}

	if (options->file != NULL)
	{
		if (fclose(copystream) != 0)
		{
			psql_error("%s: %s\n", options->file, strerror(errno));
			success = false;
		}
	}
	free_copy_options(options);
	return success;
}


/*
 * Functions for handling COPY IN/OUT data transfer.
 *
 * If you want to use COPY TO STDOUT/FROM STDIN in your application,
 * this is the code to steal ;)
 */

/*
 * handleCopyOut
 * receives data as a result of a COPY ... TO STDOUT command
 *
 * conn should be a database connection that you just issued COPY TO on
 * and got back a PGRES_COPY_OUT result.
 * copystream is the file stream for the data to go to.
 *
 * result is true if successful, false if not.
 */
bool
handleCopyOut(PGconn *conn, FILE *copystream)
{
	bool		OK = true;
	char	   *buf;
	int			ret;
	PGresult   *res;

	for (;;)
	{
		ret = PQgetCopyData(conn, &buf, 0);

		if (ret < 0)
			break;				/* done or error */

		if (buf)
		{
			if (fwrite(buf, 1, ret, copystream) != ret)
			{
				if (OK)			/* complain only once, keep reading data */
					psql_error("could not write COPY data: %s\n",
							   strerror(errno));
				OK = false;
			}
			PQfreemem(buf);
		}
	}

	if (OK && fflush(copystream))
	{
		psql_error("could not write COPY data: %s\n",
				   strerror(errno));
		OK = false;
	}

	if (ret == -2)
	{
		psql_error("COPY data transfer failed: %s", PQerrorMessage(conn));
		OK = false;
	}

	/* Check command status and return to normal libpq state */
	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		psql_error("%s", PQerrorMessage(conn));
		OK = false;
	}
	PQclear(res);

	return OK;
}

/*
 * handleCopyIn
 * sends data to complete a COPY ... FROM STDIN command
 *
 * conn should be a database connection that you just issued COPY FROM on
 * and got back a PGRES_COPY_IN result.
 * copystream is the file stream to read the data from.
 * isbinary can be set from PQbinaryTuples().
 *
 * result is true if successful, false if not.
 */

/* read chunk size for COPY IN - size is not critical */
#define COPYBUFSIZ 8192

bool
handleCopyIn(PGconn *conn, FILE *copystream, bool isbinary)
{
	bool		OK;
	const char *prompt;
	char		buf[COPYBUFSIZ];
	PGresult   *res;

	/*
	 * Establish longjmp destination for exiting from wait-for-input. (This is
	 * only effective while sigint_interrupt_enabled is TRUE.)
	 */
	if (sigsetjmp(sigint_interrupt_jmp, 1) != 0)
	{
		/* got here with longjmp */

		/* Terminate data transfer */
		PQputCopyEnd(conn, _("canceled by user"));

		/* Check command status and return to normal libpq state */
		res = PQgetResult(conn);
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
			psql_error("%s", PQerrorMessage(conn));
		PQclear(res);

		return false;
	}

	/* Prompt if interactive input */
	if (isatty(fileno(copystream)))
	{
		if (!pset.quiet)
			puts(_("Enter data to be copied followed by a newline.\n"
				   "End with a backslash and a period on a line by itself."));
		prompt = get_prompt(PROMPT_COPY);
	}
	else
		prompt = NULL;

	OK = true;

	if (isbinary)
	{
		/* interactive input probably silly, but give one prompt anyway */
		if (prompt)
		{
			fputs(prompt, stdout);
			fflush(stdout);
		}

		for (;;)
		{
			int			buflen;

			/* enable longjmp while waiting for input */
			sigint_interrupt_enabled = true;

			buflen = fread(buf, 1, COPYBUFSIZ, copystream);

			sigint_interrupt_enabled = false;

			if (buflen <= 0)
				break;

			if (PQputCopyData(conn, buf, buflen) <= 0)
			{
				OK = false;
				break;
			}
		}
	}
	else
	{
		bool		copydone = false;

		while (!copydone)
		{						/* for each input line ... */
			bool		firstload;
			bool		linedone;

			if (prompt)
			{
				fputs(prompt, stdout);
				fflush(stdout);
			}

			firstload = true;
			linedone = false;

			while (!linedone)
			{					/* for each bufferload in line ... */
				int			linelen;
				char	   *fgresult;

				/* enable longjmp while waiting for input */
				sigint_interrupt_enabled = true;

				fgresult = fgets(buf, sizeof(buf), copystream);

				sigint_interrupt_enabled = false;

				if (!fgresult)
				{
					copydone = true;
					break;
				}

				linelen = strlen(buf);

				/* current line is done? */
				if (linelen > 0 && buf[linelen - 1] == '\n')
					linedone = true;

				/* check for EOF marker, but not on a partial line */
				if (firstload)
				{
					if (strcmp(buf, "\\.\n") == 0 ||
						strcmp(buf, "\\.\r\n") == 0)
					{
						copydone = true;
						break;
					}

					firstload = false;
				}

				if (PQputCopyData(conn, buf, linelen) <= 0)
				{
					OK = false;
					copydone = true;
					break;
				}
			}

			pset.lineno++;
		}
	}

	/* Check for read error */
	if (ferror(copystream))
		OK = false;

	/* Terminate data transfer */
	if (PQputCopyEnd(conn,
					 OK ? NULL : _("aborted because of read failure")) <= 0)
		OK = false;

	/* Check command status and return to normal libpq state */
	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		psql_error("%s", PQerrorMessage(conn));
		OK = false;
	}
	PQclear(res);

	return OK;
}
