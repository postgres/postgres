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
#include "catalog/keyring_min.h"

extern bool RegisterKeyProvider(const TDEKeyringRoutine *routine, ProviderType type);

extern KeyringReturnCodes KeyringStoreKey(GenericKeyring *keyring, KeyInfo *key, bool throw_error);
extern KeyInfo *KeyringGetKey(GenericKeyring *keyring, const char *key_name, bool throw_error, KeyringReturnCodes *returnCode);
extern KeyInfo *KeyringGenerateNewKeyAndStore(GenericKeyring *keyring, const char *key_name, unsigned key_len, bool throw_error);
extern KeyInfo *KeyringGenerateNewKey(const char *key_name, unsigned key_len);

#endif							/* KEYRING_API_H */
