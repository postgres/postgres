/*-------------------------------------------------------------------------
 *
 * like.c
 *	  like expression handling code.
 *
 *	 NOTES
 *		A big hack of the regexp.c code!! Contributed by
 *		Keith Parks <emkxp01@mtcc.demon.co.uk> (7/95).
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	$Header: /cvsroot/pgsql/src/backend/utils/adt/like.c,v 1.38 2000/08/06 18:05:41 thomas Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "mb/pg_wchar.h"
#include "utils/builtins.h"


#define LIKE_TRUE						1
#define LIKE_FALSE						0
#define LIKE_ABORT						(-1)


static int MatchText(pg_wchar * t, int tlen, pg_wchar * p, int plen, char *e);
static int MatchTextLower(pg_wchar * t, int tlen, pg_wchar * p, int plen, char *e);


/*
 *	interface routines called by the function manager
 */

Datum
namelike(PG_FUNCTION_ARGS)
{
	Name		n = PG_GETARG_NAME(0);
	text	   *p = PG_GETARG_TEXT_P(1);

	PG_RETURN_BOOL(MatchText(NameStr(*n), strlen(NameStr(*n)),
							 VARDATA(p), (VARSIZE(p)-VARHDRSZ),
							 NULL)
				   == LIKE_TRUE);
}

Datum
namenlike(PG_FUNCTION_ARGS)
{
	Name		n = PG_GETARG_NAME(0);
	text	   *p = PG_GETARG_TEXT_P(1);

	PG_RETURN_BOOL(MatchText(NameStr(*n), strlen(NameStr(*n)),
							 VARDATA(p), (VARSIZE(p)-VARHDRSZ),
							 NULL)
				   != LIKE_TRUE);
}

Datum
namelike_escape(PG_FUNCTION_ARGS)
{
	Name		n = PG_GETARG_NAME(0);
	text	   *p = PG_GETARG_TEXT_P(1);
	text	   *e = PG_GETARG_TEXT_P(2);

	PG_RETURN_BOOL(MatchText(NameStr(*n), strlen(NameStr(*n)),
							 VARDATA(p), (VARSIZE(p)-VARHDRSZ),
							 ((VARSIZE(e)-VARHDRSZ) > 0? VARDATA(e): NULL))
				   == LIKE_TRUE);
}

Datum
namenlike_escape(PG_FUNCTION_ARGS)
{
	Name		n = PG_GETARG_NAME(0);
	text	   *p = PG_GETARG_TEXT_P(1);
	text	   *e = PG_GETARG_TEXT_P(2);

	PG_RETURN_BOOL(MatchText(NameStr(*n), strlen(NameStr(*n)),
							 VARDATA(p), (VARSIZE(p)-VARHDRSZ),
							 ((VARSIZE(e)-VARHDRSZ) > 0? VARDATA(e): NULL))
				   != LIKE_TRUE);
}

Datum
textlike(PG_FUNCTION_ARGS)
{
	text	   *s = PG_GETARG_TEXT_P(0);
	text	   *p = PG_GETARG_TEXT_P(1);

	PG_RETURN_BOOL(MatchText(VARDATA(s), (VARSIZE(s)-VARHDRSZ),
							 VARDATA(p), (VARSIZE(p)-VARHDRSZ),
							 NULL)
				   == LIKE_TRUE);
}

Datum
textnlike(PG_FUNCTION_ARGS)
{
	text	   *s = PG_GETARG_TEXT_P(0);
	text	   *p = PG_GETARG_TEXT_P(1);

	PG_RETURN_BOOL(MatchText(VARDATA(s), (VARSIZE(s)-VARHDRSZ),
							 VARDATA(p), (VARSIZE(p)-VARHDRSZ),
							 NULL)
				   != LIKE_TRUE);
}

Datum
textlike_escape(PG_FUNCTION_ARGS)
{
	text	   *s = PG_GETARG_TEXT_P(0);
	text	   *p = PG_GETARG_TEXT_P(1);
	text	   *e = PG_GETARG_TEXT_P(2);

	PG_RETURN_BOOL(MatchText(VARDATA(s), (VARSIZE(s)-VARHDRSZ),
							 VARDATA(p), (VARSIZE(p)-VARHDRSZ),
							 ((VARSIZE(e)-VARHDRSZ) > 0? VARDATA(e): NULL))
				   == LIKE_TRUE);
}

Datum
textnlike_escape(PG_FUNCTION_ARGS)
{
	text	   *s = PG_GETARG_TEXT_P(0);
	text	   *p = PG_GETARG_TEXT_P(1);
	text	   *e = PG_GETARG_TEXT_P(2);

	PG_RETURN_BOOL(MatchText(VARDATA(s), (VARSIZE(s)-VARHDRSZ),
							 VARDATA(p), (VARSIZE(p)-VARHDRSZ),
							 ((VARSIZE(e)-VARHDRSZ) > 0? VARDATA(e): NULL))
				   != LIKE_TRUE);
}

/*
 * Case-insensitive versions
 */

