
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
static List *registeredKeyProviders = NIL;
#else
static SimplePtrList registeredKeyProviders = {NULL, NULL};
#endif

static KeyProviders *find_key_provider(ProviderType type);
static void KeyringStoreKey(GenericKeyring *keyring, KeyInfo *key);
static KeyInfo *KeyringGenerateNewKey(const char *key_name, unsigned key_len);

#ifndef FRONTEND
static KeyProviders *
find_key_provider(ProviderType type)
{
	ListCell   *lc;

	foreach(lc, registeredKeyProviders)
	{
		KeyProviders *kp = (KeyProviders *) lfirst(lc);

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
#endif							/* !FRONTEND */

bool
RegisterKeyProvider(const TDEKeyringRoutine *routine, ProviderType type)
{
	KeyProviders *kp;
#ifndef FRONTEND
	MemoryContext oldcontext;
#endif

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
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
#endif
	kp = palloc(sizeof(KeyProviders));
	kp->routine = (TDEKeyringRoutine *) routine;
	kp->type = type;
#ifndef FRONTEND
	registeredKeyProviders = lappend(registeredKeyProviders, kp);
	MemoryContextSwitchTo(oldcontext);
#else
	simple_ptr_list_append(&registeredKeyProviders, kp);
#endif

	return true;
}

KeyInfo *
KeyringGetKey(GenericKeyring *keyring, const char *key_name, KeyringReturnCodes *returnCode)
{
	KeyProviders *kp = find_key_provider(keyring->type);

	if (kp == NULL)
	{
		ereport(WARNING,
				(errmsg("Key provider of type %d not registered", keyring->type)));
		*returnCode = KEYRING_CODE_INVALID_PROVIDER;
		return NULL;
	}
	return kp->routine->keyring_get_key(keyring, key_name, returnCode);
}

static void
KeyringStoreKey(GenericKeyring *keyring, KeyInfo *key)
{
	KeyProviders *kp = find_key_provider(keyring->type);

	if (kp == NULL)
		ereport(ERROR,
				(errmsg("Key provider of type %d not registered", keyring->type)));

	kp->routine->keyring_store_key(keyring, key);
}

static KeyInfo *
KeyringGenerateNewKey(const char *key_name, unsigned key_len)
{
	KeyInfo    *key;

	Assert(key_len <= 32);
	/* Struct will be saved to disk so keep clean */
	key = palloc0(sizeof(KeyInfo));
	key->data.len = key_len;
	if (!RAND_bytes(key->data.data, key_len))
	{
		pfree(key);
		return NULL;			/* openssl error */
	}
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
