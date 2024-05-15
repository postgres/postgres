/*-------------------------------------------------------------------------
 * case_test.c
 *		Program to test Unicode case mapping functions.
 *
 * Portions Copyright (c) 2017-2024, PostgreSQL Global Development Group
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
#include <unicode/uchar.h>
#endif
#include "common/unicode_case.h"
#include "common/unicode_category.h"
#include "common/unicode_version.h"

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

			if (icu_category == PG_U_UNASSIGNED)
			{
				skipped_mismatch++;
				continue;
			}

			icu_test_simple(code);
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
test_strlower(const char *test_string, const char *expected)
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
	needed = unicode_strlower(dst1, dst1len, src1, src1len);
	if (needed != strlen(expected))
	{
		printf("case_test: convert_case test1 FAILURE: needed %zu\n", needed);
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
	needed = unicode_strlower(dst2, dst2len, src1, src1len);
	if (needed != strlen(expected))
	{
		printf("case_test: convert_case test2 FAILURE: needed %zu\n", needed);
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
	needed = unicode_strlower(dst1, dst1len, src2, src2len);
	if (needed != strlen(expected))
	{
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
	needed = unicode_strlower(dst2, dst2len, src2, src2len);
	if (needed != strlen(expected))
	{
		printf("case_test: convert_case test4 FAILURE: needed %zu\n", needed);
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

static void
test_convert_case()
{
	/* test string with no case changes */
	test_strlower("√∞", "√∞");
	/* test string with case changes */
	test_strlower("ABC", "abc");
	/* test string with case changes and byte length changes */
	test_strlower("ȺȺȺ", "ⱥⱥⱥ");

	printf("case_test: convert_case: success\n");
}

int
main(int argc, char **argv)
{
	printf("case_test: Postgres Unicode version:\t%s\n", PG_UNICODE_VERSION);
#ifdef USE_ICU
	printf("case_test: ICU Unicode version:\t\t%s\n", U_UNICODE_VERSION);
	test_icu();
#else
	printf("case_test: ICU not available; skipping\n");
#endif

	test_convert_case();
	exit(0);
}
