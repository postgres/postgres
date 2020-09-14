/*-------------------------------------------------------------------------
 *
 * parse_manifest.c
 *	  Parse a backup manifest in JSON format.
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pg_verifybackup/parse_manifest.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "parse_manifest.h"
#include "common/jsonapi.h"

/*
 * Semantic states for JSON manifest parsing.
 */
typedef enum
{
	JM_EXPECT_TOPLEVEL_START,
	JM_EXPECT_TOPLEVEL_END,
	JM_EXPECT_TOPLEVEL_FIELD,
	JM_EXPECT_VERSION_VALUE,
	JM_EXPECT_FILES_START,
	JM_EXPECT_FILES_NEXT,
	JM_EXPECT_THIS_FILE_FIELD,
	JM_EXPECT_THIS_FILE_VALUE,
	JM_EXPECT_WAL_RANGES_START,
	JM_EXPECT_WAL_RANGES_NEXT,
	JM_EXPECT_THIS_WAL_RANGE_FIELD,
	JM_EXPECT_THIS_WAL_RANGE_VALUE,
	JM_EXPECT_MANIFEST_CHECKSUM_VALUE,
	JM_EXPECT_EOF
} JsonManifestSemanticState;

/*
 * Possible fields for one file as described by the manifest.
 */
typedef enum
{
	JMFF_PATH,
	JMFF_ENCODED_PATH,
	JMFF_SIZE,
	JMFF_LAST_MODIFIED,
	JMFF_CHECKSUM_ALGORITHM,
	JMFF_CHECKSUM
} JsonManifestFileField;

/*
 * Possible fields for one file as described by the manifest.
 */
typedef enum
{
	JMWRF_TIMELINE,
	JMWRF_START_LSN,
	JMWRF_END_LSN
} JsonManifestWALRangeField;

/*
 * Internal state used while decoding the JSON-format backup manifest.
 */
typedef struct
{
	JsonManifestParseContext *context;
	JsonManifestSemanticState state;

	/* These fields are used for parsing objects in the list of files. */
	JsonManifestFileField file_field;
	char	   *pathname;
	char	   *encoded_pathname;
	char	   *size;
	char	   *algorithm;
	pg_checksum_type checksum_algorithm;
	char	   *checksum;

	/* These fields are used for parsing objects in the list of WAL ranges. */
	JsonManifestWALRangeField wal_range_field;
	char	   *timeline;
	char	   *start_lsn;
	char	   *end_lsn;

	/* Miscellaneous other stuff. */
	bool		saw_version_field;
	char	   *manifest_checksum;
} JsonManifestParseState;

static void json_manifest_object_start(void *state);
static void json_manifest_object_end(void *state);
static void json_manifest_array_start(void *state);
static void json_manifest_array_end(void *state);
static void json_manifest_object_field_start(void *state, char *fname,
											 bool isnull);
static void json_manifest_scalar(void *state, char *token,
								 JsonTokenType tokentype);
static void json_manifest_finalize_file(JsonManifestParseState *parse);
static void json_manifest_finalize_wal_range(JsonManifestParseState *parse);
static void verify_manifest_checksum(JsonManifestParseState *parse,
									 char *buffer, size_t size);
static void json_manifest_parse_failure(JsonManifestParseContext *context,
										char *msg);

static int	hexdecode_char(char c);
static bool hexdecode_string(uint8 *result, char *input, int nbytes);
static bool parse_xlogrecptr(XLogRecPtr *result, char *input);

/*
 * Main entrypoint to parse a JSON-format backup manifest.
 *
 * Caller should set up the parsing context and then invoke this function.
 * For each file whose information is extracted from the manifest,
 * context->perfile_cb is invoked.  In case of trouble, context->error_cb is
 * invoked and is expected not to return.
 */
