#include <config.h>
#include <c.h>
#include "copy.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#ifndef WIN32
#include <unistd.h>				/* for isatty */
#else
#include <io.h>					/* I think */
#endif

#include <libpq-fe.h>

#include "settings.h"
#include "common.h"
#include "stringutils.h"

#ifdef WIN32
#define strcasecmp(x,y) stricmp(x,y)
#endif

/*
 * parse_slash_copy
 * -- parses \copy command line
 *
 * Accepted syntax: \copy [binary] table|"table" [with oids] from|to filename|'filename' using delimiters ['<char>']
 * (binary is not here yet)
 *
 * returns a malloc'ed structure with the options, or NULL on parsing error
 */

struct copy_options
{
	char	   *table;
	char	   *file;
	bool		from;
	bool		binary;
	bool		oids;
	char	   *delim;
};


static void
free_copy_options(struct copy_options * ptr)
{
	if (!ptr)
		return;
	free(ptr->table);
	free(ptr->file);
	free(ptr->delim);
	free(ptr);
}


static struct copy_options *
parse_slash_copy(const char *args)
{
	struct copy_options *result;
	char	   *line;
	char	   *token;
	bool		error = false;
	char		quote;

	line = xstrdup(args);

	if (!(result = calloc(1, sizeof(struct copy_options))))
	{
		perror("calloc");
		exit(EXIT_FAILURE);
	}

	token = strtokx(line, " \t", "\"", '\\', &quote, NULL);
	if (!token)
		error = true;
	else
	{
		if (!quote && strcasecmp(token, "binary") == 0)
		{
			result->binary = true;
			token = strtokx(NULL, " \t", "\"", '\\', &quote, NULL);
			if (!token)
				error = true;
		}
		if (token)
			result->table = xstrdup(token);
	}

#ifdef USE_ASSERT_CHECKING
	assert(error || result->table);
#endif

	if (!error)
	{
		token = strtokx(NULL, " \t", NULL, '\\', NULL, NULL);
		if (!token)
			error = true;
		else
		{
			if (strcasecmp(token, "with") == 0)
			{
				token = strtokx(NULL, " \t", NULL, '\\', NULL, NULL);
				if (!token || strcasecmp(token, "oids") != 0)
					error = true;
				else
					result->oids = true;

				if (!error)
				{
					token = strtokx(NULL, " \t", NULL, '\\', NULL, NULL);
					if (!token)
						error = true;
				}
			}

			if (!error && strcasecmp(token, "from") == 0)
				result->from = true;
			else if (!error && strcasecmp(token, "to") == 0)
				result->from = false;
			else
				error = true;
		}
	}

	if (!error)
	{
		token = strtokx(NULL, " \t", "'", '\\', NULL, NULL);
		if (!token)
			error = true;
		else
			result->file = xstrdup(token);
	}

#ifdef USE_ASSERT_CHECKING
	assert(error || result->file);
#endif

	if (!error)
	{
		token = strtokx(NULL, " \t", NULL, '\\', NULL, NULL);
		if (token)
		{
			if (strcasecmp(token, "using") != 0)
				error = true;
			else
			{
				token = strtokx(NULL, " \t", NULL, '\\', NULL, NULL);
				if (!token || strcasecmp(token, "delimiters") != 0)
					error = true;
				else
				{
					token = strtokx(NULL, " \t", "'", '\\', NULL, NULL);
					if (token)
						result->delim = xstrdup(token);
					else
						error = true;
				}
			}
		}
	}

	free(line);

	if (error)
	{
		fputs("Parse error at ", stderr);
		if (!token)
			fputs("end of line.", stderr);
		else
			fprintf(stderr, "'%s'.", token);
		fputs("\n", stderr);
		free(result);
		return NULL;
	}
	else
		return result;
}



/*
 * Execute a \copy command (frontend copy). We have to open a file, then
 * submit a COPY query to the backend and either feed it data from the
 * file or route its response into the file.
 */
bool
do_copy(const char *args, PsqlSettings *pset)
{
	char		query[128 + NAMEDATALEN];
	FILE	   *copystream;
	struct copy_options *options;
	PGresult   *result;
	bool		success;

	/* parse options */
	options = parse_slash_copy(args);

	if (!options)
		return false;

	strcpy(query, "COPY ");
	if (options->binary)
		fputs("Warning: \\copy binary is not implemented. Resorting to text output.\n", stderr);
/*	strcat(query, "BINARY "); */

	strcat(query, "\"");
	strncat(query, options->table, NAMEDATALEN);
	strcat(query, "\" ");
	if (options->oids)
		strcat(query, "WITH OIDS ");

	if (options->from)
		strcat(query, "FROM stdin");
	else
		strcat(query, "TO stdout");


	if (options->delim)
	{

		/*
		 * backend copy only uses the first character here, but that might
		 * be the escape backslash (makes me wonder though why it's called
		 * delimiterS)
		 */
		strncat(query, " USING DELIMITERS '", 2);
		strcat(query, options->delim);
		strcat(query, "'");
	}


	if (options->from)
#ifndef __CYGWIN32__
		copystream = fopen(options->file, "r");
#else
		copystream = fopen(options->file, "rb");
#endif
	else
#ifndef __CYGWIN32__
		copystream = fopen(options->file, "w");
#else
		copystream = fopen(options->file, "wb");
#endif

	if (!copystream)
	{
		fprintf(stderr,
				"Unable to open file %s which to copy: %s\n",
				options->from ? "from" : "to", strerror(errno));
		free_copy_options(options);
		return false;
	}

	result = PSQLexec(pset, query);

	switch (PQresultStatus(result))
	{
		case PGRES_COPY_OUT:
			success = handleCopyOut(pset->db, copystream);
			break;
		case PGRES_COPY_IN:
			success = handleCopyIn(pset->db, copystream, NULL);
			break;
		case PGRES_NONFATAL_ERROR:
		case PGRES_FATAL_ERROR:
		case PGRES_BAD_RESPONSE:
			success = false;
			fputs(PQerrorMessage(pset->db), stderr);
			break;
		default:
			success = false;
			fprintf(stderr, "Unexpected response (%d)\n", PQresultStatus(result));
	}

	PQclear(result);

	if (!GetVariable(pset->vars, "quiet"))
	{
		if (success)
			puts("Successfully copied.");
		else
			puts("Copy failed.");
	}

	fclose(copystream);
	free_copy_options(options);
	return success;
}


#define COPYBUFSIZ BLCKSZ


/*
 * handeCopyOut
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
	return !PQendcopy(conn);
}



/*
 * handeCopyOut
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
	char		copybuf[COPYBUFSIZ];
	char	   *s;
	int			buflen;
	int			c = 0;

	while (!copydone)
	{							/* for each input line ... */
		if (prompt && isatty(fileno(stdin)))
		{
			fputs(prompt, stdout);
			fflush(stdout);
		}
		firstload = true;
		linedone = false;
		while (!linedone)
		{						/* for each buffer ... */
			s = copybuf;
			for (buflen = COPYBUFSIZ; buflen > 1; buflen--)
			{
				c = getc(copystream);
				if (c == '\n' || c == EOF)
				{
					linedone = true;
					break;
				}
				*s++ = c;
			}
			*s = '\0';
			if (c == EOF)
			{
				PQputline(conn, "\\.");
				copydone = true;
				break;
			}
			PQputline(conn, copybuf);
			if (firstload)
			{
				if (!strcmp(copybuf, "\\."))
					copydone = true;
				firstload = false;
			}
		}
		PQputline(conn, "\n");
	}
	return !PQendcopy(conn);
}
