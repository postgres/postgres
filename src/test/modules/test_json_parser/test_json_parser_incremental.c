/*-------------------------------------------------------------------------
 *
 * test_json_parser_incremental.c
 *    Test program for incremental JSON parser
 *
 * Copyright (c) 2024-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    src/test/modules/test_json_parser/test_json_parser_incremental.c
 *
 * This program tests incremental parsing of json. The input is fed into
 * the parser in very small chunks. In practice you would normally use
 * much larger chunks, but doing this makes it more likely that the
 * full range of increment handling, especially in the lexer, is exercised.
 * If the "-c SIZE" option is provided, that chunk size is used instead
 * of the default of 60.
 *
 * If the -s flag is given, the program does semantic processing. This should
 * just mirror back the json, albeit with white space changes.
 *
 * If the -o flag is given, the JSONLEX_CTX_OWNS_TOKENS flag is set. (This can
 * be used in combination with a leak sanitizer; without the option, the parser
 * may leak memory with invalid JSON.)
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
#include "common/logging.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "pg_getopt.h"

#define BUFSIZE 6000
#define DEFAULT_CHUNK_SIZE 60

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

static JsonSemAction sem = {
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

static bool lex_owns_tokens = false;

int
main(int argc, char **argv)
{
	char		buff[BUFSIZE];
	FILE	   *json_file;
	JsonParseErrorType result;
	JsonLexContext *lex;
	StringInfoData json;
	int			n_read;
	size_t		chunk_size = DEFAULT_CHUNK_SIZE;
	struct stat statbuf;
	off_t		bytes_left;
	const JsonSemAction *testsem = &nullSemAction;
	char	   *testfile;
	int			c;
	bool		need_strings = false;
	int			ret = 0;

	pg_logging_init(argv[0]);

	lex = calloc(1, sizeof(JsonLexContext));
	if (!lex)
		pg_fatal("out of memory");

	while ((c = getopt(argc, argv, "c:os")) != -1)
	{
		switch (c)
		{
			case 'c':			/* chunksize */
				chunk_size = strtou64(optarg, NULL, 10);
				if (chunk_size > BUFSIZE)
					pg_fatal("chunk size cannot exceed %d", BUFSIZE);
				break;
			case 'o':			/* switch token ownership */
				lex_owns_tokens = true;
				break;
			case 's':			/* do semantic processing */
				testsem = &sem;
				sem.semstate = palloc(sizeof(struct DoState));
				((struct DoState *) sem.semstate)->lex = lex;
				((struct DoState *) sem.semstate)->buf = makeStringInfo();
				need_strings = true;
				break;
		}
	}

	if (optind < argc)
	{
		testfile = argv[optind];
		optind++;
	}
	else
	{
		usage(argv[0]);
		exit(1);
	}

	makeJsonLexContextIncremental(lex, PG_UTF8, need_strings);
	setJsonLexContextOwnsTokens(lex, lex_owns_tokens);
	initStringInfo(&json);

	if ((json_file = fopen(testfile, PG_BINARY_R)) == NULL)
		pg_fatal("error opening input: %m");

	if (fstat(fileno(json_file), &statbuf) != 0)
		pg_fatal("error statting input: %m");

	bytes_left = statbuf.st_size;

	for (;;)
	{
		/* We will break when there's nothing left to read */

		if (bytes_left < chunk_size)
			chunk_size = bytes_left;

		n_read = fread(buff, 1, chunk_size, json_file);
		if (n_read < chunk_size)
			pg_fatal("error reading input file: %d", ferror(json_file));

		appendBinaryStringInfo(&json, buff, n_read);

		/*
		 * Append some trailing junk to the buffer passed to the parser. This
		 * helps us ensure that the parser does the right thing even if the
		 * chunk isn't terminated with a '\0'.
		 */
		appendStringInfoString(&json, "1+23 trailing junk");
		bytes_left -= n_read;
		if (bytes_left > 0)
		{
			result = pg_parse_json_incremental(lex, testsem,
											   json.data, n_read,
											   false);
			if (result != JSON_INCOMPLETE)
			{
				fprintf(stderr, "%s\n", json_errdetail(result, lex));
				ret = 1;
				goto cleanup;
			}
			resetStringInfo(&json);
		}
		else
		{
			result = pg_parse_json_incremental(lex, testsem,
											   json.data, n_read,
											   true);
			if (result != JSON_SUCCESS)
			{
				fprintf(stderr, "%s\n", json_errdetail(result, lex));
				ret = 1;
				goto cleanup;
			}
			if (!need_strings)
				printf("SUCCESS!\n");
			break;
		}
	}

cleanup:
	fclose(json_file);
	freeJsonLexContext(lex);
	free(json.data);
	free(lex);

	return ret;
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
	if (!lex_owns_tokens)
		free(fname);

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

	if (!lex_owns_tokens)
		free(token);

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
	fprintf(stderr, "  -c chunksize      size of piece fed to parser (default 64)\n");
	fprintf(stderr, "  -o                set JSONLEX_CTX_OWNS_TOKENS for leak checking\n");
	fprintf(stderr, "  -s                do semantic processing\n");

}
