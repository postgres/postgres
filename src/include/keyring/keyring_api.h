/*-------------------------------------------------------------------------
 *
 * keyring_api.h
 * src/include/keyring/keyring_api.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef KEYRING_API_H
#define KEYRING_API_H

#include "catalog/tde_keyring.h"

#define TDE_KEY_NAME_LEN 256
#define MAX_KEY_DATA_SIZE 32	/* maximum 256 bit encryption */
#define INTERNAL_KEY_LEN 16

typedef struct keyName
{
	char name[TDE_KEY_NAME_LEN];
} keyName;

typedef struct keyData
{
	unsigned char data[MAX_KEY_DATA_SIZE];
	unsigned len;
} keyData;

typedef struct keyInfo
{
	keyName	name;
	keyData	data;
} keyInfo;

typedef enum KeyringReturnCodes
{
	KEYRING_CODE_SUCCESS = 0,
	KEYRING_CODE_INVALID_PROVIDER,
	KEYRING_CODE_RESOURCE_NOT_AVAILABLE,
	KEYRING_CODE_RESOURCE_NOT_ACCESSABLE,
	KEYRING_CODE_INVALID_OPERATION,
	KEYRING_CODE_INVALID_RESPONSE,
	KEYRING_CODE_INVALID_KEY_SIZE,
	KEYRING_CODE_DATA_CORRUPTED
} KeyringReturnCodes;

typedef struct TDEKeyringRoutine
{
	keyInfo    *(*keyring_get_key) (GenericKeyring *keyring, const char *key_name, bool throw_error, KeyringReturnCodes * returnCode);
				KeyringReturnCodes(*keyring_store_key) (GenericKeyring *keyring, keyInfo *key, bool throw_error);
} TDEKeyringRoutine;

extern bool RegisterKeyProvider(const TDEKeyringRoutine *routine, ProviderType type);

extern KeyringReturnCodes KeyringStoreKey(GenericKeyring *keyring, keyInfo *key, bool throw_error);
extern keyInfo *KeyringGetKey(GenericKeyring *keyring, const char *key_name, bool throw_error, KeyringReturnCodes * returnCode);
extern keyInfo *KeyringGenerateNewKeyAndStore(GenericKeyring *keyring, const char *key_name, unsigned key_len, bool throw_error);
extern keyInfo *KeyringGenerateNewKey(const char *key_name, unsigned key_len);

#endif /* KEYRING_API_H */
