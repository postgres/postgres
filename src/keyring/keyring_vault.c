
#include "keyring/keyring_vault.h"
#include "keyring/keyring_config.h"
#include "pg_tde_defines.h"

#include <stdio.h>
#include <json.h>

#include <curl/curl.h>

#include "common/base64.h"

char keyringVaultToken[128];
char keyringVaultUrl[128];
char keyringVaultCaPath[256];
char keyringVaultMountPath[128];

CURL* curl = NULL;
struct curl_slist *curlList = NULL;

typedef struct curlString {
  char *ptr;
  size_t len;
} curlString;

static size_t writefunc(void *ptr, size_t size, size_t nmemb, struct curlString *s)
{
  size_t new_len = s->len + size*nmemb;
  s->ptr = repalloc(s->ptr, new_len+1);
  if (s->ptr == NULL) {
    exit(EXIT_FAILURE);
  }
  memcpy(s->ptr+s->len, ptr, size*nmemb);
  s->ptr[new_len] = '\0';
  s->len = new_len;

  return size*nmemb;
}

static bool curlSetupSession(const char* url, curlString* str)
{
	if(curl == NULL)
	{
		curl = curl_easy_init();

		if(curl == NULL) return 0;
	}

	if(curlList == NULL)
	{
		char tokenHeader[256];
		strcpy(tokenHeader, "X-Vault-Token:");
		strcat(tokenHeader, keyringVaultToken);

		curlList = curl_slist_append(curlList, tokenHeader);
		if(curlList == NULL) return 0;

		curlList = curl_slist_append(curlList, "Content-Type: application/json");
		if(curlList == NULL) return 0;
	}


	if(curl_easy_setopt(curl, CURLOPT_HTTPHEADER, curlList) != CURLE_OK) return 0;
	if(curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1) != CURLE_OK) return 0;
	if(curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 1) != CURLE_OK) return 0;
	if(curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL) != CURLE_OK) return 0;
	if(strlen(keyringVaultCaPath) > 0 && curl_easy_setopt(
            curl, CURLOPT_CAINFO, keyringVaultCaPath) != CURLE_OK) return 0;
	if(curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L) != CURLE_OK) return 0;
	if(curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3) != CURLE_OK) return 0;
	if(curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10) != CURLE_OK) return 0;
	if(curl_easy_setopt(curl, CURLOPT_HTTP_VERSION,(long)CURL_HTTP_VERSION_1_1) != CURLE_OK) return 0;
	if(curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,writefunc) != CURLE_OK) return 0;
	if(curl_easy_setopt(curl, CURLOPT_WRITEDATA,str) != CURLE_OK) return 0;
	if(curl_easy_setopt(curl, CURLOPT_URL, url) != CURLE_OK) return 0;

	return 1;
}

static bool curlPerform(const char* url, curlString* outStr, long* httpCode, const char* postData)
{
#if KEYRING_DEBUG
	elog(DEBUG1, "Performing Vault HTTP [%s] request to '%s'", postData != NULL ? "POST" : "GET", url);
	if(postData != NULL)
	{
		elog(DEBUG2, "Postdata: '%s'", postData);
	}
#endif
	outStr->ptr = palloc0(1);
	outStr->len = 0;

	if(!curlSetupSession(url, outStr)) return 0;

	if(postData != NULL)
	{
		if(curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postData) != CURLE_OK) return 0;
	} else
	{
		if(curl_easy_setopt(curl, CURLOPT_POSTFIELDS, NULL) != CURLE_OK) return 0;
		if(curl_easy_setopt(curl, CURLOPT_POST, 0) != CURLE_OK) return 0;
	}

	if(curl_easy_perform(curl) != CURLE_OK) return 0;
	if(curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, httpCode) != CURLE_OK) return 0;

#if KEYRING_DEBUG
	elog(DEBUG2, "Vault response [%li] '%s'", *httpCode, outStr->ptr != NULL ? outStr->ptr : "");
#endif

	return 1;
}


int keyringVaultPreloadCache(void)
{
	// nop
	return 1;
}

