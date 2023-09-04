
#ifndef KEYRING_FILE_H
#define KEYRING_FILE_H

#include "postgres.h"

#include <json.h>

#include "keyring_api.h"

int keyringFileParseConfiguration(json_object* configRoot);

int keyringFileStoreKey(const keyInfo* ki);

int keyringFilePreloadCache(void);

#endif // KEYRING_FILE_H
