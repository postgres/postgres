/*-------------------------------------------------------------------------
 *
 * like.c
 *	  like expression handling code.
 *
 *	 NOTES
 *		A big hack of the regexp.c code!! Contributed by
 *		Keith Parks <emkxp01@mtcc.demon.co.uk> (7/95).
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	$Header: /cvsroot/pgsql/src/backend/utils/adt/like.c,v 1.44 2001/01/24 19:43:14 momjian Exp $
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


static int MatchText(unsigned char * t, int tlen,
					 unsigned char * p, int plen);
static int MatchTextIC(unsigned char * t, int tlen,
					   unsigned char * p, int plen);


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
 * Support routine for MatchTextIC. Compares given multibyte streams
 * as wide characters ignoring case.
 * If they match, returns 1 otherwise returns 0.
 *--------------------
 */
#define CHARMAX 0x80

static int iwchareq(unsigned char *p1, unsigned char *p2)
{
	int c1[2], c2[2];
	int l;

	/* short cut. if *p1 and *p2 is lower than CHARMAX, then
	   we could assume they are ASCII */
	if (*p1 < CHARMAX && *p2 < CHARMAX)
		return(tolower(*p1) == tolower(*p2));

	/* if one of them is an ASCII while the other is not, then
	   they must be different characters
	*/
	else if (*p1 < CHARMAX || *p2 < CHARMAX)
		return(0);

	/* ok, p1 and p2 are both > CHARMAX, then they must be multi-byte
	   characters
	*/
	l = pg_mblen(p1);
	(void)pg_mb2wchar_with_len(p1, (pg_wchar *)c1, l);
	c1[0] = tolower(c1[0]);
	l = pg_mblen(p2);
	(void)pg_mb2wchar_with_len(p2, (pg_wchar *)c2, l);
	c2[0] = tolower(c2[0]);
	return(c1[0] == c2[0]);
}

#endif

#ifdef MULTIBYTE
#define CHAREQ(p1, p2) wchareq(p1, p2)
#define ICHAREQ(p1, p2) iwchareq(p1, p2)
#define NextChar(p, plen) \
	do { int __l = pg_mblen(p); (p) +=__l; (plen) -=__l; } while (0)
#define CopyAdvChar(dst, src, srclen) \
	do { int __l = pg_mblen(src); \
		 (srclen) -= __l; \
		 while (__l-- > 0) \
			 *(dst)++ = *(src)++; \
	   } while (0)
#else
#define CHAREQ(p1, p2) (*(p1) == *(p2))
#define ICHAREQ(p1, p2) (tolower(*(p1)) == tolower(*(p2)))
#define NextChar(p, plen) ((p)++, (plen)--)
#define CopyAdvChar(dst, src, srclen) (*(dst)++ = *(src)++, (srclen)--)
#endif


/*
 *	interface routines called by the function manager
 */