void
json_parse_manifest(JsonManifestParseContext *context, char *buffer,
					size_t size)
{
	JsonLexContext *lex;
	JsonParseErrorType json_error;
	JsonSemAction sem;
	JsonManifestParseState parse;

	/* Set up our private parsing context. */
	parse.context = context;
	parse.state = JM_EXPECT_TOPLEVEL_START;
	parse.saw_version_field = false;

	/* Create a JSON lexing context. */
	lex = makeJsonLexContextCstringLen(buffer, size, PG_UTF8, true);

	/* Set up semantic actions. */
	sem.semstate = &parse;
	sem.object_start = json_manifest_object_start;
	sem.object_end = json_manifest_object_end;
	sem.array_start = json_manifest_array_start;
	sem.array_end = json_manifest_array_end;
	sem.object_field_start = json_manifest_object_field_start;
	sem.object_field_end = NULL;
	sem.array_element_start = NULL;
	sem.array_element_end = NULL;
	sem.scalar = json_manifest_scalar;

	/* Run the actual JSON parser. */
	json_error = pg_parse_json(lex, &sem);
	if (json_error != JSON_SUCCESS)
		json_manifest_parse_failure(context, json_errdetail(json_error, lex));
	if (parse.state != JM_EXPECT_EOF)
		json_manifest_parse_failure(context, "manifest ended unexpectedly");

	/* Verify the manifest checksum. */
	verify_manifest_checksum(&parse, buffer, size);
}

/*
 * Invoked at the start of each object in the JSON document.
 *
 * The document as a whole is expected to be an object; each file and each
 * WAL range is also expected to be an object. If we're anywhere else in the
 * document, it's an error.
 */
static void
json_manifest_object_start(void *state)
{
	JsonManifestParseState *parse = state;

	switch (parse->state)
	{
		case JM_EXPECT_TOPLEVEL_START:
			parse->state = JM_EXPECT_TOPLEVEL_FIELD;
			break;
		case JM_EXPECT_FILES_NEXT:
			parse->state = JM_EXPECT_THIS_FILE_FIELD;
			parse->pathname = NULL;
			parse->encoded_pathname = NULL;
			parse->size = NULL;
			parse->algorithm = NULL;
			parse->checksum = NULL;
			break;
		case JM_EXPECT_WAL_RANGES_NEXT:
			parse->state = JM_EXPECT_THIS_WAL_RANGE_FIELD;
			parse->timeline = NULL;
			parse->start_lsn = NULL;
			parse->end_lsn = NULL;
			break;
		default:
			json_manifest_parse_failure(parse->context,
										"unexpected object start");
			break;
	}
}

/*
 * Invoked at the end of each object in the JSON document.
 *
 * The possible cases here are the same as for json_manifest_object_start.
 * There's nothing special to do at the end of the document, but when we
 * reach the end of an object representing a particular file or WAL range,
 * we must call json_manifest_finalize_file() to save the associated details.
 */
static void
json_manifest_object_end(void *state)
{
	JsonManifestParseState *parse = state;

	switch (parse->state)
	{
		case JM_EXPECT_TOPLEVEL_END:
			parse->state = JM_EXPECT_EOF;
			break;
		case JM_EXPECT_THIS_FILE_FIELD:
			json_manifest_finalize_file(parse);
			parse->state = JM_EXPECT_FILES_NEXT;
			break;
		case JM_EXPECT_THIS_WAL_RANGE_FIELD:
			json_manifest_finalize_wal_range(parse);
			parse->state = JM_EXPECT_WAL_RANGES_NEXT;
			break;
		default:
			json_manifest_parse_failure(parse->context,
										"unexpected object end");
			break;
	}
}

/*
 * Invoked at the start of each array in the JSON document.
 *
 * Within the toplevel object, the value associated with the "Files" key
 * should be an array. Similarly for the "WAL-Ranges" key. No other arrays
 * are expected.
 */
static void
json_manifest_array_start(void *state)
{
	JsonManifestParseState *parse = state;

	switch (parse->state)
	{
		case JM_EXPECT_FILES_START:
			parse->state = JM_EXPECT_FILES_NEXT;
			break;
		case JM_EXPECT_WAL_RANGES_START:
			parse->state = JM_EXPECT_WAL_RANGES_NEXT;
			break;
		default:
			json_manifest_parse_failure(parse->context,
										"unexpected array start");
			break;
	}
}

