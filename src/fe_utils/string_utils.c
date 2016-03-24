/*-------------------------------------------------------------------------
 *
 * String-processing utility routines for frontend code
 *
 * Assorted utility functions that are useful in constructing SQL queries
 * and interpreting backend output.
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/fe_utils/string_utils.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <ctype.h>

#include "fe_utils/string_utils.h"

#include "common/keywords.h"


static PQExpBuffer defaultGetLocalPQExpBuffer(void);

/* Globals exported by this file */
int			quote_all_identifiers = 0;
PQExpBuffer (*getLocalPQExpBuffer) (void) = defaultGetLocalPQExpBuffer;


/*
 * Returns a temporary PQExpBuffer, valid until the next call to the function.
 * This is used by fmtId and fmtQualifiedId.
 *
 * Non-reentrant and non-thread-safe but reduces memory leakage. You can
 * replace this with a custom version by setting the getLocalPQExpBuffer
 * function pointer.
 */
static PQExpBuffer
defaultGetLocalPQExpBuffer(void)
{
	static PQExpBuffer id_return = NULL;

	if (id_return)				/* first time through? */
	{
		/* same buffer, just wipe contents */
		resetPQExpBuffer(id_return);
	}
	else
	{
		/* new buffer */
		id_return = createPQExpBuffer();
	}

	return id_return;
}

/*
 *	Quotes input string if it's not a legitimate SQL identifier as-is.
 *
 *	Note that the returned string must be used before calling fmtId again,
 *	since we re-use the same return buffer each time.
 */
const char *
fmtId(const char *rawid)
{
	PQExpBuffer id_return = getLocalPQExpBuffer();

	const char *cp;
	bool		need_quotes = false;

	/*
	 * These checks need to match the identifier production in scan.l. Don't
	 * use islower() etc.
	 */
	if (quote_all_identifiers)
		need_quotes = true;
	/* slightly different rules for first character */
	else if (!((rawid[0] >= 'a' && rawid[0] <= 'z') || rawid[0] == '_'))
		need_quotes = true;
	else
	{
		/* otherwise check the entire string */
		for (cp = rawid; *cp; cp++)
		{
			if (!((*cp >= 'a' && *cp <= 'z')
				  || (*cp >= '0' && *cp <= '9')
				  || (*cp == '_')))
			{
				need_quotes = true;
				break;
			}
		}
	}

	if (!need_quotes)
	{
		/*
		 * Check for keyword.  We quote keywords except for unreserved ones.
		 * (In some cases we could avoid quoting a col_name or type_func_name
		 * keyword, but it seems much harder than it's worth to tell that.)
		 *
		 * Note: ScanKeywordLookup() does case-insensitive comparison, but
		 * that's fine, since we already know we have all-lower-case.
		 */
		const ScanKeyword *keyword = ScanKeywordLookup(rawid,
													   ScanKeywords,
													   NumScanKeywords);

		if (keyword != NULL && keyword->category != UNRESERVED_KEYWORD)
			need_quotes = true;
	}

	if (!need_quotes)
	{
		/* no quoting needed */
		appendPQExpBufferStr(id_return, rawid);
	}
	else
	{
		appendPQExpBufferChar(id_return, '"');
		for (cp = rawid; *cp; cp++)
		{
			/*
			 * Did we find a double-quote in the string? Then make this a
			 * double double-quote per SQL99. Before, we put in a
			 * backslash/double-quote pair. - thomas 2000-08-05
			 */
			if (*cp == '"')
				appendPQExpBufferChar(id_return, '"');
			appendPQExpBufferChar(id_return, *cp);
		}
		appendPQExpBufferChar(id_return, '"');
	}

	return id_return->data;
}

/*
 * fmtQualifiedId - convert a qualified name to the proper format for
 * the source database.
 *
 * Like fmtId, use the result before calling again.
 *
 * Since we call fmtId and it also uses getThreadLocalPQExpBuffer() we cannot
 * use it until we're finished with calling fmtId().
 */
