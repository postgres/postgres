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
 *	$Header: /cvsroot/pgsql/src/backend/utils/adt/like.c,v 1.40 2000/08/09 14:13:03 thomas Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include <ctype.h>
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
	bool		result;
	Name		str = PG_GETARG_NAME(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
	pg_wchar   *s, *p;
	int			slen, plen;

#ifdef MULTIBYTE
	pg_wchar   *ss, *pp;

	slen = strlen(NameStr(*str));
	s = (pg_wchar *) palloc((slen+1) * sizeof(pg_wchar));
	(void) pg_mb2wchar_with_len((unsigned char *) NameStr(*str), s, slen);
	for (ss = s, slen = 0; *ss != 0; ss++) slen++;

	plen = (VARSIZE(pat)-VARHDRSZ);
	p = (pg_wchar *) palloc((plen+1) * sizeof(pg_wchar));
	(void) pg_mb2wchar_with_len((unsigned char *) VARDATA(pat), p, plen);
	for (pp = p, plen = 0; *pp != 0; pp++) plen++;
#else
	s = NameStr(*str);
	slen = strlen(s);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);
#endif

	result = (MatchText(s, slen, p, plen, "\\") == LIKE_TRUE);

#ifdef MULTIBYTE
	pfree(s);
	pfree(p);
#endif

	PG_RETURN_BOOL(result);
}

Datum
namenlike(PG_FUNCTION_ARGS)
{
	bool		result;
	Name		str = PG_GETARG_NAME(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
	pg_wchar   *s, *p;
	int			slen, plen;

#ifdef MULTIBYTE
	pg_wchar   *ss, *pp;

	slen = strlen(NameStr(*str));
	s = (pg_wchar *) palloc((slen+1) * sizeof(pg_wchar));
	(void) pg_mb2wchar_with_len((unsigned char *) NameStr(*str), s, slen);
	for (ss = s, slen = 0; *ss != 0; ss++) slen++;

	plen = (VARSIZE(pat)-VARHDRSZ);
	p = (pg_wchar *) palloc((plen+1) * sizeof(pg_wchar));
	(void) pg_mb2wchar_with_len((unsigned char *) VARDATA(pat), p, plen);
	for (pp = p, plen = 0; *pp != 0; pp++) plen++;
#else
	s = NameStr(*str);
	slen = strlen(s);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);
#endif

	result = (MatchText(s, slen, p, plen, "\\") != LIKE_TRUE);

#ifdef MULTIBYTE
	pfree(s);
	pfree(p);
#endif

	PG_RETURN_BOOL(result);
}

Datum
namelike_escape(PG_FUNCTION_ARGS)
{
	bool		result;
	Name		str = PG_GETARG_NAME(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
	text	   *esc = PG_GETARG_TEXT_P(2);
	pg_wchar   *s, *p;
	int			slen, plen;
	char	   *e;

#ifdef MULTIBYTE
	pg_wchar   *ss, *pp;

	slen = strlen(NameStr(*str));
	s = (pg_wchar *) palloc((slen+1) * sizeof(pg_wchar));
	(void) pg_mb2wchar_with_len((unsigned char *) NameStr(*str), s, slen);
	for (ss = s, slen = 0; *ss != 0; ss++) slen++;

	plen = (VARSIZE(pat)-VARHDRSZ);
	p = (pg_wchar *) palloc((plen+1) * sizeof(pg_wchar));
	(void) pg_mb2wchar_with_len((unsigned char *) VARDATA(pat), p, plen);
	for (pp = p, plen = 0; *pp != 0; pp++) plen++;
#else
	s = NameStr(*str);
	slen = strlen(s);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);
#endif

	e = ((VARSIZE(esc)-VARHDRSZ) > 0? VARDATA(esc): NULL);

	result = (MatchText(s, slen, p, plen, e) == LIKE_TRUE);

#ifdef MULTIBYTE
	pfree(s);
	pfree(p);
#endif

	PG_RETURN_BOOL(result);
}