/*
 * Invoked at the end of each array in the JSON document.
 *
 * The cases here are analogous to those in json_manifest_array_start.
 */
static void
json_manifest_array_end(void *state)
{
	JsonManifestParseState *parse = state;

	switch (parse->state)
	{
		case JM_EXPECT_FILES_NEXT:
		case JM_EXPECT_WAL_RANGES_NEXT:
			parse->state = JM_EXPECT_TOPLEVEL_FIELD;
			break;
		default:
			json_manifest_parse_failure(parse->context,
										"unexpected array end");
			break;
	}
}

/*
 * Invoked at the start of each object field in the JSON document.
 */
static void
json_manifest_object_field_start(void *state, char *fname, bool isnull)
{
	JsonManifestParseState *parse = state;

	switch (parse->state)
	{
		case JM_EXPECT_TOPLEVEL_FIELD:

			/*
			 * Inside toplevel object. The version indicator should always be
			 * the first field.
			 */
			if (!parse->saw_version_field)
			{
				if (strcmp(fname, "PostgreSQL-Backup-Manifest-Version") != 0)
					json_manifest_parse_failure(parse->context,
												"expected version indicator");
				parse->state = JM_EXPECT_VERSION_VALUE;
				parse->saw_version_field = true;
				break;
			}

			/* Is this the list of files? */
			if (strcmp(fname, "Files") == 0)
			{
				parse->state = JM_EXPECT_FILES_START;
				break;
			}

			/* Is this the list of WAL ranges? */
			if (strcmp(fname, "WAL-Ranges") == 0)
			{
				parse->state = JM_EXPECT_WAL_RANGES_START;
				break;
			}

			/* Is this the manifest checksum? */
			if (strcmp(fname, "Manifest-Checksum") == 0)
			{
				parse->state = JM_EXPECT_MANIFEST_CHECKSUM_VALUE;
				break;
			}

			/* It's not a field we recognize. */
			json_manifest_parse_failure(parse->context,
										"unrecognized top-level field");
			break;

		case JM_EXPECT_THIS_FILE_FIELD:
			/* Inside object for one file; which key have we got? */
			if (strcmp(fname, "Path") == 0)
				parse->file_field = JMFF_PATH;
			else if (strcmp(fname, "Encoded-Path") == 0)
				parse->file_field = JMFF_ENCODED_PATH;
			else if (strcmp(fname, "Size") == 0)
				parse->file_field = JMFF_SIZE;
			else if (strcmp(fname, "Last-Modified") == 0)
				parse->file_field = JMFF_LAST_MODIFIED;
			else if (strcmp(fname, "Checksum-Algorithm") == 0)
				parse->file_field = JMFF_CHECKSUM_ALGORITHM;
			else if (strcmp(fname, "Checksum") == 0)
				parse->file_field = JMFF_CHECKSUM;
			else
				json_manifest_parse_failure(parse->context,
											"unexpected file field");
			parse->state = JM_EXPECT_THIS_FILE_VALUE;
			break;

		case JM_EXPECT_THIS_WAL_RANGE_FIELD:
			/* Inside object for one file; which key have we got? */
			if (strcmp(fname, "Timeline") == 0)
				parse->wal_range_field = JMWRF_TIMELINE;
			else if (strcmp(fname, "Start-LSN") == 0)
				parse->wal_range_field = JMWRF_START_LSN;
			else if (strcmp(fname, "End-LSN") == 0)
				parse->wal_range_field = JMWRF_END_LSN;
			else
				json_manifest_parse_failure(parse->context,
											"unexpected WAL range field");
			parse->state = JM_EXPECT_THIS_WAL_RANGE_VALUE;
			break;

		default:
			json_manifest_parse_failure(parse->context,
										"unexpected object field");
			break;
	}
}

