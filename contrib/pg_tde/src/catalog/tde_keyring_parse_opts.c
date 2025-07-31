/*
 * Parser routines for the keyring JSON options
 *
 * We expect one-dimentional JSON object with scalar fields
 */

#include "postgres.h"

#include "common/jsonapi.h"
#include "mb/pg_wchar.h"
#include "utils/jsonfuncs.h"

#include "catalog/tde_keyring.h"

#ifdef FRONTEND
#include "pg_tde_fe.h"
#endif

/*
 * JSON parser state
 */
typedef enum JsonKeyringSemState
{
	JK_EXPECT_TOP_LEVEL_OBJECT,
	JK_EXPECT_FIELD,
} JsonKeyringSemState;

typedef enum JsonKeyringField
{
	JK_FIELD_UNKNOWN,

	/* Settings specific for the individual key provider types. */
	JK_FILE_PATH,

	JK_VAULT_TOKEN_PATH,
	JK_VAULT_URL,
	JK_VAULT_MOUNT_PATH,
	JK_VAULT_CA_PATH,

	JK_KMIP_HOST,
	JK_KMIP_PORT,
	JK_KMIP_CA_PATH,
	JK_KMIP_CERT_PATH,
	JK_KMIP_KEY_PATH,

	/* must be the last */
	JK_FIELDS_TOTAL
} JsonKeyringField;

static const char *JK_FIELD_NAMES[JK_FIELDS_TOTAL] = {
	[JK_FIELD_UNKNOWN] = "unknownField",

	/*
	 * These values should match pg_tde_add_database_key_provider_vault_v2,
	 * pg_tde_add_database_key_provider_file and
	 * pg_tde_add_database_key_provider_kmip SQL interfaces
	 */
	[JK_FILE_PATH] = "path",

	[JK_VAULT_TOKEN_PATH] = "tokenPath",
	[JK_VAULT_URL] = "url",
	[JK_VAULT_MOUNT_PATH] = "mountPath",
	[JK_VAULT_CA_PATH] = "caPath",

	[JK_KMIP_HOST] = "host",
	[JK_KMIP_PORT] = "port",
	[JK_KMIP_CA_PATH] = "caPath",
	[JK_KMIP_CERT_PATH] = "certPath",
	[JK_KMIP_KEY_PATH] = "keyPath",
};

typedef struct JsonKeyringState
{
	ProviderType provider_type;

	/* Caller's options to be set from JSON values. */
	GenericKeyring *provider_opts;

	JsonKeyringField current_field;

	JsonKeyringSemState state;
} JsonKeyringState;

static JsonParseErrorType json_kring_scalar(void *state, char *token, JsonTokenType tokentype);
static JsonParseErrorType json_kring_array_start(void *state);
static JsonParseErrorType json_kring_object_field_start(void *state, char *fname, bool isnull);
static JsonParseErrorType json_kring_object_start(void *state);

static void json_kring_assign_scalar(JsonKeyringState *parse, JsonKeyringField field, char *value);

/*
 * Parses json input for the given provider type and sets the provided options.
 * out_opts should be a palloc'd keyring object matching the provider_type.
 */
void
ParseKeyringJSONOptions(ProviderType provider_type, GenericKeyring *out_opts, char *in_buf, int buf_len)
{
	JsonLexContext *jlex;
	JsonKeyringState parse = {0};
	JsonSemAction sem;

	/* Set up parsing context and initial semantic state */
	parse.provider_type = provider_type;
	parse.provider_opts = out_opts;
	parse.state = JK_EXPECT_TOP_LEVEL_OBJECT;
	jlex = makeJsonLexContextCstringLen(NULL, in_buf, buf_len, PG_UTF8, true);

	/*
	 * Set up semantic actions. The function below will be called when the
	 * parser reaches the appropriate state. See comments on the functions.
	 */
	sem.semstate = &parse;
	sem.object_start = json_kring_object_start;
	sem.object_end = NULL;
	sem.array_start = json_kring_array_start;
	sem.array_end = NULL;
	sem.object_field_start = json_kring_object_field_start;
	sem.object_field_end = NULL;
	sem.array_element_start = NULL;
	sem.array_element_end = NULL;
	sem.scalar = json_kring_scalar;

#ifndef FRONTEND
	pg_parse_json_or_ereport(jlex, &sem);
#else
	{
		JsonParseErrorType jerr = pg_parse_json(jlex, &sem);

		if (jerr != JSON_SUCCESS)
		{
			ereport(ERROR,
					errmsg("parsing of keyring options failed: %s",
						   json_errdetail(jerr, jlex)));
		}

	}
#endif

	freeJsonLexContext(jlex);
}

/*
 * JSON parser semantic actions
*/

static JsonParseErrorType
json_kring_array_start(void *state)
{
	JsonKeyringState *parse = state;

	switch (parse->state)
	{
		case JK_EXPECT_TOP_LEVEL_OBJECT:
			ereport(ERROR,
					errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("key provider options must be an object"));
			break;
		case JK_EXPECT_FIELD:
			ereport(ERROR,
					errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("unexpected array in field \"%s\"", JK_FIELD_NAMES[parse->current_field]));
			break;
	}

	/* Never reached */
	Assert(false);
	return JSON_SEM_ACTION_FAILED;
}

/*
 * Invoked at the start of each object in the JSON document.
 */
static JsonParseErrorType
json_kring_object_start(void *state)
{
	JsonKeyringState *parse = state;

	switch (parse->state)
	{
		case JK_EXPECT_TOP_LEVEL_OBJECT:
			parse->state = JK_EXPECT_FIELD;
			break;
		case JK_EXPECT_FIELD:
			elog(ERROR, "key provider value cannot be an object");
			break;
	}

	return JSON_SUCCESS;
}

