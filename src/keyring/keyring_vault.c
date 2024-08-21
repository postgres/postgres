/*-------------------------------------------------------------------------
 *
 * keyring_vault.c
 *      HashiCorp Vault 2 based keyring provider
 *
 * IDENTIFICATION
 *    contrib/pg_tde/src/keyring/keyring_vault.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "keyring/keyring_vault.h"
#include "keyring/keyring_config.h"
#include "keyring/keyring_curl.h"
#include "keyring/keyring_api.h"
#include "pg_tde_defines.h"
#include "common/jsonapi.h"
#include "mb/pg_wchar.h"
#include "utils/builtins.h"
	
#include <stdio.h>

#include <curl/curl.h>

#include "common/base64.h"

/*
 * JSON parser state
*/

typedef enum
{
	JRESP_EXPECT_TOP_DATA,
	JRESP_EXPECT_DATA,
	JRESP_EXPECT_KEY
} JsonVaultRespSemState;

typedef enum
{
	JRESP_F_UNUSED,

	JRESP_F_KEY
} JsonVaultRespField;

typedef struct JsonVaultRespState
{
	JsonVaultRespSemState	state;
	JsonVaultRespField		field;
	int		level;
	
	char	*key;
} JsonVaultRespState;

static JsonParseErrorType json_resp_object_start(void *state);
static JsonParseErrorType json_resp_object_end(void *state);
static JsonParseErrorType json_resp_scalar(void *state, char *token, JsonTokenType tokentype);
static JsonParseErrorType json_resp_object_field_start(void *state, char *fname, bool isnull);
static JsonParseErrorType parse_json_response(JsonVaultRespState	*parse, JsonLexContext *lex);

struct curl_slist *curlList = NULL;

static bool curl_setup_token(VaultV2Keyring *keyring);
static char *get_keyring_vault_url(VaultV2Keyring *keyring, const char *key_name, char *out, size_t out_size);
static bool curl_perform(VaultV2Keyring *keyring, const char *url, CurlString *outStr, long *httpCode, const char *postData);

static KeyringReturnCodes set_key_by_name(GenericKeyring *keyring, keyInfo *key, bool throw_error);
static keyInfo *get_key_by_name(GenericKeyring *keyring, const char *key_name, bool throw_error, KeyringReturnCodes *return_code);

const TDEKeyringRoutine keyringVaultV2Routine = {
	.keyring_get_key = get_key_by_name,
	.keyring_store_key = set_key_by_name
};


bool InstallVaultV2Keyring(void)
{
	return RegisterKeyProvider(&keyringVaultV2Routine, VAULT_V2_KEY_PROVIDER);
}

static bool
curl_setup_token(VaultV2Keyring *keyring)
{
	if(curlList == NULL)
	{
		char tokenHeader[256];
		strcpy(tokenHeader, "X-Vault-Token:");
		strcat(tokenHeader, keyring->vault_token);

		curlList = curl_slist_append(curlList, tokenHeader);
		if(curlList == NULL) return 0;

		curlList = curl_slist_append(curlList, "Content-Type: application/json");
		if(curlList == NULL) return 0;
	}

	if(curl_easy_setopt(keyringCurl, CURLOPT_HTTPHEADER, curlList) != CURLE_OK) return 0;

	return 1;
}

static bool
curl_perform(VaultV2Keyring *keyring, const char *url, CurlString *outStr, long *httpCode, const char *postData)
{
	CURLcode ret;
#if KEYRING_DEBUG
	elog(DEBUG1, "Performing Vault HTTP [%s] request to '%s'", postData != NULL ? "POST" : "GET", url);
	if(postData != NULL)
	{
		elog(DEBUG2, "Postdata: '%s'", postData);
	}
#endif
	outStr->ptr = palloc0(1);
	outStr->len = 0;

	if (!curlSetupSession(url, keyring->vault_ca_path, outStr))
		return 0;

	if (!curl_setup_token(keyring))
		return 0;

	if(postData != NULL)
	{
		if(curl_easy_setopt(keyringCurl, CURLOPT_POSTFIELDS, postData) != CURLE_OK) return 0;
	}

	ret = curl_easy_perform(keyringCurl);
	if (ret != CURLE_OK)
	{
		elog(LOG, "curl_easy_perform failed with return code: %d", ret);
		return 0;
	}

	if(curl_easy_getinfo(keyringCurl, CURLINFO_RESPONSE_CODE, httpCode) != CURLE_OK) return 0;

#if KEYRING_DEBUG
	elog(DEBUG2, "Vault response [%li] '%s'", *httpCode, outStr->ptr != NULL ? outStr->ptr : "");
#endif

	return 1;
}

