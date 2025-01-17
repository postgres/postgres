/*-------------------------------------------------------------------------
 * case_test.c
 *		Program to test Unicode case mapping functions.
 *
 * Portions Copyright (c) 2017-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/common/unicode/case_test.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

#ifdef USE_ICU
#include <unicode/ucasemap.h>
#include <unicode/uchar.h>
#endif
#include "common/unicode_case.h"
#include "common/unicode_category.h"
#include "common/unicode_version.h"

/* enough to hold largest source or result string, including NUL */
#define BUFSZ 256

#ifdef USE_ICU
static UCaseMap * casemap = NULL;
#endif

typedef size_t (*TestFunc) (char *dst, size_t dstsize, const char *src,
							ssize_t srclen);

/* simple boundary iterator copied from pg_locale_builtin.c */
struct WordBoundaryState
{
	const char *str;
	size_t		len;
	size_t		offset;
	bool		init;
	bool		prev_alnum;
};

static size_t
initcap_wbnext(void *state)
{
	struct WordBoundaryState *wbstate = (struct WordBoundaryState *) state;

	while (wbstate->offset < wbstate->len &&
		   wbstate->str[wbstate->offset] != '\0')
	{
		pg_wchar	u = utf8_to_unicode((unsigned char *) wbstate->str +
										wbstate->offset);
		bool		curr_alnum = pg_u_isalnum(u, true);

		if (!wbstate->init || curr_alnum != wbstate->prev_alnum)
		{
			size_t		prev_offset = wbstate->offset;

			wbstate->init = true;
			wbstate->offset += unicode_utf8len(u);
			wbstate->prev_alnum = curr_alnum;
			return prev_offset;
		}

		wbstate->offset += unicode_utf8len(u);
	}

	return wbstate->len;
}

#ifdef USE_ICU

static void
icu_test_simple(pg_wchar code)
{
	pg_wchar	lower = unicode_lowercase_simple(code);
	pg_wchar	title = unicode_titlecase_simple(code);
	pg_wchar	upper = unicode_uppercase_simple(code);
	pg_wchar	iculower = u_tolower(code);
	pg_wchar	icutitle = u_totitle(code);
	pg_wchar	icuupper = u_toupper(code);

	if (lower != iculower || title != icutitle || upper != icuupper)
	{
		printf("case_test: FAILURE for codepoint 0x%06x\n", code);
		printf("case_test: Postgres lower/title/upper:	0x%06x/0x%06x/0x%06x\n",
			   lower, title, upper);
		printf("case_test: ICU lower/title/upper:		0x%06x/0x%06x/0x%06x\n",
			   iculower, icutitle, icuupper);
		printf("\n");
		exit(1);
	}
}

static void
icu_test_full(char *str)
{
	char		lower[BUFSZ];
	char		title[BUFSZ];
	char		upper[BUFSZ];
	char		icu_lower[BUFSZ];
	char		icu_title[BUFSZ];
	char		icu_upper[BUFSZ];
	UErrorCode	status;
	struct WordBoundaryState wbstate = {
		.str = str,
		.len = strlen(str),
		.offset = 0,
		.init = false,
		.prev_alnum = false,
	};

	unicode_strlower(lower, BUFSZ, str, -1, true);
	unicode_strtitle(title, BUFSZ, str, -1, true, initcap_wbnext, &wbstate);
	unicode_strupper(upper, BUFSZ, str, -1, true);
	status = U_ZERO_ERROR;
	ucasemap_utf8ToLower(casemap, icu_lower, BUFSZ, str, -1, &status);
	status = U_ZERO_ERROR;
	ucasemap_utf8ToTitle(casemap, icu_title, BUFSZ, str, -1, &status);
	status = U_ZERO_ERROR;
	ucasemap_utf8ToUpper(casemap, icu_upper, BUFSZ, str, -1, &status);

	if (strcmp(lower, icu_lower) != 0)
	{
		printf("case_test: str='%s' lower='%s' icu_lower='%s'\n", str, lower,
			   icu_lower);
		exit(1);
	}
	if (strcmp(title, icu_title) != 0)
	{
		printf("case_test: str='%s' title='%s' icu_title='%s'\n", str, title,
			   icu_title);
		exit(1);
	}
	if (strcmp(upper, icu_upper) != 0)
	{
		printf("case_test: str='%s' upper='%s' icu_upper='%s'\n", str, upper,
			   icu_upper);
		exit(1);
	}
}