/*
 * Invoked at the start of each object field in the JSON document.
 *
 * Based on the given field name and the semantic state we set the state so
 * that when we get the value, we know what is it and where to assign it.
 */
static JsonParseErrorType
json_kring_object_field_start(void *state, char *fname, bool isnull)
{
	JsonKeyringState *parse = state;

	switch (parse->state)
	{
		case JK_EXPECT_TOP_LEVEL_OBJECT:
			Assert(false);
			elog(ERROR, "invalid semantic state");
			break;
		case JK_EXPECT_FIELD:
			switch (parse->provider_type)
			{
				case FILE_KEY_PROVIDER:
					if (strcmp(fname, JK_FIELD_NAMES[JK_FILE_PATH]) == 0)
						parse->current_field = JK_FILE_PATH;
					else
					{
						parse->current_field = JK_FIELD_UNKNOWN;
						ereport(ERROR,
								errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								errmsg("unexpected field \"%s\" for file provider", fname));
					}
					break;

				case VAULT_V2_KEY_PROVIDER:
					if (strcmp(fname, JK_FIELD_NAMES[JK_VAULT_TOKEN_PATH]) == 0)
						parse->current_field = JK_VAULT_TOKEN_PATH;
					else if (strcmp(fname, JK_FIELD_NAMES[JK_VAULT_URL]) == 0)
						parse->current_field = JK_VAULT_URL;
					else if (strcmp(fname, JK_FIELD_NAMES[JK_VAULT_MOUNT_PATH]) == 0)
						parse->current_field = JK_VAULT_MOUNT_PATH;
					else if (strcmp(fname, JK_FIELD_NAMES[JK_VAULT_CA_PATH]) == 0)
						parse->current_field = JK_VAULT_CA_PATH;
					else
					{
						parse->current_field = JK_FIELD_UNKNOWN;
						ereport(ERROR,
								errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								errmsg("unexpected field \"%s\" for vault-v2 provider", fname));
					}
					break;

				case KMIP_KEY_PROVIDER:
					if (strcmp(fname, JK_FIELD_NAMES[JK_KMIP_HOST]) == 0)
						parse->current_field = JK_KMIP_HOST;
					else if (strcmp(fname, JK_FIELD_NAMES[JK_KMIP_PORT]) == 0)
						parse->current_field = JK_KMIP_PORT;
					else if (strcmp(fname, JK_FIELD_NAMES[JK_KMIP_CA_PATH]) == 0)
						parse->current_field = JK_KMIP_CA_PATH;
					else if (strcmp(fname, JK_FIELD_NAMES[JK_KMIP_CERT_PATH]) == 0)
						parse->current_field = JK_KMIP_CERT_PATH;
					else if (strcmp(fname, JK_FIELD_NAMES[JK_KMIP_KEY_PATH]) == 0)
						parse->current_field = JK_KMIP_KEY_PATH;
					else
					{
						parse->current_field = JK_FIELD_UNKNOWN;
						ereport(ERROR,
								errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								errmsg("unexpected field \"%s\" for vault-v2 provider", fname));
					}
					break;

				case UNKNOWN_KEY_PROVIDER:
					return JSON_INVALID_TOKEN;
			}
			break;
	}

	pfree(fname);
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
	char	   *value;

	if (parse->state == JK_EXPECT_TOP_LEVEL_OBJECT)
	{
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("key provider options must be an object"));
	}

	switch (tokentype)
	{
		case JSON_TOKEN_STRING:
		case JSON_TOKEN_NUMBER:
			value = token;
			break;
		case JSON_TOKEN_TRUE:
		case JSON_TOKEN_FALSE:
			ereport(ERROR,
					errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("unexpected boolean in field \"%s\"", JK_FIELD_NAMES[parse->current_field]));
			break;
		case JSON_TOKEN_NULL:
			value = NULL;
			pfree(token);
			break;
		default:
			Assert(false);
			elog(ERROR, "invalid token type");
			break;
	}

	json_kring_assign_scalar(parse, parse->current_field, value);

	return JSON_SUCCESS;
}

static void
json_kring_assign_scalar(JsonKeyringState *parse, JsonKeyringField field, char *value)
{
	VaultV2Keyring *vault = (VaultV2Keyring *) parse->provider_opts;
	FileKeyring *file = (FileKeyring *) parse->provider_opts;
	KmipKeyring *kmip = (KmipKeyring *) parse->provider_opts;

	switch (field)
	{
		case JK_FILE_PATH:
			file->file_name = value;
			break;

		case JK_VAULT_TOKEN_PATH:
			vault->vault_token_path = value;
			break;
		case JK_VAULT_URL:
			vault->vault_url = value;
			break;
		case JK_VAULT_MOUNT_PATH:
			vault->vault_mount_path = value;
			break;
		case JK_VAULT_CA_PATH:
			vault->vault_ca_path = value;
			break;

		case JK_KMIP_HOST:
			kmip->kmip_host = value;
			break;
		case JK_KMIP_PORT:
			kmip->kmip_port = value;
			break;
		case JK_KMIP_CA_PATH:
			kmip->kmip_ca_path = value;
			break;
		case JK_KMIP_CERT_PATH:
			kmip->kmip_cert_path = value;
			break;
		case JK_KMIP_KEY_PATH:
			kmip->kmip_key_path = value;
			break;

		default:
			Assert(false);
			elog(ERROR, "json keyring: unexpected scalar field %d", field);
	}
}
