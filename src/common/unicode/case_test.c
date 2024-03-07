/*-------------------------------------------------------------------------
 * case_test.c
 *		Program to test Unicode case mapping functions.
 *
 * Portions Copyright (c) 2017-2023, PostgreSQL Global Development Group
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

/*
 * Exhaustively compare case mappings with the results from libc and ICU.
 */
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

	exit(0);
}