Datum
namenlike_escape(PG_FUNCTION_ARGS)
{
	bool		result;
	Name		str = PG_GETARG_NAME(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
	text	   *esc = PG_GETARG_TEXT_P(2);
	pg_wchar   *s, *p;
	int			slen, plen;
	char	   *e;

#ifdef MULTIBYTE
	pg_wchar   *ss, *pp;

	slen = strlen(NameStr(*str));
	s = (pg_wchar *) palloc((slen+1) * sizeof(pg_wchar));
	(void) pg_mb2wchar_with_len((unsigned char *) NameStr(*str), s, slen);
	for (ss = s, slen = 0; *ss != 0; ss++) slen++;

	plen = (VARSIZE(pat)-VARHDRSZ);
	p = (pg_wchar *) palloc((plen+1) * sizeof(pg_wchar));
	(void) pg_mb2wchar_with_len((unsigned char *) VARDATA(pat), p, plen);
	for (pp = p, plen = 0; *pp != 0; pp++) plen++;
#else
	s = NameStr(*str);
	slen = strlen(s);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);
#endif

	e = ((VARSIZE(esc)-VARHDRSZ) > 0? VARDATA(esc): NULL);

	result = (MatchText(s, slen, p, plen, e) != LIKE_TRUE);

#ifdef MULTIBYTE
	pfree(s);
	pfree(p);
#endif

	PG_RETURN_BOOL(result);
}

Datum
textlike(PG_FUNCTION_ARGS)
{
	bool		result;
	text	   *str = PG_GETARG_TEXT_P(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
	pg_wchar   *s, *p;
	int			slen, plen;

#ifdef MULTIBYTE
	pg_wchar   *ss, *pp;

	slen = (VARSIZE(str)-VARHDRSZ);
	s = (pg_wchar *) palloc((slen+1) * sizeof(pg_wchar));
	(void) pg_mb2wchar_with_len((unsigned char *) VARDATA(str), s, slen);
	for (ss = s, slen = 0; *ss != 0; ss++) slen++;

	plen = (VARSIZE(pat)-VARHDRSZ);
	p = (pg_wchar *) palloc((plen+1) * sizeof(pg_wchar));
	(void) pg_mb2wchar_with_len((unsigned char *) VARDATA(pat), p, plen);
	for (pp = p, plen = 0; *pp != 0; pp++) plen++;
#else
	s = VARDATA(str);
	slen = (VARSIZE(str)-VARHDRSZ);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);
#endif

	result = (MatchText(s, slen, p, plen, NULL) == LIKE_TRUE);

#ifdef MULTIBYTE
	pfree(s);
	pfree(p);
#endif

	PG_RETURN_BOOL(result);
}

Datum
textnlike(PG_FUNCTION_ARGS)
{
	bool		result;
	text	   *str = PG_GETARG_TEXT_P(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
	pg_wchar   *s, *p;
	int			slen, plen;

#ifdef MULTIBYTE
	pg_wchar   *ss, *pp;

	slen = (VARSIZE(str)-VARHDRSZ);
	s = (pg_wchar *) palloc((slen+1) * sizeof(pg_wchar));
	(void) pg_mb2wchar_with_len((unsigned char *) VARDATA(str), s, slen);
	for (ss = s, slen = 0; *ss != 0; ss++) slen++;

	plen = (VARSIZE(pat)-VARHDRSZ);
	p = (pg_wchar *) palloc((plen+1) * sizeof(pg_wchar));
	(void) pg_mb2wchar_with_len((unsigned char *) VARDATA(pat), p, plen);
	for (pp = p, plen = 0; *pp != 0; pp++) plen++;
#else
	s = VARDATA(str);
	slen = (VARSIZE(str)-VARHDRSZ);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);
#endif

	result = (MatchText(s, slen, p, plen, "\\") != LIKE_TRUE);

#ifdef MULTIBYTE
	pfree(s);
	pfree(p);
#endif

	PG_RETURN_BOOL(result);
}

