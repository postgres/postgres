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
 *	$Header: /cvsroot/pgsql/src/backend/utils/adt/like.c,v 1.41 2000/08/22 06:33:57 ishii Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include <ctype.h>
#ifdef MULTIBYTE
#include "mb/pg_wchar.h"
#endif
#include "utils/builtins.h"


#define LIKE_TRUE						1
#define LIKE_FALSE						0
#define LIKE_ABORT						(-1)


static int MatchText(unsigned char * t, int tlen, unsigned char * p, int plen, char *e);
static int MatchTextLower(unsigned char * t, int tlen, unsigned char * p, int plen, char *e);


/*
 *	interface routines called by the function manager
 */

Datum
namelike(PG_FUNCTION_ARGS)
{
	bool		result;
	Name		str = PG_GETARG_NAME(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
	unsigned char   *s, *p;
	int			slen, plen;

	s = NameStr(*str);
	slen = strlen(s);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);

	result = (MatchText(s, slen, p, plen, "\\") == LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
namenlike(PG_FUNCTION_ARGS)
{
	bool		result;
	Name		str = PG_GETARG_NAME(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
	unsigned char   *s, *p;
	int			slen, plen;

	s = NameStr(*str);
	slen = strlen(s);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);

	result = (MatchText(s, slen, p, plen, "\\") != LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
namelike_escape(PG_FUNCTION_ARGS)
{
	bool		result;
	Name		str = PG_GETARG_NAME(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
	text	   *esc = PG_GETARG_TEXT_P(2);
	unsigned char   *s, *p;
	int			slen, plen;
	char	   *e;

	s = NameStr(*str);
	slen = strlen(s);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);
	e = ((VARSIZE(esc)-VARHDRSZ) > 0? VARDATA(esc): NULL);

	result = (MatchText(s, slen, p, plen, e) == LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
namenlike_escape(PG_FUNCTION_ARGS)
{
	bool		result;
	Name		str = PG_GETARG_NAME(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
	text	   *esc = PG_GETARG_TEXT_P(2);
	unsigned char   *s, *p;
	int			slen, plen;
	char	   *e;

	s = NameStr(*str);
	slen = strlen(s);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);
	e = ((VARSIZE(esc)-VARHDRSZ) > 0? VARDATA(esc): NULL);

	result = (MatchText(s, slen, p, plen, e) != LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
textlike(PG_FUNCTION_ARGS)
{
	bool		result;
	text	   *str = PG_GETARG_TEXT_P(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
	unsigned char   *s, *p;
	int			slen, plen;

	s = VARDATA(str);
	slen = (VARSIZE(str)-VARHDRSZ);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);

	result = (MatchText(s, slen, p, plen, NULL) == LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
textnlike(PG_FUNCTION_ARGS)
{
	bool		result;
	text	   *str = PG_GETARG_TEXT_P(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
	unsigned char   *s, *p;
	int			slen, plen;

	s = VARDATA(str);
	slen = (VARSIZE(str)-VARHDRSZ);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);

	result = (MatchText(s, slen, p, plen, "\\") != LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
textlike_escape(PG_FUNCTION_ARGS)
{
	bool		result;
	text	   *str = PG_GETARG_TEXT_P(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
	text	   *esc = PG_GETARG_TEXT_P(2);
	unsigned char   *s, *p;
	int			slen, plen;
	char	   *e;

	s = VARDATA(str);
	slen = (VARSIZE(str)-VARHDRSZ);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);
	e = ((VARSIZE(esc)-VARHDRSZ) > 0? VARDATA(esc): NULL);

	result = (MatchText(s, slen, p, plen, e) == LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
textnlike_escape(PG_FUNCTION_ARGS)
{
	bool		result;
	text	   *str = PG_GETARG_TEXT_P(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
	text	   *esc = PG_GETARG_TEXT_P(2);
	unsigned char   *s, *p;
	int			slen, plen;
	char	   *e;

	s = VARDATA(str);
	slen = (VARSIZE(str)-VARHDRSZ);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);
	e = ((VARSIZE(esc)-VARHDRSZ) > 0? VARDATA(esc): NULL);

	result = (MatchText(s, slen, p, plen, e) != LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

/*
 * Case-insensitive versions
 */

Datum
inamelike(PG_FUNCTION_ARGS)
{
	bool		result;
	Name		str = PG_GETARG_NAME(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
	unsigned char   *s, *p;
	int			slen, plen;

	s = NameStr(*str);
	slen = strlen(s);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);

	result = (MatchTextLower(s, slen, p, plen, "\\") == LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
inamenlike(PG_FUNCTION_ARGS)
{
	bool		result;
	Name		str = PG_GETARG_NAME(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
	unsigned char   *s, *p;
	int			slen, plen;

	s = NameStr(*str);
	slen = strlen(s);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);

	result = (MatchTextLower(s, slen, p, plen, "\\") != LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
inamelike_escape(PG_FUNCTION_ARGS)
{
	bool		result;
	Name		str = PG_GETARG_NAME(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
	text	   *esc = PG_GETARG_TEXT_P(2);
	unsigned char   *s, *p;
	int			slen, plen;
	char	   *e;

	s = NameStr(*str);
	slen = strlen(s);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);
	e = ((VARSIZE(esc)-VARHDRSZ) > 0? VARDATA(esc): NULL);

	result = (MatchTextLower(s, slen, p, plen, e) == LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
inamenlike_escape(PG_FUNCTION_ARGS)
{
	bool		result;
	Name		str = PG_GETARG_NAME(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
	text	   *esc = PG_GETARG_TEXT_P(2);
	unsigned char   *s, *p;
	int			slen, plen;
	char	   *e;

	s = NameStr(*str);
	slen = strlen(s);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);
	e = ((VARSIZE(esc)-VARHDRSZ) > 0? VARDATA(esc): NULL);

	result = (MatchTextLower(s, slen, p, plen, e) != LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
itextlike(PG_FUNCTION_ARGS)
{
	bool		result;
	text	   *str = PG_GETARG_TEXT_P(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
	unsigned char   *s, *p;
	int			slen, plen;

	s = VARDATA(str);
	slen = (VARSIZE(str)-VARHDRSZ);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);

	result = (MatchTextLower(s, slen, p, plen, "\\") == LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
itextnlike(PG_FUNCTION_ARGS)
{
	bool		result;
	text	   *str = PG_GETARG_TEXT_P(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
	unsigned char   *s, *p;
	int			slen, plen;

	s = VARDATA(str);
	slen = (VARSIZE(str)-VARHDRSZ);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);

	result = (MatchTextLower(s, slen, p, plen, "\\") != LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
itextlike_escape(PG_FUNCTION_ARGS)
{
	bool		result;
	text	   *str = PG_GETARG_TEXT_P(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
	text	   *esc = PG_GETARG_TEXT_P(2);
	unsigned char   *s, *p;
	int			slen, plen;
	char	   *e;

	s = VARDATA(str);
	slen = (VARSIZE(str)-VARHDRSZ);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);
	e = ((VARSIZE(esc)-VARHDRSZ) > 0? VARDATA(esc): NULL);

	result = (MatchTextLower(s, slen, p, plen, e) == LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
itextnlike_escape(PG_FUNCTION_ARGS)
{
	bool		result;
	text	   *str = PG_GETARG_TEXT_P(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
	text	   *esc = PG_GETARG_TEXT_P(2);
	unsigned char   *s, *p;
	int			slen, plen;
	char	   *e;

	s = VARDATA(str);
	slen = (VARSIZE(str)-VARHDRSZ);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);
	e = ((VARSIZE(esc)-VARHDRSZ) > 0? VARDATA(esc): NULL);

	result = (MatchTextLower(s, slen, p, plen, e) != LIKE_TRUE);

	PG_RETURN_BOOL(result);
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

#ifdef MULTIBYTE
/*--------------------
 * Support routine for MatchText. Compares given multibyte streams
 * as wide characters. If they match, returns 1 otherwise returns 0.
 *--------------------
 */
static int wchareq(unsigned char *p1, unsigned char *p2)
{
	int l;

	l = pg_mblen(p1);
	if (pg_mblen(p2) != l) {
		return(0);
	}
	while (l--) {
		if (*p1++ != *p2++)
			return(0);
	}
	return(1);
}

/*--------------------
 * Support routine for MatchTextLower. Compares given multibyte streams
 * as wide characters ignoring case.
 * If they match, returns 1 otherwise returns 0.
 *--------------------
 */
#define UCHARMAX 0xff

static int iwchareq(unsigned char *p1, unsigned char *p2)
{
	int c1, c2;
	int l;

	/* short cut. if *p1 and *p2 is lower than UCHARMAX, then
	   we assume they are ASCII */
	if (*p1 < UCHARMAX && *p2 < UCHARMAX)
		return(tolower(*p1) == tolower(*p2));

	if (*p1 < UCHARMAX)
		c1 = tolower(*p1);
	else
	{
		l = pg_mblen(p1);
		(void)pg_mb2wchar_with_len(p1, (pg_wchar *)&c1, l);
		c1 = tolower(c1);
	}
	if (*p2 < UCHARMAX)
		c2 = tolower(*p2);
	else
	{
		l = pg_mblen(p2);
		(void)pg_mb2wchar_with_len(p2, (pg_wchar *)&c2, l);
		c2 = tolower(c2);
	}
	return(c1 == c2);
}
#endif

#ifdef MULTIBYTE
#define CHAREQ(p1, p2) wchareq(p1, p2)
#define ICHAREQ(p1, p2) iwchareq(p1, p2)
#define NextChar(p, plen) {int __l = pg_mblen(p); (p) +=__l; (plen) -=__l;}
#else
#define CHAREQ(p1, p2) (*(p1) == *(p2))
#define ICHAREQ(p1, p2) (tolower(*(p1)) == tolower(*(p2)))
#define NextChar(p, plen) (p)++, (plen)--
#endif

static int
MatchText(unsigned char * t, int tlen, unsigned char * p, int plen, char *e)
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
		if ((e != NULL) && CHAREQ(p,e))
		{
			NextChar(p, plen);
			if ((plen <= 0) || !CHAREQ(t,p))
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
				if (CHAREQ(t,p) || (*p == '_')
					|| ((e != NULL) && CHAREQ(p,e)))
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
		else if ((*p != '_') && !CHAREQ(t,p))
		{
			/* Not the single-character wildcard and no explicit match?
			 * Then time to quit...
			 */
			return LIKE_FALSE;
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
MatchTextLower(unsigned char * t, int tlen, unsigned char * p, int plen, char *e)
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
		if ((e != NULL) && ICHAREQ(p,e))
		{
			NextChar(p, plen);
			if ((plen <= 0) || !ICHAREQ(t,p))
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
				if (ICHAREQ(t,p) || (*p == '_')
					|| ((e != NULL) && ICHAREQ(p,e)))
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
		else if ((*p != '_') && !ICHAREQ(t,p))
		{
			return LIKE_FALSE;
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