/*
 * Function builds the vault url in out parameter.
 * so enough memory should be allocated to out pointer
 */
static char *
get_keyring_vault_url(VaultV2Keyring *keyring, const char *key_name, char *out, size_t out_size)
{
	Assert(keyring != NULL);
	Assert(key_name != NULL);
	Assert(out != NULL);

	snprintf(out, out_size, "%s/v1/%s/data/%s", keyring->vault_url, keyring->vault_mount_path, key_name);
	return out;
}

static KeyringReturnCodes
set_key_by_name(GenericKeyring* keyring, keyInfo *key, bool throw_error)
{
	VaultV2Keyring *vault_keyring = (VaultV2Keyring *)keyring;
	char url[VAULT_URL_MAX_LEN];
	CurlString str;
	long httpCode = 0;
	char jsonText[512];
	char keyData[64];
	int keyLen = 0;

	Assert(key != NULL);

	// Since we are only building a very limited JSON with a single base64 string, we build it by hand
	// Simpler than using the limited pg json api
	keyLen = pg_b64_encode((char *)key->data.data, key->data.len, keyData, 64);
	keyData[keyLen] = 0;
	
	snprintf(jsonText, 512, "{\"data\":{\"key\":\"%s\"}}", keyData);

#if KEYRING_DEBUG
	elog(DEBUG1, "Sending base64 key: %s", keyData);
#endif

	get_keyring_vault_url(vault_keyring, key->name.name, url, sizeof(url));

	if (!curl_perform(vault_keyring, url, &str, &httpCode, jsonText))
	{
		if (str.ptr != NULL)
			pfree(str.ptr);

		ereport(throw_error ? ERROR : WARNING,
				(errmsg("HTTP(S) request to keyring provider \"%s\" failed",
						vault_keyring->keyring.provider_name)));

		return KEYRING_CODE_INVALID_RESPONSE;
	}

	if (str.ptr != NULL)
		pfree(str.ptr);

	if (httpCode / 100 == 2)
		return KEYRING_CODE_SUCCESS;

	return KEYRING_CODE_INVALID_RESPONSE;
}

static keyInfo *
get_key_by_name(GenericKeyring *keyring, const char *key_name, bool throw_error, KeyringReturnCodes *return_code)
{
	VaultV2Keyring *vault_keyring = (VaultV2Keyring *)keyring;
	keyInfo* key = NULL;
	char url[VAULT_URL_MAX_LEN];
	CurlString str;
	long httpCode = 0;
	JsonParseErrorType	json_error;
	JsonLexContext		*jlex = NULL;
	JsonVaultRespState	parse;

	const char* responseKey;

	*return_code = KEYRING_CODE_SUCCESS;

	get_keyring_vault_url(vault_keyring, key_name, url, sizeof(url));

	if (!curl_perform(vault_keyring, url, &str, &httpCode, NULL))
	{
		*return_code = KEYRING_CODE_INVALID_KEY_SIZE;
		ereport(throw_error ? ERROR : WARNING,
				(errmsg("HTTP(S) request to keyring provider \"%s\" failed",
						vault_keyring->keyring.provider_name)));
		goto cleanup;
	}

	if (httpCode == 404)
	{
		*return_code = KEYRING_CODE_RESOURCE_NOT_AVAILABLE;
		goto cleanup;
	}

	if (httpCode / 100 != 2)
	{
		*return_code = KEYRING_CODE_INVALID_RESPONSE;
		ereport(throw_error ? ERROR : WARNING,
				(errmsg("HTTP(S) request to keyring provider \"%s\" returned invalid response %li",
						vault_keyring->keyring.provider_name, httpCode)));
		goto cleanup;
	}

#if PG_VERSION_NUM < 170000
	jlex = makeJsonLexContextCstringLen(str.ptr, str.len, PG_UTF8, true);
#else
	jlex = makeJsonLexContextCstringLen(NULL, str.ptr, str.len, PG_UTF8, true);
#endif
	json_error = parse_json_response(&parse, jlex);

	if (json_error != JSON_SUCCESS)
	{
		*return_code = KEYRING_CODE_INVALID_RESPONSE;
		ereport(throw_error ? ERROR : WARNING,
				(errmsg("HTTP(S) request to keyring provider \"%s\" returned incorrect JSON: %s",
						vault_keyring->keyring.provider_name, json_errdetail(json_error, jlex))));
		goto cleanup;
	}

	responseKey = parse.key;

#if KEYRING_DEBUG
	elog(DEBUG1, "Retrieved base64 key: %s", responseKey);
#endif

	key = palloc(sizeof(keyInfo));
	key->data.len = pg_b64_decode(responseKey, strlen(responseKey), (char *)key->data.data, MAX_KEY_DATA_SIZE);

	if (key->data.len > MAX_KEY_DATA_SIZE)
	{
		*return_code = KEYRING_CODE_INVALID_KEY_SIZE;
		ereport(throw_error ? ERROR : WARNING,
				(errmsg("keyring provider \"%s\" returned invalid key size: %d",
						vault_keyring->keyring.provider_name, key->data.len)));
		pfree(key);
		key = NULL;
		goto cleanup;
	}

cleanup:
	if(str.ptr != NULL) 
		pfree(str.ptr);
#if PG_VERSION_NUM >= 170000
	if (jlex != NULL)
		freeJsonLexContext(jlex);
#endif
	return key;
}

