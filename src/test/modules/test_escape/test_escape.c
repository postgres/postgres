/*
 * test_escape.c Test escape functions
 *
 * Copyright (c) 2022-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_escape/test_escape.c
 */

#include "postgres_fe.h"

#include <string.h>
#include <stdio.h>

#include "common/jsonapi.h"
#include "fe_utils/psqlscan.h"
#include "fe_utils/string_utils.h"
#include "getopt_long.h"
#include "libpq-fe.h"
#include "mb/pg_wchar.h"
#include "utils/memdebug.h"


typedef struct pe_test_config
{
	int			verbosity;
	bool		force_unsupported;
	const char *conninfo;
	PGconn	   *conn;

	int			test_count;
	int			failure_count;
} pe_test_config;

#define NEVER_ACCESS_STR "\xff never-to-be-touched"


/*
 * An escape function to be tested by this test.
 */
typedef struct pe_test_escape_func
{
	const char *name;

	/*
	 * Can the escape method report errors? If so, we validate that it does in
	 * case of various invalid inputs.
	 */
	bool		reports_errors;

	/*
	 * Is the escape method known to not handle invalidly encoded input? If
	 * so, we don't run the test unless --force-unsupported is used.
	 */
	bool		supports_only_valid;

	/*
	 * Is the escape method known to only handle encodings where no byte in a
	 * multi-byte characters are valid ascii.
	 */
	bool		supports_only_ascii_overlap;

	/*
	 * Does the escape function have a length input?
	 */
	bool		supports_input_length;

	bool		(*escape) (PGconn *conn, PQExpBuffer target,
						   const char *unescaped, size_t unescaped_len,
						   PQExpBuffer escape_err);
} pe_test_escape_func;

/*
 * A single test input for this test.
 */
typedef struct pe_test_vector
{
	const char *client_encoding;
	size_t		escape_len;
	const char *escape;
} pe_test_vector;


/*
 * Callback functions from flex lexer. Not currently used by the test.
 */
static const PsqlScanCallbacks test_scan_callbacks = {
	NULL
};


/*
 * Print the string into buf, making characters outside of plain ascii
 * somewhat easier to recognize.
 *
 * The output format could stand to be improved significantly, it's not at all
 * unambiguous.
 */
static void
escapify(PQExpBuffer buf, const char *str, size_t len)
{
	for (size_t i = 0; i < len; i++)
	{
		char		c = *str;

		if (c == '\n')
			appendPQExpBufferStr(buf, "\\n");
		else if (c == '\0')
			appendPQExpBufferStr(buf, "\\0");
		else if (c < ' ' || c > '~')
			appendPQExpBuffer(buf, "\\x%2x", (uint8_t) c);
		else
			appendPQExpBufferChar(buf, c);
		str++;
	}
}

static void
report_result(pe_test_config *tc,
			  bool success,
			  const char *testname,
			  const char *details,
			  const char *subname,
			  const char *resultdesc)
{
	int			test_id = ++tc->test_count;
	bool		print_details = true;
	bool		print_result = true;

	if (success)
	{
		if (tc->verbosity <= 0)
			print_details = false;
		if (tc->verbosity < 0)
			print_result = false;
	}
	else
		tc->failure_count++;

	if (print_details)
		printf("%s", details);

	if (print_result)
		printf("%s %d - %s: %s: %s\n",
			   success ? "ok" : "not ok",
			   test_id, testname,
			   subname,
			   resultdesc);
}

/*
 * Return true for encodings in which bytes in a multi-byte character look
 * like valid ascii characters.
 */
static bool
encoding_conflicts_ascii(int encoding)
{
	/*
	 * We don't store this property directly anywhere, but whether an encoding
	 * is a client-only encoding is a good proxy.
	 */
	if (encoding > PG_ENCODING_BE_LAST)
		return true;
	return false;
}


/*
 * Confirm escaping doesn't read past the end of an allocation.  Consider the
 * result of malloc(4096), in the absence of freelist entries satisfying the
 * allocation.  On OpenBSD, reading one byte past the end of that object
 * yields SIGSEGV.
 *
 * Run this test before the program's other tests, so freelists are minimal.
 * len=4096 didn't SIGSEGV, likely due to free() calls in libpq.  len=8192
 * did.  Use 128 KiB, to somewhat insulate the outcome from distant new free()
 * calls and libc changes.
 */