Datum
inamelike(PG_FUNCTION_ARGS)
{
	Name		n = PG_GETARG_NAME(0);
	text	   *p = PG_GETARG_TEXT_P(1);

	PG_RETURN_BOOL(MatchTextLower(NameStr(*n), strlen(NameStr(*n)),
								  VARDATA(p), (VARSIZE(p)-VARHDRSZ),
								  NULL)
				   == LIKE_TRUE);
}

Datum
inamenlike(PG_FUNCTION_ARGS)
{
	Name		n = PG_GETARG_NAME(0);
	text	   *p = PG_GETARG_TEXT_P(1);

	PG_RETURN_BOOL(MatchTextLower(NameStr(*n), strlen(NameStr(*n)),
								  VARDATA(p), (VARSIZE(p)-VARHDRSZ),
								  NULL)
				   != LIKE_TRUE);
}

Datum
inamelike_escape(PG_FUNCTION_ARGS)
{
	Name		n = PG_GETARG_NAME(0);
	text	   *p = PG_GETARG_TEXT_P(1);
	text	   *e = PG_GETARG_TEXT_P(2);

	PG_RETURN_BOOL(MatchTextLower(NameStr(*n), strlen(NameStr(*n)),
								  VARDATA(p), (VARSIZE(p)-VARHDRSZ),
								  ((VARSIZE(e)-VARHDRSZ) > 0? VARDATA(e): NULL))
				   == LIKE_TRUE);
}

Datum
inamenlike_escape(PG_FUNCTION_ARGS)
{
	Name		n = PG_GETARG_NAME(0);
	text	   *p = PG_GETARG_TEXT_P(1);
	text	   *e = PG_GETARG_TEXT_P(2);

	PG_RETURN_BOOL(MatchTextLower(NameStr(*n), strlen(NameStr(*n)),
								  VARDATA(p), (VARSIZE(p)-VARHDRSZ),
								  ((VARSIZE(e)-VARHDRSZ) > 0? VARDATA(e): NULL))
				   != LIKE_TRUE);
}

Datum
itextlike(PG_FUNCTION_ARGS)
{
	text	   *s = PG_GETARG_TEXT_P(0);
	text	   *p = PG_GETARG_TEXT_P(1);

	PG_RETURN_BOOL(MatchTextLower(VARDATA(s), (VARSIZE(s)-VARHDRSZ),
								  VARDATA(p), (VARSIZE(p)-VARHDRSZ),
								  NULL)
				   == LIKE_TRUE);
}

Datum
itextnlike(PG_FUNCTION_ARGS)
{
	text	   *s = PG_GETARG_TEXT_P(0);
	text	   *p = PG_GETARG_TEXT_P(1);

	PG_RETURN_BOOL(MatchTextLower(VARDATA(s), (VARSIZE(s)-VARHDRSZ),
								  VARDATA(p), (VARSIZE(p)-VARHDRSZ),
								  NULL)
				   != LIKE_TRUE);
}

Datum
itextlike_escape(PG_FUNCTION_ARGS)
{
	text	   *s = PG_GETARG_TEXT_P(0);
	text	   *p = PG_GETARG_TEXT_P(1);
	text	   *e = PG_GETARG_TEXT_P(2);

	PG_RETURN_BOOL(MatchTextLower(VARDATA(s), (VARSIZE(s)-VARHDRSZ),
								  VARDATA(p), (VARSIZE(p)-VARHDRSZ),
								  ((VARSIZE(e)-VARHDRSZ) > 0? VARDATA(e): NULL))
				   == LIKE_TRUE);
}

