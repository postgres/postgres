/*-------------------------------------------------------------------------
 *
 * regexp.c
 *	  regular expression handling code.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/regexp.c,v 1.32 2000/07/06 05:48:11 tgl Exp $
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
 *		is existent on a platform, I've prepended the string "pg95_" to
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
#endif	 /* DISABLE_XOPEN_NLS */

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

/* attempt to compile `re' as an re, then match it against text */
/* cflags - flag to regcomp indicates case sensitivity */
static bool
RE_compile_and_execute(text *text_re, char *text, int cflags)
{
	char	   *re;
	int			oldest;
	int			i;
	int			regcomp_result;

	/* Convert 'text' pattern to null-terminated string */
	re = DatumGetCString(DirectFunctionCall1(textout,
											 PointerGetDatum(text_re)));

	/* find a previously compiled regular expression */
	for (i = 0; i < rec; i++)
	{
		if (rev[i].cre_s)
		{
			if (strcmp(rev[i].cre_s, re) == 0 &&
				rev[i].cre_type == cflags)
			{
				rev[i].cre_lru = ++lru;
				pfree(re);
				return (pg95_regexec(&rev[i].cre_re,
									 text, 0,
									 (regmatch_t *) NULL, 0) == 0);
			}
		}
	}

	/* we didn't find it - make room in the cache for it */
	if (rec == MAX_CACHED_RES)
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
			rev[i].cre_lru = (rev[i].cre_lru - rev[oldest].cre_lru) / 2;
			if (rev[i].cre_lru > lru)
				lru = rev[i].cre_lru;
		}
		pg95_regfree(&rev[oldest].cre_re);

		/*
		 * use malloc/free for the cre_s field because the storage has to
		 * persist across transactions
		 */
		free(rev[oldest].cre_s);
		rev[oldest].cre_s = (char *) NULL;
	}

	/* compile the re */
	regcomp_result = pg95_regcomp(&rev[oldest].cre_re, re, cflags);
	if (regcomp_result == 0)
	{
		/*
		 * use malloc/free for the cre_s field because the storage has to
		 * persist across transactions
		 */
		rev[oldest].cre_s = strdup(re);
		rev[oldest].cre_lru = ++lru;
		rev[oldest].cre_type = cflags;
		pfree(re);
		/* agc - fixed an old typo here */
		return (pg95_regexec(&rev[oldest].cre_re, text, 0,
							 (regmatch_t *) NULL, 0) == 0);
	}
	else
	{
		char		errMsg[1000];

		/* re didn't compile */
		pg95_regerror(regcomp_result, &rev[oldest].cre_re, errMsg,
					  sizeof(errMsg));
		elog(ERROR, "regcomp failed with error %s", errMsg);
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
	StrNCpy(sterm, s, charlen + 1);

	result = RE_compile_and_execute(p, sterm, cflags);

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

	PG_RETURN_BOOL(! fixedlen_regexeq(NameStr(*n),
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

	PG_RETURN_BOOL(! fixedlen_regexeq(VARDATA(s),
									  p,
									  VARSIZE(s) - VARHDRSZ,
									  REG_EXTENDED));
}


/*
 *  routines that use the regexp stuff, but ignore the case.
 *	for this, we use the REG_ICASE flag to pg95_regcomp
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

	PG_RETURN_BOOL(! fixedlen_regexeq(VARDATA(s),
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

	PG_RETURN_BOOL(! fixedlen_regexeq(NameStr(*n),
									  p,
									  strlen(NameStr(*n)),
									  REG_ICASE | REG_EXTENDED));
}