static void
test_gb18030_page_multiple(pe_test_config *tc)
{
	PQExpBuffer testname;
	size_t		input_len = 0x20000;
	char	   *input;

	/* prepare input */
	input = pg_malloc(input_len);
	memset(input, '-', input_len - 1);
	input[input_len - 1] = 0xfe;

	/* name to describe the test */
	testname = createPQExpBuffer();
	appendPQExpBuffer(testname, ">repeat(%c, %zu)", input[0], input_len - 1);
	escapify(testname, input + input_len - 1, 1);
	appendPQExpBuffer(testname, "< - GB18030 - PQescapeLiteral");

	/* test itself */
	PQsetClientEncoding(tc->conn, "GB18030");
	report_result(tc, PQescapeLiteral(tc->conn, input, input_len) == NULL,
				  testname->data, "",
				  "input validity vs escape success", "ok");

	destroyPQExpBuffer(testname);
	pg_free(input);
}

/*
 * Confirm json parsing doesn't read past the end of an allocation.  This
 * exercises wchar.c infrastructure like the true "escape" tests do, but this
 * isn't an "escape" test.
 */
static void
test_gb18030_json(pe_test_config *tc)
{
	PQExpBuffer raw_buf;
	PQExpBuffer testname;
	const char	input[] = "{\"\\u\xFE";
	size_t		input_len = sizeof(input) - 1;
	JsonLexContext *lex;
	JsonSemAction sem = {0};	/* no callbacks */
	JsonParseErrorType json_error;

	/* prepare input like test_one_vector_escape() does */
	raw_buf = createPQExpBuffer();
	appendBinaryPQExpBuffer(raw_buf, input, input_len);
	appendPQExpBufferStr(raw_buf, NEVER_ACCESS_STR);
	VALGRIND_MAKE_MEM_NOACCESS(&raw_buf->data[input_len],
							   raw_buf->len - input_len);

	/* name to describe the test */
	testname = createPQExpBuffer();
	appendPQExpBuffer(testname, ">");
	escapify(testname, input, input_len);
	appendPQExpBuffer(testname, "< - GB18030 - pg_parse_json");

	/* test itself */
	lex = makeJsonLexContextCstringLen(NULL, raw_buf->data, input_len,
									   PG_GB18030, false);
	json_error = pg_parse_json(lex, &sem);
	report_result(tc, json_error == JSON_UNICODE_ESCAPE_FORMAT,
				  testname->data, "",
				  "diagnosed", json_errdetail(json_error, lex));

	freeJsonLexContext(lex);
	destroyPQExpBuffer(testname);
	destroyPQExpBuffer(raw_buf);
}


static bool
escape_literal(PGconn *conn, PQExpBuffer target,
			   const char *unescaped, size_t unescaped_len,
			   PQExpBuffer escape_err)
{
	char	   *escaped;

	escaped = PQescapeLiteral(conn, unescaped, unescaped_len);
	if (!escaped)
	{
		appendPQExpBufferStr(escape_err, PQerrorMessage(conn));
		escape_err->data[escape_err->len - 1] = 0;
		escape_err->len--;
		return false;
	}
	else
	{
		appendPQExpBufferStr(target, escaped);
		PQfreemem(escaped);
		return true;
	}
}

static bool
escape_identifier(PGconn *conn, PQExpBuffer target,
				  const char *unescaped, size_t unescaped_len,
				  PQExpBuffer escape_err)
{
	char	   *escaped;

	escaped = PQescapeIdentifier(conn, unescaped, unescaped_len);
	if (!escaped)
	{
		appendPQExpBufferStr(escape_err, PQerrorMessage(conn));
		escape_err->data[escape_err->len - 1] = 0;
		escape_err->len--;
		return false;
	}
	else
	{
		appendPQExpBufferStr(target, escaped);
		PQfreemem(escaped);
		return true;
	}
}

