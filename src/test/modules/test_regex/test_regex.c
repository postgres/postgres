/*--------------------------------------------------------------------------
 *
 * test_regex.c
 *		Test harness for the regular expression package.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/test/modules/test_regex/test_regex.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "funcapi.h"
#include "regex/regex.h"
#include "utils/array.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC;


/* all the options of interest for regex functions */
typedef struct test_re_flags
{
	int			cflags;			/* compile flags for Spencer's regex code */
	int			eflags;			/* execute flags for Spencer's regex code */
	long		info;			/* expected re_info bits */
	bool		glob;			/* do it globally (for each occurrence) */
	bool		indices;		/* report indices not actual strings */
	bool		partial;		/* expect partial match */
} test_re_flags;

/* cross-call state for test_regex() */
typedef struct test_regex_ctx
{
	test_re_flags re_flags;		/* flags */
	rm_detail_t details;		/* "details" from execution */
	text	   *orig_str;		/* data string in original TEXT form */
	int			nmatches;		/* number of places where pattern matched */
	int			npatterns;		/* number of capturing subpatterns */
	/* We store start char index and end+1 char index for each match */
	/* so the number of entries in match_locs is nmatches * npatterns * 2 */
	int		   *match_locs;		/* 0-based character indexes */
	int			next_match;		/* 0-based index of next match to process */
	/* workspace for build_test_match_result() */
	Datum	   *elems;			/* has npatterns+1 elements */
	bool	   *nulls;			/* has npatterns+1 elements */
	pg_wchar   *wide_str;		/* wide-char version of original string */
	char	   *conv_buf;		/* conversion buffer, if needed */
	int			conv_bufsiz;	/* size thereof */
} test_regex_ctx;

/* Local functions */
static void test_re_compile(text *text_re, int cflags, Oid collation,
							regex_t *result_re);
static void parse_test_flags(test_re_flags *flags, text *opts);
static test_regex_ctx *setup_test_matches(text *orig_str,
										  regex_t *cpattern,
										  test_re_flags *re_flags,
										  Oid collation,
										  bool use_subpatterns);
static ArrayType *build_test_info_result(regex_t *cpattern,
										 test_re_flags *flags);
static ArrayType *build_test_match_result(test_regex_ctx *matchctx);


/*
 * test_regex(pattern text, string text, flags text) returns setof text[]
 *
 * This is largely based on regexp.c's regexp_matches, with additions
 * for debugging purposes.
 */
PG_FUNCTION_INFO_V1(test_regex);

Datum
test_regex(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	test_regex_ctx *matchctx;
	ArrayType  *result_ary;

	if (SRF_IS_FIRSTCALL())
	{
		text	   *pattern = PG_GETARG_TEXT_PP(0);
		text	   *flags = PG_GETARG_TEXT_PP(2);
		Oid			collation = PG_GET_COLLATION();
		test_re_flags re_flags;
		regex_t		cpattern;
		MemoryContext oldcontext;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* Determine options */
		parse_test_flags(&re_flags, flags);

		/* set up the compiled pattern */
		test_re_compile(pattern, re_flags.cflags, collation, &cpattern);

		/* be sure to copy the input string into the multi-call ctx */
		matchctx = setup_test_matches(PG_GETARG_TEXT_P_COPY(1), &cpattern,
									  &re_flags,
									  collation,
									  true);

		/* Pre-create workspace that build_test_match_result needs */
		matchctx->elems = (Datum *) palloc(sizeof(Datum) *
										   (matchctx->npatterns + 1));
		matchctx->nulls = (bool *) palloc(sizeof(bool) *
										  (matchctx->npatterns + 1));

		MemoryContextSwitchTo(oldcontext);
		funcctx->user_fctx = matchctx;

		/*
		 * Return the first result row, which is info equivalent to Tcl's
		 * "regexp -about" output
		 */
		result_ary = build_test_info_result(&cpattern, &re_flags);

		pg_regfree(&cpattern);

		SRF_RETURN_NEXT(funcctx, PointerGetDatum(result_ary));
	}
	else
	{
		/* Each subsequent row describes one match */
		funcctx = SRF_PERCALL_SETUP();
		matchctx = (test_regex_ctx *) funcctx->user_fctx;

		if (matchctx->next_match < matchctx->nmatches)
		{
			result_ary = build_test_match_result(matchctx);
			matchctx->next_match++;
			SRF_RETURN_NEXT(funcctx, PointerGetDatum(result_ary));
		}
	}

	SRF_RETURN_DONE(funcctx);
}