/*
 * Exhaustively compare case mappings with the results from ICU.
 */
static void
test_icu(void)
{
	int			successful = 0;
	int			skipped_mismatch = 0;

	for (pg_wchar code = 0; code <= 0x10ffff; code++)
	{
		pg_unicode_category category = unicode_category(code);

		if (category != PG_U_UNASSIGNED)
		{
			uint8_t		icu_category = u_charType(code);
			char		code_str[5] = {0};

			if (icu_category == PG_U_UNASSIGNED)
			{
				skipped_mismatch++;
				continue;
			}

			icu_test_simple(code);
			unicode_to_utf8(code, (unsigned char *) code_str);
			icu_test_full(code_str);

			successful++;
		}
	}

	if (skipped_mismatch > 0)
		printf("case_test: skipped %d codepoints unassigned in ICU due to Unicode version mismatch\n",
			   skipped_mismatch);

	printf("case_test: ICU simple mapping test: %d codepoints successful\n",
		   successful);
}
#endif

static void
test_convert(TestFunc tfunc, const char *test_string, const char *expected)
{
	size_t		src1len = strlen(test_string);
	size_t		src2len = -1;	/* NUL-terminated */
	size_t		dst1len = strlen(expected);
	size_t		dst2len = strlen(expected) + 1; /* NUL-terminated */
	char	   *src1 = malloc(src1len);
	char	   *dst1 = malloc(dst1len);
	char	   *src2 = strdup(test_string);
	char	   *dst2 = malloc(dst2len);
	size_t		needed;

	memcpy(src1, test_string, src1len); /* not NUL-terminated */

	/* neither source nor destination are NUL-terminated */
	memset(dst1, 0x7F, dst1len);
	needed = tfunc(dst1, dst1len, src1, src1len);
	if (needed != strlen(expected))
	{
		printf("case_test: convert_case test1 FAILURE: '%s' needed %zu expected %zu\n",
			   test_string, needed, strlen(expected));
		exit(1);
	}
	if (memcmp(dst1, expected, dst1len) != 0)
	{
		printf("case_test: convert_case test1 FAILURE: test: '%s' result: '%.*s' expected: '%s'\n",
			   test_string, (int) dst1len, dst1, expected);
		exit(1);
	}

	/* destination is NUL-terminated and source is not */
	memset(dst2, 0x7F, dst2len);
	needed = tfunc(dst2, dst2len, src1, src1len);
	if (needed != strlen(expected))
	{
		printf("case_test: convert_case test2 FAILURE: '%s' needed %zu expected %zu\n",
			   test_string, needed, strlen(expected));
		exit(1);
	}
	if (strcmp(dst2, expected) != 0)
	{
		printf("case_test: convert_case test2 FAILURE: test: '%s' result: '%s' expected: '%s'\n",
			   test_string, dst2, expected);
		exit(1);
	}

	/* source is NUL-terminated and destination is not */
	memset(dst1, 0x7F, dst1len);
	needed = tfunc(dst1, dst1len, src2, src2len);
	if (needed != strlen(expected))
	{
		printf("case_test: convert_case test3 FAILURE: '%s' needed %zu expected %zu\n",
			   test_string, needed, strlen(expected));
		printf("case_test: convert_case test3 FAILURE: needed %zu\n", needed);
		exit(1);
	}
	if (memcmp(dst1, expected, dst1len) != 0)
	{
		printf("case_test: convert_case test3 FAILURE: test: '%s' result: '%.*s' expected: '%s'\n",
			   test_string, (int) dst1len, dst1, expected);
		exit(1);
	}

	/* both source and destination are NUL-terminated */
	memset(dst2, 0x7F, dst2len);
	needed = tfunc(dst2, dst2len, src2, src2len);
	if (needed != strlen(expected))
	{
		printf("case_test: convert_case test4 FAILURE: '%s' needed %zu expected %zu\n",
			   test_string, needed, strlen(expected));
		exit(1);
	}
	if (strcmp(dst2, expected) != 0)
	{
		printf("case_test: convert_case test4 FAILURE: test: '%s' result: '%s' expected: '%s'\n",
			   test_string, dst2, expected);
		exit(1);
	}

	free(src1);
	free(dst1);
	free(src2);
	free(dst2);
}