static bool
escape_string_conn(PGconn *conn, PQExpBuffer target,
				   const char *unescaped, size_t unescaped_len,
				   PQExpBuffer escape_err)
{
	int			error;
	size_t		sz;

	appendPQExpBufferChar(target, '\'');
	enlargePQExpBuffer(target, unescaped_len * 2 + 1);
	sz = PQescapeStringConn(conn, target->data + target->len,
							unescaped, unescaped_len,
							&error);

	target->len += sz;
	appendPQExpBufferChar(target, '\'');

	if (error)
	{
		appendPQExpBufferStr(escape_err, PQerrorMessage(conn));
		escape_err->data[escape_err->len - 1] = 0;
		escape_err->len--;
		return false;
	}
	else
	{
		return true;
	}
}

static bool
escape_string(PGconn *conn, PQExpBuffer target,
			  const char *unescaped, size_t unescaped_len,
			  PQExpBuffer escape_err)
{
	size_t		sz;

	appendPQExpBufferChar(target, '\'');
	enlargePQExpBuffer(target, unescaped_len * 2 + 1);
	sz = PQescapeString(target->data + target->len,
						unescaped, unescaped_len);
	target->len += sz;
	appendPQExpBufferChar(target, '\'');


	return true;
}

/*
 * Escape via s/'/''/.  Non-core drivers invariably wrap libpq or use this
 * method.  It suffices iff the input passes encoding validation, so it's
 * marked as supports_only_valid.
 */
static bool
escape_replace(PGconn *conn, PQExpBuffer target,
			   const char *unescaped, size_t unescaped_len,
			   PQExpBuffer escape_err)
{
	const char *s = unescaped;

	appendPQExpBufferChar(target, '\'');

	for (int i = 0; i < unescaped_len; i++)
	{
		char		c = *s;

		if (c == '\'')
		{
			appendPQExpBufferStr(target, "''");
		}
		else
			appendPQExpBufferChar(target, c);
		s++;
	}
	appendPQExpBufferChar(target, '\'');

	return true;
}

static bool
escape_append_literal(PGconn *conn, PQExpBuffer target,
					  const char *unescaped, size_t unescaped_len,
					  PQExpBuffer escape_err)
{
	appendStringLiteral(target, unescaped, PQclientEncoding(conn), 1);

	return true;
}

static bool
escape_fmt_id(PGconn *conn, PQExpBuffer target,
			  const char *unescaped, size_t unescaped_len,
			  PQExpBuffer escape_err)
{
	setFmtEncoding(PQclientEncoding(conn));
	appendPQExpBufferStr(target, fmtId(unescaped));

	return true;
}

static pe_test_escape_func pe_test_escape_funcs[] =
{
	{
		.name = "PQescapeLiteral",
		.reports_errors = true,
		.supports_input_length = true,
		.escape = escape_literal,
	},
	{
		.name = "PQescapeIdentifier",
		.reports_errors = true,
		.supports_input_length = true,
		.escape = escape_identifier
	},
	{
		.name = "PQescapeStringConn",
		.reports_errors = true,
		.supports_input_length = true,
		.escape = escape_string_conn
	},
	{
		.name = "PQescapeString",
		.reports_errors = false,
		.supports_input_length = true,
		.escape = escape_string
	},
	{
		.name = "replace",
		.reports_errors = false,
		.supports_only_valid = true,
		.supports_only_ascii_overlap = true,
		.supports_input_length = true,
		.escape = escape_replace
	},
	{
		.name = "appendStringLiteral",
		.reports_errors = false,
		.escape = escape_append_literal
	},
	{
		.name = "fmtId",
		.reports_errors = false,
		.escape = escape_fmt_id
	},
};