/*
 * Invoked at the start of each scalar in the JSON document.
 *
 * Object field names don't reach this code; those are handled by
 * json_manifest_object_field_start. When we're inside of the object for
 * a particular file or WAL range, that function will have noticed the name
 * of the field, and we'll get the corresponding value here. When we're in
 * the toplevel object, the parse state itself tells us which field this is.
 *
 * In all cases except for PostgreSQL-Backup-Manifest-Version, which we
 * can just check on the spot, the goal here is just to save the value in
 * the parse state for later use. We don't actually do anything until we
 * reach either the end of the object representing this file, or the end
 * of the manifest, as the case may be.
 */
static void
json_manifest_scalar(void *state, char *token, JsonTokenType tokentype)
{
	JsonManifestParseState *parse = state;

	switch (parse->state)
	{
		case JM_EXPECT_VERSION_VALUE:
			if (strcmp(token, "1") != 0)
				json_manifest_parse_failure(parse->context,
											"unexpected manifest version");
			parse->state = JM_EXPECT_TOPLEVEL_FIELD;
			break;

		case JM_EXPECT_THIS_FILE_VALUE:
			switch (parse->file_field)
			{
				case JMFF_PATH:
					parse->pathname = token;
					break;
				case JMFF_ENCODED_PATH:
					parse->encoded_pathname = token;
					break;
				case JMFF_SIZE:
					parse->size = token;
					break;
				case JMFF_LAST_MODIFIED:
					pfree(token);	/* unused */
					break;
				case JMFF_CHECKSUM_ALGORITHM:
					parse->algorithm = token;
					break;
				case JMFF_CHECKSUM:
					parse->checksum = token;
					break;
			}
			parse->state = JM_EXPECT_THIS_FILE_FIELD;
			break;

		case JM_EXPECT_THIS_WAL_RANGE_VALUE:
			switch (parse->wal_range_field)
			{
				case JMWRF_TIMELINE:
					parse->timeline = token;
					break;
				case JMWRF_START_LSN:
					parse->start_lsn = token;
					break;
				case JMWRF_END_LSN:
					parse->end_lsn = token;
					break;
			}
			parse->state = JM_EXPECT_THIS_WAL_RANGE_FIELD;
			break;

		case JM_EXPECT_MANIFEST_CHECKSUM_VALUE:
			parse->state = JM_EXPECT_TOPLEVEL_END;
			parse->manifest_checksum = token;
			break;

		default:
			json_manifest_parse_failure(parse->context, "unexpected scalar");
			break;
	}
}

/*
 * Do additional parsing and sanity-checking of the details gathered for one
 * file, and invoke the per-file callback so that the caller gets those
 * details. This happens for each file when the corresponding JSON object is
 * completely parsed.
 */
