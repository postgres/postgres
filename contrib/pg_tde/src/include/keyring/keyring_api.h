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

extern void RegisterKeyProvider(const TDEKeyringRoutine *routine, ProviderType type);

extern KeyInfo *KeyringGetKey(GenericKeyring *keyring, const char *key_name, KeyringReturnCodes *returnCode);
extern KeyInfo *KeyringGenerateNewKeyAndStore(GenericKeyring *keyring, const char *key_name, unsigned key_len);

#endif							/* KEYRING_API_H */
