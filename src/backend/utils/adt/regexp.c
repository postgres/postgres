/*-------------------------------------------------------------------------
 *
 * regexp.c
 *	  regular expression handling code.
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/regexp.c,v 1.43 2002/09/22 17:27:23 tgl Exp $
 *
 *		Alistair Crooks added the code for the regex caching
 *		agc - cached the regular expressions used - there's a good chance
 *		that we'll get a hit, so this saves a compile step for every
 *		attempted match. I haven't actually measured the speed improvement,
 *		but it `looks' a lot quicker visually when watching regression
 *		test output.
 *
 *		agc - incorporated Keith Bostic's Berkeley regex code into
 *		the tree for all ports. To distinguish this regex code from any that
 *		is existent on a platform, I've prepended the string "pg_" to
 *		the functions regcomp, regerror, regexec and regfree.
 *		Fixed a bug that was originally a typo by me, where `i' was used
 *		instead of `oldest' when compiling regular expressions - benign
 *		results mostly, although occasionally it bit you...
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "regex/regex.h"
#include "utils/builtins.h"

#if defined(DISABLE_XOPEN_NLS)
#undef _XOPEN_SOURCE
#endif   /* DISABLE_XOPEN_NLS */

/* this is the number of cached regular expressions held. */
#ifndef MAX_CACHED_RES
#define MAX_CACHED_RES	32
#endif

/* this structure describes a cached regular expression */
struct cached_re_str
{
	char	   *cre_s;			/* pattern as null-terminated string */
	int			cre_type;		/* compiled-type: extended,icase etc */
	regex_t		cre_re;			/* the compiled regular expression */
	unsigned long cre_lru;		/* lru tag */
};

static int	rec = 0;			/* # of cached re's */
static struct cached_re_str rev[MAX_CACHED_RES];		/* cached re's */
static unsigned long lru;		/* system lru tag */
static int	pg_lastrec = 0;

/* attempt to compile `re' as an re, then match it against text */
/* cflags - flag to regcomp indicates case sensitivity */
static bool
RE_compile_and_execute(text *text_re, char *text, int cflags,
					   int nmatch, regmatch_t *pmatch)
{
	char	   *re;
	int			oldest;
	int			i;
	int			regcomp_result;

	/* Convert 'text' pattern to null-terminated string */
	re = DatumGetCString(DirectFunctionCall1(textout,
											 PointerGetDatum(text_re)));

	/*
	 * Find a previously compiled regular expression. Run the cache as a
	 * ring buffer, starting the search from the previous match if any.
	 */
	i = pg_lastrec;
	while (i < rec)
	{
		if (rev[i].cre_s != NULL)
		{
			if (strcmp(rev[i].cre_s, re) == 0 &&
				rev[i].cre_type == cflags)
			{
				pg_lastrec = i;
				rev[i].cre_lru = ++lru;
				pfree(re);
				return (pg_regexec(&rev[i].cre_re,
								   text, nmatch,
								   pmatch, 0) == 0);
			}
		}
		i++;

		/*
		 * If we were not at the first slot to start, then think about
		 * wrapping if necessary.
		 */
		if (pg_lastrec != 0)
		{
			if (i >= rec)
				i = 0;
			else if (i == pg_lastrec)
				break;
		}
	}

	/* we didn't find it - make room in the cache for it */
	if (rec >= MAX_CACHED_RES)
	{
		/* cache is full - find the oldest entry */
		for (oldest = 0, i = 1; i < rec; i++)
		{
			if (rev[i].cre_lru < rev[oldest].cre_lru)
				oldest = i;
		}
	}
	else
		oldest = rec++;

	/* if there was an old re, then de-allocate the space it used */
	if (rev[oldest].cre_s != (char *) NULL)
	{
		for (lru = i = 0; i < rec; i++)
		{
			/* downweight all of the other cached entries */
			rev[i].cre_lru = (rev[i].cre_lru - rev[oldest].cre_lru) / 2;
			if (rev[i].cre_lru > lru)
				lru = rev[i].cre_lru;
		}
		pg_regfree(&rev[oldest].cre_re);

		/*
		 * use malloc/free for the cre_s field because the storage has to
		 * persist across transactions
		 */
		free(rev[oldest].cre_s);
		rev[oldest].cre_s = (char *) NULL;
	}

	/* compile the re */
	regcomp_result = pg_regcomp(&rev[oldest].cre_re, re, cflags);
	if (regcomp_result == 0)
	{
		pg_lastrec = oldest;

		/*
		 * use malloc/free for the cre_s field because the storage has to
		 * persist across transactions
		 */
		rev[oldest].cre_s = strdup(re);
		rev[oldest].cre_lru = ++lru;
		rev[oldest].cre_type = cflags;
		pfree(re);
		/* agc - fixed an old typo here */
		return (pg_regexec(&rev[oldest].cre_re, text,
						   nmatch, pmatch, 0) == 0);
	}
	else
	{
		char		errMsg[1000];

		/* re didn't compile */
		pg_regerror(regcomp_result, &rev[oldest].cre_re, errMsg,
					sizeof(errMsg));
		elog(ERROR, "Invalid regular expression: %s", errMsg);
	}

	/* not reached */
	return false;
}