static void
json_manifest_finalize_file(JsonManifestParseState *parse)
{
	JsonManifestParseContext *context = parse->context;
	size_t		size;
	char	   *ep;
	int			checksum_string_length;
	pg_checksum_type checksum_type;
	int			checksum_length;
	uint8	   *checksum_payload;

	/* Pathname and size are required. */
	if (parse->pathname == NULL && parse->encoded_pathname == NULL)
		json_manifest_parse_failure(parse->context, "missing path name");
	if (parse->pathname != NULL && parse->encoded_pathname != NULL)
		json_manifest_parse_failure(parse->context,
									"both path name and encoded path name");
	if (parse->size == NULL)
		json_manifest_parse_failure(parse->context, "missing size");
	if (parse->algorithm == NULL && parse->checksum != NULL)
		json_manifest_parse_failure(parse->context,
									"checksum without algorithm");

	/* Decode encoded pathname, if that's what we have. */
	if (parse->encoded_pathname != NULL)
	{
		int			encoded_length = strlen(parse->encoded_pathname);
		int			raw_length = encoded_length / 2;

		parse->pathname = palloc(raw_length + 1);
		if (encoded_length % 2 != 0 ||
			!hexdecode_string((uint8 *) parse->pathname,
							  parse->encoded_pathname,
							  raw_length))
			json_manifest_parse_failure(parse->context,
										"could not decode file name");
		parse->pathname[raw_length] = '\0';
		pfree(parse->encoded_pathname);
		parse->encoded_pathname = NULL;
	}

	/* Parse size. */
	size = strtoul(parse->size, &ep, 10);
	if (*ep)
		json_manifest_parse_failure(parse->context,
									"file size is not an integer");

	/* Parse the checksum algorithm, if it's present. */
	if (parse->algorithm == NULL)
		checksum_type = CHECKSUM_TYPE_NONE;
	else if (!pg_checksum_parse_type(parse->algorithm, &checksum_type))
		context->error_cb(context, "unrecognized checksum algorithm: \"%s\"",
						  parse->algorithm);

	/* Parse the checksum payload, if it's present. */
	checksum_string_length = parse->checksum == NULL ? 0
		: strlen(parse->checksum);
	if (checksum_string_length == 0)
	{
		checksum_length = 0;
		checksum_payload = NULL;
	}
	else
	{
		checksum_length = checksum_string_length / 2;
		checksum_payload = palloc(checksum_length);
		if (checksum_string_length % 2 != 0 ||
			!hexdecode_string(checksum_payload, parse->checksum,
							  checksum_length))
			context->error_cb(context,
							  "invalid checksum for file \"%s\": \"%s\"",
							  parse->pathname, parse->checksum);
	}

	/* Invoke the callback with the details we've gathered. */
	context->perfile_cb(context, parse->pathname, size,
						checksum_type, checksum_length, checksum_payload);

	/* Free memory we no longer need. */
	if (parse->size != NULL)
	{
		pfree(parse->size);
		parse->size = NULL;
	}
	if (parse->algorithm != NULL)
	{
		pfree(parse->algorithm);
		parse->algorithm = NULL;
	}
	if (parse->checksum != NULL)
	{
		pfree(parse->checksum);
		parse->checksum = NULL;
	}
}

/*
 * Do additional parsing and sanity-checking of the details gathered for one
 * WAL range, and invoke the per-WAL-range callback so that the caller gets
 * those details. This happens for each WAL range when the corresponding JSON
 * object is completely parsed.
 */
static void
json_manifest_finalize_wal_range(JsonManifestParseState *parse)
{
	JsonManifestParseContext *context = parse->context;
	TimeLineID	tli;
	XLogRecPtr	start_lsn,
				end_lsn;
	char	   *ep;

	/* Make sure all fields are present. */
	if (parse->timeline == NULL)
		json_manifest_parse_failure(parse->context, "missing timeline");
	if (parse->start_lsn == NULL)
		json_manifest_parse_failure(parse->context, "missing start LSN");
	if (parse->end_lsn == NULL)
		json_manifest_parse_failure(parse->context, "missing end LSN");

	/* Parse timeline. */
	tli = strtoul(parse->timeline, &ep, 10);
	if (*ep)
		json_manifest_parse_failure(parse->context,
									"timeline is not an integer");
	if (!parse_xlogrecptr(&start_lsn, parse->start_lsn))
		json_manifest_parse_failure(parse->context,
									"could not parse start LSN");
	if (!parse_xlogrecptr(&end_lsn, parse->end_lsn))
		json_manifest_parse_failure(parse->context,
									"could not parse end LSN");

	/* Invoke the callback with the details we've gathered. */
	context->perwalrange_cb(context, tli, start_lsn, end_lsn);

	/* Free memory we no longer need. */
	if (parse->timeline != NULL)
	{
		pfree(parse->timeline);
		parse->timeline = NULL;
	}
	if (parse->start_lsn != NULL)
	{
		pfree(parse->start_lsn);
		parse->start_lsn = NULL;
	}
	if (parse->end_lsn != NULL)
	{
		pfree(parse->end_lsn);
		parse->end_lsn = NULL;
	}
}

