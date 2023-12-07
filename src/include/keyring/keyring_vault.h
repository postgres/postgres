
#ifndef KEYRING_VAULT_H
#define KEYRING_VAULT_H

#include "postgres.h"

#include <json.h>

#include "keyring_api.h"

int keyringVaultPreloadCache(void);

int keyringVaultParseConfiguration(json_object* configRoot);

int keyringVaultStoreKey(const keyInfo* ki);

int keyringVaultGetKey(keyName name, keyData* outData);

#endif // KEYRING_FILE_H
