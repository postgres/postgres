/*-------------------------------------------------------------------------
 * category_test.c
 *		Program to test Unicode general category functions.
 *
 * Portions Copyright (c) 2017-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/common/unicode/category_test.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef USE_ICU
#include <unicode/uchar.h>
#endif
#include "common/unicode_category.h"
#include "common/unicode_version.h"

/*
 * Parse version into integer for easy comparison.
 */
#ifdef USE_ICU
static int
parse_unicode_version(const char *version)
{
	int			n,
				major,
				minor;

	n = sscanf(version, "%d.%d", &major, &minor);

	Assert(n == 2);
	Assert(minor < 100);

	return major * 100 + minor;
}
#endif

/*
 * Exhaustively test that the Unicode category for each codepoint matches that
 * returned by ICU.
 */
int
main(int argc, char **argv)
{
#ifdef USE_ICU
	int			pg_unicode_version = parse_unicode_version(PG_UNICODE_VERSION);
	int			icu_unicode_version = parse_unicode_version(U_UNICODE_VERSION);
	int			pg_skipped_codepoints = 0;
	int			icu_skipped_codepoints = 0;

	printf("Postgres Unicode Version:\t%s\n", PG_UNICODE_VERSION);
	printf("ICU Unicode Version:\t\t%s\n", U_UNICODE_VERSION);

	for (UChar32 code = 0; code <= 0x10ffff; code++)
	{
		uint8_t		pg_category = unicode_category(code);
		uint8_t		icu_category = u_charType(code);

		if (pg_category != icu_category)
		{
			/*
			 * A version mismatch means that some assigned codepoints in the
			 * newer version may be unassigned in the older version. That's
			 * OK, though the test will not cover those codepoints marked
			 * unassigned in the older version (that is, it will no longer be
			 * an exhaustive test).
			 */
			if (pg_category == PG_U_UNASSIGNED &&
				pg_unicode_version < icu_unicode_version)
				pg_skipped_codepoints++;
			else if (icu_category == PG_U_UNASSIGNED &&
					 icu_unicode_version < pg_unicode_version)
				icu_skipped_codepoints++;
			else
			{
				printf("FAILURE for codepoint %06x\n", code);
				printf("Postgres category:	%02d %s %s\n", pg_category,
					   unicode_category_abbrev(pg_category),
					   unicode_category_string(pg_category));
				printf("ICU category:		%02d %s %s\n", icu_category,
					   unicode_category_abbrev(icu_category),
					   unicode_category_string(icu_category));
				printf("\n");
				exit(1);
			}
		}
	}

	if (pg_skipped_codepoints > 0)
		printf("Skipped %d codepoints unassigned in Postgres due to Unicode version mismatch.\n",
			   pg_skipped_codepoints);
	if (icu_skipped_codepoints > 0)
		printf("Skipped %d codepoints unassigned in ICU due to Unicode version mismatch.\n",
			   icu_skipped_codepoints);

	printf("category_test: All tests successful!\n");
	exit(0);
#else
	printf("ICU support required for test; skipping.\n");
	exit(0);
#endif
}
