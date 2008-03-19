/*-------------------------------------------------------------------------
 *
 * regexp.c
 *	  Postgres' interface to the regular expression package.
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/regexp.c,v 1.66.2.2 2008/03/19 02:40:53 tgl Exp $
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
#include "utils/guc.h"


/* GUC-settable flavor parameter */
static int	regex_flavor = REG_ADVANCED;


/*
 * We cache precompiled regular expressions using a "self organizing list"
 * structure, in which recently-used items tend to be near the front.
 * Whenever we use an entry, it's moved up to the front of the list.
 * Over time, an item's average position corresponds to its frequency of use.
 *
 * When we first create an entry, it's inserted at the front of
 * the array, dropping the entry at the end of the array if necessary to
 * make room.  (This might seem to be weighting the new entry too heavily,
 * but if we insert new entries further back, we'll be unable to adjust to
 * a sudden shift in the query mix where we are presented with MAX_CACHED_RES
 * never-before-seen items used circularly.  We ought to be able to handle
 * that case, so we have to insert at the front.)
 *
 * Knuth mentions a variant strategy in which a used item is moved up just
 * one place in the list.  Although he says this uses fewer comparisons on
 * average, it seems not to adapt very well to the situation where you have
 * both some reusable patterns and a steady stream of non-reusable patterns.
 * A reusable pattern that isn't used at least as often as non-reusable
 * patterns are seen will "fail to keep up" and will drop off the end of the
 * cache.  With move-to-front, a reusable pattern is guaranteed to stay in
 * the cache as long as it's used at least once in every MAX_CACHED_RES uses.
 */

/* this is the maximum number of cached regular expressions */
#ifndef MAX_CACHED_RES
#define MAX_CACHED_RES	32
#endif

/* this structure describes one cached regular expression */
typedef struct cached_re_str
{
	text	   *cre_pat;		/* original RE (untoasted TEXT form) */
	int			cre_flags;		/* compile flags: extended,icase etc */
	regex_t		cre_re;			/* the compiled regular expression */
} cached_re_str;

static int	num_res = 0;		/* # of cached re's */
static cached_re_str re_array[MAX_CACHED_RES];	/* cached re's */


/*
 * RE_compile_and_cache - compile a RE, caching if possible
 *
 * Returns regex_t *
 *
 *	text_re --- the pattern, expressed as an *untoasted* TEXT object
 *	cflags --- compile options for the pattern
 *
 * Pattern is given in the database encoding.  We internally convert to
 * array of pg_wchar which is what Spencer's regex package wants.
 */
static regex_t *
RE_compile_and_cache(text *text_re, int cflags)
{
	int			text_re_len = VARSIZE(text_re);
	pg_wchar   *pattern;
	size_t		pattern_len;
	int			i;
	int			regcomp_result;
	cached_re_str re_temp;
	char		errMsg[100];

	/*
	 * Look for a match among previously compiled REs.	Since the data
	 * structure is self-organizing with most-used entries at the front, our
	 * search strategy can just be to scan from the front.
	 */
	for (i = 0; i < num_res; i++)
	{
		if (VARSIZE(re_array[i].cre_pat) == text_re_len &&
			memcmp(re_array[i].cre_pat, text_re, text_re_len) == 0 &&
			re_array[i].cre_flags == cflags)
		{
			/*
			 * Found a match; move it to front if not there already.
			 */
			if (i > 0)
			{
				re_temp = re_array[i];
				memmove(&re_array[1], &re_array[0], i * sizeof(cached_re_str));
				re_array[0] = re_temp;
			}

			return &re_array[0].cre_re;
		}
	}

	/*
	 * Couldn't find it, so try to compile the new RE.  To avoid leaking
	 * resources on failure, we build into the re_temp local.
	 */

	/* Convert pattern string to wide characters */
	pattern = (pg_wchar *) palloc((text_re_len - VARHDRSZ + 1) * sizeof(pg_wchar));
	pattern_len = pg_mb2wchar_with_len(VARDATA(text_re),
									   pattern,
									   text_re_len - VARHDRSZ);

	regcomp_result = pg_regcomp(&re_temp.cre_re,
								pattern,
								pattern_len,
								cflags);

	pfree(pattern);

	if (regcomp_result != REG_OKAY)
	{
		/* re didn't compile */
		pg_regerror(regcomp_result, &re_temp.cre_re, errMsg, sizeof(errMsg));
		/* XXX should we pg_regfree here? */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
				 errmsg("invalid regular expression: %s", errMsg)));
	}

	/*
	 * use malloc/free for the cre_pat field because the storage has to
	 * persist across transactions
	 */
	re_temp.cre_pat = malloc(text_re_len);
	if (re_temp.cre_pat == NULL)
	{
		pg_regfree(&re_temp.cre_re);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	}
	memcpy(re_temp.cre_pat, text_re, text_re_len);
	re_temp.cre_flags = cflags;

	/*
	 * Okay, we have a valid new item in re_temp; insert it into the storage
	 * array.  Discard last entry if needed.
	 */
	if (num_res >= MAX_CACHED_RES)
	{
		--num_res;
		Assert(num_res < MAX_CACHED_RES);
		pg_regfree(&re_array[num_res].cre_re);
		free(re_array[num_res].cre_pat);
	}

	if (num_res > 0)
		memmove(&re_array[1], &re_array[0], num_res * sizeof(cached_re_str));

	re_array[0] = re_temp;
	num_res++;

	return &re_array[0].cre_re;
}

