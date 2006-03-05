/*-------------------------------------------------------------------------
 *
 * like_match.c
 *	  like expression handling internal code.
 *
 * This file is included by like.c *twice*, to provide an optimization
 * for single-byte encodings.
 *
 * Before the inclusion, we need to define following macros:
 *
 * CHAREQ
 * ICHAREQ
 * NextChar
 * CopyAdvChar
 * MatchText (MBMatchText)
 * MatchTextIC (MBMatchTextIC)
 * do_like_escape (MB_do_like_escape)
 *
 * Copyright (c) 1996-2006, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	$PostgreSQL: pgsql/src/backend/utils/adt/like_match.c,v 1.13 2006/03/05 15:58:42 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

/*
**	Originally written by Rich $alz, mirror!rs, Wed Nov 26 19:03:17 EST 1986.
**	Rich $alz is now <rsalz@bbn.com>.
**	Special thanks to Lars Mathiesen <thorinn@diku.dk> for the LABORT code.
**
**	This code was shamelessly stolen from the "pql" code by myself and
**	slightly modified :)
**
**	All references to the word "star" were replaced by "percent"
**	All references to the word "wild" were replaced by "like"
**
**	All the nice shell RE matching stuff was replaced by just "_" and "%"
**
**	As I don't have a copy of the SQL standard handy I wasn't sure whether
**	to leave in the '\' escape character handling.
**
**	Keith Parks. <keith@mtcc.demon.co.uk>
**
**	SQL92 lets you specify the escape character by saying
**	LIKE <pattern> ESCAPE <escape character>. We are a small operation
**	so we force you to use '\'. - ay 7/95
**
**	Now we have the like_escape() function that converts patterns with
**	any specified escape character (or none at all) to the internal
**	default escape character, which is still '\'. - tgl 9/2000
**
** The code is rewritten to avoid requiring null-terminated strings,
** which in turn allows us to leave out some memcpy() operations.
** This code should be faster and take less memory, but no promises...
** - thomas 2000-08-06
**
*/


/*--------------------
 *	Match text and p, return LIKE_TRUE, LIKE_FALSE, or LIKE_ABORT.
 *
 *	LIKE_TRUE: they match
 *	LIKE_FALSE: they don't match
 *	LIKE_ABORT: not only don't they match, but the text is too short.
 *
 * If LIKE_ABORT is returned, then no suffix of the text can match the
 * pattern either, so an upper-level % scan can stop scanning now.
 *--------------------
 */

static int
MatchText(char *t, int tlen, char *p, int plen)
{
	/* Fast path for match-everything pattern */
	if ((plen == 1) && (*p == '%'))
		return LIKE_TRUE;

	while ((tlen > 0) && (plen > 0))
	{
		if (*p == '\\')
		{
			/* Next pattern char must match literally, whatever it is */
			NextChar(p, plen);
			if ((plen <= 0) || !CHAREQ(t, p))
				return LIKE_FALSE;
		}
		else if (*p == '%')
		{
			/* %% is the same as % according to the SQL standard */
			/* Advance past all %'s */
			while ((plen > 0) && (*p == '%'))
				NextChar(p, plen);
			/* Trailing percent matches everything. */
			if (plen <= 0)
				return LIKE_TRUE;

			/*
			 * Otherwise, scan for a text position at which we can match the
			 * rest of the pattern.
			 */
			while (tlen > 0)
			{
				/*
				 * Optimization to prevent most recursion: don't recurse
				 * unless first pattern char might match this text char.
				 */
				if (CHAREQ(t, p) || (*p == '\\') || (*p == '_'))
				{
					int			matched = MatchText(t, tlen, p, plen);

					if (matched != LIKE_FALSE)
						return matched; /* TRUE or ABORT */
				}

				NextChar(t, tlen);
			}

			/*
			 * End of text with no match, so no point in trying later places
			 * to start matching this pattern.
			 */
			return LIKE_ABORT;
		}
		else if ((*p != '_') && !CHAREQ(t, p))
		{
			/*
			 * Not the single-character wildcard and no explicit match? Then
			 * time to quit...
			 */
			return LIKE_FALSE;
		}

		NextChar(t, tlen);
		NextChar(p, plen);
	}

	if (tlen > 0)
		return LIKE_FALSE;		/* end of pattern, but not of text */

	/* End of input string.  Do we have matching pattern remaining? */
	while ((plen > 0) && (*p == '%'))	/* allow multiple %'s at end of
										 * pattern */
		NextChar(p, plen);
	if (plen <= 0)
		return LIKE_TRUE;

	/*
	 * End of text with no match, so no point in trying later places to start
	 * matching this pattern.
	 */
	return LIKE_ABORT;
}	/* MatchText() */

