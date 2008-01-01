/*-------------------------------------------------------------------------
 *
 * like_match.c
 *	  like expression handling internal code.
 *
 * This file is included by like.c four times, to provide natching code for
 * single-byte encodings, UTF8, and for other multi-byte encodings,
 * and case insensitive matches for single byte encodings.
 * UTF8 is a special case because we can use a much more efficient version
 * of NextChar than can be used for other multi-byte encodings.
 *
 * Before the inclusion, we need to define following macros:
 *
 * NextChar
 * MatchText - to name of function wanted
 * do_like_escape - name of function if wanted - needs CHAREQ and CopyAdvChar
 * MATCH_LOWER - define iff using to_lower on text chars
 *
 * Copyright (c) 1996-2008, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	$PostgreSQL: pgsql/src/backend/utils/adt/like_match.c,v 1.20 2008/01/01 19:45:52 momjian Exp $
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

#ifdef MATCH_LOWER
#define TCHAR(t) tolower((t))
#else
#define TCHAR(t) (t)
#endif

static int
MatchText(char *t, int tlen, char *p, int plen)
{
	/* Fast path for match-everything pattern */
	if ((plen == 1) && (*p == '%'))
		return LIKE_TRUE;

	/*
	 * In this loop, we advance by char when matching wildcards (and thus on
	 * recursive entry to this function we are properly char-synced). On other
	 * occasions it is safe to advance by byte, as the text and pattern will
	 * be in lockstep. This allows us to perform all comparisons  between the
	 * text and pattern on a byte by byte basis, even for multi-byte
	 * encodings.
	 */

	while ((tlen > 0) && (plen > 0))
	{
		if (*p == '\\')
		{
			/* Next byte must match literally, whatever it is */
			NextByte(p, plen);
			if ((plen <= 0) || *p != *t)
				return LIKE_FALSE;
		}
		else if (*p == '%')
		{
			/*
			 * % processing is essentially a search for a match for what
			 * follows the %, plus a recursive match of the remainder. We
			 * succeed if and only if both conditions are met.
			 */

			/* %% is the same as % according to the SQL standard */
			/* Advance past all %'s */
			while ((plen > 0) && (*p == '%'))
				NextByte(p, plen);
			/* Trailing percent matches everything. */
			if (plen <= 0)
				return LIKE_TRUE;

			/*
			 * Otherwise, scan for a text position at which we can match the
			 * rest of the pattern.
			 */
			if (*p == '_')

			{
				/* %_ is the same as _% - avoid matching _ repeatedly */

				NextChar(t, tlen);
				NextByte(p, plen);

				if (tlen <= 0)
				{
					return (plen <= 0) ? LIKE_TRUE : LIKE_ABORT;
				}
				else if (plen <= 0)
				{
					return LIKE_FALSE;
				}

				while (tlen > 0)
				{
					int			matched = MatchText(t, tlen, p, plen);

					if (matched != LIKE_FALSE)
						return matched; /* TRUE or ABORT */

					NextChar(t, tlen);
				}
			}
			else
			{

				char		firstpat = TCHAR(*p);

				if (*p == '\\')
				{
					if (plen < 2)
						return LIKE_FALSE;
					firstpat = TCHAR(p[1]);
				}

				while (tlen > 0)
				{
					/*
					 * Optimization to prevent most recursion: don't recurse
					 * unless first pattern byte matches first text byte.
					 */
					if (TCHAR(*t) == firstpat)
					{
						int			matched = MatchText(t, tlen, p, plen);

						if (matched != LIKE_FALSE)
							return matched;		/* TRUE or ABORT */
					}

					NextChar(t, tlen);

				}
			}

			/*
			 * End of text with no match, so no point in trying later places
			 * to start matching this pattern.
			 */
			return LIKE_ABORT;
		}
		else if (*p == '_')
		{
			NextChar(t, tlen);
			NextByte(p, plen);
			continue;
		}
		else if (TCHAR(*t) != TCHAR(*p))
		{
			/*
			 * Not the single-character wildcard and no explicit match? Then
			 * time to quit...
			 */
			return LIKE_FALSE;
		}

		/*
		 * It is safe to use NextByte instead of NextChar here, even for
		 * multi-byte character sets, because we are not following immediately
		 * after a wildcard character. If we are in the middle of a multibyte
		 * character, we must already have matched at least one byte of the
		 * character from both text and pattern; so we cannot get out-of-sync
		 * on character boundaries.  And we know that no backend-legal
		 * encoding allows ASCII characters such as '%' to appear as non-first
		 * bytes of characters, so we won't mistakenly detect a new wildcard.
		 */
		NextByte(t, tlen);
		NextByte(p, plen);
	}

	if (tlen > 0)
		return LIKE_FALSE;		/* end of pattern, but not of text */

	/* End of input string.  Do we have matching pattern remaining? */
	while ((plen > 0) && (*p == '%'))	/* allow multiple %'s at end of
										 * pattern */
		NextByte(p, plen);

	if (plen <= 0)
		return LIKE_TRUE;

	/*
	 * End of text with no match, so no point in trying later places to start
	 * matching this pattern.
	 */
	return LIKE_ABORT;
}	/* MatchText() */

/*
 * like_escape() --- given a pattern and an ESCAPE string,
 * convert the pattern to use Postgres' standard backslash escape convention.
 */
#ifdef do_like_escape

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

	p = VARDATA_ANY(pat);
	plen = VARSIZE_ANY_EXHDR(pat);
	e = VARDATA_ANY(esc);
	elen = VARSIZE_ANY_EXHDR(esc);

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

		e = VARDATA_ANY(esc);

		/*
		 * If specified escape is '\', just copy the pattern as-is.
		 */
		if (*e == '\\')
		{
			memcpy(result, pat, VARSIZE_ANY(pat));
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

	SET_VARSIZE(result, r - ((char *) result));

	return result;
}
#endif   /* do_like_escape */

#ifdef CHAREQ
#undef CHAREQ
#endif

#undef NextChar
#undef CopyAdvChar
#undef MatchText

#ifdef do_like_escape
#undef do_like_escape
#endif

#undef TCHAR

#ifdef MATCH_LOWER
#undef MATCH_LOWER

#endif