/*
 * RE_execute - execute a RE
 *
 * Returns TRUE on match, FALSE on no match
 *
 *	re --- the compiled pattern as returned by RE_compile_and_cache
 *	dat --- the data to match against (need not be null-terminated)
 *	dat_len --- the length of the data string
 *	nmatch, pmatch	--- optional return area for match details
 *
 * Data is given in the database encoding.	We internally
 * convert to array of pg_wchar which is what Spencer's regex package wants.
 */
static bool
RE_execute(regex_t *re, char *dat, int dat_len,
		   int nmatch, regmatch_t *pmatch)
{
	pg_wchar   *data;
	size_t		data_len;
	int			regexec_result;
	char		errMsg[100];

	/* Convert data string to wide characters */
	data = (pg_wchar *) palloc((dat_len + 1) * sizeof(pg_wchar));
	data_len = pg_mb2wchar_with_len(dat, data, dat_len);

	/* Perform RE match and return result */
	regexec_result = pg_regexec(re,
								data,
								data_len,
								0,
								NULL,	/* no details */
								nmatch,
								pmatch,
								0);

	pfree(data);

	if (regexec_result != REG_OKAY && regexec_result != REG_NOMATCH)
	{
		/* re failed??? */
		pg_regerror(regexec_result, re, errMsg, sizeof(errMsg));
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
				 errmsg("regular expression failed: %s", errMsg)));
	}

	return (regexec_result == REG_OKAY);
}

/*
 * RE_compile_and_execute - compile and execute a RE
 *
 * Returns TRUE on match, FALSE on no match
 *
 *	text_re --- the pattern, expressed as an *untoasted* TEXT object
 *	dat --- the data to match against (need not be null-terminated)
 *	dat_len --- the length of the data string
 *	cflags --- compile options for the pattern
 *	nmatch, pmatch	--- optional return area for match details
 *
 * Both pattern and data are given in the database encoding.  We internally
 * convert to array of pg_wchar which is what Spencer's regex package wants.
 */
static bool
RE_compile_and_execute(text *text_re, char *dat, int dat_len,
					   int cflags, int nmatch, regmatch_t *pmatch)
{
	regex_t    *re;

	/* Compile RE */
	re = RE_compile_and_cache(text_re, cflags);

	/* Perform RE match and return result */
	return RE_execute(re, dat, dat_len, nmatch, pmatch);
}


/*
 * assign_regex_flavor - GUC hook to validate and set REGEX_FLAVOR
 */
const char *
assign_regex_flavor(const char *value,
					bool doit, GucSource source)
{
	if (pg_strcasecmp(value, "advanced") == 0)
	{
		if (doit)
			regex_flavor = REG_ADVANCED;
	}
	else if (pg_strcasecmp(value, "extended") == 0)
	{
		if (doit)
			regex_flavor = REG_EXTENDED;
	}
	else if (pg_strcasecmp(value, "basic") == 0)
	{
		if (doit)
			regex_flavor = REG_BASIC;
	}
	else
		return NULL;			/* fail */
	return value;				/* OK */
}


/*
 *	interface routines called by the function manager
 */

