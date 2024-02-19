#ifndef KEYRING_API_H
#define KEYRING_API_H

#include "postgres.h"

typedef struct keyName
{
	char name[256]; // enough for now
} keyName;

typedef struct keyData
{
	unsigned char data[32]; // maximum 256 bit encryption
	unsigned len;
} keyData;

typedef struct keyInfo
{
	keyName name;
	keyData data;
} keyInfo;

// TODO: this type should be hidden in the C file
#define MAX_CACHE_ENTRIES 1024
typedef struct keyringCache
{
	keyInfo keys[MAX_CACHE_ENTRIES];
	unsigned keyCount;
} keyringCache;


// Keys are named in the following format: <internalName>-<version>-<serverID>

// Returned keyInfo struts are all referenced to the internal key cache

// Functions that work with internal names and versions
keyName keyringConstructKeyName(const char* internalName, unsigned version); // returns palloc
const keyInfo* keyringGetLatestKey(const char* internalName);

// Generates next available version with the given internalName
// We assume that there are no gaps in the version sequence!
const keyInfo* keyringGenerateKey(const char* internalName, unsigned keyLen);

// Functions that work on full key names
const keyInfo* keyringGetKey(keyName name);
const keyInfo* keyringStoreKey(keyName name, keyData data);

// Functions that interact with the cache
unsigned keyringCacheMemorySize(void);
void keyringInitCache(void);
const keyInfo* keyringCacheStoreKey(keyName name, keyData data);
const char * tde_sprint_masterkey(const keyData *k);

const keyInfo* getMasterKey(const char* internalName, bool doGenerateKey, bool doRaiseError);

#endif // KEYRING_API_H
