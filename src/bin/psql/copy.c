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
 * Accepted syntax: \copy [binary] table|"table" [with oids] from|to filename|'filename' using delimiters ['<char>'] [ with null as 'string' ]
 * (binary is not here yet)
 *
 * returns a malloc'ed structure with the options, or NULL on parsing error
 */

struct copy_options
{
	char	   *table;
	char	   *file;  /* NULL = stdin/stdout */
	bool		from;
	bool		binary;
	bool		oids;
	char	   *delim;
    char       *null;
};


static void
free_copy_options(struct copy_options * ptr)
{
	if (!ptr)
		return;
	free(ptr->table);
	free(ptr->file);
	free(ptr->delim);
    free(ptr->null);
	free(ptr);
}


static struct copy_options *
parse_slash_copy(const char *args, int encoding)
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

	token = strtokx(line, " \t", "\"", '\\', &quote, NULL, encoding);
	if (!token)
		error = true;
	else
	{
#ifdef NOT_USED
        /* this is not implemented yet */
        if (!quote && strcasecmp(token, "binary") == 0)
		{
			result->binary = true;
			token = strtokx(NULL, " \t", "\"", '\\', &quote, NULL, encoding);
			if (!token)
				error = true;
		}
		if (token)
#endif
			result->table = xstrdup(token);
	}

#ifdef USE_ASSERT_CHECKING
	assert(error || result->table);
#endif

	if (!error)
	{
		token = strtokx(NULL, " \t", NULL, '\\', NULL, NULL, encoding);
		if (!token)
			error = true;
		else
		{
			if (strcasecmp(token, "with") == 0)
			{
				token = strtokx(NULL, " \t", NULL, '\\', NULL, NULL, encoding);
				if (!token || strcasecmp(token, "oids") != 0)
					error = true;
				else
					result->oids = true;

				if (!error)
				{
					token = strtokx(NULL, " \t", NULL, '\\', NULL, NULL, encoding);
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
		token = strtokx(NULL, " \t", "'", '\\', &quote, NULL, encoding);
		if (!token)
			error = true;
		else if (!quote && (strcasecmp(token, "stdin")==0 || strcasecmp(token, "stdout")==0))
            result->file = NULL;
        else
			result->file = xstrdup(token);
	}

	if (!error)
	{
		token = strtokx(NULL, " \t", NULL, '\\', NULL, NULL, encoding);
		if (token)
		{
			if (strcasecmp(token, "using") == 0)
			{
				token = strtokx(NULL, " \t", NULL, '\\', NULL, NULL, encoding);
				if (!token || strcasecmp(token, "delimiters") != 0)
					error = true;
				else
				{
					token = strtokx(NULL, " \t", "'", '\\', NULL, NULL, encoding);
					if (token)
                    {
						result->delim = xstrdup(token);
                        token = strtokx(NULL, " \t", NULL, '\\', NULL, NULL, encoding);
                    }
					else
						error = true;
				}
			}

            if (!error && token)
            {
                if (strcasecmp(token, "with") == 0)
                {
                    token = strtokx(NULL, " \t", NULL, '\\', NULL, NULL, encoding);
                    if (!token || strcasecmp(token, "null") != 0)
                        error = true;
                    else
                    {
                        token = strtokx(NULL, " \t", NULL, '\\', NULL, NULL, encoding);
                        if (!token || strcasecmp(token, "as") != 0)
                            error = true;
                        else
                        {
                            token = strtokx(NULL, " \t", "'", '\\', NULL, NULL, encoding);
                            if (token)
                                result->null = xstrdup(token);
                        }
                    }
                }
            }
		}
	}

	free(line);

	if (error)
	{
        if (!pset.cur_cmd_interactive)
            fprintf(stderr, "%s: ", pset.progname);
        fputs("\\copy: parse error at ", stderr);
		if (!token)
			fputs("end of line", stderr);
		else
			fprintf(stderr, "'%s'", token);
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
do_copy(const char *args, int encoding)
{
	char		query[128 + NAMEDATALEN];
	FILE	   *copystream;
	struct copy_options *options;
	PGresult   *result;
	bool		success;

	/* parse options */
	options = parse_slash_copy(args, encoding);

	if (!options)
		return false;

	strcpy(query, "COPY ");
	if (options->binary)
        strcat(query, "BINARY ");

	strcat(query, "\"");
	strncat(query, options->table, NAMEDATALEN);
	strcat(query, "\" ");
	if (options->oids)
		strcat(query, "WITH OIDS ");

	if (options->from)
		strcat(query, "FROM STDIN");
	else
		strcat(query, "TO STDOUT");


	if (options->delim)
	{
		strcat(query, " USING DELIMITERS '");
		strcat(query, options->delim);
		strcat(query, "'");
	}

    if (options->null)
    {
		strcat(query, " WITH NULL AS '");
		strcat(query, options->null);
		strcat(query, "'");
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
        if (!pset.cur_cmd_interactive)
            fprintf(stderr, "%s: ", pset.progname);
		fprintf(stderr,
				"unable to open file %s: %s\n",
				options->file, strerror(errno));
		free_copy_options(options);
		return false;
	}

	result = PSQLexec(query);

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
			fputs(PQerrorMessage(pset.db), stderr);
			break;
		default:
			success = false;
            if (!pset.cur_cmd_interactive)
                fprintf(stderr, "%s: ", pset.progname);
			fprintf(stderr, "\\copy: unexpected response (%d)\n", PQresultStatus(result));
	}

	PQclear(result);

	if (!success)
	{
        if (!pset.cur_cmd_interactive)
            fprintf(stderr, "%s: ", pset.progname);
        fprintf(stderr, "\\copy failed\n");
	}

    if (copystream != stdout && copystream != stdin)
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
		if (prompt && isatty(fileno(copystream)))
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
