/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2003, PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/stringutils.c,v 1.34 2003/08/04 23:59:40 tgl Exp $
 */
#include "postgres_fe.h"

#include <assert.h>
#include <ctype.h>

#include "libpq-fe.h"
#include "settings.h"
#include "stringutils.h"


static void strip_quotes(char *source, char quote, char escape, int encoding);


/*
 * Replacement for strtok() (a.k.a. poor man's flex)
 *
 * Splits a string into tokens, returning one token per call, then NULL
 * when no more tokens exist in the given string.
 *
 * The calling convention is similar to that of strtok, but with more
 * frammishes.
 *
 * s -			string to parse, if NULL continue parsing the last string
 * whitespace - set of whitespace characters that separate tokens
 * delim -		set of non-whitespace separator characters (or NULL)
 * quote -		set of characters that can quote a token (NULL if none)
 * escape -		character that can quote quotes (0 if none)
 * del_quotes - if TRUE, strip quotes from the returned token, else return
 *				it exactly as found in the string
 * encoding -	the active character-set encoding
 *
 * Characters in 'delim', if any, will be returned as single-character
 * tokens unless part of a quoted token.
 *
 * Double occurrences of the quoting character are always taken to represent
 * a single quote character in the data.  If escape isn't 0, then escape
 * followed by anything (except \0) is a data character too.
 *
 * Note that the string s is _not_ overwritten in this implementation.
 *
 * NB: it's okay to vary delim, quote, and escape from one call to the
 * next on a single source string, but changing whitespace is a bad idea
 * since you might lose data.
 */
char *
strtokx(const char *s,
		const char *whitespace,
		const char *delim,
		const char *quote,
		char escape,
		bool del_quotes,
		int encoding)
{
	static char *storage = NULL;/* store the local copy of the users
								 * string here */
	static char *string = NULL; /* pointer into storage where to continue
								 * on next call */

	/* variously abused variables: */
	unsigned int offset;
	char	   *start;
	char	   *p;

	if (s)
	{
		free(storage);

		/*
		 * We may need extra space to insert delimiter nulls for adjacent
		 * tokens.	2X the space is a gross overestimate, but it's
		 * unlikely that this code will be used on huge strings anyway.
		 */
		storage = (char *) malloc(2 * strlen(s) + 1);
		if (!storage)
			return NULL;		/* really "out of memory" */
		strcpy(storage, s);
		string = storage;
	}

	if (!storage)
		return NULL;

	/* skip leading whitespace */
	offset = strspn(string, whitespace);
	start = &string[offset];

	/* end of string reached? */
	if (*start == '\0')
	{
		/* technically we don't need to free here, but we're nice */
		free(storage);
		storage = NULL;
		string = NULL;
		return NULL;
	}

	/* test if delimiter character */
	if (delim && strchr(delim, *start))
	{
		/*
		 * If not at end of string, we need to insert a null to terminate
		 * the returned token.	We can just overwrite the next character
		 * if it happens to be in the whitespace set ... otherwise move
		 * over the rest of the string to make room.  (This is why we
		 * allocated extra space above).
		 */
		p = start + 1;
		if (*p != '\0')
		{
			if (!strchr(whitespace, *p))
				memmove(p + 1, p, strlen(p) + 1);
			*p = '\0';
			string = p + 1;
		}
		else
		{
			/* at end of string, so no extra work */
			string = p;
		}

		return start;
	}

	/* test if quoting character */
	if (quote && strchr(quote, *start))
	{
		/* okay, we have a quoted token, now scan for the closer */
		char		thisquote = *start;

		for (p = start + 1; *p; p += PQmblen(p, encoding))
		{
			if (*p == escape && p[1] != '\0')
				p++;			/* process escaped anything */
			else if (*p == thisquote && p[1] == thisquote)
				p++;			/* process doubled quote */
			else if (*p == thisquote)
			{
				p++;			/* skip trailing quote */
				break;
			}
		}

		/*
		 * If not at end of string, we need to insert a null to terminate
		 * the returned token.	See notes above.
		 */
		if (*p != '\0')
		{
			if (!strchr(whitespace, *p))
				memmove(p + 1, p, strlen(p) + 1);
			*p = '\0';
			string = p + 1;
		}
		else
		{
			/* at end of string, so no extra work */
			string = p;
		}

		/* Clean up the token if caller wants that */
		if (del_quotes)
			strip_quotes(start, thisquote, escape, encoding);

		return start;
	}

	/*
	 * Otherwise no quoting character.	Scan till next whitespace,
	 * delimiter or quote.	NB: at this point, *start is known not to be
	 * '\0', whitespace, delim, or quote, so we will consume at least one
	 * character.
	 */
	offset = strcspn(start, whitespace);

	if (delim)
	{
		unsigned int offset2 = strcspn(start, delim);

		if (offset > offset2)
			offset = offset2;
	}

	if (quote)
	{
		unsigned int offset2 = strcspn(start, quote);

		if (offset > offset2)
			offset = offset2;
	}

	p = start + offset;

	/*
	 * If not at end of string, we need to insert a null to terminate the
	 * returned token.	See notes above.
	 */
	if (*p != '\0')
	{
		if (!strchr(whitespace, *p))
			memmove(p + 1, p, strlen(p) + 1);
		*p = '\0';
		string = p + 1;
	}
	else
	{
		/* at end of string, so no extra work */
		string = p;
	}

	return start;
}


/*
 * strip_quotes
 *
 * Remove quotes from the string at *source.  Leading and trailing occurrences
 * of 'quote' are removed; embedded double occurrences of 'quote' are reduced
 * to single occurrences; if 'escape' is not 0 then 'escape' removes special
 * significance of next character.
 *
 * Note that the source string is overwritten in-place.
 */
static void
strip_quotes(char *source, char quote, char escape, int encoding)
{
	char	   *src;
	char	   *dst;

#ifdef USE_ASSERT_CHECKING
	assert(source);
	assert(quote);
#endif

	src = dst = source;

	if (*src && *src == quote)
		src++;					/* skip leading quote */

	while (*src)
	{
		char		c = *src;
		int			i;

		if (c == quote && src[1] == '\0')
			break;				/* skip trailing quote */
		else if (c == quote && src[1] == quote)
			src++;				/* process doubled quote */
		else if (c == escape && src[1] != '\0')
			src++;				/* process escaped character */

		i = PQmblen(src, encoding);
		while (i--)
			*dst++ = *src++;
	}

	*dst = '\0';
}