/*
 * JSON parser routines
 * 
 * We expect the response in the form of:
 * {
 * ...
 *   "data": {
 *     "data": {
 *       "key": "key_value"
 *     },
 *   }
 * ...
 * }
 * 
 * the rest fields are ignored
 */

static JsonParseErrorType
parse_json_response(JsonVaultRespState	*parse, JsonLexContext *lex)
{
	JsonSemAction		sem;

	parse->state = JRESP_EXPECT_TOP_DATA;
	parse->level = -1;
	parse->field = JRESP_F_UNUSED;
	parse->key = NULL;

	sem.semstate = parse;
	sem.object_start = json_resp_object_start;
	sem.object_end = json_resp_object_end;
	sem.array_start = NULL;
	sem.array_end = NULL;
	sem.object_field_start = json_resp_object_field_start;
	sem.object_field_end = NULL;
	sem.array_element_start = NULL;
	sem.array_element_end = NULL;
	sem.scalar = json_resp_scalar;

	return pg_parse_json(lex, &sem);
}

/*
 * Invoked at the start of each object in the JSON document.
 *
 * It just keeps track of the current nesting level
 */
static JsonParseErrorType
json_resp_object_start(void *state)
{
	((JsonVaultRespState *) state)->level++;

	return JSON_SUCCESS;
}

/*
 * Invoked at the end of each object in the JSON document.
 *
 * It just keeps track of the current nesting level
 */
static JsonParseErrorType
json_resp_object_end(void *state)
{
	((JsonVaultRespState *) state)->level--;

	return JSON_SUCCESS;
}

/*
 * Invoked at the start of each scalar in the JSON document.
 *
 * We have only the string value of the field. And rely on the state set by
 * `json_resp_object_field_start` for defining what the field is.
 */
static JsonParseErrorType
json_resp_scalar(void *state, char *token, JsonTokenType tokentype)
{
	JsonVaultRespState *parse = state;

	switch (parse->field)
	{
		case JRESP_F_KEY:
			parse->key = token;
			parse->field = JRESP_F_UNUSED;
			break;
	}
	return JSON_SUCCESS;
}

/*
 * Invoked at the start of each object field in the JSON document.
 *
 * Based on the given field name and the level we set the state so that when
 * we get the value, we know what is it and where to assign it.
 */
static JsonParseErrorType
json_resp_object_field_start(void *state, char *fname, bool isnull)
{
	JsonVaultRespState *parse = state;

	switch (parse->state)
	{
		case JRESP_EXPECT_TOP_DATA:
			if (strcmp(fname, "data") == 0 && parse->level == 0)
				parse->state = JRESP_EXPECT_DATA;
			break;
		case JRESP_EXPECT_DATA:
			if (strcmp(fname, "data") == 0 && parse->level == 1)
				parse->state = JRESP_EXPECT_KEY;
			break;
		case JRESP_EXPECT_KEY:
			if (strcmp(fname, "key") == 0 && parse->level == 2)
				parse->field = JRESP_F_KEY;
			break;
	}
		
	return JSON_SUCCESS;
}