#define TV(enc, string) {.client_encoding = (enc), .escape=string, .escape_len=sizeof(string) - 1, }
#define TV_LEN(enc, string, len) {.client_encoding = (enc), .escape=string, .escape_len=len, }
static pe_test_vector pe_test_vectors[] =
{
	/* expected to work sanity checks */
	TV("UTF-8", "1"),
	TV("UTF-8", "'"),
	TV("UTF-8", "\""),

	TV("UTF-8", "\'"),
	TV("UTF-8", "\""),

	TV("UTF-8", "\\"),

	TV("UTF-8", "\\'"),
	TV("UTF-8", "\\\""),

	/* trailing multi-byte character, paddable in available space */
	TV("UTF-8", "1\xC0"),
	TV("UTF-8", "1\xE0 "),
	TV("UTF-8", "1\xF0 "),
	TV("UTF-8", "1\xF0  "),
	TV("UTF-8", "1\xF0   "),

	/* trailing multi-byte character, not enough space to pad */
	TV("UTF-8", "1\xE0"),
	TV("UTF-8", "1\xF0"),
	TV("UTF-8", "\xF0"),

	/* try to smuggle in something in invalid characters */
	TV("UTF-8", "1\xE0'"),
	TV("UTF-8", "1\xE0\""),
	TV("UTF-8", "1\xF0'"),
	TV("UTF-8", "1\xF0\""),
	TV("UTF-8", "1\xF0'; "),
	TV("UTF-8", "1\xF0\"; "),
	TV("UTF-8", "1\xF0';;;;"),
	TV("UTF-8", "1\xF0  ';;;;"),
	TV("UTF-8", "1\xF0  \";;;;"),
	TV("UTF-8", "1\xE0'; \\l ; "),
	TV("UTF-8", "1\xE0\"; \\l ; "),

	/* null byte handling */
	TV("UTF-8", "some\0thing"),
	TV("UTF-8", "some\0"),
	TV("UTF-8", "some\xF0'\0"),
	TV("UTF-8", "some\xF0'\0'"),
	TV("UTF-8", "some\xF0" "ab\0'"),

	/* GB18030's 4 byte encoding requires a 2nd byte limited values */
	TV("GB18030", "\x90\x31"),
	TV("GB18030", "\\\x81\x5c'"),
	TV("GB18030", "\\\x81\x5c\""),
	TV("GB18030", "\\\x81\x5c\0'"),

	/*
	 * \x81 indicates a 2 byte char. ' and " are not a valid second byte, but
	 * that requires encoding verification to know. E.g. replace_string()
	 * doesn't cope.
	 */
	TV("GB18030", "\\\x81';"),
	TV("GB18030", "\\\x81\";"),

	/*
	 * \x81 indicates a 2 byte char. \ is a valid second character.
	 */
	TV("GB18030", "\\\x81\\';"),
	TV("GB18030", "\\\x81\\\";"),
	TV("GB18030", "\\\x81\0;"),
	TV("GB18030", "\\\x81\0'"),
	TV("GB18030", "\\\x81'\0"),

	TV("SJIS", "\xF0\x40;"),

	TV("SJIS", "\xF0';"),
	TV("SJIS", "\xF0\";"),
	TV("SJIS", "\xF0\0'"),
	TV("SJIS", "\\\xF0\\';"),
	TV("SJIS", "\\\xF0\\\";"),

	TV("gbk", "\x80';"),
	TV("gbk", "\x80"),
	TV("gbk", "\x80'"),
	TV("gbk", "\x80\""),
	TV("gbk", "\x80\\"),

	TV("mule_internal", "\\\x9c';\0;"),

	TV("sql_ascii", "1\xC0'"),

	/*
	 * Testcases that are not null terminated for the specified input length.
	 * That's interesting to verify that escape functions don't read beyond
	 * the intended input length.
	 *
	 * One interesting special case is GB18030, which has the odd behaviour
	 * needing to read beyond the first byte to determine the length of a
	 * multi-byte character.
	 */
	TV_LEN("gbk", "\x80", 1),
	TV_LEN("GB18030", "\x80", 1),
	TV_LEN("GB18030", "\x80\0", 2),
	TV_LEN("GB18030", "\x80\x30", 2),
	TV_LEN("GB18030", "\x80\x30\0", 3),
	TV_LEN("GB18030", "\x80\x30\x30", 3),
	TV_LEN("GB18030", "\x80\x30\x30\0", 4),
	TV_LEN("UTF-8", "\xC3\xb6  ", 1),
	TV_LEN("UTF-8", "\xC3\xb6  ", 2),
};


static const char *
scan_res_s(PsqlScanResult res)
{
#define TOSTR_CASE(sym) case sym: return #sym

	switch (res)
	{
			TOSTR_CASE(PSCAN_SEMICOLON);
			TOSTR_CASE(PSCAN_BACKSLASH);
			TOSTR_CASE(PSCAN_INCOMPLETE);
			TOSTR_CASE(PSCAN_EOL);
	}

	pg_unreachable();
	return "";					/* silence compiler */
}