static bool keyringConfigExtractParameter(json_object* configRoot, const char* name, char* out, unsigned outMaxLen, bool optional)
{
	json_object* dataO;
	const char* stringData;

	if(!json_object_object_get_ex(configRoot, name, &dataO))
	{
		if(!optional)
		{
			elog(ERROR, "Missing '%s' attribute.", name);
		}
		return 0;
	}

	stringData = keyringParseStringParam(dataO);

	if(stringData == NULL)
	{
		if(!optional)
		{
			elog(ERROR, "Couldn't parse '%s' attribute.", name);
		}
		return 0;
	}

	if(strlen(stringData) > outMaxLen-1)
	{
		elog(WARNING, "Attribute '%s' is too long, maximum is %u, truncated.", name, outMaxLen);
	}

	strncpy(out, stringData, outMaxLen-1);

	return 1;
}

int keyringVaultParseConfiguration(json_object* configRoot)
{
	if(!keyringConfigExtractParameter(configRoot, "token", keyringVaultToken, 128, 0))
	{
		return 0;
	}

	if(!keyringConfigExtractParameter(configRoot, "url", keyringVaultUrl, 128, 0))
	{
		return 0;
	}

	if(!keyringConfigExtractParameter(configRoot, "mountPath", keyringVaultMountPath, 128, 0))
	{
		return 0;
	}

	keyringConfigExtractParameter(configRoot, "caPath", keyringVaultCaPath, 256, 1);

	return 1;
}

static void keyringVaultKeyUrl(char* out, keyName name)
{
	strcpy(out, keyringVaultUrl);
	strcat(out, "/v1/");
	strcat(out, keyringVaultMountPath);
	strcat(out, "/data/");
	strcat(out, name.name);
}

int keyringVaultStoreKey(const keyInfo* ki)
{
	char url[512];
	curlString str;
	long httpCode = 0;
	json_object *request = json_object_new_object();
	json_object *data = json_object_new_object();
	char keyData[64];
	int keyLen = 0;

	keyLen = pg_b64_encode((char*)ki->data.data, ki->data.len, keyData, 64);
	keyData[keyLen] = 0;
	json_object_object_add(data, "key", json_object_new_string(keyData));
	json_object_object_add(request, "data", data);

#if KEYRING_DEBUG
	elog(DEBUG1, "Sending base64 key: %s", keyData);
#endif

	keyringVaultKeyUrl(url, ki->name);
	if(!curlPerform(url, &str, &httpCode, json_object_to_json_string(request)))
	{
		elog(ERROR, "HTTP(S) request to vault failed.");
		if(str.ptr != NULL) pfree(str.ptr);
		json_object_put(request);
		return 0;
	}

	json_object_put(request);
	if(str.ptr != NULL) pfree(str.ptr);

	return httpCode / 100 == 2;
}

int keyringVaultGetKey(keyName name, keyData* outData)
{
	char url[512];
	curlString str;
	json_object *response = NULL;
	long httpCode = 0;
	int ret = 0;

	json_object *data = NULL;
	json_object *data2 = NULL;
	json_object *keyO = NULL;
	const char *key = NULL;

	keyringVaultKeyUrl(url, name);
	if(!curlPerform(url, &str, &httpCode, NULL))
	{
		elog(ERROR, "HTTP(S) request to vault failed.");
		goto cleanup;
	}

	if(httpCode / 100 != 2)
	{
		if(httpCode != 404)
		{
			elog(ERROR, "Unexpected HTTP code: %li", httpCode);
		}
		goto cleanup;
	}

	response = json_tokener_parse(str.ptr);

	if(response == NULL)
	{
		elog(ERROR, "Vault response is not a json object.");
		goto cleanup;
	}

	if(!json_object_object_get_ex(response, "data", &data))
	{
		elog(ERROR, "No data attribute in Vault response.");
		goto cleanup;
	}

	if(!json_object_object_get_ex(data, "data", &data2))
	{
		elog(ERROR, "No data.data attribute in Vault response.");
		goto cleanup;
	}

	if(!json_object_object_get_ex(data2, "key", &keyO))
	{
		elog(ERROR, "No data.data.key attribute in Vault response.");
		goto cleanup;
	}

	key = json_object_get_string(keyO);
	if(key == NULL || strlen(key) == 0)
	{
		elog(ERROR, "Key doesn't exist or empty");
		goto cleanup;
	}

#if KEYRING_DEBUG
	elog(DEBUG1, "Retrieved base64 key: %s", key);
#endif

	outData->len = pg_b64_decode(key, strlen(key), (char*)outData->data, 32);

	if(outData->len != 32)
	{
		goto cleanup;
	}

	ret = 1;

cleanup:
		if(str.ptr != NULL) pfree(str.ptr);
		if(response != NULL) json_object_put(response);
	return ret;
}


