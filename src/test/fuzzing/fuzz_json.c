/*-------------------------------------------------------------------------
 *
 * fuzz_json.c
 *    Fuzzing harness for the non-incremental JSON parser
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    src/test/fuzzing/fuzz_json.c
 *
 * This harness feeds arbitrary byte sequences to pg_parse_json() via
 * makeJsonLexContextCstringLen().  It uses the null semantic action so
 * that only lexing and structural validation are exercised.
 *
 * Build with a fuzzing engine (e.g. libFuzzer via -fsanitize=fuzzer)
 * or in standalone mode, which reads files named on the command line.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <stdio.h>

#include "common/jsonapi.h"
#include "mb/pg_wchar.h"

/*
 * Entry point for libFuzzer and other engines that call
 * LLVMFuzzerTestOneInput().
 */
int			LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	JsonLexContext lex;

	if (size == 0)
		return 0;

	makeJsonLexContextCstringLen(&lex, (const char *) data, size,
								 PG_UTF8, true);
	setJsonLexContextOwnsTokens(&lex, true);

	(void) pg_parse_json(&lex, &nullSemAction);

	freeJsonLexContext(&lex);

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
