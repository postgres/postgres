#include "keyring/keyring_api.h"

#include "postgres.h"
#include "nodes/pg_list.h"
#include "utils/memutils.h"
#ifdef FRONTEND
#include "fe_utils/simple_list.h"
#include "pg_tde_fe.h"
#endif

#include <assert.h>
#include <openssl/rand.h>
#include <openssl/err.h>

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
KeyringGetKey(GenericKeyring *keyring, const char *key_name, KeyringReturnCodes *returnCode)
{
	RegisteredKeyProviderType *kp = find_key_provider_type(keyring->type);

	if (kp == NULL)
	{
		ereport(WARNING,
				errmsg("Key provider of type %d not registered", keyring->type));
		*returnCode = KEYRING_CODE_INVALID_PROVIDER;
		return NULL;
	}
	return kp->routine->keyring_get_key(keyring, key_name, returnCode);
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