Datum
itextnlike_escape(PG_FUNCTION_ARGS)
{
	text	   *s = PG_GETARG_TEXT_P(0);
	text	   *p = PG_GETARG_TEXT_P(1);
	text	   *e = PG_GETARG_TEXT_P(2);

	PG_RETURN_BOOL(MatchTextLower(VARDATA(s), (VARSIZE(s)-VARHDRSZ),
								  VARDATA(p), (VARSIZE(p)-VARHDRSZ),
								  ((VARSIZE(e)-VARHDRSZ) > 0? VARDATA(e): NULL))
				   != LIKE_TRUE);
}


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
**	[SQL92 lets you specify the escape character by saying
**	 LIKE <pattern> ESCAPE <escape character>. We are a small operation
**	 so we force you to use '\'. - ay 7/95]
**
** OK, we now support the SQL9x LIKE <pattern> ESCAPE <char> syntax.
** We should kill the backslash escaping mechanism since it is non-standard
** and undocumented afaik.
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

#define NextChar(p, plen) (p)++, (plen)--

static int
MatchText(pg_wchar * t, int tlen, pg_wchar * p, int plen, char *e)
{
	/* Fast path for match-everything pattern
	 * Include weird case of escape character as a percent sign or underscore,
	 * when presumably that wildcard character becomes a literal.
	 */
	if ((plen == 1) && (*p == '%')
		&& ! ((e != NULL) && (*e == '%')))
		return LIKE_TRUE;

	while ((tlen > 0) && (plen > 0))
	{
		/* If an escape character was specified and we find it here in the pattern,
		 * then we'd better have an exact match for the next character.
		 */
		if ((e != NULL) && (*p == *e))
		{
			NextChar(p, plen);
			if ((plen <= 0) || (*t != *p))
				return LIKE_FALSE;
		}
		else
		{
			switch (*p)
			{
				case '\\':
					/* Literal match with following character. */
					NextChar(p, plen);
					/* FALLTHROUGH */
				default:
					if (*t != *p)
						return LIKE_FALSE;
					break;
				case '_':
					/* Match any single character. */
					break;
				case '%':
					/* %% is the same as % according to the SQL standard */
					/* Advance past all %'s */
					while ((plen > 0) && (*p == '%'))
						NextChar(p, plen);
					/* Trailing percent matches everything. */
					if (plen <= 0)
						return LIKE_TRUE;

					/*
					 * Otherwise, scan for a text position at which we can
					 * match the rest of the pattern.
					 */
					while (tlen > 0)
					{
						/*
						 * Optimization to prevent most recursion: don't
						 * recurse unless first pattern char might match this
						 * text char.
						 */
						if ((*t == *p) || (*p == '\\') || (*p == '_')
							|| ((e != NULL) && (*p == *e)))
						{
							int matched = MatchText(t, tlen, p, plen, e);

							if (matched != LIKE_FALSE)
								return matched;		/* TRUE or ABORT */
						}

						NextChar(t, tlen);
					}

					/*
					 * End of text with no match, so no point in trying later
					 * places to start matching this pattern.
					 */
					return LIKE_ABORT;
			}
		}

		NextChar(t, tlen);
		NextChar(p, plen);
	}

	if (tlen > 0)
		return LIKE_FALSE;		/* end of pattern, but not of text */

	/* End of input string.  Do we have matching pattern remaining? */
	while ((plen > 0) && (*p == '%'))	/* allow multiple %'s at end of pattern */
		NextChar(p, plen);
	if (plen <= 0)
		return LIKE_TRUE;

	/*
	 * End of text with no match, so no point in trying later places to
	 * start matching this pattern.
	 */
	return LIKE_ABORT;
} /* MatchText() */

static int
MatchTextLower(pg_wchar * t, int tlen, pg_wchar * p, int plen, char *e)
{
	/* Fast path for match-everything pattern
	 * Include weird case of escape character as a percent sign or underscore,
	 * when presumably that wildcard character becomes a literal.
	 */
	if ((plen == 1) && (*p == '%')
		&& ! ((e != NULL) && (*e == '%')))
		return LIKE_TRUE;

	while ((tlen > 0) && (plen > 0))
	{
		/* If an escape character was specified and we find it here in the pattern,
		 * then we'd better have an exact match for the next character.
		 */
		if ((e != NULL) && (tolower(*p) == tolower(*e)))
		{
			NextChar(p, plen);
			if ((plen <= 0) || (tolower(*t) != tolower(*p)))
				return LIKE_FALSE;
		}
		else
		{
			switch (*p)
			{
				case '\\':
					/* Literal match with following character. */
					NextChar(p, plen);
					/* FALLTHROUGH */
				default:
					if (tolower(*t) != tolower(*p))
						return LIKE_FALSE;
					break;
				case '_':
					/* Match any single character. */
					break;
				case '%':
					/* %% is the same as % according to the SQL standard */
					/* Advance past all %'s */
					while ((plen > 0) && (*p == '%'))
						NextChar(p, plen);
					/* Trailing percent matches everything. */
					if (plen <= 0)
						return LIKE_TRUE;

					/*
					 * Otherwise, scan for a text position at which we can
					 * match the rest of the pattern.
					 */
					while (tlen > 0)
					{
						/*
						 * Optimization to prevent most recursion: don't
						 * recurse unless first pattern char might match this
						 * text char.
						 */
						if ((tolower(*t) == tolower(*p)) || (*p == '\\') || (*p == '_')
							|| ((e != NULL) && (tolower(*p) == tolower(*e))))
						{
							int matched = MatchText(t, tlen, p, plen, e);

							if (matched != LIKE_FALSE)
								return matched;		/* TRUE or ABORT */
						}

						NextChar(t, tlen);
					}

					/*
					 * End of text with no match, so no point in trying later
					 * places to start matching this pattern.
					 */
					return LIKE_ABORT;
			}
		}

		NextChar(t, tlen);
		NextChar(p, plen);
	}

	if (tlen > 0)
		return LIKE_FALSE;		/* end of pattern, but not of text */

	/* End of input string.  Do we have matching pattern remaining? */
	while ((plen > 0) && (*p == '%'))	/* allow multiple %'s at end of pattern */
		NextChar(p, plen);
	if (plen <= 0)
		return LIKE_TRUE;

	/*
	 * End of text with no match, so no point in trying later places to
	 * start matching this pattern.
	 */
	return LIKE_ABORT;
} /* MatchTextLower() */