/*
 * Verify that psql parses the input as a single statement. If this property
 * is violated, the escape function does not effectively protect against
 * smuggling in a second statement.
 */
static void
test_psql_parse(pe_test_config *tc, PQExpBuffer testname,
				PQExpBuffer input_buf, PQExpBuffer details)
{
	PsqlScanState scan_state;
	PsqlScanResult scan_result;
	PQExpBuffer query_buf;
	promptStatus_t prompt_status = PROMPT_READY;
	int			matches = 0;
	bool		test_fails;
	const char *resdesc;

	query_buf = createPQExpBuffer();

	scan_state = psql_scan_create(&test_scan_callbacks);

	/*
	 * TODO: This hardcodes standard conforming strings, it would be useful to
	 * test without as well.
	 */
	psql_scan_setup(scan_state, input_buf->data, input_buf->len,
					PQclientEncoding(tc->conn), 1);

	do
	{
		resetPQExpBuffer(query_buf);

		scan_result = psql_scan(scan_state, query_buf,
								&prompt_status);

		appendPQExpBuffer(details,
						  "#\t\t %d: scan_result: %s prompt: %u, query_buf: ",
						  matches, scan_res_s(scan_result), prompt_status);
		escapify(details, query_buf->data, query_buf->len);
		appendPQExpBufferChar(details, '\n');

		matches++;
	}
	while (scan_result != PSCAN_INCOMPLETE && scan_result != PSCAN_EOL);

	psql_scan_destroy(scan_state);
	destroyPQExpBuffer(query_buf);

	test_fails = matches > 1 || scan_result != PSCAN_EOL;

	if (matches > 1)
		resdesc = "more than one match";
	else if (scan_result != PSCAN_EOL)
		resdesc = "unexpected end state";
	else
		resdesc = "ok";

	report_result(tc, !test_fails, testname->data, details->data,
				  "psql parse",
				  resdesc);
}

