/*-------------------------------------------------------------------------
 *
 * tde_keyring_parse_opts.c
 *      Parser routines for the keyring JSON options
 *
 * Each value in the JSON document can be either scalar (string) - a value itself
 * or a reference to the external object that contains the value. Though the top
 * level field "type" can be only scalar.
 *
 * Examples:
 * 	{"type" : "file", "path" : "/tmp/keyring_data_file"}
 * 	{"type" : "file", "path" : {"type" : "file", "path" : "/tmp/datafile-location"}}
 * in the latter one, /tmp/datafile-location contains not keyring data but the
 * location of such.
 *
 * A field type can be "file", in this case, we expect "path" field. Or "remote",
 * when "url" field is expected.
 *
 * IDENTIFICATION
 *    contrib/pg_tde/src/catalog/tde_keyring_parse_opts.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "common/file_perm.h"
#include "common/jsonapi.h"
#include "mb/pg_wchar.h"
#include "storage/fd.h"

#include "catalog/tde_keyring.h"
#include "keyring/keyring_curl.h"

#ifdef FRONTEND
#include "pg_tde_fe.h"
#endif

#include <unistd.h>

#define MAX_CONFIG_FILE_DATA_LENGTH 1024

/*
 * JSON parser state
 */

typedef enum JsonKeringSemState
{
	JK_EXPECT_TOP_FIELD,
	JK_EXPECT_EXTERN_VAL,
} JsonKeringSemState;

#define KEYRING_REMOTE_FIELD_TYPE "remote"
#define KEYRING_FILE_FIELD_TYPE "file"

typedef enum JsonKeyringField
{
	JK_FIELD_UNKNOWN,

	JK_KRING_TYPE,

	JK_FIELD_TYPE,
	JK_REMOTE_URL,
	JK_FIELD_PATH,

	JF_FILE_PATH,

	JK_VAULT_TOKEN,
	JK_VAULT_URL,
	JK_VAULT_MOUNT_PATH,
	JK_VAULT_CA_PATH,

	/* must be the last */
	JK_FIELDS_TOTAL
} JsonKeyringField;

static const char *JK_FIELD_NAMES[JK_FIELDS_TOTAL] = {
	[JK_FIELD_UNKNOWN] = "unknownField",
	[JK_KRING_TYPE] = "type",
	[JK_FIELD_TYPE] = "type",
	[JK_REMOTE_URL] = "url",
	[JK_FIELD_PATH] = "path",

	/*
	 * These values should match pg_tde_add_key_provider_vault_v2 and
	 * pg_tde_add_key_provider_file SQL interfaces
	 */
	[JF_FILE_PATH] = "path",
	[JK_VAULT_TOKEN] = "token",
	[JK_VAULT_URL] = "url",
	[JK_VAULT_MOUNT_PATH] = "mountPath",
	[JK_VAULT_CA_PATH] = "caPath",
};

#define MAX_JSON_DEPTH 64
typedef struct JsonKeyringState
{
	ProviderType provider_type;

	/*
	 * Caller's options to be set from JSON values. Expected either
	 * `VaultV2Keyring` or `FileKeyring`
	 */
	void *provider_opts;

	/*
	 * A field hierarchy of the current branch, field[level] is the current
	 * one, field[level-1] is the parent and so on. We need to track parent
	 * fields because of the external values
	 */
	JsonKeyringField field[MAX_JSON_DEPTH];
	JsonKeringSemState state;
	int	level;

	/*
	 * The rest of the scalar fields might be in the JSON document but has no
	 * direct value for the caller. Although we need them for the values
	 * extraction or state tracking.
	 */
	char *kring_type;
	char *field_type;
	char *extern_url;
	char *extern_path;
} JsonKeyringState;

static JsonParseErrorType json_kring_scalar(void *state, char *token, JsonTokenType tokentype);
static JsonParseErrorType json_kring_object_field_start(void *state, char *fname, bool isnull);
static JsonParseErrorType json_kring_object_start(void *state);
static JsonParseErrorType json_kring_object_end(void *state);

static void json_kring_assign_scalar(JsonKeyringState *parse, JsonKeyringField field, char *value);
static char *get_remote_kring_value(const char *url, const char *field_name);
static char *get_file_kring_value(const char *path, const char *field_name);