/*
 * test_re_compile - compile a RE
 *
 *	text_re --- the pattern, expressed as a TEXT object
 *	cflags --- compile options for the pattern
 *	collation --- collation to use for LC_CTYPE-dependent behavior
 *  result_re --- output, compiled RE is stored here
 *
 * Pattern is given in the database encoding.  We internally convert to
 * an array of pg_wchar, which is what Spencer's regex package wants.
 *
 * Caller must eventually pg_regfree the resulting RE to avoid memory leaks.
 */
static void
test_re_compile(text *text_re, int cflags, Oid collation,
				regex_t *result_re)
{
	int			text_re_len = VARSIZE_ANY_EXHDR(text_re);
	char	   *text_re_val = VARDATA_ANY(text_re);
	pg_wchar   *pattern;
	int			pattern_len;
	int			regcomp_result;
	char		errMsg[100];

	/* Convert pattern string to wide characters */
	pattern = (pg_wchar *) palloc((text_re_len + 1) * sizeof(pg_wchar));
	pattern_len = pg_mb2wchar_with_len(text_re_val,
									   pattern,
									   text_re_len);

	regcomp_result = pg_regcomp(result_re,
								pattern,
								pattern_len,
								cflags,
								collation);

	pfree(pattern);

	if (regcomp_result != REG_OKAY)
	{
		/* re didn't compile (no need for pg_regfree, if so) */
		pg_regerror(regcomp_result, result_re, errMsg, sizeof(errMsg));
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
				 errmsg("invalid regular expression: %s", errMsg)));
	}
}

/*
 * test_re_execute - execute a RE on pg_wchar data
 *
 * Returns true on match, false on no match
 * Arguments are as for pg_regexec
 */
static bool
test_re_execute(regex_t *re, pg_wchar *data, int data_len,
				int start_search,
				rm_detail_t *details,
				int nmatch, regmatch_t *pmatch,
				int eflags)
{
	int			regexec_result;
	char		errMsg[100];

	/* Initialize match locations in case engine doesn't */
	details->rm_extend.rm_so = -1;
	details->rm_extend.rm_eo = -1;
	for (int i = 0; i < nmatch; i++)
	{
		pmatch[i].rm_so = -1;
		pmatch[i].rm_eo = -1;
	}

	/* Perform RE match and return result */
	regexec_result = pg_regexec(re,
								data,
								data_len,
								start_search,
								details,
								nmatch,
								pmatch,
								eflags);

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
 * parse_test_flags - parse the flags argument
 *
 *	flags --- output argument, filled with desired options
 *	opts --- TEXT object, or NULL for defaults
 */
static void
parse_test_flags(test_re_flags *flags, text *opts)
{
	/* these defaults must match Tcl's */
	int			cflags = REG_ADVANCED;
	int			eflags = 0;
	long		info = 0;

	flags->glob = false;
	flags->indices = false;
	flags->partial = false;

	if (opts)
	{
		char	   *opt_p = VARDATA_ANY(opts);
		int			opt_len = VARSIZE_ANY_EXHDR(opts);
		int			i;

		for (i = 0; i < opt_len; i++)
		{
			switch (opt_p[i])
			{
				case '-':
					/* allowed, no-op */
					break;
				case '!':
					flags->partial = true;
					break;
				case '*':
					/* test requires Unicode --- ignored here */
					break;
				case '0':
					flags->indices = true;
					break;

					/* These flags correspond to user-exposed RE options: */
				case 'g':		/* global match */
					flags->glob = true;
					break;
				case 'i':		/* case insensitive */
					cflags |= REG_ICASE;
					break;
				case 'n':		/* \n affects ^ $ . [^ */
					cflags |= REG_NEWLINE;
					break;
				case 'p':		/* ~Perl, \n affects . [^ */
					cflags |= REG_NLSTOP;
					cflags &= ~REG_NLANCH;
					break;
				case 'w':		/* weird, \n affects ^ $ only */
					cflags &= ~REG_NLSTOP;
					cflags |= REG_NLANCH;
					break;
				case 'x':		/* expanded syntax */
					cflags |= REG_EXPANDED;
					break;

					/* These flags correspond to Tcl's -xflags options: */
				case 'a':
					cflags |= REG_ADVF;
					break;
				case 'b':
					cflags &= ~REG_ADVANCED;
					break;
				case 'c':

					/*
					 * Tcl calls this TCL_REG_CANMATCH, but it's really
					 * REG_EXPECT.  In this implementation we must also set
					 * the partial and indices flags, so that
					 * setup_test_matches and build_test_match_result will
					 * emit the desired data.  (They'll emit more fields than
					 * Tcl would, but that's fine.)
					 */
					cflags |= REG_EXPECT;
					flags->partial = true;
					flags->indices = true;
					break;
				case 'e':
					cflags &= ~REG_ADVANCED;
					cflags |= REG_EXTENDED;
					break;
				case 'q':
					cflags &= ~REG_ADVANCED;
					cflags |= REG_QUOTE;
					break;
				case 'o':		/* o for opaque */
					cflags |= REG_NOSUB;
					break;
				case 's':		/* s for start */
					cflags |= REG_BOSONLY;
					break;
				case '+':
					cflags |= REG_FAKE;
					break;
				case ',':
					cflags |= REG_PROGRESS;
					break;
				case '.':
					cflags |= REG_DUMP;
					break;
				case ':':
					eflags |= REG_MTRACE;
					break;
				case ';':
					eflags |= REG_FTRACE;
					break;
				case '^':
					eflags |= REG_NOTBOL;
					break;
				case '$':
					eflags |= REG_NOTEOL;
					break;
				case 't':
					cflags |= REG_EXPECT;
					break;
				case '%':
					eflags |= REG_SMALL;
					break;

					/* These flags define expected info bits: */
				case 'A':
					info |= REG_UBSALNUM;
					break;
				case 'B':
					info |= REG_UBRACES;
					break;
				case 'E':
					info |= REG_UBBS;
					break;
				case 'H':
					info |= REG_ULOOKAROUND;
					break;
				case 'I':
					info |= REG_UIMPOSSIBLE;
					break;
				case 'L':
					info |= REG_ULOCALE;
					break;
				case 'M':
					info |= REG_UUNPORT;
					break;
				case 'N':
					info |= REG_UEMPTYMATCH;
					break;
				case 'P':
					info |= REG_UNONPOSIX;
					break;
				case 'Q':
					info |= REG_UBOUNDS;
					break;
				case 'R':
					info |= REG_UBACKREF;
					break;
				case 'S':
					info |= REG_UUNSPEC;
					break;
				case 'T':
					info |= REG_USHORTEST;
					break;
				case 'U':
					info |= REG_UPBOTCH;
					break;

				default:
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("invalid regular expression test option: \"%.*s\"",
									pg_mblen(opt_p + i), opt_p + i)));
					break;
			}
		}
	}
	flags->cflags = cflags;
	flags->eflags = eflags;
	flags->info = info;
}