static void
test_one_vector_escape(pe_test_config *tc, const pe_test_vector *tv, const pe_test_escape_func *ef)
{
	PQExpBuffer testname;
	PQExpBuffer details;
	PQExpBuffer raw_buf;
	PQExpBuffer escape_buf;
	PQExpBuffer escape_err;
	size_t		input_encoding_validlen;
	bool		input_encoding_valid;
	size_t		input_encoding0_validlen;
	bool		input_encoding0_valid;
	bool		escape_success;
	size_t		escape_encoding_length;
	bool		escape_encoding_valid;

	escape_err = createPQExpBuffer();
	testname = createPQExpBuffer();
	details = createPQExpBuffer();
	raw_buf = createPQExpBuffer();
	escape_buf = createPQExpBuffer();

	if (ef->supports_only_ascii_overlap &&
		encoding_conflicts_ascii(PQclientEncoding(tc->conn)))
	{
		goto out;
	}

	/* name to describe the test */
	appendPQExpBufferChar(testname, '>');
	escapify(testname, tv->escape, tv->escape_len);
	appendPQExpBuffer(testname, "< - %s - %s",
					  tv->client_encoding, ef->name);

	/* details to describe the test, to allow for debugging */
	appendPQExpBuffer(details, "#\t input: %zd bytes: ",
					  tv->escape_len);
	escapify(details, tv->escape, tv->escape_len);
	appendPQExpBufferChar(details, '\n');
	appendPQExpBuffer(details, "#\t encoding: %s\n",
					  tv->client_encoding);


	/* check encoding of input, to compare with after the test */
	input_encoding_validlen = pg_encoding_verifymbstr(PQclientEncoding(tc->conn),
													  tv->escape,
													  tv->escape_len);
	input_encoding_valid = input_encoding_validlen == tv->escape_len;
	appendPQExpBuffer(details, "#\t input encoding valid: %d\n",
					  input_encoding_valid);

	input_encoding0_validlen = pg_encoding_verifymbstr(PQclientEncoding(tc->conn),
													   tv->escape,
													   strnlen(tv->escape, tv->escape_len));
	input_encoding0_valid = input_encoding0_validlen == strnlen(tv->escape, tv->escape_len);
	appendPQExpBuffer(details, "#\t input encoding valid till 0: %d\n",
					  input_encoding0_valid);

	appendPQExpBuffer(details, "#\t escape func: %s\n",
					  ef->name);

	if (!input_encoding_valid && ef->supports_only_valid
		&& !tc->force_unsupported)
		goto out;


	/*
	 * Put the to-be-escaped data into a buffer, so that we
	 *
	 * a) can mark memory beyond end of the string as inaccessible when using
	 * valgrind
	 *
	 * b) can append extra data beyond the length passed to the escape
	 * function, to verify that that data is not processed.
	 *
	 * TODO: Should we instead/additionally escape twice, once with unmodified
	 * and once with appended input? That way we could compare the two.
	 */
	appendBinaryPQExpBuffer(raw_buf, tv->escape, tv->escape_len);

	if (ef->supports_input_length)
	{
		/*
		 * Append likely invalid string that does *not* contain a null byte
		 * (which'd prevent some invalid accesses to later memory).
		 */
		appendPQExpBufferStr(raw_buf, NEVER_ACCESS_STR);

		VALGRIND_MAKE_MEM_NOACCESS(&raw_buf->data[tv->escape_len],
								   raw_buf->len - tv->escape_len);
	}
	else
	{
		/* append invalid string, after \0 */
		appendPQExpBufferChar(raw_buf, 0);
		appendPQExpBufferStr(raw_buf, NEVER_ACCESS_STR);

		VALGRIND_MAKE_MEM_NOACCESS(&raw_buf->data[tv->escape_len + 1],
								   raw_buf->len - tv->escape_len - 1);
	}

	/* call the to-be-tested escape function */
	escape_success = ef->escape(tc->conn, escape_buf,
								raw_buf->data, tv->escape_len,
								escape_err);
	if (!escape_success)
	{
		appendPQExpBuffer(details, "#\t escape error: %s\n",
						  escape_err->data);
	}

	if (escape_buf->len > 0)
	{
		bool		contains_never;

		appendPQExpBuffer(details, "#\t escaped string: %zd bytes: ", escape_buf->len);
		escapify(details, escape_buf->data, escape_buf->len);
		appendPQExpBufferChar(details, '\n');

		escape_encoding_length = pg_encoding_verifymbstr(PQclientEncoding(tc->conn),
														 escape_buf->data,
														 escape_buf->len);
		escape_encoding_valid = escape_encoding_length == escape_buf->len;

		appendPQExpBuffer(details, "#\t escape encoding valid: %d\n",
						  escape_encoding_valid);

		/*
		 * Verify that no data beyond the end of the input is included in the
		 * escaped string.  It'd be better to use something like memmem()
		 * here, but that's not available everywhere.
		 */
		contains_never = strstr(escape_buf->data, NEVER_ACCESS_STR) == NULL;
		report_result(tc, contains_never, testname->data, details->data,
					  "escaped data beyond end of input",
					  contains_never ? "no" : "all secrets revealed");
	}
	else
	{
		escape_encoding_length = 0;
		escape_encoding_valid = 1;
	}

	/*
	 * If the test reports errors, and the input was invalidly encoded,
	 * escaping should fail.  One edge-case that we accept for now is that the
	 * input could have an embedded null byte, which the escape functions will
	 * just treat as a shorter string. If the encoding error is after the zero
	 * byte, the output thus won't contain it.
	 */
	if (ef->reports_errors)
	{
		bool		ok = true;
		const char *resdesc = "ok";

		if (escape_success)
		{
			if (!input_encoding0_valid)
			{
				ok = false;
				resdesc = "invalid input escaped successfully";
			}
			else if (!input_encoding_valid)
				resdesc = "invalid input escaped successfully, due to zero byte";
		}
		else
		{
			if (input_encoding0_valid)
			{
				ok = false;
				resdesc = "valid input failed to escape";
			}
			else if (input_encoding_valid)
				resdesc = "valid input failed to escape, due to zero byte";
		}

		report_result(tc, ok, testname->data, details->data,
					  "input validity vs escape success",
					  resdesc);
	}

	/*
	 * If the input is invalidly encoded, the output should also be invalidly
	 * encoded. We accept the same zero-byte edge case as above.
	 */
	{
		bool		ok = true;
		const char *resdesc = "ok";

		if (input_encoding0_valid && !input_encoding_valid && escape_encoding_valid)
		{
			resdesc = "invalid input produced valid output, due to zero byte";
		}
		else if (input_encoding0_valid && !escape_encoding_valid)
		{
			ok = false;
			resdesc = "valid input produced invalid output";
		}
		else if (!input_encoding0_valid &&
				 (!ef->reports_errors || escape_success) &&
				 escape_encoding_valid)
		{
			ok = false;
			resdesc = "invalid input produced valid output";
		}

		report_result(tc, ok, testname->data, details->data,
					  "input and escaped encoding validity",
					  resdesc);
	}

	/*
	 * Test psql parsing whenever we get any string back, even if the escape
	 * function returned a failure.
	 */
	if (escape_buf->len > 0)
	{
		test_psql_parse(tc, testname,
						escape_buf, details);
	}

out:
	destroyPQExpBuffer(escape_err);
	destroyPQExpBuffer(details);
	destroyPQExpBuffer(testname);
	destroyPQExpBuffer(escape_buf);
	destroyPQExpBuffer(raw_buf);
}

