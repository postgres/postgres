/*-------------------------------------------------------------------------
 *
 * fuzz_regex.c
 *    Fuzzing harness for the PostgreSQL regular expression engine
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    src/test/fuzzing/fuzz_regex.c
 *
 * This harness feeds arbitrary byte sequences to pg_regcomp() and
 * pg_regexec(), exercising the full POSIX/ARE regex compiler and
 * executor.  The first byte selects regex flags; the remaining bytes
 * are split between the regex pattern and a test subject string.
 *
 * The harness links against postgres_lib using archive semantics.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <stdio.h>

#include "catalog/pg_collation.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "regex/regex.h"
#include "utils/memutils.h"
#include "utils/palloc.h"

/* Stubs for symbols from main/main.c */
const char *progname = "fuzz_regex";

DispatchOption
parse_dispatch_option(const char *name)
{
	return DISPATCH_POSTMASTER;
}

static bool initialized = false;

static void fuzz_initialize(void);

int			LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void
fuzz_initialize(void)
{
	MemoryContextInit();
	SetDatabaseEncoding(PG_UTF8);
	SetMessageEncoding(PG_UTF8);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	uint8_t		flags_byte;
	int			re_flags;
	size_t		pat_len;
	size_t		subj_len;
	const char *pat_start;
	const char *subj_start;
	pg_wchar   *pat_wchar;
	pg_wchar   *subj_wchar;
	int			pat_wlen;
	int			subj_wlen;
	regex_t		re;
	regmatch_t	matches[10];
	MemoryContext fuzz_context;
	MemoryContext oldcontext;

	if (!initialized)
	{
		fuzz_initialize();
		initialized = true;
	}

	/* Need at least flags byte + 1 byte of pattern */
	if (size < 2)
		return 0;

	/*
	 * First byte selects regex flags. We map bits to useful flag combinations
	 * to get good coverage of different regex modes.
	 */
	flags_byte = data[0];
	re_flags = REG_ADVANCED;
	if (flags_byte & 0x01)
		re_flags = REG_EXTENDED;	/* ERE instead of ARE */
	if (flags_byte & 0x02)
		re_flags |= REG_ICASE;
	if (flags_byte & 0x04)
		re_flags |= REG_NEWLINE;
	if (flags_byte & 0x08)
		re_flags |= REG_NOSUB;

	data++;
	size--;

	/* Split remaining input: first half pattern, second half subject */
	pat_len = size / 2;
	if (pat_len == 0)
		pat_len = 1;
	subj_len = size - pat_len;

	pat_start = (const char *) data;
	subj_start = (const char *) data + pat_len;

	fuzz_context = AllocSetContextCreate(TopMemoryContext,
										 "Fuzz Context",
										 ALLOCSET_DEFAULT_SIZES);
	oldcontext = MemoryContextSwitchTo(fuzz_context);

	/* Convert to pg_wchar for the regex API */
	pat_wchar = palloc((pat_len + 1) * sizeof(pg_wchar));
	pat_wlen = pg_mb2wchar_with_len(pat_start, pat_wchar, (int) pat_len);

	if (pg_regcomp(&re, pat_wchar, pat_wlen, re_flags, C_COLLATION_OID) == 0)
	{
		/* Compile succeeded — try executing against the subject */
		if (subj_len > 0)
		{
			subj_wchar = palloc((subj_len + 1) * sizeof(pg_wchar));
			subj_wlen = pg_mb2wchar_with_len(subj_start, subj_wchar,
											 (int) subj_len);

			(void) pg_regexec(&re, subj_wchar, subj_wlen, 0, NULL,
							  lengthof(matches), matches, 0);
		}

		pg_regfree(&re);
	}

	MemoryContextSwitchTo(oldcontext);
	MemoryContextDelete(fuzz_context);

	return 0;
}

#ifdef STANDALONE_FUZZ_TARGET
int
main(int argc, char **argv)
{
	int			i;
	int			ret = 0;

	for (i = 1; i < argc; i++)
	{
		FILE	   *f = fopen(argv[i], "rb");
		long		len;
		uint8_t    *buf;
		size_t		n_read;

		if (!f)
		{
			fprintf(stderr, "%s: could not open %s: %m\n", argv[0], argv[i]);
			ret = 1;
			continue;
		}

		fseek(f, 0, SEEK_END);
		len = ftell(f);
		fseek(f, 0, SEEK_SET);

		if (len < 0)
		{
			fprintf(stderr, "%s: could not determine size of %s\n",
					argv[0], argv[i]);
			fclose(f);
			ret = 1;
			continue;
		}

		buf = malloc(len);
		if (!buf)
		{
			fprintf(stderr, "%s: out of memory\n", argv[0]);
			fclose(f);
			return 1;
		}

		n_read = fread(buf, 1, len, f);
		fclose(f);

		LLVMFuzzerTestOneInput(buf, n_read);
		free(buf);
	}

	return ret;
}
#endif							/* STANDALONE_FUZZ_TARGET */
