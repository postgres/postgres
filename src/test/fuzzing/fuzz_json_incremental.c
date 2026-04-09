/*-------------------------------------------------------------------------
 *
 * fuzz_json_incremental.c
 *    Fuzzing harness for the incremental JSON parser
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    src/test/fuzzing/fuzz_json_incremental.c
 *
 * This harness feeds arbitrary byte sequences to
 * pg_parse_json_incremental() in small chunks, exercising the
 * incremental lexer's boundary handling.  The first byte of the input
 * is used to vary the chunk size so that the fuzzer can explore
 * different splitting strategies.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <stdio.h>

#include "common/jsonapi.h"
#include "mb/pg_wchar.h"

int			LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	JsonLexContext lex;
	size_t		chunk_size;
	size_t		offset;

	if (size < 2)
		return 0;

	/*
	 * Use the first byte to select a chunk size between 1 and 128.  This lets
	 * the fuzzer explore different ways of splitting the same input across
	 * incremental parse calls.
	 */
	chunk_size = (data[0] % 128) + 1;
	data++;
	size--;

	makeJsonLexContextIncremental(&lex, PG_UTF8, true);
	setJsonLexContextOwnsTokens(&lex, true);

	offset = 0;
	while (offset < size)
	{
		size_t		remaining = size - offset;
		size_t		to_feed = (remaining < chunk_size) ? remaining : chunk_size;
		bool		is_last = (offset + to_feed >= size);
		JsonParseErrorType result;

		result = pg_parse_json_incremental(&lex, &nullSemAction,
										   (const char *) data + offset,
										   to_feed, is_last);

		offset += to_feed;

		if (result != JSON_SUCCESS && result != JSON_INCOMPLETE)
			break;
		if (result == JSON_SUCCESS)
			break;
	}

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
