/*-------------------------------------------------------------------------
 *
 * Utility routines for SQL dumping
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * $Header: /cvsroot/pgsql/src/bin/pg_dump/dumputils.c,v 1.1 2002/08/27 18:57:26 petere Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "dumputils.h"

#include "parser/keywords.h"



/*
 *	Quotes input string if it's not a legitimate SQL identifier as-is.
 *
 *	Note that the returned string must be used before calling fmtId again,
 *	since we re-use the same return buffer each time.  Non-reentrant but
 *	avoids memory leakage.
 */
const char *
fmtId(const char *rawid)
{
	static PQExpBuffer id_return = NULL;
	const char *cp;
	bool need_quotes = false;

	if (id_return)				/* first time through? */
		resetPQExpBuffer(id_return);
	else
		id_return = createPQExpBuffer();

	/* These checks need to match the identifier production in scan.l.
	 * Don't use islower() etc. */

	if (ScanKeywordLookup(rawid))
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
		/* no quoting needed */
		appendPQExpBufferStr(id_return, rawid);
	}
	else
	{
		appendPQExpBufferChar(id_return, '\"');
		for (cp = rawid; *cp; cp++)
		{
			/*
			 * Did we find a double-quote in the string? Then make this a
			 * double double-quote per SQL99. Before, we put in a
			 * backslash/double-quote pair. - thomas 2000-08-05
			 */
			if (*cp == '\"')
				appendPQExpBufferChar(id_return, '\"');
			appendPQExpBufferChar(id_return, *cp);
		}
		appendPQExpBufferChar(id_return, '\"');
	}

	return id_return->data;
}



/*
 * Convert a string value to an SQL string literal and append it to
 * the given buffer.
 *
 * Special characters are escaped. Quote mark ' goes to '' per SQL
 * standard, other stuff goes to \ sequences.  If escapeAll is false,
 * whitespace characters are not escaped (tabs, newlines, etc.).  This
 * is appropriate for dump file output.
 */
void
appendStringLiteral(PQExpBuffer buf, const char *str, bool escapeAll)
{
	appendPQExpBufferChar(buf, '\'');
	while (*str)
	{
		char		ch = *str++;

		if (ch == '\\' || ch == '\'')
		{
			appendPQExpBufferChar(buf, ch);		/* double these */
			appendPQExpBufferChar(buf, ch);
		}
		else if ((unsigned char) ch < (unsigned char) ' ' &&
				 (escapeAll
				  || (ch != '\t' && ch != '\n' && ch != '\v' && ch != '\f' && ch != '\r')
				  ))
		{
			/*
			 * generate octal escape for control chars other than
			 * whitespace
			 */
			appendPQExpBufferChar(buf, '\\');
			appendPQExpBufferChar(buf, ((ch >> 6) & 3) + '0');
			appendPQExpBufferChar(buf, ((ch >> 3) & 7) + '0');
			appendPQExpBufferChar(buf, (ch & 7) + '0');
		}
		else
			appendPQExpBufferChar(buf, ch);
	}
	appendPQExpBufferChar(buf, '\'');
}
