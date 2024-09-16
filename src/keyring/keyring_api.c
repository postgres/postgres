
#include "keyring/keyring_api.h"
#include "keyring/keyring_file.h"
#include "keyring/keyring_vault.h"

#include "postgres.h"
#include "access/xlog.h"
#include "storage/shmem.h"
#include "nodes/pg_list.h"
#include "utils/memutils.h"
#ifdef FRONTEND
#include "fe_utils/simple_list.h"
#include "pg_tde_fe.h"
#endif

#include <assert.h>
#include <openssl/rand.h>

typedef struct KeyProviders
{
	TDEKeyringRoutine *routine;
	ProviderType type;
} KeyProviders;

#ifndef FRONTEND
List *registeredKeyProviders = NIL;
#else
SimplePtrList registeredKeyProviders = {NULL, NULL};
#endif
static KeyProviders *find_key_provider(ProviderType type);

#ifndef FRONTEND
static KeyProviders *
find_key_provider(ProviderType type)
{
	ListCell *lc;
	foreach (lc, registeredKeyProviders)
	{
		KeyProviders *kp = (KeyProviders *)lfirst(lc);
		if (kp->type == type)
		{
			return kp;
		}
	}
	return NULL;
}
#else
static KeyProviders *
find_key_provider(ProviderType type)
{
	SimplePtrListCell *lc;
	for (lc = registeredKeyProviders.head; lc; lc = lc->next)
	{
		KeyProviders *kp = (KeyProviders *) lc->ptr;
		if (kp->type == type)
		{
			return kp;
		}
	}
	return NULL;
}
#endif /* !FRONTEND */

bool RegisterKeyProvider(const TDEKeyringRoutine *routine, ProviderType type)
{
	KeyProviders *kp;

	Assert(routine != NULL);
	Assert(routine->keyring_get_key != NULL);
	Assert(routine->keyring_store_key != NULL);

	kp = find_key_provider(type);
	if (kp)
	{
		ereport(ERROR,
				(errmsg("Key provider of type %d already registered", type)));
		return false;
	}

#ifndef FRONTEND
	MemoryContext oldcontext;
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
#endif
	kp = palloc(sizeof(KeyProviders));
	kp->routine = (TDEKeyringRoutine *)routine;
	kp->type = type;
#ifndef FRONTEND
	registeredKeyProviders = lappend(registeredKeyProviders, kp);
	MemoryContextSwitchTo(oldcontext);
#else
	simple_ptr_list_append(&registeredKeyProviders, kp);
#endif

	return true;
}

keyInfo *
KeyringGetKey(GenericKeyring *keyring, const char *key_name, bool throw_error, KeyringReturnCodes *returnCode)
{
	KeyProviders *kp = find_key_provider(keyring->type);
	if (kp == NULL)
	{
		ereport(throw_error ? ERROR : WARNING,
				(errmsg("Key provider of type %d not registered", keyring->type)));
		*returnCode = KEYRING_CODE_INVALID_PROVIDER;
		return NULL;
	}
	return kp->routine->keyring_get_key(keyring, key_name, throw_error, returnCode);
}

KeyringReturnCodes
KeyringStoreKey(GenericKeyring *keyring, keyInfo *key, bool throw_error)
{
	KeyProviders *kp = find_key_provider(keyring->type);
	if (kp == NULL)
	{
		ereport(throw_error ? ERROR : WARNING,
				(errmsg("Key provider of type %d not registered", keyring->type)));
		return KEYRING_CODE_INVALID_PROVIDER;
	}
	return kp->routine->keyring_store_key(keyring, key, throw_error);
}

keyInfo *
KeyringGenerateNewKey(const char *key_name, unsigned key_len)
{
	keyInfo *key;
	Assert(key_len <= 32);
	key = palloc(sizeof(keyInfo));
	key->data.len = key_len;
	if (!RAND_bytes(key->data.data, key_len))
	{
		pfree(key);
		return NULL; /*openssl error*/
	}
	strncpy(key->name.name, key_name, sizeof(key->name.name));
	return key;
}

keyInfo *
KeyringGenerateNewKeyAndStore(GenericKeyring *keyring, const char *key_name, unsigned key_len, bool throw_error)
{
	keyInfo *key = KeyringGenerateNewKey(key_name, key_len);
	if (key == NULL)
	{
		ereport(throw_error ? ERROR : WARNING,
				(errmsg("Failed to generate key")));
		return NULL;
	}
	if (KeyringStoreKey(keyring, key, throw_error) != KEYRING_CODE_SUCCESS)
	{
		pfree(key);
		return NULL;
	}
	return key;
}