/*
 * Parses json input for the given provider type and sets the provided options
 * out_opts should be a palloc'd `VaultV2Keyring` or `FileKeyring` struct as the
 * respective option values will be mem copied into it.
 * Returns `true` if parsing succeded and `false` otherwise.
*/
bool
ParseKeyringJSONOptions(ProviderType provider_type, void *out_opts, char *in_buf, int buf_len)
{
	JsonLexContext *jlex;
	JsonKeyringState parse = {0};
	JsonSemAction sem;
	JsonParseErrorType jerr;

	/* Set up parsing context and initial semantic state */
	parse.provider_type = provider_type;
	parse.provider_opts = out_opts;
	parse.level = -1;
	parse.state = JK_EXPECT_TOP_FIELD;
	memset(parse.field, 0, MAX_JSON_DEPTH * sizeof(JsonKeyringField));

#if PG_VERSION_NUM >= 170000
	jlex = makeJsonLexContextCstringLen(NULL, in_buf, buf_len, PG_UTF8, true);
#else
	jlex = makeJsonLexContextCstringLen(in_buf, buf_len, PG_UTF8, true);
#endif

	/*
	 * Set up semantic actions. The function below will be called when the
	 * parser reaches the appropriate state. See comments on the functions.
	 */
	sem.semstate = &parse;
	sem.object_start = json_kring_object_start;
	sem.object_end = json_kring_object_end;
	sem.array_start = NULL;
	sem.array_end = NULL;
	sem.object_field_start = json_kring_object_field_start;
	sem.object_field_end = NULL;
	sem.array_element_start = NULL;
	sem.array_element_end = NULL;
	sem.scalar = json_kring_scalar;

	/* Run the parser */
	jerr = pg_parse_json(jlex, &sem);
	if (jerr != JSON_SUCCESS)
	{
		ereport(WARNING,
				(errmsg("parsing of keyring options failed: %s",
						json_errdetail(jerr, jlex))));

	}
#if PG_VERSION_NUM >= 170000
	freeJsonLexContext(jlex);
#endif

	return jerr == JSON_SUCCESS;
}

/*
 * JSON parser semantic actions
*/

/*
 * Invoked at the start of each object in the JSON document.
 *
 * Every new object increases the level of nesting as the whole document is the
 * object itself (level 0) and every next one means going deeper into nesting.
 *
 * On the top level, we expect either scalar (string) values or objects referencing
 * the external value of the field. Hence, if we are on level 1, we expect an
 * "external field object" e.g. ({"type" : "remote", "url" : "http://localhost:8888/hello"})
 */
static JsonParseErrorType
json_kring_object_start(void *state)
{
	JsonKeyringState *parse = state;

	if (MAX_JSON_DEPTH == ++parse->level)
	{
		elog(WARNING, "reached max depth of JSON nesting");
		return JSON_SEM_ACTION_FAILED;
	}

	switch (parse->level)
	{
		case 0:
			parse->state = JK_EXPECT_TOP_FIELD;
			break;
		case 1:
			parse->state = JK_EXPECT_EXTERN_VAL;
			break;
	}

	return JSON_SUCCESS;
}

/*
 * Invoked at the end of each object in the JSON document.
 *
 * First, it means we are going back to the higher level. Plus, if it was the
 * level 1, we expect only external objects there, which means we have all
 * the necessary info to extract the value and assign the result to the
 * appropriate (parent) field.
 */
static JsonParseErrorType
json_kring_object_end(void *state)
{
	JsonKeyringState *parse = state;

	/*
	 * we're done with the nested object and if it's an external field, the
	 * value should be extracted and assigned to the parent "field". for
	 * example if : "field" : {"type" : "remote", "url" :
	 * "http://localhost:8888/hello"} or "field" : {"type" : "file", "path" :
	 * "/tmp/datafile-location"} the "field"'s value should be the content of
	 * "path" or "url" respectively
	 */
	if (parse->level == 1)
	{
		if (parse->state == JK_EXPECT_EXTERN_VAL)
		{
			JsonKeyringField parent_field = parse->field[0];

			char	   *value = NULL;

			if (strcmp(parse->field_type, KEYRING_REMOTE_FIELD_TYPE) == 0)
				value = get_remote_kring_value(parse->extern_url, JK_FIELD_NAMES[parent_field]);
			if (strcmp(parse->field_type, KEYRING_FILE_FIELD_TYPE) == 0)
				value = get_file_kring_value(parse->extern_path, JK_FIELD_NAMES[parent_field]);

			json_kring_assign_scalar(parse, parent_field, value);
		}

		parse->state = JK_EXPECT_TOP_FIELD;
	}

	parse->level--;

	return JSON_SUCCESS;
}

/*
 * Invoked at the start of each object field in the JSON document.
 *
 * Based on the given field name and the semantic state (we expect a top-level
 * field or an external object) we set the state so that when we get the value,
 * we know what is it and where to assign it.
 */