const char *
fmtQualifiedId(int remoteVersion, const char *schema, const char *id)
{
	PQExpBuffer id_return;
	PQExpBuffer lcl_pqexp = createPQExpBuffer();

	/* Suppress schema name if fetching from pre-7.3 DB */
	if (remoteVersion >= 70300 && schema && *schema)
	{
		appendPQExpBuffer(lcl_pqexp, "%s.", fmtId(schema));
	}
	appendPQExpBufferStr(lcl_pqexp, fmtId(id));

	id_return = getLocalPQExpBuffer();

	appendPQExpBufferStr(id_return, lcl_pqexp->data);
	destroyPQExpBuffer(lcl_pqexp);

	return id_return->data;
}


/*
 * Convert a string value to an SQL string literal and append it to
 * the given buffer.  We assume the specified client_encoding and
 * standard_conforming_strings settings.
 *
 * This is essentially equivalent to libpq's PQescapeStringInternal,
 * except for the output buffer structure.  We need it in situations
 * where we do not have a PGconn available.  Where we do,
 * appendStringLiteralConn is a better choice.
 */
void
appendStringLiteral(PQExpBuffer buf, const char *str,
					int encoding, bool std_strings)
{
	size_t		length = strlen(str);
	const char *source = str;
	char	   *target;

	if (!enlargePQExpBuffer(buf, 2 * length + 2))
		return;

	target = buf->data + buf->len;
	*target++ = '\'';

	while (*source != '\0')
	{
		char		c = *source;
		int			len;
		int			i;

		/* Fast path for plain ASCII */
		if (!IS_HIGHBIT_SET(c))
		{
			/* Apply quoting if needed */
			if (SQL_STR_DOUBLE(c, !std_strings))
				*target++ = c;
			/* Copy the character */
			*target++ = c;
			source++;
			continue;
		}

		/* Slow path for possible multibyte characters */
		len = PQmblen(source, encoding);

		/* Copy the character */
		for (i = 0; i < len; i++)
		{
			if (*source == '\0')
				break;
			*target++ = *source++;
		}

		/*
		 * If we hit premature end of string (ie, incomplete multibyte
		 * character), try to pad out to the correct length with spaces. We
		 * may not be able to pad completely, but we will always be able to
		 * insert at least one pad space (since we'd not have quoted a
		 * multibyte character).  This should be enough to make a string that
		 * the server will error out on.
		 */
		if (i < len)
		{
			char	   *stop = buf->data + buf->maxlen - 2;

			for (; i < len; i++)
			{
				if (target >= stop)
					break;
				*target++ = ' ';
			}
			break;
		}
	}

	/* Write the terminating quote and NUL character. */
	*target++ = '\'';
	*target = '\0';

	buf->len = target - buf->data;
}


/*
 * Convert a string value to an SQL string literal and append it to
 * the given buffer.  Encoding and string syntax rules are as indicated
 * by current settings of the PGconn.
 */
void
appendStringLiteralConn(PQExpBuffer buf, const char *str, PGconn *conn)
{
	size_t		length = strlen(str);

	/*
	 * XXX This is a kluge to silence escape_string_warning in our utility
	 * programs.  It should go away someday.
	 */
	if (strchr(str, '\\') != NULL && PQserverVersion(conn) >= 80100)
	{
		/* ensure we are not adjacent to an identifier */
		if (buf->len > 0 && buf->data[buf->len - 1] != ' ')
			appendPQExpBufferChar(buf, ' ');
		appendPQExpBufferChar(buf, ESCAPE_STRING_SYNTAX);
		appendStringLiteral(buf, str, PQclientEncoding(conn), false);
		return;
	}
	/* XXX end kluge */

	if (!enlargePQExpBuffer(buf, 2 * length + 2))
		return;
	appendPQExpBufferChar(buf, '\'');
	buf->len += PQescapeStringConn(conn, buf->data + buf->len,
								   str, length, NULL);
	appendPQExpBufferChar(buf, '\'');
}


/*
 * Convert a string value to a dollar quoted literal and append it to
 * the given buffer. If the dqprefix parameter is not NULL then the
 * dollar quote delimiter will begin with that (after the opening $).
 *
 * No escaping is done at all on str, in compliance with the rules
 * for parsing dollar quoted strings.  Also, we need not worry about
 * encoding issues.
 */
