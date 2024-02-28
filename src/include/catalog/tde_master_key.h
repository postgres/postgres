/*-------------------------------------------------------------------------
 *
 * tde_master_key.h
 *	  TDE master key handling
 *
 * src/include/catalog/tde_master_key.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TDE_MASTER_KEY_H
#define TDE_MASTER_KEY_H


#include "postgres.h"
#include "catalog/tde_keyring.h"
#include "keyring/keyring_api.h"
#include "nodes/pg_list.h"

#define MASTER_KEY_NAME_LEN TDE_KEY_NAME_LEN

typedef struct TDEMasterKey
{
	Oid databaseId;
	uint32 keyVersion;
	Oid keyringId;
	char keyName[MASTER_KEY_NAME_LEN];
	unsigned char keyData[MAX_KEY_DATA_SIZE];
	uint32 keyLength;
} TDEMasterKey;


typedef struct TDEMasterKeyInfo
{
	Oid keyId;
	Oid keyringId;
	Oid databaseId;
	Oid userId;
	struct timeval creationTime;
	int keyVersion;
	char keyName[MASTER_KEY_NAME_LEN];
} TDEMasterKeyInfo;

extern void InitializeMasterKeyInfo(void);
extern TDEMasterKey* GetMasterKey(void);
TDEMasterKey* SetMasterKey(const char* key_name, const char* provider_name);

#endif /*TDE_MASTER_KEY_H*/
