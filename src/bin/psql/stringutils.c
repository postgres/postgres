#include <config.h>
#include <c.h>
#include "stringutils.h"

//
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
//
#include <stdio.h>

#include <postgres.h>
#ifndef HAVE_STRDUP
#include <strdup.h>
#endif
#include <libpq-fe.h>



static void
			unescape_quotes(char *source, char quote, char escape);


/*
 * Replacement for strtok() (a.k.a. poor man's flex)
 *
 * The calling convention is similar to that of strtok.
 * s -			string to parse, if NULL continue parsing the last string
 * delim -		set of characters that delimit tokens (usually whitespace)
 * quote -		set of characters that quote stuff, they're not part of the token
 * escape -		character than can quote quotes
 * was_quoted - if not NULL, stores the quoting character if any was encountered
 * token_pos -	if not NULL, receives a count to the start of the token in the
 *				parsed string
 *
 * Note that the string s is _not_ overwritten in this implementation.
 */
char *
strtokx(const char *s,
		const char *delim,
		const char *quote,
		char escape,
		char *was_quoted,
		unsigned int *token_pos)
{
	static char *storage = NULL;/* store the local copy of the users
								 * string here */
	static char *string = NULL; /* pointer into storage where to continue
								 * on next call */

	/* variously abused variables: */
	unsigned int offset;
	char	   *start;
	char	   *cp = NULL;

	if (s)
	{
		free(storage);
		storage = strdup(s);
		string = storage;
	}

	if (!storage)
		return NULL;

	/* skip leading "whitespace" */
	offset = strspn(string, delim);

	/* end of string reached */
	if (string[offset] == '\0')
	{
		/* technically we don't need to free here, but we're nice */
		free(storage);
		storage = NULL;
		string = NULL;
		return NULL;
	}

	/* test if quoting character */
	if (quote)
		cp = strchr(quote, string[offset]);

	if (cp)
	{
		/* okay, we have a quoting character, now scan for the closer */
		char	   *p;

		start = &string[offset + 1];

		if (token_pos)
			*token_pos = start - storage;

		for (p = start;
			 *p && (*p != *cp || *(p - 1) == escape);
#ifdef MULTIBYTE
			 p += PQmblen(p)
#else
			 p++
#endif
			);

		/* not yet end of string? */
		if (*p != '\0')
		{
			*p = '\0';
			string = p + 1;
			if (was_quoted)
				*was_quoted = *cp;
			unescape_quotes(start, *cp, escape);
			return start;
		}
		else
		{
			if (was_quoted)
				*was_quoted = *cp;
			string = p;

			unescape_quotes(start, *cp, escape);
			return start;
		}
	}

	/* otherwise no quoting character. scan till next delimiter */
	start = &string[offset];

	if (token_pos)
		*token_pos = start - storage;

	offset = strcspn(start, delim);
	if (was_quoted)
		*was_quoted = 0;

	if (start[offset] != '\0')
	{
		start[offset] = '\0';
		string = &start[offset] + 1;

		return start;
	}
	else
	{
		string = &start[offset];
		return start;
	}
}




/*
 * unescape_quotes
 *
 * Resolves escaped quotes. Used by strtokx above.
 */
static void
unescape_quotes(char *source, char quote, char escape)
{
	char	   *p;
	char	   *destination,
			   *tmp;

#ifdef USE_ASSERT_CHECKING
	assert(source);
#endif

	destination = (char *) calloc(1, strlen(source) + 1);
	if (!destination)
	{
		perror("calloc");
		exit(EXIT_FAILURE);
	}

	tmp = destination;

	for (p = source; *p; p++)
	{
		char		c;

		if (*p == escape && *(p + 1) && quote == *(p + 1))
		{
			c = *(p + 1);
			p++;
		}
		else
			c = *p;

		*tmp = c;
		tmp++;
	}

	/* Terminating null character */
	*tmp = '\0';

	strcpy(source, destination);
}
