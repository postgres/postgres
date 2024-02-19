
#include "keyring/keyring_api.h"
#include "keyring/keyring_file.h"
#include "keyring/keyring_vault.h"
#include "keyring/keyring_config.h"

#include "postgres.h"
#include "access/xlog.h"
#include "storage/shmem.h"

#include <assert.h>
#include <openssl/rand.h>

// TODO: locking!

keyringCache* cache = NULL;

unsigned keyringCacheMemorySize(void)
{
	return sizeof(keyringCache);
}

void keyringInitCache(void)
{
	bool found = false;

	cache = ShmemInitStruct("pgtde_keyring_cache", keyringCacheMemorySize(), &found);
	if(!found)
	{
		memset(cache, 0, keyringCacheMemorySize());
	}

	switch(keyringProvider)
	{
		case PROVIDER_FILE:
		keyringFilePreloadCache();
		break;
		case PROVIDER_VAULT_V2:
		keyringVaultPreloadCache();
		break;
		case PROVIDER_UNKNOWN:
		// nop
		break;
	}
}

const keyInfo* keyringCacheStoreKey(keyName name, keyData data)
{
	cache->keys[cache->keyCount].name = name;
	cache->keys[cache->keyCount].data = data;
	cache->keyCount++;

	return &cache->keys[cache->keyCount-1];
}

const keyInfo* keyringGetKey(keyName name)
{
#if KEYRING_DEBUG
	fprintf(stderr, "Looking up key: %s\n", name.name);
#endif
	for(int i = 0; i < cache->keyCount; ++i)
	{
		if(strcmp(cache->keys[i].name.name, name.name) == 0)
		{
#if KEYRING_DEBUG
	fprintf(stderr, " -- found key\n");
#endif
			return &cache->keys[i];
		}
	}
	// not found in cache, try to look up
	switch(keyringProvider)
	{
		case PROVIDER_FILE:
			// nop, not implmeneted
		break;
		case PROVIDER_VAULT_V2:
		{
			keyData data;
			data.len = 0;
			keyringVaultGetKey(name, &data);
			if(data.len > 0)
			{
				return keyringCacheStoreKey(name, data);
			}
			break;
		}
		case PROVIDER_UNKNOWN:
		// nop
		break;
	}

#if KEYRING_DEBUG
	fprintf(stderr, " -- not found\n");
#endif
	return NULL;
}

const keyInfo* keyringStoreKey(keyName name, keyData data)
{
	const keyInfo* ki = keyringCacheStoreKey(name, data);
#if KEYRING_DEBUG
	fprintf(stderr, "Storing key: %s\n", name.name);
#endif
	switch(keyringProvider)
	{
		case PROVIDER_FILE:
		if(keyringFileStoreKey(ki)) return ki;
		break;
		case PROVIDER_VAULT_V2:
		if(keyringVaultStoreKey(ki)) return ki;
		break;
		case PROVIDER_UNKNOWN:
		// nop
		break;
	}

	//  if we are here, storeKey failed, remove from cache
	cache->keyCount--;

	return NULL;
}

keyName keyringConstructKeyName(const char* internalName, unsigned version)
{
	keyName name;
	if(keyringKeyPrefix != NULL && strlen(keyringKeyPrefix) > 0)
	{
		snprintf(name.name, sizeof(name.name), "%s-%s-%u", keyringKeyPrefix, internalName, version);
	} else 
	{
		snprintf(name.name, sizeof(name.name), "%s-%u", internalName, version);
	}
	return name;
}

const keyInfo* keyringGetLatestKey(const char* internalName)
{
	int i = 1;
	const keyInfo* curr = NULL;
	const keyInfo* prev = NULL;
	while((curr = keyringGetKey(keyringConstructKeyName(internalName, i))) != NULL)
	{
		prev = curr;
		++i;
	}

	return prev;
}

const keyInfo* keyringGenerateKey(const char* internalName, unsigned keyLen)
{
	int i = 1;
	keyData kd;


	while(keyringGetKey(keyringConstructKeyName(internalName, i)) != NULL)
	{
		++i;
	}

	assert(keyLen <= 32);

	kd.len = keyLen;
	if (!RAND_bytes(kd.data, keyLen)) 
	{
		return NULL; // openssl error
	}


	return keyringStoreKey(keyringConstructKeyName(internalName, i), kd);
}

/*
 * Simplifying the interface to get the master key without having to worry
 * generating a new one. If master key does not exist, and doGenerateKey is
 * set, a new key is generated. This is useful during write operations.
 *
 * However, when performing a read operation and a master is expected to exist,
 * doGenerateKey should be false and doRaiseError should be set to indicate
 * that master key is expected but could not be accessed.
 */
const keyInfo* getMasterKey(const char* internalName, bool doGenerateKey, bool doRaiseError)
{
	const keyInfo* key = NULL;

	key = keyringGetLatestKey(internalName);

	if (key == NULL && doGenerateKey)
	{
		key = keyringGenerateKey(internalName, 16);
	}

	if (key == NULL && doRaiseError)
	{
        ereport(ERROR,
                (errmsg("failed to retrieve master key")));
	}

	return key;
}
