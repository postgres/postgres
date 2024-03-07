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

#include <stdio.h>
#include <json.h>

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

	json_object *request = json_object_new_object();
	json_object *data = json_object_new_object();
	char keyData[64];
	int keyLen = 0;

	Assert(key != NULL);

	keyLen = pg_b64_encode((char *)key->data.data, key->data.len, keyData, 64);
	keyData[keyLen] = 0;
	json_object_object_add(data, "key", json_object_new_string(keyData));
	json_object_object_add(request, "data", data);

#if KEYRING_DEBUG
	elog(DEBUG1, "Sending base64 key: %s", keyData);
#endif

	get_keyring_vault_url(vault_keyring, key->name.name, url, sizeof(url));

	if (!curl_perform(vault_keyring, url, &str, &httpCode, json_object_to_json_string(request)))
	{
		if (str.ptr != NULL)
			pfree(str.ptr);
		json_object_put(request);

		ereport(throw_error ? ERROR : WARNING,
				(errmsg("HTTP(S) request to keyring provider \"%s\" failed",
						vault_keyring->keyring.provider_name)));

		return KEYRING_CODE_INVALID_RESPONSE;
	}

	json_object_put(request);
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
	json_object *response = NULL;
	long httpCode = 0;

	json_object *data = NULL;
	json_object *data2 = NULL;
	json_object *keyO = NULL;
	const char *response_key = NULL;

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

	response = json_tokener_parse(str.ptr);

	if (response == NULL)
	{
		*return_code = KEYRING_CODE_INVALID_RESPONSE;
		ereport(throw_error ? ERROR : WARNING,
				(errmsg("keyring provider \"%s\" returned invalid JSON",
						vault_keyring->keyring.provider_name)));
		goto cleanup;
	}

	if (!json_object_object_get_ex(response, "data", &data))
	{
		*return_code = KEYRING_CODE_INVALID_RESPONSE;
		ereport(throw_error ? ERROR : WARNING,
				(errmsg("keyring provider \"%s\" returned invalid JSON",
						vault_keyring->keyring.provider_name),
				 errdetail("No data attribute in Vault response.")));
		goto cleanup;
	}

	if (!json_object_object_get_ex(data, "data", &data2))
	{
		*return_code = KEYRING_CODE_INVALID_RESPONSE;
		ereport(throw_error ? ERROR : WARNING,
				(errmsg("keyring provider \"%s\" returned invalid JSON",
						vault_keyring->keyring.provider_name),
				 errdetail("No data.data attribute in Vault response.")));
		goto cleanup;
	}

	if (!json_object_object_get_ex(data2, "key", &keyO))
	{
		*return_code = KEYRING_CODE_INVALID_RESPONSE;
		ereport(throw_error ? ERROR : WARNING,
				(errmsg("keyring provider \"%s\" returned invalid JSON",
						vault_keyring->keyring.provider_name),
				 errdetail("No data.data.key attribute in Vault response.")));
		goto cleanup;
	}

	response_key = json_object_get_string(keyO);
	if (response_key == NULL || strlen(response_key) == 0)
	{
		*return_code = KEYRING_CODE_INVALID_RESPONSE;
		ereport(throw_error ? ERROR : WARNING,
				(errmsg("keyring provider \"%s\" returned invalid JSON",
						vault_keyring->keyring.provider_name),
				 errdetail("Key doesn't exist or empty.")));
		goto cleanup;
	}

#if KEYRING_DEBUG
	elog(DEBUG1, "Retrieved base64 key: %s", response_key);
#endif

	key = palloc(sizeof(keyInfo));
	key->data.len = pg_b64_decode(response_key, strlen(response_key), (char *)key->data.data, MAX_KEY_DATA_SIZE);

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
	if(response != NULL) json_object_put(response);
	return key;
}
