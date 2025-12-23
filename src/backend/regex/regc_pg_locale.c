/*-------------------------------------------------------------------------
 *
 * regc_pg_locale.c
 *	  ctype functions adapted to work on pg_wchar (a/k/a chr),
 *	  and functions to cache the results of wholesale ctype probing.
 *
 * This file is #included by regcomp.c; it's not meant to compile standalone.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/regex/regc_pg_locale.c
 *
 *-------------------------------------------------------------------------
 */

#include "catalog/pg_collation.h"
#include "common/unicode_case.h"
#include "common/unicode_category.h"
#include "utils/pg_locale.h"
#include "utils/pg_locale_c.h"

static pg_locale_t pg_regex_locale;


/*
 * pg_set_regex_collation: set collation for these functions to obey
 *
 * This is called when beginning compilation or execution of a regexp.
 * Since there's no need for reentrancy of regexp operations, it's okay
 * to store the results in static variables.
 */
void
pg_set_regex_collation(Oid collation)
{
	pg_locale_t locale = 0;

	if (!OidIsValid(collation))
	{
		/*
		 * This typically means that the parser could not resolve a conflict
		 * of implicit collations, so report it that way.
		 */
		ereport(ERROR,
				(errcode(ERRCODE_INDETERMINATE_COLLATION),
				 errmsg("could not determine which collation to use for regular expression"),
				 errhint("Use the COLLATE clause to set the collation explicitly.")));
	}

	locale = pg_newlocale_from_collation(collation);

	if (!locale->deterministic)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("nondeterministic collations are not supported for regular expressions")));

	pg_regex_locale = locale;
}

/*
 * The following functions overlap with those defined in pg_locale.c. XXX:
 * consider refactor.
 */

static int
regc_wc_isdigit(pg_wchar c)
{
	if (pg_regex_locale->ctype_is_c)
		return (c <= (pg_wchar) 127 &&
				(pg_char_properties[c] & PG_ISDIGIT));
	else
		return pg_regex_locale->ctype->wc_isdigit(c, pg_regex_locale);
}

static int
regc_wc_isalpha(pg_wchar c)
{
	if (pg_regex_locale->ctype_is_c)
		return (c <= (pg_wchar) 127 &&
				(pg_char_properties[c] & PG_ISALPHA));
	else
		return pg_regex_locale->ctype->wc_isalpha(c, pg_regex_locale);
}

static int
regc_wc_isalnum(pg_wchar c)
{
	if (pg_regex_locale->ctype_is_c)
		return (c <= (pg_wchar) 127 &&
				(pg_char_properties[c] & PG_ISALNUM));
	else
		return pg_regex_locale->ctype->wc_isalnum(c, pg_regex_locale);
}

static int
regc_wc_isword(pg_wchar c)
{
	/* We define word characters as alnum class plus underscore */
	if (c == CHR('_'))
		return 1;
	return regc_wc_isalnum(c);
}

static int
regc_wc_isupper(pg_wchar c)
{
	if (pg_regex_locale->ctype_is_c)
		return (c <= (pg_wchar) 127 &&
				(pg_char_properties[c] & PG_ISUPPER));
	else
		return pg_regex_locale->ctype->wc_isupper(c, pg_regex_locale);
}

static int
regc_wc_islower(pg_wchar c)
{
	if (pg_regex_locale->ctype_is_c)
		return (c <= (pg_wchar) 127 &&
				(pg_char_properties[c] & PG_ISLOWER));
	else
		return pg_regex_locale->ctype->wc_islower(c, pg_regex_locale);
}

static int
regc_wc_isgraph(pg_wchar c)
{
	if (pg_regex_locale->ctype_is_c)
		return (c <= (pg_wchar) 127 &&
				(pg_char_properties[c] & PG_ISGRAPH));
	else
		return pg_regex_locale->ctype->wc_isgraph(c, pg_regex_locale);
}

static int
regc_wc_isprint(pg_wchar c)
{
	if (pg_regex_locale->ctype_is_c)
		return (c <= (pg_wchar) 127 &&
				(pg_char_properties[c] & PG_ISPRINT));
	else
		return pg_regex_locale->ctype->wc_isprint(c, pg_regex_locale);
}

static int
regc_wc_ispunct(pg_wchar c)
{
	if (pg_regex_locale->ctype_is_c)
		return (c <= (pg_wchar) 127 &&
				(pg_char_properties[c] & PG_ISPUNCT));
	else
		return pg_regex_locale->ctype->wc_ispunct(c, pg_regex_locale);
}