Datum
textlike_escape(PG_FUNCTION_ARGS)
{
	bool		result;
	text	   *str = PG_GETARG_TEXT_P(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
	text	   *esc = PG_GETARG_TEXT_P(2);
	pg_wchar   *s, *p;
	int			slen, plen;
	char	   *e;

#ifdef MULTIBYTE
	pg_wchar   *ss, *pp;

	slen = (VARSIZE(str)-VARHDRSZ);
	s = (pg_wchar *) palloc((slen+1) * sizeof(pg_wchar));
	(void) pg_mb2wchar_with_len((unsigned char *) VARDATA(str), s, slen);
	for (ss = s, slen = 0; *ss != 0; ss++) slen++;

	plen = (VARSIZE(pat)-VARHDRSZ);
	p = (pg_wchar *) palloc((plen+1) * sizeof(pg_wchar));
	(void) pg_mb2wchar_with_len((unsigned char *) VARDATA(pat), p, plen);
	for (pp = p, plen = 0; *pp != 0; pp++) plen++;
#else
	s = VARDATA(str);
	slen = (VARSIZE(str)-VARHDRSZ);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);
#endif

	e = ((VARSIZE(esc)-VARHDRSZ) > 0? VARDATA(esc): NULL);

	result = (MatchText(s, slen, p, plen, e) == LIKE_TRUE);

#ifdef MULTIBYTE
	pfree(s);
	pfree(p);
#endif

	PG_RETURN_BOOL(result);
}

Datum
textnlike_escape(PG_FUNCTION_ARGS)
{
	bool		result;
	text	   *str = PG_GETARG_TEXT_P(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
	text	   *esc = PG_GETARG_TEXT_P(2);
	pg_wchar   *s, *p;
	int			slen, plen;
	char	   *e;

#ifdef MULTIBYTE
	pg_wchar   *ss, *pp;

	slen = (VARSIZE(str)-VARHDRSZ);
	s = (pg_wchar *) palloc((slen+1) * sizeof(pg_wchar));
	(void) pg_mb2wchar_with_len((unsigned char *) VARDATA(str), s, slen);
	for (ss = s, slen = 0; *ss != 0; ss++) slen++;

	plen = (VARSIZE(pat)-VARHDRSZ);
	p = (pg_wchar *) palloc((plen+1) * sizeof(pg_wchar));
	(void) pg_mb2wchar_with_len((unsigned char *) VARDATA(pat), p, plen);
	for (pp = p, plen = 0; *pp != 0; pp++) plen++;
#else
	s = VARDATA(str);
	slen = (VARSIZE(str)-VARHDRSZ);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);
#endif

	e = ((VARSIZE(esc)-VARHDRSZ) > 0? VARDATA(esc): NULL);

	result = (MatchText(s, slen, p, plen, e) != LIKE_TRUE);

#ifdef MULTIBYTE
	pfree(s);
	pfree(p);
#endif

	PG_RETURN_BOOL(result);
}

/*
 * Case-insensitive versions
 */