Datum
nameregexeq(PG_FUNCTION_ARGS)
{
	Name		n = PG_GETARG_NAME(0);
	text	   *p = PG_GETARG_TEXT_P(1);

	PG_RETURN_BOOL(RE_compile_and_execute(p,
										  NameStr(*n),
										  strlen(NameStr(*n)),
										  regex_flavor,
										  0, NULL));
}

Datum
nameregexne(PG_FUNCTION_ARGS)
{
	Name		n = PG_GETARG_NAME(0);
	text	   *p = PG_GETARG_TEXT_P(1);

	PG_RETURN_BOOL(!RE_compile_and_execute(p,
										   NameStr(*n),
										   strlen(NameStr(*n)),
										   regex_flavor,
										   0, NULL));
}

Datum
textregexeq(PG_FUNCTION_ARGS)
{
	text	   *s = PG_GETARG_TEXT_P(0);
	text	   *p = PG_GETARG_TEXT_P(1);

	PG_RETURN_BOOL(RE_compile_and_execute(p,
										  VARDATA(s),
										  VARSIZE(s) - VARHDRSZ,
										  regex_flavor,
										  0, NULL));
}

Datum
textregexne(PG_FUNCTION_ARGS)
{
	text	   *s = PG_GETARG_TEXT_P(0);
	text	   *p = PG_GETARG_TEXT_P(1);

	PG_RETURN_BOOL(!RE_compile_and_execute(p,
										   VARDATA(s),
										   VARSIZE(s) - VARHDRSZ,
										   regex_flavor,
										   0, NULL));
}


/*
 *	routines that use the regexp stuff, but ignore the case.
 *	for this, we use the REG_ICASE flag to pg_regcomp
 */


Datum
nameicregexeq(PG_FUNCTION_ARGS)
{
	Name		n = PG_GETARG_NAME(0);
	text	   *p = PG_GETARG_TEXT_P(1);

	PG_RETURN_BOOL(RE_compile_and_execute(p,
										  NameStr(*n),
										  strlen(NameStr(*n)),
										  regex_flavor | REG_ICASE,
										  0, NULL));
}

Datum
nameicregexne(PG_FUNCTION_ARGS)
{
	Name		n = PG_GETARG_NAME(0);
	text	   *p = PG_GETARG_TEXT_P(1);

	PG_RETURN_BOOL(!RE_compile_and_execute(p,
										   NameStr(*n),
										   strlen(NameStr(*n)),
										   regex_flavor | REG_ICASE,
										   0, NULL));
}

Datum
texticregexeq(PG_FUNCTION_ARGS)
{
	text	   *s = PG_GETARG_TEXT_P(0);
	text	   *p = PG_GETARG_TEXT_P(1);

	PG_RETURN_BOOL(RE_compile_and_execute(p,
										  VARDATA(s),
										  VARSIZE(s) - VARHDRSZ,
										  regex_flavor | REG_ICASE,
										  0, NULL));
}

Datum
texticregexne(PG_FUNCTION_ARGS)
{
	text	   *s = PG_GETARG_TEXT_P(0);
	text	   *p = PG_GETARG_TEXT_P(1);

	PG_RETURN_BOOL(!RE_compile_and_execute(p,
										   VARDATA(s),
										   VARSIZE(s) - VARHDRSZ,
										   regex_flavor | REG_ICASE,
										   0, NULL));
}


/*
 * textregexsubstr()
 *		Return a substring matched by a regular expression.
 */
Datum
textregexsubstr(PG_FUNCTION_ARGS)
{
	text	   *s = PG_GETARG_TEXT_P(0);
	text	   *p = PG_GETARG_TEXT_P(1);
	regex_t    *re;
	regmatch_t	pmatch[2];
	int			so,
				eo;

	/* Compile RE */
	re = RE_compile_and_cache(p, regex_flavor);

	/*
	 * We pass two regmatch_t structs to get info about the overall match and
	 * the match for the first parenthesized subexpression (if any). If there
	 * is a parenthesized subexpression, we return what it matched; else
	 * return what the whole regexp matched.
	 */
	if (!RE_execute(re,
					VARDATA(s), VARSIZE(s) - VARHDRSZ,
					2, pmatch))
		PG_RETURN_NULL();		/* definitely no match */

	if (re->re_nsub > 0)
	{
		/* has parenthesized subexpressions, use the first one */
		so = pmatch[1].rm_so;
		eo = pmatch[1].rm_eo;
	}
	else
	{
		/* no parenthesized subexpression, use whole match */
		so = pmatch[0].rm_so;
		eo = pmatch[0].rm_eo;
	}

	/*
	 * It is possible to have a match to the whole pattern but no match
	 * for a subexpression; for example 'foo(bar)?' is considered to match
	 * 'foo' but there is no subexpression match.  So this extra test for
	 * match failure is not redundant.
	 */
	if (so < 0 || eo < 0)
		PG_RETURN_NULL();

	return DirectFunctionCall3(text_substr,
							   PointerGetDatum(s),
							   Int32GetDatum(so + 1),
							   Int32GetDatum(eo - so));
}

