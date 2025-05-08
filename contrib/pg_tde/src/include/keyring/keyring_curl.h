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

#include "c.h"
#include <curl/curl.h>

typedef struct CurlString
{
	char	   *ptr;
	size_t		len;
} CurlString;

extern CURL *keyringCurl;

extern bool curlSetupSession(const char *url, const char *caFile, CurlString *outStr);

#endif							/* //KEYRING_CURL_H */
