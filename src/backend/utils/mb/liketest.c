#include "postgres_fe.h"

#include <ctype.h>

#include "mb/pg_wchar.h"

#define LIKE_FALSE 0
#define LIKE_TRUE 1
#define LIKE_ABORT 2

#define PG_CHAR unsigned char
#define UCHARMAX 0xff
/*----------------------------------------------------------------*/

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
MatchText(PG_CHAR * t, int tlen, PG_CHAR * p, int plen, char *e)
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
MatchTextLower(PG_CHAR * t, int tlen, PG_CHAR * p, int plen, char *e)
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

main()
{
	unsigned char *t = "¿ÍZ01²¼";
	unsigned char *p = "_Z%";
	int tlen, plen;
	tlen = strlen(t);
	plen = strlen(p);
	printf("%d\n",MatchTextLower(t,tlen,p,plen,"\\"));
}
