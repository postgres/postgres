/*
 * Contains common curl related methods.
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
