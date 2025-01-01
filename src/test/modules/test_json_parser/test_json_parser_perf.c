/*-------------------------------------------------------------------------
 *
 * test_json_parser_perf.c
 *    Performance test program for both flavors of the JSON parser
 *
 * Copyright (c) 2024-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    src/test/modules/test_json_parser/test_json_parser_perf.c
 *
 * This program tests either the standard (recursive descent) JSON parser
 * or the incremental (table driven) parser, but without breaking the input
 * into chunks in the latter case. Thus it can be used to compare the pure
 * parsing speed of the two parsers. If the "-i" option is used, then the
 * table driven parser is used. Otherwise, the recursive descent parser is
 * used.
 *
 * The remaining arguments are the number of parsing iterations to be done
 * and the file containing the JSON input.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"
#include "common/jsonapi.h"
#include "common/logging.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include <stdio.h>
#include <string.h>

#define BUFSIZE 6000

int
main(int argc, char **argv)
{
	char		buff[BUFSIZE];
	FILE	   *json_file;
	JsonParseErrorType result;
	JsonLexContext *lex;
	StringInfoData json;
	int			n_read;
	int			iter;
	int			use_inc = 0;

	pg_logging_init(argv[0]);

	initStringInfo(&json);

	if (strcmp(argv[1], "-i") == 0)
	{
		use_inc = 1;
		argv++;
	}

	sscanf(argv[1], "%d", &iter);

	if ((json_file = fopen(argv[2], PG_BINARY_R)) == NULL)
		pg_fatal("Could not open input file '%s': %m", argv[2]);

	while ((n_read = fread(buff, 1, 6000, json_file)) > 0)
	{
		appendBinaryStringInfo(&json, buff, n_read);
	}
	fclose(json_file);
	for (int i = 0; i < iter; i++)
	{
		if (use_inc)
		{
			lex = makeJsonLexContextIncremental(NULL, PG_UTF8, false);
			result = pg_parse_json_incremental(lex, &nullSemAction,
											   json.data, json.len,
											   true);
			freeJsonLexContext(lex);
		}
		else
		{
			lex = makeJsonLexContextCstringLen(NULL, json.data, json.len,
											   PG_UTF8, false);
			result = pg_parse_json(lex, &nullSemAction);
			freeJsonLexContext(lex);
		}
		if (result != JSON_SUCCESS)
			pg_fatal("unexpected result %d (expecting %d) on parse",
					 result, JSON_SUCCESS);
	}
	exit(0);
}