/*
   fixedlen_regexeq:

   a generic fixed length regexp routine
		 s		- the string to match against (not necessarily null-terminated)
		 p		- the pattern (as a text*)
		 charlen   - the length of the string
*/
static bool
fixedlen_regexeq(char *s, text *p, int charlen, int cflags)
{
	char	   *sterm;
	bool		result;

	/* be sure sterm is null-terminated */
	sterm = (char *) palloc(charlen + 1);
	memcpy(sterm, s, charlen);
	sterm[charlen] = '\0';

	result = RE_compile_and_execute(p, sterm, cflags, 0, NULL);

	pfree(sterm);

	return result;
}


/*
 *	interface routines called by the function manager
 */

Datum
nameregexeq(PG_FUNCTION_ARGS)
{
	Name		n = PG_GETARG_NAME(0);
	text	   *p = PG_GETARG_TEXT_P(1);

	PG_RETURN_BOOL(fixedlen_regexeq(NameStr(*n),
									p,
									strlen(NameStr(*n)),
									REG_EXTENDED));
}

Datum
nameregexne(PG_FUNCTION_ARGS)
{
	Name		n = PG_GETARG_NAME(0);
	text	   *p = PG_GETARG_TEXT_P(1);

	PG_RETURN_BOOL(!fixedlen_regexeq(NameStr(*n),
									 p,
									 strlen(NameStr(*n)),
									 REG_EXTENDED));
}

Datum
textregexeq(PG_FUNCTION_ARGS)
{
	text	   *s = PG_GETARG_TEXT_P(0);
	text	   *p = PG_GETARG_TEXT_P(1);

	PG_RETURN_BOOL(fixedlen_regexeq(VARDATA(s),
									p,
									VARSIZE(s) - VARHDRSZ,
									REG_EXTENDED));
}

Datum
textregexne(PG_FUNCTION_ARGS)
{
	text	   *s = PG_GETARG_TEXT_P(0);
	text	   *p = PG_GETARG_TEXT_P(1);

	PG_RETURN_BOOL(!fixedlen_regexeq(VARDATA(s),
									 p,
									 VARSIZE(s) - VARHDRSZ,
									 REG_EXTENDED));
}


/*
 *	routines that use the regexp stuff, but ignore the case.
 *	for this, we use the REG_ICASE flag to pg_regcomp
 */


Datum
texticregexeq(PG_FUNCTION_ARGS)
{
	text	   *s = PG_GETARG_TEXT_P(0);
	text	   *p = PG_GETARG_TEXT_P(1);

	PG_RETURN_BOOL(fixedlen_regexeq(VARDATA(s),
									p,
									VARSIZE(s) - VARHDRSZ,
									REG_ICASE | REG_EXTENDED));
}

Datum
texticregexne(PG_FUNCTION_ARGS)
{
	text	   *s = PG_GETARG_TEXT_P(0);
	text	   *p = PG_GETARG_TEXT_P(1);

	PG_RETURN_BOOL(!fixedlen_regexeq(VARDATA(s),
									 p,
									 VARSIZE(s) - VARHDRSZ,
									 REG_ICASE | REG_EXTENDED));
}

Datum
nameicregexeq(PG_FUNCTION_ARGS)
{
	Name		n = PG_GETARG_NAME(0);
	text	   *p = PG_GETARG_TEXT_P(1);

	PG_RETURN_BOOL(fixedlen_regexeq(NameStr(*n),
									p,
									strlen(NameStr(*n)),
									REG_ICASE | REG_EXTENDED));
}