void
appendStringLiteralDQ(PQExpBuffer buf, const char *str, const char *dqprefix)
{
	static const char suffixes[] = "_XXXXXXX";
	int			nextchar = 0;
	PQExpBuffer delimBuf = createPQExpBuffer();

	/* start with $ + dqprefix if not NULL */
	appendPQExpBufferChar(delimBuf, '$');
	if (dqprefix)
		appendPQExpBufferStr(delimBuf, dqprefix);

	/*
	 * Make sure we choose a delimiter which (without the trailing $) is not
	 * present in the string being quoted. We don't check with the trailing $
	 * because a string ending in $foo must not be quoted with $foo$.
	 */
	while (strstr(str, delimBuf->data) != NULL)
	{
		appendPQExpBufferChar(delimBuf, suffixes[nextchar++]);
		nextchar %= sizeof(suffixes) - 1;
	}

	/* add trailing $ */
	appendPQExpBufferChar(delimBuf, '$');

	/* quote it and we are all done */
	appendPQExpBufferStr(buf, delimBuf->data);
	appendPQExpBufferStr(buf, str);
	appendPQExpBufferStr(buf, delimBuf->data);

	destroyPQExpBuffer(delimBuf);
}


/*
 * Convert a bytea value (presented as raw bytes) to an SQL string literal
 * and append it to the given buffer.  We assume the specified
 * standard_conforming_strings setting.
 *
 * This is needed in situations where we do not have a PGconn available.
 * Where we do, PQescapeByteaConn is a better choice.
 */
void
appendByteaLiteral(PQExpBuffer buf, const unsigned char *str, size_t length,
				   bool std_strings)
{
	const unsigned char *source = str;
	char	   *target;

	static const char hextbl[] = "0123456789abcdef";

	/*
	 * This implementation is hard-wired to produce hex-format output. We do
	 * not know the server version the output will be loaded into, so making
	 * an intelligent format choice is impossible.  It might be better to
	 * always use the old escaped format.
	 */
	if (!enlargePQExpBuffer(buf, 2 * length + 5))
		return;

	target = buf->data + buf->len;
	*target++ = '\'';
	if (!std_strings)
		*target++ = '\\';
	*target++ = '\\';
	*target++ = 'x';

	while (length-- > 0)
	{
		unsigned char c = *source++;

		*target++ = hextbl[(c >> 4) & 0xF];
		*target++ = hextbl[c & 0xF];
	}

	/* Write the terminating quote and NUL character. */
	*target++ = '\'';
	*target = '\0';

	buf->len = target - buf->data;
}


/*
 * Deconstruct the text representation of a 1-dimensional Postgres array
 * into individual items.
 *
 * On success, returns true and sets *itemarray and *nitems to describe
 * an array of individual strings.  On parse failure, returns false;
 * *itemarray may exist or be NULL.
 *
 * NOTE: free'ing itemarray is sufficient to deallocate the working storage.
 */
