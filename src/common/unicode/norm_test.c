/*-------------------------------------------------------------------------
 * norm_test.c
 *		Program to test Unicode normalization functions.
 *
 * Portions Copyright (c) 2017-2018, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/common/unicode/norm_test.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/unicode_norm.h"

#include "norm_test_table.h"

static char *
print_wchar_str(const pg_wchar *s)
{
#define BUF_DIGITS 50
	static char buf[BUF_DIGITS * 2 + 1];
	int			i;

	i = 0;
	while (*s && i < BUF_DIGITS)
	{
		snprintf(&buf[i * 2], 3, "%04X", *s);
		i++;
		s++;
	}
	buf[i * 2] = '\0';
	return buf;
}

static int
pg_wcscmp(const pg_wchar *s1, const pg_wchar *s2)
{
	for (;;)
	{
		if (*s1 < *s2)
			return -1;
		if (*s1 > *s2)
			return 1;
		if (*s1 == 0)
			return 0;
		s1++;
		s2++;
	}
}

int
main(int argc, char **argv)
{
	const		pg_unicode_test *test;

	for (test = UnicodeNormalizationTests; test->input[0] != 0; test++)
	{
		pg_wchar   *result;

		result = unicode_normalize_kc(test->input);

		if (pg_wcscmp(test->output, result) != 0)
		{
			printf("FAILURE (Normalizationdata.txt line %d):\n", test->linenum);
			printf("input:\t%s\n", print_wchar_str(test->input));
			printf("expected:\t%s\n", print_wchar_str(test->output));
			printf("got\t%s\n", print_wchar_str(result));
			printf("\n");
			exit(1);
		}
	}

	printf("All tests successful!\n");
	exit(0);
}
