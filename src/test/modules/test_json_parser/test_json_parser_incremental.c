/*-------------------------------------------------------------------------
 *
 * test_json_parser_incremental.c
 *    Test program for incremental JSON parser
 *
 * Copyright (c) 2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    src/test/modules/test_json_parser/test_json_parser_incremental.c
 *
 * This progam tests incremental parsing of json. The input is fed into
 * the parser in very small chunks. In practice you would normally use
 * much larger chunks, but doing this makes it more likely that the
 * full range of incement handling, especially in the lexer, is exercised.
 * If the "-c SIZE" option is provided, that chunk size is used instead.
 *
 * The argument specifies the file containing the JSON input.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/jsonapi.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "pg_getopt.h"

typedef struct DoState
{
	JsonLexContext *lex;
	bool		elem_is_first;
	StringInfo	buf;
} DoState;

static void usage(const char *progname);
static void escape_json(StringInfo buf, const char *str);

/* semantic action functions for parser */
static JsonParseErrorType do_object_start(void *state);
static JsonParseErrorType do_object_end(void *state);
static JsonParseErrorType do_object_field_start(void *state, char *fname, bool isnull);
static JsonParseErrorType do_object_field_end(void *state, char *fname, bool isnull);
static JsonParseErrorType do_array_start(void *state);
static JsonParseErrorType do_array_end(void *state);
static JsonParseErrorType do_array_element_start(void *state, bool isnull);
static JsonParseErrorType do_array_element_end(void *state, bool isnull);
static JsonParseErrorType do_scalar(void *state, char *token, JsonTokenType tokentype);

JsonSemAction sem = {
	.object_start = do_object_start,
	.object_end = do_object_end,
	.object_field_start = do_object_field_start,
	.object_field_end = do_object_field_end,
	.array_start = do_array_start,
	.array_end = do_array_end,
	.array_element_start = do_array_element_start,
	.array_element_end = do_array_element_end,
	.scalar = do_scalar
};

int
main(int argc, char **argv)
{
	/* max delicious line length is less than this */
	char		buff[6001];
	FILE	   *json_file;
	JsonParseErrorType result;
	JsonLexContext lex;
	StringInfoData json;
	int			n_read;
	size_t		chunk_size = 60;
	struct stat statbuf;
	off_t		bytes_left;
	JsonSemAction *testsem = &nullSemAction;
	char	   *testfile;
	int			c;
	bool		need_strings = false;

	while ((c = getopt(argc, argv, "c:s")) != -1)
	{
		switch (c)
		{
			case 'c':			/* chunksize */
				sscanf(optarg, "%zu", &chunk_size);
				break;
			case 's':			/* do semantic processing */
				testsem = &sem;
				sem.semstate = palloc(sizeof(struct DoState));
				((struct DoState *) sem.semstate)->lex = &lex;
				((struct DoState *) sem.semstate)->buf = makeStringInfo();
				need_strings = true;
				break;
		}
	}

	if (optind < argc)
	{
		testfile = pg_strdup(argv[optind]);
		optind++;
	}
	else
	{
		usage(argv[0]);
		exit(1);
	}

	makeJsonLexContextIncremental(&lex, PG_UTF8, need_strings);
	initStringInfo(&json);

	json_file = fopen(testfile, "r");
	fstat(fileno(json_file), &statbuf);
	bytes_left = statbuf.st_size;

	for (;;)
	{
		n_read = fread(buff, 1, chunk_size, json_file);
		appendBinaryStringInfo(&json, buff, n_read);
		appendStringInfoString(&json, "1+23 trailing junk");
		bytes_left -= n_read;
		if (bytes_left > 0)
		{
			result = pg_parse_json_incremental(&lex, testsem,
											   json.data, n_read,
											   false);
			if (result != JSON_INCOMPLETE)
			{
				fprintf(stderr, "%s\n", json_errdetail(result, &lex));
				exit(1);
			}
			resetStringInfo(&json);
		}
		else
		{
			result = pg_parse_json_incremental(&lex, testsem,
											   json.data, n_read,
											   true);
			if (result != JSON_SUCCESS)
			{
				fprintf(stderr, "%s\n", json_errdetail(result, &lex));
				exit(1);
			}
			if (!need_strings)
				printf("SUCCESS!\n");
			break;
		}
	}
	fclose(json_file);
	exit(0);
}