/*
 * setup_test_matches --- do the initial matching
 *
 * To simplify memory management, we do all the matching in one swoop.
 * The returned test_regex_ctx contains the locations of all the substrings
 * matching the pattern.
 */
static test_regex_ctx *
setup_test_matches(text *orig_str,
				   regex_t *cpattern, test_re_flags *re_flags,
				   Oid collation,
				   bool use_subpatterns)
{
	test_regex_ctx *matchctx = palloc0(sizeof(test_regex_ctx));
	int			eml = pg_database_encoding_max_length();
	int			orig_len;
	pg_wchar   *wide_str;
	int			wide_len;
	regmatch_t *pmatch;
	int			pmatch_len;
	int			array_len;
	int			array_idx;
	int			prev_match_end;
	int			start_search;
	int			maxlen = 0;		/* largest fetch length in characters */

	/* save flags */
	matchctx->re_flags = *re_flags;

	/* save original string --- we'll extract result substrings from it */
	matchctx->orig_str = orig_str;

	/* convert string to pg_wchar form for matching */
	orig_len = VARSIZE_ANY_EXHDR(orig_str);
	wide_str = (pg_wchar *) palloc(sizeof(pg_wchar) * (orig_len + 1));
	wide_len = pg_mb2wchar_with_len(VARDATA_ANY(orig_str), wide_str, orig_len);

	/* do we want to remember subpatterns? */
	if (use_subpatterns && cpattern->re_nsub > 0)
	{
		matchctx->npatterns = cpattern->re_nsub + 1;
		pmatch_len = cpattern->re_nsub + 1;
	}
	else
	{
		use_subpatterns = false;
		matchctx->npatterns = 1;
		pmatch_len = 1;
	}

	/* temporary output space for RE package */
	pmatch = palloc(sizeof(regmatch_t) * pmatch_len);

	/*
	 * the real output space (grown dynamically if needed)
	 *
	 * use values 2^n-1, not 2^n, so that we hit the limit at 2^28-1 rather
	 * than at 2^27
	 */
	array_len = re_flags->glob ? 255 : 31;
	matchctx->match_locs = (int *) palloc(sizeof(int) * array_len);
	array_idx = 0;

	/* search for the pattern, perhaps repeatedly */
	prev_match_end = 0;
	start_search = 0;
	while (test_re_execute(cpattern, wide_str, wide_len,
						   start_search,
						   &matchctx->details,
						   pmatch_len, pmatch,
						   re_flags->eflags))
	{
		/* enlarge output space if needed */
		while (array_idx + matchctx->npatterns * 2 + 1 > array_len)
		{
			array_len += array_len + 1; /* 2^n-1 => 2^(n+1)-1 */
			if (array_len > MaxAllocSize / sizeof(int))
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("too many regular expression matches")));
			matchctx->match_locs = (int *) repalloc(matchctx->match_locs,
													sizeof(int) * array_len);
		}

		/* save this match's locations */
		for (int i = 0; i < matchctx->npatterns; i++)
		{
			int			so = pmatch[i].rm_so;
			int			eo = pmatch[i].rm_eo;

			matchctx->match_locs[array_idx++] = so;
			matchctx->match_locs[array_idx++] = eo;
			if (so >= 0 && eo >= 0 && (eo - so) > maxlen)
				maxlen = (eo - so);
		}
		matchctx->nmatches++;
		prev_match_end = pmatch[0].rm_eo;

		/* if not glob, stop after one match */
		if (!re_flags->glob)
			break;

		/*
		 * Advance search position.  Normally we start the next search at the
		 * end of the previous match; but if the match was of zero length, we
		 * have to advance by one character, or we'd just find the same match
		 * again.
		 */
		start_search = prev_match_end;
		if (pmatch[0].rm_so == pmatch[0].rm_eo)
			start_search++;
		if (start_search > wide_len)
			break;
	}

	/*
	 * If we had no match, but "partial" and "indices" are set, emit the
	 * details.
	 */
	if (matchctx->nmatches == 0 && re_flags->partial && re_flags->indices)
	{
		/* enlarge output space if needed */
		while (array_idx + matchctx->npatterns * 2 + 1 > array_len)
		{
			array_len += array_len + 1; /* 2^n-1 => 2^(n+1)-1 */
			if (array_len > MaxAllocSize / sizeof(int))
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("too many regular expression matches")));
			matchctx->match_locs = (int *) repalloc(matchctx->match_locs,
													sizeof(int) * array_len);
		}

		matchctx->match_locs[array_idx++] = matchctx->details.rm_extend.rm_so;
		matchctx->match_locs[array_idx++] = matchctx->details.rm_extend.rm_eo;
		/* we don't have pmatch data, so emit -1 */
		for (int i = 1; i < matchctx->npatterns; i++)
		{
			matchctx->match_locs[array_idx++] = -1;
			matchctx->match_locs[array_idx++] = -1;
		}
		matchctx->nmatches++;
	}

	Assert(array_idx <= array_len);

	if (eml > 1)
	{
		int64		maxsiz = eml * (int64) maxlen;
		int			conv_bufsiz;

		/*
		 * Make the conversion buffer large enough for any substring of
		 * interest.
		 *
		 * Worst case: assume we need the maximum size (maxlen*eml), but take
		 * advantage of the fact that the original string length in bytes is
		 * an upper bound on the byte length of any fetched substring (and we
		 * know that len+1 is safe to allocate because the varlena header is
		 * longer than 1 byte).
		 */
		if (maxsiz > orig_len)
			conv_bufsiz = orig_len + 1;
		else
			conv_bufsiz = maxsiz + 1;	/* safe since maxsiz < 2^30 */

		matchctx->conv_buf = palloc(conv_bufsiz);
		matchctx->conv_bufsiz = conv_bufsiz;
		matchctx->wide_str = wide_str;
	}
	else
	{
		/* No need to keep the wide string if we're in a single-byte charset. */
		pfree(wide_str);
		matchctx->wide_str = NULL;
		matchctx->conv_buf = NULL;
		matchctx->conv_bufsiz = 0;
	}

	/* Clean up temp storage */
	pfree(pmatch);

	return matchctx;
}

