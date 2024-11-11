/*-------------------------------------------------------------------------
 *
 * keyring_curl.h
 *      Contains common curl related methods.
 *
 * IDENTIFICATION
 * src/include/keyring/keyring_curl.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef KEYRING_CURL_H
#define KEYRING_CURL_H

#include "pg_tde_defines.h"

#define VAULT_URL_MAX_LEN 512

#include <stdbool.h>
#include <curl/curl.h>

typedef struct CurlString
{
	char *ptr;
	size_t len;
} CurlString;

extern CURL * keyringCurl;

bool curlSetupSession(const char *url, const char *caFile, CurlString *outStr);

#endif	/* //KEYRING_CURL_H */