Datum
namelike(PG_FUNCTION_ARGS)
{
	Name		str = PG_GETARG_NAME(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
	bool		result;
	unsigned char   *s, *p;
	int			slen, plen;

	s = NameStr(*str);
	slen = strlen(s);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);

	result = (MatchText(s, slen, p, plen) == LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
namenlike(PG_FUNCTION_ARGS)
{
	Name		str = PG_GETARG_NAME(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
	bool		result;
	unsigned char   *s, *p;
	int			slen, plen;

	s = NameStr(*str);
	slen = strlen(s);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);

	result = (MatchText(s, slen, p, plen) != LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
textlike(PG_FUNCTION_ARGS)
{
	text	   *str = PG_GETARG_TEXT_P(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
	bool		result;
	unsigned char   *s, *p;
	int			slen, plen;

	s = VARDATA(str);
	slen = (VARSIZE(str)-VARHDRSZ);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);

	result = (MatchText(s, slen, p, plen) == LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
textnlike(PG_FUNCTION_ARGS)
{
	text	   *str = PG_GETARG_TEXT_P(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
	bool		result;
	unsigned char   *s, *p;
	int			slen, plen;

	s = VARDATA(str);
	slen = (VARSIZE(str)-VARHDRSZ);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);

	result = (MatchText(s, slen, p, plen) != LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

/*
 * Case-insensitive versions
 */

Datum
nameiclike(PG_FUNCTION_ARGS)
{
	Name		str = PG_GETARG_NAME(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
	bool		result;
	unsigned char   *s, *p;
	int			slen, plen;

	s = NameStr(*str);
	slen = strlen(s);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);

	result = (MatchTextIC(s, slen, p, plen) == LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
nameicnlike(PG_FUNCTION_ARGS)
{
	Name		str = PG_GETARG_NAME(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
	bool		result;
	unsigned char   *s, *p;
	int			slen, plen;

	s = NameStr(*str);
	slen = strlen(s);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);

	result = (MatchTextIC(s, slen, p, plen) != LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
texticlike(PG_FUNCTION_ARGS)
{
	text	   *str = PG_GETARG_TEXT_P(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
	bool		result;
	unsigned char   *s, *p;
	int			slen, plen;

	s = VARDATA(str);
	slen = (VARSIZE(str)-VARHDRSZ);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);

	result = (MatchTextIC(s, slen, p, plen) == LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

Datum
texticnlike(PG_FUNCTION_ARGS)
{
	text	   *str = PG_GETARG_TEXT_P(0);
	text	   *pat = PG_GETARG_TEXT_P(1);
	bool		result;
	unsigned char   *s, *p;
	int			slen, plen;

	s = VARDATA(str);
	slen = (VARSIZE(str)-VARHDRSZ);
	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);

	result = (MatchTextIC(s, slen, p, plen) != LIKE_TRUE);

	PG_RETURN_BOOL(result);
}

/*
 * like_escape() --- given a pattern and an ESCAPE string,
 * convert the pattern to use Postgres' standard backslash escape convention.
 */
Datum
like_escape(PG_FUNCTION_ARGS)
{
	text	   *pat = PG_GETARG_TEXT_P(0);
	text	   *esc = PG_GETARG_TEXT_P(1);
	text	   *result;
	unsigned char   *p, *e, *r;
	int			plen, elen;
	bool		afterescape;

	p = VARDATA(pat);
	plen = (VARSIZE(pat)-VARHDRSZ);
	e = VARDATA(esc);
	elen = (VARSIZE(esc)-VARHDRSZ);

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
			elog(ERROR, "ESCAPE string must be empty or one character");
		e = VARDATA(esc);
		/*
		 * If specified escape is '\', just copy the pattern as-is.
		 */
		if (*e == '\\')
		{
			memcpy(result, pat, VARSIZE(pat));
			PG_RETURN_TEXT_P(result);
		}
		/*
		 * Otherwise, convert occurrences of the specified escape character
		 * to '\', and double occurrences of '\' --- unless they immediately
		 * follow an escape character!
		 */
		afterescape = false;
		while (plen > 0)
		{
			if (CHAREQ(p,e) && !afterescape)
			{
				*r++ = '\\';
				NextChar(p, plen);
				afterescape = true;
			}
			else if (*p == '\\')
			{
				*r++ = '\\';
				if (! afterescape)
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

	VARATT_SIZEP(result) = r - ((unsigned char *) result);

	PG_RETURN_TEXT_P(result);
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
MatchText(unsigned char * t, int tlen, unsigned char * p, int plen)
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
				if (CHAREQ(t,p) || (*p == '\\')  || (*p == '_'))
				{
					int matched = MatchText(t, tlen, p, plen);

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

/*
 * Same as above, but ignore case
 */
static int
MatchTextIC(unsigned char * t, int tlen, unsigned char * p, int plen)
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
				if (ICHAREQ(t,p) || (*p == '\\')  || (*p == '_'))
				{
					int matched = MatchTextIC(t, tlen, p, plen);

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
} /* MatchTextIC() */