/*
 * textregexreplace_noopt()
 *		Return a string matched by a regular expression, with replacement.
 *
 * This version doesn't have an option argument: we default to case
 * sensitive match, replace the first instance only.
 */
Datum
textregexreplace_noopt(PG_FUNCTION_ARGS)
{
	text	   *s = PG_GETARG_TEXT_P(0);
	text	   *p = PG_GETARG_TEXT_P(1);
	text	   *r = PG_GETARG_TEXT_P(2);
	regex_t    *re;

	re = RE_compile_and_cache(p, regex_flavor);

	PG_RETURN_TEXT_P(replace_text_regexp(s, (void *) re, r, false));
}

/*
 * textregexreplace()
 *		Return a string matched by a regular expression, with replacement.
 */
Datum
textregexreplace(PG_FUNCTION_ARGS)
{
	text	   *s = PG_GETARG_TEXT_P(0);
	text	   *p = PG_GETARG_TEXT_P(1);
	text	   *r = PG_GETARG_TEXT_P(2);
	text	   *opt = PG_GETARG_TEXT_P(3);
	char	   *opt_p = VARDATA(opt);
	int			opt_len = (VARSIZE(opt) - VARHDRSZ);
	int			i;
	bool		glob = false;
	bool		ignorecase = false;
	regex_t    *re;

	/* parse options */
	for (i = 0; i < opt_len; i++)
	{
		switch (opt_p[i])
		{
			case 'i':
				ignorecase = true;
				break;
			case 'g':
				glob = true;
				break;
			default:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid option of regexp_replace: %c",
								opt_p[i])));
				break;
		}
	}

	if (ignorecase)
		re = RE_compile_and_cache(p, regex_flavor | REG_ICASE);
	else
		re = RE_compile_and_cache(p, regex_flavor);

	PG_RETURN_TEXT_P(replace_text_regexp(s, (void *) re, r, glob));
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
	char	   *p,
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
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_ESCAPE_SEQUENCE),
					 errmsg("invalid escape string"),
				  errhint("Escape string must be empty or one character.")));
	}

	/*----------
	 * We surround the transformed input string with
	 *			***:^(?: ... )$
	 * which is bizarre enough to require some explanation.  "***:" is a
	 * director prefix to force the regex to be treated as an ARE regardless
	 * of the current regex_flavor setting.  We need "^" and "$" to force
	 * the pattern to match the entire input string as per SQL99 spec.	The
	 * "(?:" and ")" are a non-capturing set of parens; we have to have
	 * parens in case the string contains "|", else the "^" and "$" will
	 * be bound into the first and last alternatives which is not what we
	 * want, and the parens must be non capturing because we don't want them
	 * to count when selecting output for SUBSTRING.
	 *----------
	 */

	/*
	 * We need room for the prefix/postfix plus as many as 2 output bytes per
	 * input byte
	 */
	result = (text *) palloc(VARHDRSZ + 10 + 2 * plen);
	r = VARDATA(result);

	*r++ = '*';
	*r++ = '*';
	*r++ = '*';
	*r++ = ':';
	*r++ = '^';
	*r++ = '(';
	*r++ = '?';
	*r++ = ':';

	while (plen > 0)
	{
		char		pchar = *p;

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
			*r++ = '.';
		else if (pchar == '\\' || pchar == '.' || pchar == '?' ||
				 pchar == '{')
		{
			*r++ = '\\';
			*r++ = pchar;
		}
		else
			*r++ = pchar;
		p++, plen--;
	}

	*r++ = ')';
	*r++ = '$';

	VARATT_SIZEP(result) = r - ((char *) result);

	PG_RETURN_TEXT_P(result);
}

/*
 * report whether regex_flavor is currently BASIC
 */
bool
regex_flavor_is_basic(void)
{
	return (regex_flavor == REG_BASIC);
}
