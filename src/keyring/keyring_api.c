
#define KEYRING_DEBUG 1

#include "keyring/keyring_api.h"
#include "keyring/keyring_file.h"

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

	// TODO: HARDCODED FILE PROVIDER
	keyringFilePreloadCache();
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
	// TODO: HARDCODED FILE PROVIDER
#if KEYRING_DEBUG
	fprintf(stderr, " -- not found\n");
#endif
	return NULL;
}

const keyInfo* keyringStoreKey(keyName name, keyData data)
{
	// TODO: HARDCODED FILE PROVIDER
	// Todo: we should first call the provider, and if it succeeds, add the key to the cache
	// But as the current file implementation just dumps the cache to disk, this is a good first prototype
	const keyInfo* ki = keyringCacheStoreKey(name, data);
#if KEYRING_DEBUG
	fprintf(stderr, "Storing key: %s\n", name.name);
#endif
	keyringFileStoreKey(ki);
	return ki;
}

keyName keyringConstructKeyName(const char* internalName, unsigned version)
{
	keyName name;
	snprintf(name.name, sizeof(name.name), "%s-%u-%lu", internalName, version, GetSystemIdentifier());
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