bool
parsePGArray(const char *atext, char ***itemarray, int *nitems)
{
	int			inputlen;
	char	  **items;
	char	   *strings;
	int			curitem;

	/*
	 * We expect input in the form of "{item,item,item}" where any item is
	 * either raw data, or surrounded by double quotes (in which case embedded
	 * characters including backslashes and quotes are backslashed).
	 *
	 * We build the result as an array of pointers followed by the actual
	 * string data, all in one malloc block for convenience of deallocation.
	 * The worst-case storage need is not more than one pointer and one
	 * character for each input character (consider "{,,,,,,,,,,}").
	 */
	*itemarray = NULL;
	*nitems = 0;
	inputlen = strlen(atext);
	if (inputlen < 2 || atext[0] != '{' || atext[inputlen - 1] != '}')
		return false;			/* bad input */
	items = (char **) malloc(inputlen * (sizeof(char *) + sizeof(char)));
	if (items == NULL)
		return false;			/* out of memory */
	*itemarray = items;
	strings = (char *) (items + inputlen);

	atext++;					/* advance over initial '{' */
	curitem = 0;
	while (*atext != '}')
	{
		if (*atext == '\0')
			return false;		/* premature end of string */
		items[curitem] = strings;
		while (*atext != '}' && *atext != ',')
		{
			if (*atext == '\0')
				return false;	/* premature end of string */
			if (*atext != '"')
				*strings++ = *atext++;	/* copy unquoted data */
			else
			{
				/* process quoted substring */
				atext++;
				while (*atext != '"')
				{
					if (*atext == '\0')
						return false;	/* premature end of string */
					if (*atext == '\\')
					{
						atext++;
						if (*atext == '\0')
							return false;		/* premature end of string */
					}
					*strings++ = *atext++;		/* copy quoted data */
				}
				atext++;
			}
		}
		*strings++ = '\0';
		if (*atext == ',')
			atext++;
		curitem++;
	}
	if (atext[1] != '\0')
		return false;			/* bogus syntax (embedded '}') */
	*nitems = curitem;
	return true;
}


/*
 * processSQLNamePattern
 *
 * Scan a wildcard-pattern string and generate appropriate WHERE clauses
 * to limit the set of objects returned.  The WHERE clauses are appended
 * to the already-partially-constructed query in buf.  Returns whether
 * any clause was added.
 *
 * conn: connection query will be sent to (consulted for escaping rules).
 * buf: output parameter.
 * pattern: user-specified pattern option, or NULL if none ("*" is implied).
 * have_where: true if caller already emitted "WHERE" (clauses will be ANDed
 * onto the existing WHERE clause).
 * force_escape: always quote regexp special characters, even outside
 * double quotes (else they are quoted only between double quotes).
 * schemavar: name of query variable to match against a schema-name pattern.
 * Can be NULL if no schema.
 * namevar: name of query variable to match against an object-name pattern.
 * altnamevar: NULL, or name of an alternative variable to match against name.
 * visibilityrule: clause to use if we want to restrict to visible objects
 * (for example, "pg_catalog.pg_table_is_visible(p.oid)").  Can be NULL.
 *
 * Formatting note: the text already present in buf should end with a newline.
 * The appended text, if any, will end with one too.
 */
