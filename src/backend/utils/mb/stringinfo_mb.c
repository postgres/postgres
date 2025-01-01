/*-------------------------------------------------------------------------
 *
 * stringinfo_mb.c
 *		Multibyte encoding-aware additional StringInfo facilities
 *
 * This is separate from common/stringinfo.c so that frontend users
 * of that file need not pull in unnecessary multibyte-encoding support
 * code.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/mb/stringinfo_mb.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "mb/pg_wchar.h"
#include "mb/stringinfo_mb.h"


/*
 * appendStringInfoStringQuoted
 *
 * Append up to maxlen bytes from s to str, or the whole input string if
 * maxlen < 0, adding single quotes around it and doubling all single quotes.
 * Add an ellipsis if the copy is incomplete.
 */
void
appendStringInfoStringQuoted(StringInfo str, const char *s, int maxlen)
{
	char	   *copy = NULL;
	const char *chunk_search_start,
			   *chunk_copy_start,
			   *chunk_end;
	int			slen;
	bool		ellipsis;

	Assert(str != NULL);

	slen = strlen(s);
	if (maxlen >= 0 && maxlen < slen)
	{
		int			finallen = pg_mbcliplen(s, slen, maxlen);

		copy = pnstrdup(s, finallen);
		chunk_search_start = copy;
		chunk_copy_start = copy;

		ellipsis = true;
	}
	else
	{
		chunk_search_start = s;
		chunk_copy_start = s;

		ellipsis = false;
	}

	appendStringInfoCharMacro(str, '\'');

	while ((chunk_end = strchr(chunk_search_start, '\'')) != NULL)
	{
		/* copy including the found delimiting ' */
		appendBinaryStringInfoNT(str,
								 chunk_copy_start,
								 chunk_end - chunk_copy_start + 1);

		/* in order to double it, include this ' into the next chunk as well */
		chunk_copy_start = chunk_end;
		chunk_search_start = chunk_end + 1;
	}

	/* copy the last chunk and terminate */
	if (ellipsis)
		appendStringInfo(str, "%s...'", chunk_copy_start);
	else
		appendStringInfo(str, "%s'", chunk_copy_start);

	if (copy)
		pfree(copy);
}
