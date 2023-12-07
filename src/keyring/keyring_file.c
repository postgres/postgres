
#include "keyring/keyring_file.h"
#include "keyring/keyring_config.h"

#include <stdio.h>

char keyringFileDataFileName[512];

char* keyringFileEncryptionKey = NULL;
unsigned keyringFileEncryptionKeyLen = 0;

extern keyringCache* cache;  // TODO: do not depend on cache internals

int keyringFileParseConfiguration(json_object* configRoot)
{
	json_object* dataO;
	const char* datafile;

	if(!json_object_object_get_ex(configRoot, "datafile", &dataO))
	{
		elog(ERROR, "Missing 'datafile' attribute.");
		return 0;
	}

	datafile = keyringParseStringParam(dataO);

	if(datafile == NULL)
	{
		elog(ERROR, "Couldn't parse 'datafile' attribute.");
		return 0;
	}

	strcpy(keyringFileDataFileName, datafile);

	return 1;
}

int keyringFilePreloadCache(void)
{
	FILE* f = fopen(keyringFileDataFileName, "r");
	if (f != NULL)
	{
		fread(cache, keyringCacheMemorySize(), 1, f);
		fclose(f);
		elog(INFO, "Keyring file '%s' found, existing keys are available, %u.", keyringFileDataFileName, cache->keyCount);
	} else {
		elog(WARNING, "Keyring file '%s' not found, not loading existing keys.", keyringFileDataFileName);
	}

	return 1;
}

int keyringFileStoreKey(const keyInfo* ki)
{
	FILE *f;

	if (strlen(keyringFileDataFileName) == 0) {
		elog(ERROR, "Keyring datafile is not set");
		return false;
	}

	// First very basic prototype: we just dump the cache to disk
	f = fopen(keyringFileDataFileName, "w");
	if(f == NULL)
	{
		elog(ERROR, "Couldn't write keyring data into '%s'", keyringFileDataFileName);
		return false;
	}

	fwrite(cache, keyringCacheMemorySize(), 1, f);

	fclose(f);

	return true;
}