/*
 * Same as above, but ignore case
 */
static int
MatchTextIC(char *t, int tlen, char *p, int plen)
{
	/* Fast path for match-everything pattern */
	if ((plen == 1) && (*p == '%'))
		return LIKE_TRUE;

	while ((tlen > 0) && (plen > 0))
	{
		if (*p == '\\')
		{
			/* Next pattern char must match literally, whatever it is */
			NextChar(p, plen);
			if ((plen <= 0) || !ICHAREQ(t, p))
				return LIKE_FALSE;
		}
		else if (*p == '%')
		{
			/* %% is the same as % according to the SQL standard */
			/* Advance past all %'s */
			while ((plen > 0) && (*p == '%'))
				NextChar(p, plen);
			/* Trailing percent matches everything. */
			if (plen <= 0)
				return LIKE_TRUE;

			/*
			 * Otherwise, scan for a text position at which we can match the
			 * rest of the pattern.
			 */
			while (tlen > 0)
			{
				/*
				 * Optimization to prevent most recursion: don't recurse
				 * unless first pattern char might match this text char.
				 */
				if (ICHAREQ(t, p) || (*p == '\\') || (*p == '_'))
				{
					int			matched = MatchTextIC(t, tlen, p, plen);

					if (matched != LIKE_FALSE)
						return matched; /* TRUE or ABORT */
				}

				NextChar(t, tlen);
			}

			/*
			 * End of text with no match, so no point in trying later places
			 * to start matching this pattern.
			 */
			return LIKE_ABORT;
		}
		else if ((*p != '_') && !ICHAREQ(t, p))
		{
			/*
			 * Not the single-character wildcard and no explicit match? Then
			 * time to quit...
			 */
			return LIKE_FALSE;
		}

		NextChar(t, tlen);
		NextChar(p, plen);
	}

	if (tlen > 0)
		return LIKE_FALSE;		/* end of pattern, but not of text */

	/* End of input string.  Do we have matching pattern remaining? */
	while ((plen > 0) && (*p == '%'))	/* allow multiple %'s at end of
										 * pattern */
		NextChar(p, plen);
	if (plen <= 0)
		return LIKE_TRUE;

	/*
	 * End of text with no match, so no point in trying later places to start
	 * matching this pattern.
	 */
	return LIKE_ABORT;
}	/* MatchTextIC() */

/*
 * like_escape() --- given a pattern and an ESCAPE string,
 * convert the pattern to use Postgres' standard backslash escape convention.
 */
static text *
do_like_escape(text *pat, text *esc)
{
	text	   *result;
	char	   *p,
			   *e,
			   *r;
	int			plen,
				elen;
	bool		afterescape;

	p = VARDATA(pat);
	plen = (VARSIZE(pat) - VARHDRSZ);
	e = VARDATA(esc);
	elen = (VARSIZE(esc) - VARHDRSZ);

	/*
	 * Worst-case pattern growth is 2x --- unlikely, but it's hardly worth
	 * trying to calculate the size more accurately than that.
	 */
	result = (text *) palloc(plen * 2 + VARHDRSZ);
	r = VARDATA(result);

	if (elen == 0)
	{
		/*
		 * No escape character is wanted.  Double any backslashes in the
		 * pattern to make them act like ordinary characters.
		 */
		while (plen > 0)
		{
			if (*p == '\\')
				*r++ = '\\';
			CopyAdvChar(r, p, plen);
		}
	}
	else
	{
		/*
		 * The specified escape must be only a single character.
		 */
		NextChar(e, elen);
		if (elen != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_ESCAPE_SEQUENCE),
					 errmsg("invalid escape string"),
				  errhint("Escape string must be empty or one character.")));

		e = VARDATA(esc);

		/*
		 * If specified escape is '\', just copy the pattern as-is.
		 */
		if (*e == '\\')
		{
			memcpy(result, pat, VARSIZE(pat));
			return result;
		}

		/*
		 * Otherwise, convert occurrences of the specified escape character to
		 * '\', and double occurrences of '\' --- unless they immediately
		 * follow an escape character!
		 */
		afterescape = false;
		while (plen > 0)
		{
			if (CHAREQ(p, e) && !afterescape)
			{
				*r++ = '\\';
				NextChar(p, plen);
				afterescape = true;
			}
			else if (*p == '\\')
			{
				*r++ = '\\';
				if (!afterescape)
					*r++ = '\\';
				NextChar(p, plen);
				afterescape = false;
			}
			else
			{
				CopyAdvChar(r, p, plen);
				afterescape = false;
			}
		}
	}

	VARATT_SIZEP(result) = r - ((char *) result);

	return result;
}