static size_t
tfunc_lower(char *dst, size_t dstsize, const char *src,
			ssize_t srclen)
{
	return unicode_strlower(dst, dstsize, src, srclen, true);
}

static size_t
tfunc_title(char *dst, size_t dstsize, const char *src,
			ssize_t srclen)
{
	struct WordBoundaryState wbstate = {
		.str = src,
		.len = srclen,
		.offset = 0,
		.init = false,
		.prev_alnum = false,
	};

	return unicode_strtitle(dst, dstsize, src, srclen, true, initcap_wbnext,
							&wbstate);
}

static size_t
tfunc_upper(char *dst, size_t dstsize, const char *src,
			ssize_t srclen)
{
	return unicode_strupper(dst, dstsize, src, srclen, true);
}


static void
test_convert_case()
{
	/* test string with no case changes */
	test_convert(tfunc_lower, "√∞", "√∞");
	/* test adjust-to-cased behavior */
	test_convert(tfunc_title, "abc 123xyz", "Abc 123xyz");
	/* test string with case changes */
	test_convert(tfunc_upper, "abc", "ABC");
	/* test string with case changes and byte length changes */
	test_convert(tfunc_lower, "ȺȺȺ", "ⱥⱥⱥ");
	/* test special case conversions */
	test_convert(tfunc_upper, "ß", "SS");
	test_convert(tfunc_lower, "ıiIİ", "ıiii\u0307");
	test_convert(tfunc_upper, "ıiIİ", "IIIİ");
	/* test final sigma */
	test_convert(tfunc_lower, "σςΣ ΣΣΣ", "σςς σσς");
	test_convert(tfunc_lower, "σς'Σ' ΣΣ'Σ'", "σς'ς' σσ'ς'");
	test_convert(tfunc_title, "σςΣ ΣΣΣ", "Σςς Σσς");

#ifdef USE_ICU
	icu_test_full("");
	icu_test_full("ȺȺȺ");
	icu_test_full("ßßß");
	icu_test_full("√∞");
	icu_test_full("a b");
	icu_test_full("abc 123xyz");
	icu_test_full("σςΣ ΣΣΣ");
	icu_test_full("ıiIİ");
	/* test <alpha><iota_subscript><acute> */
	icu_test_full("\u0391\u0345\u0301");
#endif

	printf("case_test: convert_case: success\n");
}

int
main(int argc, char **argv)
{
#ifdef USE_ICU
	UErrorCode	status = U_ZERO_ERROR;

	/*
	 * Disable ICU's word break adjustment for titlecase to match the expected
	 * behavior of unicode_strtitle().
	 */
	casemap = ucasemap_open("und", U_TITLECASE_NO_BREAK_ADJUSTMENT, &status);
	if (U_FAILURE(status))
	{
		printf("case_test: failure opening UCaseMap: %s\n",
			   u_errorName(status));
		exit(1);
	}
#endif

	printf("case_test: Postgres Unicode version:\t%s\n", PG_UNICODE_VERSION);
#ifdef USE_ICU
	printf("case_test: ICU Unicode version:\t\t%s\n", U_UNICODE_VERSION);
	test_icu();
#else
	printf("case_test: ICU not available; skipping\n");
#endif

	test_convert_case();

#ifdef USE_ICU
	ucasemap_close(casemap);
#endif
	exit(0);
}
