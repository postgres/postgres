/*-------------------------------------------------------------------------
 *
 * regexp.c
 *	  Postgres' interface to the regular expression package.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/regexp.c,v 1.49.4.3 2006/04/13 18:01:53 tgl Exp $
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
#include "mb/pg_wchar.h"
#include "utils/builtins.h"


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
 * RE_compile_and_execute - compile and execute a RE, caching if possible
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
RE_compile_and_execute(text *text_re, unsigned char *dat, int dat_len,
					   int cflags, int nmatch, regmatch_t *pmatch)
{
	int			text_re_len = VARSIZE(text_re);
	pg_wchar   *data;
	size_t		data_len;
	pg_wchar   *pattern;
	size_t		pattern_len;
	int			i;
	int			regcomp_result;
	int			regexec_result;
	cached_re_str re_temp;
	char		errMsg[100];

	/* Convert data string to wide characters */
	data = (pg_wchar *) palloc((dat_len + 1) * sizeof(pg_wchar));
	data_len = pg_mb2wchar_with_len(dat, data, dat_len);

	/*
	 * Look for a match among previously compiled REs.	Since the data
	 * structure is self-organizing with most-used entries at the front,
	 * our search strategy can just be to scan from the front.
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

			/* Perform RE match and return result */
			regexec_result = pg_regexec(&re_array[0].cre_re,
										data,
										data_len,
										NULL,	/* no details */
										nmatch,
										pmatch,
										0);

			pfree(data);

			if (regexec_result != REG_OKAY && regexec_result != REG_NOMATCH)
			{
				/* re failed??? */
				pg_regerror(regexec_result, &re_array[0].cre_re,
							errMsg, sizeof(errMsg));
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
						 errmsg("regular expression failed: %s", errMsg)));
			}

			return (regexec_result == REG_OKAY);
		}
	}

	/*
	 * Couldn't find it, so try to compile the new RE.  To avoid leaking
	 * resources on failure, we build into the re_temp local.
	 */

	/* Convert pattern string to wide characters */
	pattern = (pg_wchar *) palloc((text_re_len - VARHDRSZ + 1) * sizeof(pg_wchar));
	pattern_len = pg_mb2wchar_with_len((unsigned char *) VARDATA(text_re),
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
	 * Okay, we have a valid new item in re_temp; insert it into the
	 * storage array.  Discard last entry if needed.
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

	/* Perform RE match and return result */
	regexec_result = pg_regexec(&re_array[0].cre_re,
								data,
								data_len,
								NULL,	/* no details */
								nmatch,
								pmatch,
								0);

	pfree(data);

	if (regexec_result != REG_OKAY && regexec_result != REG_NOMATCH)
	{
		/* re failed??? */
		pg_regerror(regexec_result, &re_array[0].cre_re,
					errMsg, sizeof(errMsg));
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
				 errmsg("regular expression failed: %s", errMsg)));
	}

	return (regexec_result == REG_OKAY);
}


/*
 * assign_regex_flavor - GUC hook to validate and set REGEX_FLAVOR
 */
const char *
assign_regex_flavor(const char *value,
					bool doit, bool interactive)
{
	if (strcasecmp(value, "advanced") == 0)
	{
		if (doit)
			regex_flavor = REG_ADVANCED;
	}
	else if (strcasecmp(value, "extended") == 0)
	{
		if (doit)
			regex_flavor = REG_EXTENDED;
	}
	else if (strcasecmp(value, "basic") == 0)
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
										  (unsigned char *) NameStr(*n),
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
										   (unsigned char *) NameStr(*n),
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
										  (unsigned char *) VARDATA(s),
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
										   (unsigned char *) VARDATA(s),
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
										  (unsigned char *) NameStr(*n),
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
										   (unsigned char *) NameStr(*n),
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
										  (unsigned char *) VARDATA(s),
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
										   (unsigned char *) VARDATA(s),
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
	bool		match;
	regmatch_t	pmatch[2];

	/*
	 * We pass two regmatch_t structs to get info about the overall match
	 * and the match for the first parenthesized subexpression (if any).
	 * If there is a parenthesized subexpression, we return what it
	 * matched; else return what the whole regexp matched.
	 */
	match = RE_compile_and_execute(p,
								   (unsigned char *) VARDATA(s),
								   VARSIZE(s) - VARHDRSZ,
								   regex_flavor,
								   2, pmatch);

	/* match? then return the substring matching the pattern */
	if (match)
	{
		int			so,
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
	 * the pattern to match the entire input string as per SQL99 spec.  The
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

	VARATT_SIZEP(result) = r - ((unsigned char *) result);

	PG_RETURN_TEXT_P(result);
}
