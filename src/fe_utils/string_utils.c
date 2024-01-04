/*-------------------------------------------------------------------------
 *
 * String-processing utility routines for frontend code
 *
 * Assorted utility functions that are useful in constructing SQL queries
 * and interpreting backend output.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/fe_utils/string_utils.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <ctype.h>

#include "common/keywords.h"
#include "fe_utils/string_utils.h"

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
		int			kwnum = ScanKeywordLookup(rawid, &ScanKeywords);

		if (kwnum >= 0 && ScanKeywordCategories[kwnum] != UNRESERVED_KEYWORD)
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
 * fmtQualifiedId - construct a schema-qualified name, with quoting as needed.
 *
 * Like fmtId, use the result before calling again.
 *
 * Since we call fmtId and it also uses getLocalPQExpBuffer() we cannot
 * use that buffer until we're finished with calling fmtId().
 */
const char *
fmtQualifiedId(const char *schema, const char *id)
{
	PQExpBuffer id_return;
	PQExpBuffer lcl_pqexp = createPQExpBuffer();

	/* Some callers might fail to provide a schema name */
	if (schema && *schema)
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
 * Format a Postgres version number (in the PG_VERSION_NUM integer format
 * returned by PQserverVersion()) as a string.  This exists mainly to
 * encapsulate knowledge about two-part vs. three-part version numbers.
 *
 * For reentrancy, caller must supply the buffer the string is put in.
 * Recommended size of the buffer is 32 bytes.
 *
 * Returns address of 'buf', as a notational convenience.
 */
char *
formatPGVersionNumber(int version_number, bool include_minor,
					  char *buf, size_t buflen)
{
	if (version_number >= 100000)
	{
		/* New two-part style */
		if (include_minor)
			snprintf(buf, buflen, "%d.%d", version_number / 10000,
					 version_number % 10000);
		else
			snprintf(buf, buflen, "%d", version_number / 10000);
	}
	else
	{
		/* Old three-part style */
		if (include_minor)
			snprintf(buf, buflen, "%d.%d.%d", version_number / 10000,
					 (version_number / 100) % 100,
					 version_number % 100);
		else
			snprintf(buf, buflen, "%d.%d", version_number / 10000,
					 (version_number / 100) % 100);
	}
	return buf;
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
 * Append the given string to the shell command being built in the buffer,
 * with shell-style quoting as needed to create exactly one argument.
 *
 * Forbid LF or CR characters, which have scant practical use beyond designing
 * security breaches.  The Windows command shell is unusable as a conduit for
 * arguments containing LF or CR characters.  A future major release should
 * reject those characters in CREATE ROLE and CREATE DATABASE, because use
 * there eventually leads to errors here.
 *
 * appendShellString() simply prints an error and dies if LF or CR appears.
 * appendShellStringNoError() omits those characters from the result, and
 * returns false if there were any.
 */
void
appendShellString(PQExpBuffer buf, const char *str)
{
	if (!appendShellStringNoError(buf, str))
	{
		fprintf(stderr,
				_("shell command argument contains a newline or carriage return: \"%s\"\n"),
				str);
		exit(EXIT_FAILURE);
	}
}

bool
appendShellStringNoError(PQExpBuffer buf, const char *str)
{
#ifdef WIN32
	int			backslash_run_length = 0;
#endif
	bool		ok = true;
	const char *p;

	/*
	 * Don't bother with adding quotes if the string is nonempty and clearly
	 * contains only safe characters.
	 */
	if (*str != '\0' &&
		strspn(str, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_./:") == strlen(str))
	{
		appendPQExpBufferStr(buf, str);
		return ok;
	}

#ifndef WIN32
	appendPQExpBufferChar(buf, '\'');
	for (p = str; *p; p++)
	{
		if (*p == '\n' || *p == '\r')
		{
			ok = false;
			continue;
		}

		if (*p == '\'')
			appendPQExpBufferStr(buf, "'\"'\"'");
		else
			appendPQExpBufferChar(buf, *p);
	}
	appendPQExpBufferChar(buf, '\'');
#else							/* WIN32 */

	/*
	 * A Windows system() argument experiences two layers of interpretation.
	 * First, cmd.exe interprets the string.  Its behavior is undocumented,
	 * but a caret escapes any byte except LF or CR that would otherwise have
	 * special meaning.  Handling of a caret before LF or CR differs between
	 * "cmd.exe /c" and other modes, and it is unusable here.
	 *
	 * Second, the new process parses its command line to construct argv (see
	 * https://msdn.microsoft.com/en-us/library/17w5ykft.aspx).  This treats
	 * backslash-double quote sequences specially.
	 */
	appendPQExpBufferStr(buf, "^\"");
	for (p = str; *p; p++)
	{
		if (*p == '\n' || *p == '\r')
		{
			ok = false;
			continue;
		}

		/* Change N backslashes before a double quote to 2N+1 backslashes. */
		if (*p == '"')
		{
			while (backslash_run_length)
			{
				appendPQExpBufferStr(buf, "^\\");
				backslash_run_length--;
			}
			appendPQExpBufferStr(buf, "^\\");
		}
		else if (*p == '\\')
			backslash_run_length++;
		else
			backslash_run_length = 0;

		/*
		 * Decline to caret-escape the most mundane characters, to ease
		 * debugging and lest we approach the command length limit.
		 */
		if (!((*p >= 'a' && *p <= 'z') ||
			  (*p >= 'A' && *p <= 'Z') ||
			  (*p >= '0' && *p <= '9')))
			appendPQExpBufferChar(buf, '^');
		appendPQExpBufferChar(buf, *p);
	}

	/*
	 * Change N backslashes at end of argument to 2N backslashes, because they
	 * precede the double quote that terminates the argument.
	 */
	while (backslash_run_length)
	{
		appendPQExpBufferStr(buf, "^\\");
		backslash_run_length--;
	}
	appendPQExpBufferStr(buf, "^\"");
#endif							/* WIN32 */

	return ok;
}


/*
 * Append the given string to the buffer, with suitable quoting for passing
 * the string as a value in a keyword/value pair in a libpq connection string.
 */
void
appendConnStrVal(PQExpBuffer buf, const char *str)
{
	const char *s;
	bool		needquotes;

	/*
	 * If the string is one or more plain ASCII characters, no need to quote
	 * it. This is quite conservative, but better safe than sorry.
	 */
	needquotes = true;
	for (s = str; *s; s++)
	{
		if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
			  (*s >= '0' && *s <= '9') || *s == '_' || *s == '.'))
		{
			needquotes = true;
			break;
		}
		needquotes = false;
	}

	if (needquotes)
	{
		appendPQExpBufferChar(buf, '\'');
		while (*str)
		{
			/* ' and \ must be escaped by to \' and \\ */
			if (*str == '\'' || *str == '\\')
				appendPQExpBufferChar(buf, '\\');

			appendPQExpBufferChar(buf, *str);
			str++;
		}
		appendPQExpBufferChar(buf, '\'');
	}
	else
		appendPQExpBufferStr(buf, str);
}


/*
 * Append a psql meta-command that connects to the given database with the
 * then-current connection's user, host and port.
 */
void
appendPsqlMetaConnect(PQExpBuffer buf, const char *dbname)
{
	const char *s;
	bool complex;

	/*
	 * If the name is plain ASCII characters, emit a trivial "\connect "foo"".
	 * For other names, even many not technically requiring it, skip to the
	 * general case.  No database has a zero-length name.
	 */
	complex = false;

	for (s = dbname; *s; s++)
	{
		if (*s == '\n' || *s == '\r')
		{
			fprintf(stderr,
					_("database name contains a newline or carriage return: \"%s\"\n"),
					dbname);
			exit(EXIT_FAILURE);
		}

		if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
			  (*s >= '0' && *s <= '9') || *s == '_' || *s == '.'))
		{
			complex = true;
		}
	}

	appendPQExpBufferStr(buf, "\\connect ");
	if (complex)
	{
		PQExpBufferData connstr;

		initPQExpBuffer(&connstr);
		appendPQExpBufferStr(&connstr, "dbname=");
		appendConnStrVal(&connstr, dbname);

		appendPQExpBufferStr(buf, "-reuse-previous=on ");

		/*
		 * As long as the name does not contain a newline, SQL identifier
		 * quoting satisfies the psql meta-command parser.  Prefer not to
		 * involve psql-interpreted single quotes, which behaved differently
		 * before PostgreSQL 9.2.
		 */
		appendPQExpBufferStr(buf, fmtId(connstr.data));

		termPQExpBuffer(&connstr);
	}
	else
		appendPQExpBufferStr(buf, fmtId(dbname));
	appendPQExpBufferChar(buf, '\n');
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
							return false;	/* premature end of string */
					}
					*strings++ = *atext++;	/* copy quoted data */
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
 * Append one element to the text representation of a 1-dimensional Postgres
 * array.
 *
 * The caller must provide the initial '{' and closing '}' of the array.
 * This function handles all else, including insertion of commas and
 * quoting of values.
 *
 * We assume that typdelim is ','.
 */
