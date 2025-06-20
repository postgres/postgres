#include "postgres.h"

#include <openssl/err.h>
#include <openssl/rand.h>

#include "nodes/pg_list.h"
#include "utils/memutils.h"

#include "keyring/keyring_api.h"

#ifdef FRONTEND
#include "fe_utils/simple_list.h"
#include "pg_tde_fe.h"
#endif

typedef struct RegisteredKeyProviderType
{
	TDEKeyringRoutine *routine;
	ProviderType type;
} RegisteredKeyProviderType;

#ifndef FRONTEND
static List *registeredKeyProviderTypes = NIL;
#else
static SimplePtrList registeredKeyProviderTypes = {NULL, NULL};
#endif

static RegisteredKeyProviderType *find_key_provider_type(ProviderType type);
static void KeyringStoreKey(GenericKeyring *keyring, KeyInfo *key);
static KeyInfo *KeyringGenerateNewKey(const char *key_name, unsigned key_len);

#ifndef FRONTEND
static RegisteredKeyProviderType *
find_key_provider_type(ProviderType type)
{
	ListCell   *lc;

	foreach(lc, registeredKeyProviderTypes)
	{
		RegisteredKeyProviderType *kp = (RegisteredKeyProviderType *) lfirst(lc);

		if (kp->type == type)
		{
			return kp;
		}
	}
	return NULL;
}
#else
static RegisteredKeyProviderType *
find_key_provider_type(ProviderType type)
{
	SimplePtrListCell *lc;

	for (lc = registeredKeyProviderTypes.head; lc; lc = lc->next)
	{
		RegisteredKeyProviderType *kp = (RegisteredKeyProviderType *) lc->ptr;

		if (kp->type == type)
		{
			return kp;
		}
	}
	return NULL;
}
#endif							/* !FRONTEND */

void
RegisterKeyProviderType(const TDEKeyringRoutine *routine, ProviderType type)
{
	RegisteredKeyProviderType *kp;
#ifndef FRONTEND
	MemoryContext oldcontext;
#endif

	Assert(routine != NULL);
	Assert(routine->keyring_get_key != NULL);
	Assert(routine->keyring_store_key != NULL);
	Assert(routine->keyring_validate != NULL);

	kp = find_key_provider_type(type);
	if (kp)
		ereport(ERROR,
				errmsg("Key provider of type %d already registered", type));

#ifndef FRONTEND
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
#endif
	kp = palloc_object(RegisteredKeyProviderType);
	kp->routine = (TDEKeyringRoutine *) routine;
	kp->type = type;
#ifndef FRONTEND
	registeredKeyProviderTypes = lappend(registeredKeyProviderTypes, kp);
	MemoryContextSwitchTo(oldcontext);
#else
	simple_ptr_list_append(&registeredKeyProviderTypes, kp);
#endif
}

KeyInfo *
KeyringGetKey(GenericKeyring *keyring, const char *key_name, KeyringReturnCode *returnCode)
{
	KeyInfo    *key = NULL;
	RegisteredKeyProviderType *kp = find_key_provider_type(keyring->type);

	if (kp == NULL)
	{
		ereport(WARNING,
				errmsg("key provider of type %d not registered", keyring->type));
		*returnCode = KEYRING_CODE_INVALID_PROVIDER;
		return NULL;
	}
	key = kp->routine->keyring_get_key(keyring, key_name, returnCode);

	if (*returnCode != KEYRING_CODE_SUCCESS || key == NULL)
		return NULL;

	if (!ValidateKey(key))
	{
		*returnCode = KEYRING_CODE_INVALID_KEY;
		pfree(key);
		return NULL;
	}

	return key;
}

bool
ValidateKey(KeyInfo *key)
{
	Assert(key != NULL);

	if (key->name[0] == '\0')
	{
		ereport(WARNING, errmsg("invalid key: name is empty"));
		return false;
	}

	if (key->data.len == 0)
	{
		ereport(WARNING, errmsg("invalid key: data length is zero"));
		return false;
	}

	/* For now we only support 128-bit keys */
	if (key->data.len != KEY_DATA_SIZE_128)
	{
		ereport(WARNING,
				errmsg("invalid key: unsupported key length \"%u\"", key->data.len));
		return false;
	}

	return true;
}

static void
KeyringStoreKey(GenericKeyring *keyring, KeyInfo *key)
{
	RegisteredKeyProviderType *kp = find_key_provider_type(keyring->type);

	if (kp == NULL)
		ereport(ERROR,
				errmsg("Key provider of type %d not registered", keyring->type));

	kp->routine->keyring_store_key(keyring, key);
}

static KeyInfo *
KeyringGenerateNewKey(const char *key_name, unsigned key_len)
{
	KeyInfo    *key;

	Assert(key_len <= sizeof(key->data));
	/* Struct will be saved to disk so keep clean */
	key = palloc0_object(KeyInfo);
	key->data.len = key_len;
	if (!RAND_bytes(key->data.data, key_len))
		ereport(ERROR,
				errcode(ERRCODE_INTERNAL_ERROR),
				errmsg("could not generate new principal key: %s",
					   ERR_error_string(ERR_get_error(), NULL)));
	strlcpy(key->name, key_name, sizeof(key->name));
	return key;
}

KeyInfo *
KeyringGenerateNewKeyAndStore(GenericKeyring *keyring, const char *key_name, unsigned key_len)
{
	KeyInfo    *key = KeyringGenerateNewKey(key_name, key_len);

	KeyringStoreKey(keyring, key);

	return key;
}

void
KeyringValidate(GenericKeyring *keyring)
{
	RegisteredKeyProviderType *kp = find_key_provider_type(keyring->type);

	if (kp == NULL)
		ereport(ERROR,
				errmsg("Key provider of type %d not registered", keyring->type));

	kp->routine->keyring_validate(keyring);
}

char *
KeyringErrorCodeToString(KeyringReturnCode code)
{
	switch (code)
	{
		case KEYRING_CODE_SUCCESS:
			return "Success";
		case KEYRING_CODE_INVALID_PROVIDER:
			return "Invalid key";
		case KEYRING_CODE_RESOURCE_NOT_AVAILABLE:
			return "Resource not available";
		case KEYRING_CODE_INVALID_RESPONSE:
			return "Invalid response from keyring provider";
		case KEYRING_CODE_INVALID_KEY:
			return "Invalid key";
		case KEYRING_CODE_DATA_CORRUPTED:
			return "Data corrupted";
		default:
			return "Unknown error code";
	}
}
