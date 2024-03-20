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
#include "fmgr.h"
#include "utils/fmgrprotos.h"
#include "utils/builtins.h"
	
#include <stdio.h>

#include <curl/curl.h>

#include "common/base64.h"

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

	Datum dataJson;
	Datum data2Json;
	Datum keyJson;

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

	PG_TRY();
	{
		dataJson = DirectFunctionCall2(json_object_field_text, CStringGetTextDatum(str.ptr), CStringGetTextDatum("data"));
		data2Json = DirectFunctionCall2(json_object_field_text, dataJson, CStringGetTextDatum("data"));
		keyJson = DirectFunctionCall2(json_object_field_text, data2Json, CStringGetTextDatum("key"));
		responseKey = TextDatumGetCString(keyJson);
	}
	PG_CATCH();
	{
		*return_code = KEYRING_CODE_INVALID_RESPONSE;
		ereport(throw_error ? ERROR : WARNING,
				(errmsg("HTTP(S) request to keyring provider \"%s\" returned incorrect JSON response",
						vault_keyring->keyring.provider_name)));
		goto cleanup;
	}
	PG_END_TRY();

#if KEYRING_DEBUG
	elog(DEBUG1, "Retrieved base64 key: %s", response_key);
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
	if(str.ptr != NULL) pfree(str.ptr);
	return key;
}