/*
 * build_test_info_result - build output array describing compiled regexp
 *
 * This borrows some code from Tcl's TclRegAbout().
 */
static ArrayType *
build_test_info_result(regex_t *cpattern, test_re_flags *flags)
{
	/* Translation data for flag bits in regex_t.re_info */
	struct infoname
	{
		int			bit;
		const char *text;
	};
	static const struct infoname infonames[] = {
		{REG_UBACKREF, "REG_UBACKREF"},
		{REG_ULOOKAROUND, "REG_ULOOKAROUND"},
		{REG_UBOUNDS, "REG_UBOUNDS"},
		{REG_UBRACES, "REG_UBRACES"},
		{REG_UBSALNUM, "REG_UBSALNUM"},
		{REG_UPBOTCH, "REG_UPBOTCH"},
		{REG_UBBS, "REG_UBBS"},
		{REG_UNONPOSIX, "REG_UNONPOSIX"},
		{REG_UUNSPEC, "REG_UUNSPEC"},
		{REG_UUNPORT, "REG_UUNPORT"},
		{REG_ULOCALE, "REG_ULOCALE"},
		{REG_UEMPTYMATCH, "REG_UEMPTYMATCH"},
		{REG_UIMPOSSIBLE, "REG_UIMPOSSIBLE"},
		{REG_USHORTEST, "REG_USHORTEST"},
		{0, NULL}
	};
	const struct infoname *inf;
	Datum		elems[lengthof(infonames) + 1];
	int			nresults = 0;
	char		buf[80];
	int			dims[1];
	int			lbs[1];

	/* Set up results: first, the number of subexpressions */
	snprintf(buf, sizeof(buf), "%d", (int) cpattern->re_nsub);
	elems[nresults++] = PointerGetDatum(cstring_to_text(buf));

	/* Report individual info bit states */
	for (inf = infonames; inf->bit != 0; inf++)
	{
		if (cpattern->re_info & inf->bit)
		{
			if (flags->info & inf->bit)
				elems[nresults++] = PointerGetDatum(cstring_to_text(inf->text));
			else
			{
				snprintf(buf, sizeof(buf), "unexpected %s!", inf->text);
				elems[nresults++] = PointerGetDatum(cstring_to_text(buf));
			}
		}
		else
		{
			if (flags->info & inf->bit)
			{
				snprintf(buf, sizeof(buf), "missing %s!", inf->text);
				elems[nresults++] = PointerGetDatum(cstring_to_text(buf));
			}
		}
	}

	/* And form an array */
	dims[0] = nresults;
	lbs[0] = 1;
	/* XXX: this hardcodes assumptions about the text type */
	return construct_md_array(elems, NULL, 1, dims, lbs,
							  TEXTOID, -1, false, TYPALIGN_INT);
}