/*
 * Verify that the manifest checksum is correct.
 *
 * The last line of the manifest file is excluded from the manifest checksum,
 * because the last line is expected to contain the checksum that covers
 * the rest of the file.
 */
static void
verify_manifest_checksum(JsonManifestParseState *parse, char *buffer,
						 size_t size)
{
	JsonManifestParseContext *context = parse->context;
	size_t		i;
	size_t		number_of_newlines = 0;
	size_t		ultimate_newline = 0;
	size_t		penultimate_newline = 0;
	pg_sha256_ctx manifest_ctx;
	uint8		manifest_checksum_actual[PG_SHA256_DIGEST_LENGTH];
	uint8		manifest_checksum_expected[PG_SHA256_DIGEST_LENGTH];

	/* Find the last two newlines in the file. */
	for (i = 0; i < size; ++i)
	{
		if (buffer[i] == '\n')
		{
			++number_of_newlines;
			penultimate_newline = ultimate_newline;
			ultimate_newline = i;
		}
	}

	/*
	 * Make sure that the last newline is right at the end, and that there are
	 * at least two lines total. We need this to be true in order for the
	 * following code, which computes the manifest checksum, to work properly.
	 */
	if (number_of_newlines < 2)
		json_manifest_parse_failure(parse->context,
									"expected at least 2 lines");
	if (ultimate_newline != size - 1)
		json_manifest_parse_failure(parse->context,
									"last line not newline-terminated");

	/* Checksum the rest. */
	pg_sha256_init(&manifest_ctx);
	pg_sha256_update(&manifest_ctx, (uint8 *) buffer, penultimate_newline + 1);
	pg_sha256_final(&manifest_ctx, manifest_checksum_actual);

	/* Now verify it. */
	if (parse->manifest_checksum == NULL)
		context->error_cb(parse->context, "manifest has no checksum");
	if (strlen(parse->manifest_checksum) != PG_SHA256_DIGEST_LENGTH * 2 ||
		!hexdecode_string(manifest_checksum_expected, parse->manifest_checksum,
						  PG_SHA256_DIGEST_LENGTH))
		context->error_cb(context, "invalid manifest checksum: \"%s\"",
						  parse->manifest_checksum);
	if (memcmp(manifest_checksum_actual, manifest_checksum_expected,
			   PG_SHA256_DIGEST_LENGTH) != 0)
		context->error_cb(context, "manifest checksum mismatch");
}

/*
 * Report a parse error.
 *
 * This is intended to be used for fairly low-level failures that probably
 * shouldn't occur unless somebody has deliberately constructed a bad manifest,
 * or unless the server is generating bad manifests due to some bug. msg should
 * be a short string giving some hint as to what the problem is.
 */
static void
json_manifest_parse_failure(JsonManifestParseContext *context, char *msg)
{
	context->error_cb(context, "could not parse backup manifest: %s", msg);
}

/*
 * Convert a character which represents a hexadecimal digit to an integer.
 *
 * Returns -1 if the character is not a hexadecimal digit.
 */
static int
hexdecode_char(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;

	return -1;
}

/*
 * Decode a hex string into a byte string, 2 hex chars per byte.
 *
 * Returns false if invalid characters are encountered; otherwise true.
 */
static bool
hexdecode_string(uint8 *result, char *input, int nbytes)
{
	int			i;

	for (i = 0; i < nbytes; ++i)
	{
		int			n1 = hexdecode_char(input[i * 2]);
		int			n2 = hexdecode_char(input[i * 2 + 1]);

		if (n1 < 0 || n2 < 0)
			return false;
		result[i] = n1 * 16 + n2;
	}

	return true;
}

/*
 * Parse an XLogRecPtr expressed using the usual string format.
 */
static bool
parse_xlogrecptr(XLogRecPtr *result, char *input)
{
	uint32		hi;
	uint32		lo;

	if (sscanf(input, "%X/%X", &hi, &lo) != 2)
		return false;
	*result = ((uint64) hi) << 32 | lo;
	return true;
}