static JsonParseErrorType
json_kring_object_field_start(void *state, char *fname, bool isnull)
{
	JsonKeyringState *parse = state;
	JsonKeyringField *field;

	Assert(parse->level >= 0);

	field = &parse->field[parse->level];

	switch (parse->state)
	{
		case JK_EXPECT_TOP_FIELD:

			/*
			 * On the top level, "type" stores a keyring type and this field
			 * is common for all keyrings. The rest of the fields depend on
			 * the keyring type.
			 */
			if (strcmp(fname, JK_FIELD_NAMES[JK_KRING_TYPE]) == 0)
			{
				*field = JK_KRING_TYPE;
				break;
			}
			switch (parse->provider_type)
			{
				case FILE_KEY_PROVIDER:
					if (strcmp(fname, JK_FIELD_NAMES[JF_FILE_PATH]) == 0)
						*field = JF_FILE_PATH;
					else
					{
						*field = JK_FIELD_UNKNOWN;
						elog(DEBUG1, "parse file keyring config: unexpected field %s", fname);
					}
					break;

				case VAULT_V2_KEY_PROVIDER:
					if (strcmp(fname, JK_FIELD_NAMES[JK_VAULT_TOKEN]) == 0)
						*field = JK_VAULT_TOKEN;
					else if (strcmp(fname, JK_FIELD_NAMES[JK_VAULT_URL]) == 0)
						*field = JK_VAULT_URL;
					else if (strcmp(fname, JK_FIELD_NAMES[JK_VAULT_MOUNT_PATH]) == 0)
						*field = JK_VAULT_MOUNT_PATH;
					else if (strcmp(fname, JK_FIELD_NAMES[JK_VAULT_CA_PATH]) == 0)
						*field = JK_VAULT_CA_PATH;
					else
					{
						*field = JK_FIELD_UNKNOWN;
						elog(DEBUG1, "parse json keyring config: unexpected field %s", fname);
					}
					break;
			}
			break;

		case JK_EXPECT_EXTERN_VAL:
			if (strcmp(fname, JK_FIELD_NAMES[JK_FIELD_TYPE]) == 0)
				*field = JK_FIELD_TYPE;
			else if (strcmp(fname, JK_FIELD_NAMES[JK_REMOTE_URL]) == 0)
				*field = JK_REMOTE_URL;
			else if (strcmp(fname, JK_FIELD_NAMES[JK_FIELD_PATH]) == 0)
				*field = JK_FIELD_PATH;
			break;
	}

	return JSON_SUCCESS;
}

/*
 * Invoked at the start of each scalar in the JSON document.
 *
 * We have only the string value of the field. And rely on the state set by
 * `json_kring_object_field_start` for defining what the field is.
 */
static JsonParseErrorType
json_kring_scalar(void *state, char *token, JsonTokenType tokentype)
{
	JsonKeyringState *parse = state;

	json_kring_assign_scalar(parse, parse->field[parse->level], token);

	return JSON_SUCCESS;
}

static void
json_kring_assign_scalar(JsonKeyringState *parse, JsonKeyringField field, char *value)
{
	VaultV2Keyring *vault = parse->provider_opts;
	FileKeyring *file = parse->provider_opts;

	switch (field)
	{
		case JK_KRING_TYPE:
			parse->kring_type = value;
			break;

		case JK_FIELD_TYPE:
			parse->field_type = value;
			break;
		case JK_REMOTE_URL:
			parse->extern_url = value;
			break;
		case JK_FIELD_PATH:
			parse->extern_path = value;
			break;

		case JF_FILE_PATH:
			strncpy(file->file_name, value, sizeof(file->file_name));
			break;

		case JK_VAULT_TOKEN:
			strncpy(vault->vault_token, value, sizeof(vault->vault_token));
			break;
		case JK_VAULT_URL:
			strncpy(vault->vault_url, value, sizeof(vault->vault_url));
			break;
		case JK_VAULT_MOUNT_PATH:
			strncpy(vault->vault_mount_path, value, sizeof(vault->vault_mount_path));
			break;
		case JK_VAULT_CA_PATH:
			strncpy(vault->vault_ca_path, value, sizeof(vault->vault_ca_path));
			break;

		default:
			elog(DEBUG1, "json keyring: unexpected scalar field %d", field);
			Assert(0);
			break;
	}
}

static char *
get_remote_kring_value(const char *url, const char *field_name)
{
	long		httpCode;
	CurlString	outStr;

	/* TODO: we never pfree it */
	outStr.ptr = palloc0(1);
	outStr.len = 0;

	if (!curlSetupSession(url, NULL, &outStr))
	{
		elog(WARNING, "CURL error for remote object %s", field_name);
		return NULL;
	}
	if (curl_easy_perform(keyringCurl) != CURLE_OK)
	{
		elog(WARNING, "HTTP request error for remote object %s", field_name);
		return NULL;
	}
	if (curl_easy_getinfo(keyringCurl, CURLINFO_RESPONSE_CODE, &httpCode) != CURLE_OK)
	{
		elog(WARNING, "HTTP error for remote object %s, HTTP code %li", field_name, httpCode);
		return NULL;
	}

	/* remove trailing whitespace */
	outStr.ptr[strcspn(outStr.ptr, " \t\n\r")] = '\0';

	return outStr.ptr;
}

static char *
get_file_kring_value(const char *path, const char *field_name)
{
	int	fd = -1;
	char *val;

	fd = BasicOpenFile(path, O_RDONLY);
	if (fd < 0)
	{
		elog(WARNING, "filed to open file %s for %s", path, field_name);
		return NULL;
	}

	/* TODO: we never pfree it */
	val = palloc0(MAX_CONFIG_FILE_DATA_LENGTH);
	pg_pread(fd, val, MAX_CONFIG_FILE_DATA_LENGTH, 0);
	/* remove trailing whitespace */
	val[strcspn(val, " \t\n\r")] = '\0';

	close(fd);
	return val;
}