/*
 * build_test_match_result - build output array for current match
 *
 * Note that if the indices flag is set, we don't need any strings,
 * just the location data.
 */
static ArrayType *
build_test_match_result(test_regex_ctx *matchctx)
{
	char	   *buf = matchctx->conv_buf;
	Datum	   *elems = matchctx->elems;
	bool	   *nulls = matchctx->nulls;
	bool		indices = matchctx->re_flags.indices;
	char		bufstr[80];
	int			dims[1];
	int			lbs[1];
	int			loc;
	int			i;

	/* Extract matching substrings from the original string */
	loc = matchctx->next_match * matchctx->npatterns * 2;
	for (i = 0; i < matchctx->npatterns; i++)
	{
		int			so = matchctx->match_locs[loc++];
		int			eo = matchctx->match_locs[loc++];

		if (indices)
		{
			/* Report eo this way for consistency with Tcl */
			snprintf(bufstr, sizeof(bufstr), "%d %d",
					 so, so < 0 ? eo : eo - 1);
			elems[i] = PointerGetDatum(cstring_to_text(bufstr));
			nulls[i] = false;
		}
		else if (so < 0 || eo < 0)
		{
			elems[i] = (Datum) 0;
			nulls[i] = true;
		}
		else if (buf)
		{
			int			len = pg_wchar2mb_with_len(matchctx->wide_str + so,
												   buf,
												   eo - so);

			Assert(len < matchctx->conv_bufsiz);
			elems[i] = PointerGetDatum(cstring_to_text_with_len(buf, len));
			nulls[i] = false;
		}
		else
		{
			elems[i] = DirectFunctionCall3(text_substr,
										   PointerGetDatum(matchctx->orig_str),
										   Int32GetDatum(so + 1),
										   Int32GetDatum(eo - so));
			nulls[i] = false;
		}
	}

	/* In EXPECT indices mode, also report the "details" */
	if (indices && (matchctx->re_flags.cflags & REG_EXPECT))
	{
		int			so = matchctx->details.rm_extend.rm_so;
		int			eo = matchctx->details.rm_extend.rm_eo;

		snprintf(bufstr, sizeof(bufstr), "%d %d",
				 so, so < 0 ? eo : eo - 1);
		elems[i] = PointerGetDatum(cstring_to_text(bufstr));
		nulls[i] = false;
		i++;
	}

	/* And form an array */
	dims[0] = i;
	lbs[0] = 1;
	/* XXX: this hardcodes assumptions about the text type */
	return construct_md_array(elems, nulls, 1, dims, lbs,
							  TEXTOID, -1, false, TYPALIGN_INT);
}