Datum
nameicregexne(PG_FUNCTION_ARGS)
{
	Name		n = PG_GETARG_NAME(0);
	text	   *p = PG_GETARG_TEXT_P(1);

	PG_RETURN_BOOL(!fixedlen_regexeq(NameStr(*n),
									 p,
									 strlen(NameStr(*n)),
									 REG_ICASE | REG_EXTENDED));
}


/* textregexsubstr()
 * Return a substring matched by a regular expression.
 */
Datum
textregexsubstr(PG_FUNCTION_ARGS)
{
	text	   *s = PG_GETARG_TEXT_P(0);
	text	   *p = PG_GETARG_TEXT_P(1);
	char	   *sterm;
	int			len;
	bool		match;
	regmatch_t	pmatch[2];

	/* be sure sterm is null-terminated */
	len = VARSIZE(s) - VARHDRSZ;
	sterm = (char *) palloc(len + 1);
	memcpy(sterm, VARDATA(s), len);
	sterm[len] = '\0';

	/*
	 * We pass two regmatch_t structs to get info about the overall match
	 * and the match for the first parenthesized subexpression (if any).
	 * If there is a parenthesized subexpression, we return what it matched;
	 * else return what the whole regexp matched.
	 */
	match = RE_compile_and_execute(p, sterm, REG_EXTENDED, 2, pmatch);

	pfree(sterm);

	/* match? then return the substring matching the pattern */
	if (match)
	{
		int		so,
				eo;

		so = pmatch[1].rm_so;
		eo = pmatch[1].rm_eo;
		if (so < 0 || eo < 0)
		{
			/* no parenthesized subexpression */
			so = pmatch[0].rm_so;
			eo = pmatch[0].rm_eo;
		}

		return (DirectFunctionCall3(text_substr,
									PointerGetDatum(s),
									Int32GetDatum(so + 1),
									Int32GetDatum(eo - so)));
	}

	PG_RETURN_NULL();
}

/* similar_escape()
 * Convert a SQL99 regexp pattern to POSIX style, so it can be used by
 * our regexp engine.
 */
Datum
similar_escape(PG_FUNCTION_ARGS)
{
	text	   *pat_text;
	text	   *esc_text;
	text	   *result;
	unsigned char *p,
			   *e,
			   *r;
	int			plen,
				elen;
	bool		afterescape = false;
	int			nquotes = 0;

	/* This function is not strict, so must test explicitly */
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();
	pat_text = PG_GETARG_TEXT_P(0);
	p = VARDATA(pat_text);
	plen = (VARSIZE(pat_text) - VARHDRSZ);
	if (PG_ARGISNULL(1))
	{
		/* No ESCAPE clause provided; default to backslash as escape */
		e = "\\";
		elen = 1;
	}
	else
	{
		esc_text = PG_GETARG_TEXT_P(1);
		e = VARDATA(esc_text);
		elen = (VARSIZE(esc_text) - VARHDRSZ);
		if (elen == 0)
			e = NULL;			/* no escape character */
		else if (elen != 1)
			elog(ERROR, "ESCAPE string must be empty or one character");
	}

	/* We need room for ^, $, and up to 2 output bytes per input byte */
	result = (text *) palloc(VARHDRSZ + 2 + 2 * plen);
	r = VARDATA(result);

	*r++ = '^';

	while (plen > 0)
	{
		unsigned char pchar = *p;

		if (afterescape)
		{
			if (pchar == '"')	/* for SUBSTRING patterns */
				*r++ = ((nquotes++ % 2) == 0) ? '(' : ')';
			else
			{
				*r++ = '\\';
				*r++ = pchar;
			}
			afterescape = false;
		}
		else if (e && pchar == *e)
		{
			/* SQL99 escape character; do not send to output */
			afterescape = true;
		}
		else if (pchar == '%')
		{
			*r++ = '.';
			*r++ = '*';
		}
		else if (pchar == '_')
		{
			*r++ = '.';
		}
		else if (pchar == '\\' || pchar == '.' || pchar == '?' ||
				 pchar == '{')
		{
			*r++ = '\\';
			*r++ = pchar;
		}
		else
		{
			*r++ = pchar;
		}
		p++, plen--;
	}

	*r++ = '$';
	
	VARATT_SIZEP(result) = r - ((unsigned char *) result);

	PG_RETURN_TEXT_P(result);
}