static void
test_one_vector(pe_test_config *tc, const pe_test_vector *tv)
{
	if (PQsetClientEncoding(tc->conn, tv->client_encoding))
	{
		fprintf(stderr, "failed to set encoding to %s:\n%s\n",
				tv->client_encoding, PQerrorMessage(tc->conn));
		exit(1);
	}

	for (int escoff = 0; escoff < lengthof(pe_test_escape_funcs); escoff++)
	{
		const pe_test_escape_func *ef = &pe_test_escape_funcs[escoff];

		test_one_vector_escape(tc, tv, ef);
	}
}

static void
usage(const char *hint)
{
	if (hint)
		fprintf(stderr, "Error: %s\n\n", hint);

	printf("PostgreSQL escape function test\n"
		   "\n"
		   "Usage:\n"
		   "  test_escape --conninfo=CONNINFO [OPTIONS]\n"
		   "\n"
		   "Options:\n"
		   "  -h, --help                show this help\n"
		   "  -c, --conninfo=CONNINFO   connection information to use\n"
		   "  -v, --verbose             show test details even for successes\n"
		   "  -q, --quiet               only show failures\n"
		   "  -f, --force-unsupported   test invalid input even if unsupported\n"
		);

	if (hint)
		exit(1);
}

int
main(int argc, char *argv[])
{
	pe_test_config tc = {0};
	int			c;
	int			option_index;

	static const struct option long_options[] = {
		{"help", no_argument, NULL, 'h'},
		{"conninfo", required_argument, NULL, 'c'},
		{"verbose", no_argument, NULL, 'v'},
		{"quiet", no_argument, NULL, 'q'},
		{"force-unsupported", no_argument, NULL, 'f'},
		{NULL, 0, NULL, 0},
	};

	while ((c = getopt_long(argc, argv, "c:fhqv", long_options, &option_index)) != -1)
	{
		switch (c)
		{
			case 'h':
				usage(NULL);
				exit(0);
				break;
			case 'c':
				tc.conninfo = optarg;
				break;
			case 'v':
				tc.verbosity++;
				break;
			case 'q':
				tc.verbosity--;
				break;
			case 'f':
				tc.force_unsupported = true;
				break;
		}
	}

	if (argc - optind >= 1)
		usage("unused option(s) specified");

	if (tc.conninfo == NULL)
		usage("--conninfo needs to be specified");

	tc.conn = PQconnectdb(tc.conninfo);

	if (!tc.conn || PQstatus(tc.conn) != CONNECTION_OK)
	{
		fprintf(stderr, "could not connect: %s\n",
				PQerrorMessage(tc.conn));
		exit(1);
	}

	test_gb18030_page_multiple(&tc);
	test_gb18030_json(&tc);

	for (int i = 0; i < lengthof(pe_test_vectors); i++)
	{
		test_one_vector(&tc, &pe_test_vectors[i]);
	}

	PQfinish(tc.conn);

	printf("# %d failures\n", tc.failure_count);
	printf("1..%d\n", tc.test_count);
	return tc.failure_count > 0;
}