static int
regc_wc_isspace(pg_wchar c)
{
	if (pg_regex_locale->ctype_is_c)
		return (c <= (pg_wchar) 127 &&
				(pg_char_properties[c] & PG_ISSPACE));
	else
		return pg_regex_locale->ctype->wc_isspace(c, pg_regex_locale);
}

static pg_wchar
regc_wc_toupper(pg_wchar c)
{
	if (pg_regex_locale->ctype_is_c)
	{
		if (c <= (pg_wchar) 127)
			return pg_ascii_toupper((unsigned char) c);
		return c;
	}
	else
		return pg_regex_locale->ctype->wc_toupper(c, pg_regex_locale);
}

static pg_wchar
regc_wc_tolower(pg_wchar c)
{
	if (pg_regex_locale->ctype_is_c)
	{
		if (c <= (pg_wchar) 127)
			return pg_ascii_tolower((unsigned char) c);
		return c;
	}
	else
		return pg_regex_locale->ctype->wc_tolower(c, pg_regex_locale);
}


/*
 * These functions cache the results of probing libc's ctype behavior for
 * all character codes of interest in a given encoding/collation.  The
 * result is provided as a "struct cvec", but notice that the representation
 * is a touch different from a cvec created by regc_cvec.c: we allocate the
 * chrs[] and ranges[] arrays separately from the struct so that we can
 * realloc them larger at need.  This is okay since the cvecs made here
 * should never be freed by freecvec().
 *
 * We use malloc not palloc since we mustn't lose control on out-of-memory;
 * the main regex code expects us to return a failure indication instead.
 */

typedef int (*regc_wc_probefunc) (pg_wchar c);

typedef struct pg_ctype_cache
{
	regc_wc_probefunc probefunc;	/* regc_wc_isalpha or a sibling */
	pg_locale_t locale;			/* locale this entry is for */
	struct cvec cv;				/* cache entry contents */
	struct pg_ctype_cache *next;	/* chain link */
} pg_ctype_cache;

static pg_ctype_cache *pg_ctype_cache_list = NULL;

/*
 * Add a chr or range to pcc->cv; return false if run out of memory
 */
static bool
store_match(pg_ctype_cache *pcc, pg_wchar chr1, int nchrs)
{
	chr		   *newchrs;

	if (nchrs > 1)
	{
		if (pcc->cv.nranges >= pcc->cv.rangespace)
		{
			pcc->cv.rangespace *= 2;
			newchrs = (chr *) realloc(pcc->cv.ranges,
									  pcc->cv.rangespace * sizeof(chr) * 2);
			if (newchrs == NULL)
				return false;
			pcc->cv.ranges = newchrs;
		}
		pcc->cv.ranges[pcc->cv.nranges * 2] = chr1;
		pcc->cv.ranges[pcc->cv.nranges * 2 + 1] = chr1 + nchrs - 1;
		pcc->cv.nranges++;
	}
	else
	{
		assert(nchrs == 1);
		if (pcc->cv.nchrs >= pcc->cv.chrspace)
		{
			pcc->cv.chrspace *= 2;
			newchrs = (chr *) realloc(pcc->cv.chrs,
									  pcc->cv.chrspace * sizeof(chr));
			if (newchrs == NULL)
				return false;
			pcc->cv.chrs = newchrs;
		}
		pcc->cv.chrs[pcc->cv.nchrs++] = chr1;
	}
	return true;
}

/*
 * Given a probe function (e.g., regc_wc_isalpha) get a struct cvec for all
 * chrs satisfying the probe function.  The active collation is the one
 * previously set by pg_set_regex_collation.  Return NULL if out of memory.
 *
 * Note that the result must not be freed or modified by caller.
 */