bool
processSQLNamePattern(PGconn *conn, PQExpBuffer buf, const char *pattern,
					  bool have_where, bool force_escape,
					  const char *schemavar, const char *namevar,
					  const char *altnamevar, const char *visibilityrule)
{
	PQExpBufferData schemabuf;
	PQExpBufferData namebuf;
	int			encoding = PQclientEncoding(conn);
	bool		inquotes;
	const char *cp;
	int			i;
	bool		added_clause = false;

#define WHEREAND() \
	(appendPQExpBufferStr(buf, have_where ? "  AND " : "WHERE "), \
	 have_where = true, added_clause = true)

	if (pattern == NULL)
	{
		/* Default: select all visible objects */
		if (visibilityrule)
		{
			WHEREAND();
			appendPQExpBuffer(buf, "%s\n", visibilityrule);
		}
		return added_clause;
	}

	initPQExpBuffer(&schemabuf);
	initPQExpBuffer(&namebuf);

	/*
	 * Parse the pattern, converting quotes and lower-casing unquoted letters.
	 * Also, adjust shell-style wildcard characters into regexp notation.
	 *
	 * We surround the pattern with "^(...)$" to force it to match the whole
	 * string, as per SQL practice.  We have to have parens in case the string
	 * contains "|", else the "^" and "$" will be bound into the first and
	 * last alternatives which is not what we want.
	 *
	 * Note: the result of this pass is the actual regexp pattern(s) we want
	 * to execute.  Quoting/escaping into SQL literal format will be done
	 * below using appendStringLiteralConn().
	 */
	appendPQExpBufferStr(&namebuf, "^(");

	inquotes = false;
	cp = pattern;

	while (*cp)
	{
		char		ch = *cp;

		if (ch == '"')
		{
			if (inquotes && cp[1] == '"')
			{
				/* emit one quote, stay in inquotes mode */
				appendPQExpBufferChar(&namebuf, '"');
				cp++;
			}
			else
				inquotes = !inquotes;
			cp++;
		}
		else if (!inquotes && isupper((unsigned char) ch))
		{
			appendPQExpBufferChar(&namebuf,
								  pg_tolower((unsigned char) ch));
			cp++;
		}
		else if (!inquotes && ch == '*')
		{
			appendPQExpBufferStr(&namebuf, ".*");
			cp++;
		}
		else if (!inquotes && ch == '?')
		{
			appendPQExpBufferChar(&namebuf, '.');
			cp++;
		}
		else if (!inquotes && ch == '.')
		{
			/* Found schema/name separator, move current pattern to schema */
			resetPQExpBuffer(&schemabuf);
			appendPQExpBufferStr(&schemabuf, namebuf.data);
			resetPQExpBuffer(&namebuf);
			appendPQExpBufferStr(&namebuf, "^(");
			cp++;
		}
		else if (ch == '$')
		{
			/*
			 * Dollar is always quoted, whether inside quotes or not. The
			 * reason is that it's allowed in SQL identifiers, so there's a
			 * significant use-case for treating it literally, while because
			 * we anchor the pattern automatically there is no use-case for
			 * having it possess its regexp meaning.
			 */
			appendPQExpBufferStr(&namebuf, "\\$");
			cp++;
		}
		else
		{
			/*
			 * Ordinary data character, transfer to pattern
			 *
			 * Inside double quotes, or at all times if force_escape is true,
			 * quote regexp special characters with a backslash to avoid
			 * regexp errors.  Outside quotes, however, let them pass through
			 * as-is; this lets knowledgeable users build regexp expressions
			 * that are more powerful than shell-style patterns.
			 */
			if ((inquotes || force_escape) &&
				strchr("|*+?()[]{}.^$\\", ch))
				appendPQExpBufferChar(&namebuf, '\\');
			i = PQmblen(cp, encoding);
			while (i-- && *cp)
			{
				appendPQExpBufferChar(&namebuf, *cp);
				cp++;
			}
		}
	}

	/*
	 * Now decide what we need to emit.  Note there will be a leading "^(" in
	 * the patterns in any case.
	 */
	if (namebuf.len > 2)
	{
		/* We have a name pattern, so constrain the namevar(s) */

		appendPQExpBufferStr(&namebuf, ")$");
		/* Optimize away a "*" pattern */
		if (strcmp(namebuf.data, "^(.*)$") != 0)
		{
			WHEREAND();
			if (altnamevar)
			{
				appendPQExpBuffer(buf, "(%s ~ ", namevar);
				appendStringLiteralConn(buf, namebuf.data, conn);
				appendPQExpBuffer(buf, "\n        OR %s ~ ", altnamevar);
				appendStringLiteralConn(buf, namebuf.data, conn);
				appendPQExpBufferStr(buf, ")\n");
			}
			else
			{
				appendPQExpBuffer(buf, "%s ~ ", namevar);
				appendStringLiteralConn(buf, namebuf.data, conn);
				appendPQExpBufferChar(buf, '\n');
			}
		}
	}

	if (schemabuf.len > 2)
	{
		/* We have a schema pattern, so constrain the schemavar */

		appendPQExpBufferStr(&schemabuf, ")$");
		/* Optimize away a "*" pattern */
		if (strcmp(schemabuf.data, "^(.*)$") != 0 && schemavar)
		{
			WHEREAND();
			appendPQExpBuffer(buf, "%s ~ ", schemavar);
			appendStringLiteralConn(buf, schemabuf.data, conn);
			appendPQExpBufferChar(buf, '\n');
		}
	}
	else
	{
		/* No schema pattern given, so select only visible objects */
		if (visibilityrule)
		{
			WHEREAND();
			appendPQExpBuffer(buf, "%s\n", visibilityrule);
		}
	}

	termPQExpBuffer(&schemabuf);
	termPQExpBuffer(&namebuf);

	return added_clause;
#undef WHEREAND
}