Datum
inamelike(PG_FUNCTION_ARGS)
{
	bool		result;
#ifndef MULTIBYTE
	Name		str = PG_GETARG_NAME(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
#endif
	pg_wchar   *s, *p;
	int			slen, plen;

#ifdef MULTIBYTE
	elog(ERROR, "Case-insensitive multi-byte comparisons are not yet supported");
#else
	s = NameStr(*str);
	slen = strlen(s);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);
#endif

	result = (MatchTextLower(s, slen, p, plen, "\\") == LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
inamenlike(PG_FUNCTION_ARGS)
{
	bool		result;
#ifndef MULTIBYTE
	Name		str = PG_GETARG_NAME(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
#endif
	pg_wchar   *s, *p;
	int			slen, plen;

#ifdef MULTIBYTE
	elog(ERROR, "Case-insensitive multi-byte comparisons are not yet supported");
#else
	s = NameStr(*str);
	slen = strlen(s);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);
#endif

	result = (MatchTextLower(s, slen, p, plen, "\\") != LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
inamelike_escape(PG_FUNCTION_ARGS)
{
	bool		result;
#ifndef MULTIBYTE
	Name		str = PG_GETARG_NAME(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
#endif
	text	   *esc = PG_GETARG_TEXT_P(2);
	pg_wchar   *s, *p;
	int			slen, plen;
	char	   *e;

#ifdef MULTIBYTE
	elog(ERROR, "Case-insensitive multi-byte comparisons are not yet supported");
#else
	s = NameStr(*str);
	slen = strlen(s);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);
#endif

	e = ((VARSIZE(esc)-VARHDRSZ) > 0? VARDATA(esc): NULL);

	result = (MatchTextLower(s, slen, p, plen, e) == LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
inamenlike_escape(PG_FUNCTION_ARGS)
{
	bool		result;
#ifndef MULTIBYTE
	Name		str = PG_GETARG_NAME(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
#endif
	text	   *esc = PG_GETARG_TEXT_P(2);
	pg_wchar   *s, *p;
	int			slen, plen;
	char	   *e;

#ifdef MULTIBYTE
	elog(ERROR, "Case-insensitive multi-byte comparisons are not yet supported");
#else
	s = NameStr(*str);
	slen = strlen(s);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);
#endif

	e = ((VARSIZE(esc)-VARHDRSZ) > 0? VARDATA(esc): NULL);

	result = (MatchTextLower(s, slen, p, plen, e) != LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
itextlike(PG_FUNCTION_ARGS)
{
	bool		result;
#ifndef MULTIBYTE
	text	   *str = PG_GETARG_TEXT_P(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
#endif
	pg_wchar   *s, *p;
	int			slen, plen;

#ifdef MULTIBYTE
	elog(ERROR, "Case-insensitive multi-byte comparisons are not yet supported");
#else
	s = VARDATA(str);
	slen = (VARSIZE(str)-VARHDRSZ);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);
#endif

	result = (MatchTextLower(s, slen, p, plen, "\\") == LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
itextnlike(PG_FUNCTION_ARGS)
{
	bool		result;
#ifndef MULTIBYTE
	text	   *str = PG_GETARG_TEXT_P(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
#endif
	pg_wchar   *s, *p;
	int			slen, plen;

#ifdef MULTIBYTE
	elog(ERROR, "Case-insensitive multi-byte comparisons are not yet supported");
#else
	s = VARDATA(str);
	slen = (VARSIZE(str)-VARHDRSZ);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);
#endif

	result = (MatchTextLower(s, slen, p, plen, "\\") != LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
itextlike_escape(PG_FUNCTION_ARGS)
{
	bool		result;
#ifndef MULTIBYTE
	text	   *str = PG_GETARG_TEXT_P(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
#endif
	text	   *esc = PG_GETARG_TEXT_P(2);
	pg_wchar   *s, *p;
	int			slen, plen;
	char	   *e;

#ifdef MULTIBYTE
	elog(ERROR, "Case-insensitive multi-byte comparisons are not yet supported");
#else
	s = VARDATA(str);
	slen = (VARSIZE(str)-VARHDRSZ);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);
#endif

	e = ((VARSIZE(esc)-VARHDRSZ) > 0? VARDATA(esc): NULL);

	result = (MatchTextLower(s, slen, p, plen, e) == LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
itextnlike_escape(PG_FUNCTION_ARGS)
{
	bool		result;
#ifndef MULTIBYTE
	text	   *str = PG_GETARG_TEXT_P(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
#endif
	text	   *esc = PG_GETARG_TEXT_P(2);
	pg_wchar   *s, *p;
	int			slen, plen;
	char	   *e;

#ifdef MULTIBYTE
	elog(ERROR, "Case-insensitive multi-byte comparisons are not yet supported");
#else
	s = VARDATA(str);
	slen = (VARSIZE(str)-VARHDRSZ);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);
#endif

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
				if ((*t == *p) || (*p == '_')
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
		else if ((*p != '_') && (*t != *p))
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
				if ((tolower(*t) == tolower(*p)) || (*p == '_')
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
		else if ((*p != '_') && (tolower(*t) != tolower(*p)))
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