void
appendPGArray(PQExpBuffer buffer, const char *value)
{
	bool		needquote;
	const char *tmp;

	if (buffer->data[buffer->len - 1] != '{')
		appendPQExpBufferChar(buffer, ',');

	/* Decide if we need quotes; this should match array_out()'s choices. */
	if (value[0] == '\0')
		needquote = true;		/* force quotes for empty string */
	else if (pg_strcasecmp(value, "NULL") == 0)
		needquote = true;		/* force quotes for literal NULL */
	else
		needquote = false;

	if (!needquote)
	{
		for (tmp = value; *tmp; tmp++)
		{
			char		ch = *tmp;

			if (ch == '"' || ch == '\\' ||
				ch == '{' || ch == '}' || ch == ',' ||
			/* these match scanner_isspace(): */
				ch == ' ' || ch == '\t' || ch == '\n' ||
				ch == '\r' || ch == '\v' || ch == '\f')
			{
				needquote = true;
				break;
			}
		}
	}

	if (needquote)
	{
		appendPQExpBufferChar(buffer, '"');
		for (tmp = value; *tmp; tmp++)
		{
			char		ch = *tmp;

			if (ch == '"' || ch == '\\')
				appendPQExpBufferChar(buffer, '\\');
			appendPQExpBufferChar(buffer, ch);
		}
		appendPQExpBufferChar(buffer, '"');
	}
	else
		appendPQExpBufferStr(buffer, value);
}


