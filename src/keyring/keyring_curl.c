/*-------------------------------------------------------------------------
 *
 * keyring_curl.c
 *      Contains common curl related methods.
 *
 * IDENTIFICATION
 *    contrib/pg_tde/src/keyring/keyring_curl.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "keyring/keyring_curl.h"
#include "pg_tde_defines.h"

CURL* keyringCurl = NULL;

static
size_t write_func(void *ptr, size_t size, size_t nmemb, struct CurlString *s)
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

bool curlSetupSession(const char* url, const char* caFile, CurlString* outStr)
{
	if(keyringCurl == NULL)
	{
		keyringCurl = curl_easy_init();

		if(keyringCurl == NULL) return 0;
	} else {
        curl_easy_reset(keyringCurl);
    }

	if(curl_easy_setopt(keyringCurl, CURLOPT_SSL_VERIFYPEER, 1) != CURLE_OK) return 0;
	if(curl_easy_setopt(keyringCurl, CURLOPT_USE_SSL, CURLUSESSL_ALL) != CURLE_OK) return 0;
	if(caFile != NULL && strlen(caFile) != 0)
	{
		if(curl_easy_setopt(keyringCurl, CURLOPT_CAINFO, caFile) != CURLE_OK) return 0;
	}
	if(curl_easy_setopt(keyringCurl, CURLOPT_FOLLOWLOCATION, 1L) != CURLE_OK) return 0;
	if(curl_easy_setopt(keyringCurl, CURLOPT_CONNECTTIMEOUT, 3) != CURLE_OK) return 0;
	if(curl_easy_setopt(keyringCurl, CURLOPT_TIMEOUT, 10) != CURLE_OK) return 0;
	if(curl_easy_setopt(keyringCurl, CURLOPT_HTTP_VERSION,(long)CURL_HTTP_VERSION_1_1) != CURLE_OK) return 0;
	if(curl_easy_setopt(keyringCurl, CURLOPT_WRITEFUNCTION,write_func) != CURLE_OK) return 0;
	if(curl_easy_setopt(keyringCurl, CURLOPT_WRITEDATA,outStr) != CURLE_OK) return 0;
	if(curl_easy_setopt(keyringCurl, CURLOPT_URL, url) != CURLE_OK) return 0;

	if(curl_easy_setopt(keyringCurl, CURLOPT_POSTFIELDS, NULL) != CURLE_OK) return 0;
	if(curl_easy_setopt(keyringCurl, CURLOPT_POST, 0) != CURLE_OK) return 0;

	return 1;
}