/*
 * The semantic routines here essentially just output the same json, except
 * for white space. We could pretty print it but there's no need for our
 * purposes. The result should be able to be fed to any JSON processor
 * such as jq for validation.
 */

static JsonParseErrorType
do_object_start(void *state)
{
	DoState    *_state = (DoState *) state;

	printf("{\n");
	_state->elem_is_first = true;

	return JSON_SUCCESS;
}

static JsonParseErrorType
do_object_end(void *state)
{
	DoState    *_state = (DoState *) state;

	printf("\n}\n");
	_state->elem_is_first = false;

	return JSON_SUCCESS;
}

static JsonParseErrorType
do_object_field_start(void *state, char *fname, bool isnull)
{
	DoState    *_state = (DoState *) state;

	if (!_state->elem_is_first)
		printf(",\n");
	resetStringInfo(_state->buf);
	escape_json(_state->buf, fname);
	printf("%s: ", _state->buf->data);
	_state->elem_is_first = false;

	return JSON_SUCCESS;
}

static JsonParseErrorType
do_object_field_end(void *state, char *fname, bool isnull)
{
	/* nothing to do really */

	return JSON_SUCCESS;
}

static JsonParseErrorType
do_array_start(void *state)
{
	DoState    *_state = (DoState *) state;

	printf("[\n");
	_state->elem_is_first = true;

	return JSON_SUCCESS;
}

static JsonParseErrorType
do_array_end(void *state)
{
	DoState    *_state = (DoState *) state;

	printf("\n]\n");
	_state->elem_is_first = false;

	return JSON_SUCCESS;
}

static JsonParseErrorType
do_array_element_start(void *state, bool isnull)
{
	DoState    *_state = (DoState *) state;

	if (!_state->elem_is_first)
		printf(",\n");
	_state->elem_is_first = false;

	return JSON_SUCCESS;
}

static JsonParseErrorType
do_array_element_end(void *state, bool isnull)
{
	/* nothing to do */

	return JSON_SUCCESS;
}

static JsonParseErrorType
do_scalar(void *state, char *token, JsonTokenType tokentype)
{
	DoState    *_state = (DoState *) state;

	if (tokentype == JSON_TOKEN_STRING)
	{
		resetStringInfo(_state->buf);
		escape_json(_state->buf, token);
		printf("%s", _state->buf->data);
	}
	else
		printf("%s", token);

	return JSON_SUCCESS;
}


/*  copied from backend code */
static void
escape_json(StringInfo buf, const char *str)
{
	const char *p;

	appendStringInfoCharMacro(buf, '"');
	for (p = str; *p; p++)
	{
		switch (*p)
		{
			case '\b':
				appendStringInfoString(buf, "\\b");
				break;
			case '\f':
				appendStringInfoString(buf, "\\f");
				break;
			case '\n':
				appendStringInfoString(buf, "\\n");
				break;
			case '\r':
				appendStringInfoString(buf, "\\r");
				break;
			case '\t':
				appendStringInfoString(buf, "\\t");
				break;
			case '"':
				appendStringInfoString(buf, "\\\"");
				break;
			case '\\':
				appendStringInfoString(buf, "\\\\");
				break;
			default:
				if ((unsigned char) *p < ' ')
					appendStringInfo(buf, "\\u%04x", (int) *p);
				else
					appendStringInfoCharMacro(buf, *p);
				break;
		}
	}
	appendStringInfoCharMacro(buf, '"');
}

static void
usage(const char *progname)
{
	fprintf(stderr, "Usage: %s [OPTION ...] testfile\n", progname);
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "  -c chunksize      size of piece fed to parser (default 64)n");
	fprintf(stderr, "  -s                do semantic processing\n");

}