/*
 * Format a reloptions array and append it to the given buffer.
 *
 * "prefix" is prepended to the option names; typically it's "" or "toast.".
 *
 * Returns false if the reloptions array could not be parsed (in which case
 * nothing will have been appended to the buffer), or true on success.
 *
 * Note: this logic should generally match the backend's flatten_reloptions()
 * (in adt/ruleutils.c).
 */
bool
appendReloptionsArray(PQExpBuffer buffer, const char *reloptions,
					  const char *prefix, int encoding, bool std_strings)
{
	char	  **options;
	int			noptions;
	int			i;

	if (!parsePGArray(reloptions, &options, &noptions))
	{
		free(options);
		return false;
	}

	for (i = 0; i < noptions; i++)
	{
		char	   *option = options[i];
		char	   *name;
		char	   *separator;
		char	   *value;

		/*
		 * Each array element should have the form name=value.  If the "=" is
		 * missing for some reason, treat it like an empty value.
		 */
		name = option;
		separator = strchr(option, '=');
		if (separator)
		{
			*separator = '\0';
			value = separator + 1;
		}
		else
			value = "";

		if (i > 0)
			appendPQExpBufferStr(buffer, ", ");
		appendPQExpBuffer(buffer, "%s%s=", prefix, fmtId(name));

		/*
		 * In general we need to quote the value; but to avoid unnecessary
		 * clutter, do not quote if it is an identifier that would not need
		 * quoting.  (We could also allow numbers, but that is a bit trickier
		 * than it looks --- for example, are leading zeroes significant?  We
		 * don't want to assume very much here about what custom reloptions
		 * might mean.)
		 */
		if (strcmp(fmtId(value), value) == 0)
			appendPQExpBufferStr(buffer, value);
		else
			appendStringLiteral(buffer, value, encoding, std_strings);
	}

	free(options);

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
 * dbnamebuf: output parameter receiving the database name portion of the
 * pattern, if any.  Can be NULL.
 * dotcnt: how many separators were parsed from the pattern, by reference.
 *
 * Formatting note: the text already present in buf should end with a newline.
 * The appended text, if any, will end with one too.
 */
bool
processSQLNamePattern(PGconn *conn, PQExpBuffer buf, const char *pattern,
					  bool have_where, bool force_escape,
					  const char *schemavar, const char *namevar,
					  const char *altnamevar, const char *visibilityrule,
					  PQExpBuffer dbnamebuf, int *dotcnt)
{
	PQExpBufferData schemabuf;
	PQExpBufferData namebuf;
	bool		added_clause = false;
	int			dcnt;

#define WHEREAND() \
	(appendPQExpBufferStr(buf, have_where ? "  AND " : "WHERE "), \
	 have_where = true, added_clause = true)

	if (dotcnt == NULL)
		dotcnt = &dcnt;
	*dotcnt = 0;
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
	 * Convert shell-style 'pattern' into the regular expression(s) we want to
	 * execute.  Quoting/escaping into SQL literal format will be done below
	 * using appendStringLiteralConn().
	 *
	 * If the caller provided a schemavar, we want to split the pattern on
	 * ".", otherwise not.
	 */
	patternToSQLRegex(PQclientEncoding(conn),
					  (schemavar ? dbnamebuf : NULL),
					  (schemavar ? &schemabuf : NULL),
					  &namebuf,
					  pattern, force_escape, true, dotcnt);

	/*
	 * Now decide what we need to emit.  We may run under a hostile
	 * search_path, so qualify EVERY name.  Note there will be a leading "^("
	 * in the patterns in any case.
	 *
	 * We want the regex matches to use the database's default collation where
	 * collation-sensitive behavior is required (for example, which characters
	 * match '\w').  That happened by default before PG v12, but if the server
	 * is >= v12 then we need to force it through explicit COLLATE clauses,
	 * otherwise the "C" collation attached to "name" catalog columns wins.
	 */
	if (namevar && namebuf.len > 2)
	{
		/* We have a name pattern, so constrain the namevar(s) */

		/* Optimize away a "*" pattern */
		if (strcmp(namebuf.data, "^(.*)$") != 0)
		{
			WHEREAND();
			if (altnamevar)
			{
				appendPQExpBuffer(buf,
								  "(%s OPERATOR(pg_catalog.~) ", namevar);
				appendStringLiteralConn(buf, namebuf.data, conn);
				if (PQserverVersion(conn) >= 120000)
					appendPQExpBufferStr(buf, " COLLATE pg_catalog.default");
				appendPQExpBuffer(buf,
								  "\n        OR %s OPERATOR(pg_catalog.~) ",
								  altnamevar);
				appendStringLiteralConn(buf, namebuf.data, conn);
				if (PQserverVersion(conn) >= 120000)
					appendPQExpBufferStr(buf, " COLLATE pg_catalog.default");
				appendPQExpBufferStr(buf, ")\n");
			}
			else
			{
				appendPQExpBuffer(buf, "%s OPERATOR(pg_catalog.~) ", namevar);
				appendStringLiteralConn(buf, namebuf.data, conn);
				if (PQserverVersion(conn) >= 120000)
					appendPQExpBufferStr(buf, " COLLATE pg_catalog.default");
				appendPQExpBufferChar(buf, '\n');
			}
		}
	}

	if (schemavar && schemabuf.len > 2)
	{
		/* We have a schema pattern, so constrain the schemavar */

		/* Optimize away a "*" pattern */
		if (strcmp(schemabuf.data, "^(.*)$") != 0 && schemavar)
		{
			WHEREAND();
			appendPQExpBuffer(buf, "%s OPERATOR(pg_catalog.~) ", schemavar);
			appendStringLiteralConn(buf, schemabuf.data, conn);
			if (PQserverVersion(conn) >= 120000)
				appendPQExpBufferStr(buf, " COLLATE pg_catalog.default");
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

/*
 * Transform a possibly qualified shell-style object name pattern into up to
 * three SQL-style regular expressions, converting quotes, lower-casing
 * unquoted letters, and adjusting shell-style wildcard characters into regexp
 * notation.
 *
 * If the dbnamebuf and schemabuf arguments are non-NULL, and the pattern
 * contains two or more dbname/schema/name separators, we parse the portions of
 * the pattern prior to the first and second separators into dbnamebuf and
 * schemabuf, and the rest into namebuf.
 *
 * If dbnamebuf is NULL and schemabuf is non-NULL, and the pattern contains at
 * least one separator, we parse the first portion into schemabuf and the rest
 * into namebuf.
 *
 * Otherwise, we parse all the pattern into namebuf.
 *
 * If the pattern contains more dotted parts than buffers to parse into, the
 * extra dots will be treated as literal characters and written into the
 * namebuf, though they will be counted.  Callers should always check the value
 * returned by reference in dotcnt and handle this error case appropriately.
 *
 * We surround the regexps with "^(...)$" to force them to match whole strings,
 * as per SQL practice.  We have to have parens in case strings contain "|",
 * else the "^" and "$" will be bound into the first and last alternatives
 * which is not what we want.  Whether this is done for dbnamebuf is controlled
 * by the want_literal_dbname parameter.
 *
 * The regexps we parse into the buffers are appended to the data (if any)
 * already present.  If we parse fewer fields than the number of buffers we
 * were given, the extra buffers are unaltered.
 *
 * encoding: the character encoding for the given pattern
 * dbnamebuf: output parameter receiving the database name portion of the
 * pattern, if any.  Can be NULL.
 * schemabuf: output parameter receiving the schema name portion of the
 * pattern, if any.  Can be NULL.
 * namebuf: output parameter receiving the database name portion of the
 * pattern, if any.  Can be NULL.
 * pattern: user-specified pattern option, or NULL if none ("*" is implied).
 * force_escape: always quote regexp special characters, even outside
 * double quotes (else they are quoted only between double quotes).
 * want_literal_dbname: if true, regexp special characters within the database
 * name portion of the pattern will not be escaped, nor will the dbname be
 * converted into a regular expression.
 * dotcnt: output parameter receiving the number of separators parsed from the
 * pattern.
 */
void
patternToSQLRegex(int encoding, PQExpBuffer dbnamebuf, PQExpBuffer schemabuf,
				  PQExpBuffer namebuf, const char *pattern, bool force_escape,
				  bool want_literal_dbname, int *dotcnt)
{
	PQExpBufferData buf[3];
	PQExpBufferData left_literal;
	PQExpBuffer curbuf;
	PQExpBuffer maxbuf;
	int			i;
	bool		inquotes;
	bool		left;
	const char *cp;

	Assert(pattern != NULL);
	Assert(namebuf != NULL);

	/* callers should never expect "dbname.relname" format */
	Assert(dbnamebuf == NULL || schemabuf != NULL);
	Assert(dotcnt != NULL);

	*dotcnt = 0;
	inquotes = false;
	cp = pattern;

	if (dbnamebuf != NULL)
		maxbuf = &buf[2];
	else if (schemabuf != NULL)
		maxbuf = &buf[1];
	else
		maxbuf = &buf[0];

	curbuf = &buf[0];
	if (want_literal_dbname)
	{
		left = true;
		initPQExpBuffer(&left_literal);
	}
	else
		left = false;
	initPQExpBuffer(curbuf);
	appendPQExpBufferStr(curbuf, "^(");
	while (*cp)
	{
		char		ch = *cp;

		if (ch == '"')
		{
			if (inquotes && cp[1] == '"')
			{
				/* emit one quote, stay in inquotes mode */
				appendPQExpBufferChar(curbuf, '"');
				if (left)
					appendPQExpBufferChar(&left_literal, '"');
				cp++;
			}
			else
				inquotes = !inquotes;
			cp++;
		}
		else if (!inquotes && isupper((unsigned char) ch))
		{
			appendPQExpBufferChar(curbuf,
								  pg_tolower((unsigned char) ch));
			if (left)
				appendPQExpBufferChar(&left_literal,
									  pg_tolower((unsigned char) ch));
			cp++;
		}
		else if (!inquotes && ch == '*')
		{
			appendPQExpBufferStr(curbuf, ".*");
			if (left)
				appendPQExpBufferChar(&left_literal, '*');
			cp++;
		}
		else if (!inquotes && ch == '?')
		{
			appendPQExpBufferChar(curbuf, '.');
			if (left)
				appendPQExpBufferChar(&left_literal, '?');
			cp++;
		}
		else if (!inquotes && ch == '.')
		{
			left = false;
			if (dotcnt)
				(*dotcnt)++;
			if (curbuf < maxbuf)
			{
				appendPQExpBufferStr(curbuf, ")$");
				curbuf++;
				initPQExpBuffer(curbuf);
				appendPQExpBufferStr(curbuf, "^(");
				cp++;
			}
			else
				appendPQExpBufferChar(curbuf, *cp++);
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
			appendPQExpBufferStr(curbuf, "\\$");
			if (left)
				appendPQExpBufferChar(&left_literal, '$');
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
			 *
			 * As an exception to that, though, always quote "[]", as that's
			 * much more likely to be an attempt to write an array type name
			 * than it is to be the start of a regexp bracket expression.
			 */
			if ((inquotes || force_escape) &&
				strchr("|*+?()[]{}.^$\\", ch))
				appendPQExpBufferChar(curbuf, '\\');
			else if (ch == '[' && cp[1] == ']')
				appendPQExpBufferChar(curbuf, '\\');
			i = PQmblenBounded(cp, encoding);
			while (i--)
			{
				if (left)
					appendPQExpBufferChar(&left_literal, *cp);
				appendPQExpBufferChar(curbuf, *cp++);
			}
		}
	}
	appendPQExpBufferStr(curbuf, ")$");

	if (namebuf)
	{
		appendPQExpBufferStr(namebuf, curbuf->data);
		termPQExpBuffer(curbuf);
		curbuf--;
	}

	if (schemabuf && curbuf >= buf)
	{
		appendPQExpBufferStr(schemabuf, curbuf->data);
		termPQExpBuffer(curbuf);
		curbuf--;
	}

	if (dbnamebuf && curbuf >= buf)
	{
		if (want_literal_dbname)
			appendPQExpBufferStr(dbnamebuf, left_literal.data);
		else
			appendPQExpBufferStr(dbnamebuf, curbuf->data);
		termPQExpBuffer(curbuf);
	}

	if (want_literal_dbname)
		termPQExpBuffer(&left_literal);
}