static struct cvec *
regc_ctype_get_cache(regc_wc_probefunc probefunc, int cclasscode)
{
	pg_ctype_cache *pcc;
	pg_wchar	max_chr;
	pg_wchar	cur_chr;
	int			nmatches;
	chr		   *newchrs;

	/*
	 * Do we already have the answer cached?
	 */
	for (pcc = pg_ctype_cache_list; pcc != NULL; pcc = pcc->next)
	{
		if (pcc->probefunc == probefunc &&
			pcc->locale == pg_regex_locale)
			return &pcc->cv;
	}

	/*
	 * Nope, so initialize some workspace ...
	 */
	pcc = (pg_ctype_cache *) malloc(sizeof(pg_ctype_cache));
	if (pcc == NULL)
		return NULL;
	pcc->probefunc = probefunc;
	pcc->locale = pg_regex_locale;
	pcc->cv.nchrs = 0;
	pcc->cv.chrspace = 128;
	pcc->cv.chrs = (chr *) malloc(pcc->cv.chrspace * sizeof(chr));
	pcc->cv.nranges = 0;
	pcc->cv.rangespace = 64;
	pcc->cv.ranges = (chr *) malloc(pcc->cv.rangespace * sizeof(chr) * 2);
	if (pcc->cv.chrs == NULL || pcc->cv.ranges == NULL)
		goto out_of_memory;
	pcc->cv.cclasscode = cclasscode;

	/*
	 * Decide how many character codes we ought to look through.  In general
	 * we don't go past MAX_SIMPLE_CHR; chr codes above that are handled at
	 * runtime using the "high colormap" mechanism.  However, in C locale
	 * there's no need to go further than 127, and if we only have a 1-byte
	 * <ctype.h> API there's no need to go further than that can handle.
	 *
	 * If it's not MAX_SIMPLE_CHR that's constraining the search, mark the
	 * output cvec as not having any locale-dependent behavior, since there
	 * will be no need to do any run-time locale checks.  (The #if's here
	 * would always be true for production values of MAX_SIMPLE_CHR, but it's
	 * useful to allow it to be small for testing purposes.)
	 */
	if (pg_regex_locale->ctype_is_c)
	{
#if MAX_SIMPLE_CHR >= 127
		max_chr = (pg_wchar) 127;
		pcc->cv.cclasscode = -1;
#else
		max_chr = (pg_wchar) MAX_SIMPLE_CHR;
#endif
	}
	else if (GetDatabaseEncoding() == PG_UTF8)
	{
		max_chr = (pg_wchar) MAX_SIMPLE_CHR;
	}
	else
	{
#if MAX_SIMPLE_CHR >= UCHAR_MAX
		max_chr = (pg_wchar) UCHAR_MAX;
		pcc->cv.cclasscode = -1;
#else
		max_chr = (pg_wchar) MAX_SIMPLE_CHR;
#endif
	}

	/*
	 * And scan 'em ...
	 */
	nmatches = 0;				/* number of consecutive matches */

	for (cur_chr = 0; cur_chr <= max_chr; cur_chr++)
	{
		if ((*probefunc) (cur_chr))
			nmatches++;
		else if (nmatches > 0)
		{
			if (!store_match(pcc, cur_chr - nmatches, nmatches))
				goto out_of_memory;
			nmatches = 0;
		}
	}

	if (nmatches > 0)
		if (!store_match(pcc, cur_chr - nmatches, nmatches))
			goto out_of_memory;

	/*
	 * We might have allocated more memory than needed, if so free it
	 */
	if (pcc->cv.nchrs == 0)
	{
		free(pcc->cv.chrs);
		pcc->cv.chrs = NULL;
		pcc->cv.chrspace = 0;
	}
	else if (pcc->cv.nchrs < pcc->cv.chrspace)
	{
		newchrs = (chr *) realloc(pcc->cv.chrs,
								  pcc->cv.nchrs * sizeof(chr));
		if (newchrs == NULL)
			goto out_of_memory;
		pcc->cv.chrs = newchrs;
		pcc->cv.chrspace = pcc->cv.nchrs;
	}
	if (pcc->cv.nranges == 0)
	{
		free(pcc->cv.ranges);
		pcc->cv.ranges = NULL;
		pcc->cv.rangespace = 0;
	}
	else if (pcc->cv.nranges < pcc->cv.rangespace)
	{
		newchrs = (chr *) realloc(pcc->cv.ranges,
								  pcc->cv.nranges * sizeof(chr) * 2);
		if (newchrs == NULL)
			goto out_of_memory;
		pcc->cv.ranges = newchrs;
		pcc->cv.rangespace = pcc->cv.nranges;
	}

	/*
	 * Success, link it into cache chain
	 */
	pcc->next = pg_ctype_cache_list;
	pg_ctype_cache_list = pcc;

	return &pcc->cv;

	/*
	 * Failure, clean up
	 */
out_of_memory:
	free(pcc->cv.chrs);
	free(pcc->cv.ranges);
	free(pcc);

	return NULL;
}
