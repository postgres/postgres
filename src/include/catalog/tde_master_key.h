/*-------------------------------------------------------------------------
 *
 * tde_master_key.h
 *	  TDE master key handling
 *
 * src/include/catalog/tde_master_key.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TDE_MASTER_KEY_H
#define PG_TDE_MASTER_KEY_H


#include "postgres.h"
#include "catalog/tde_keyring.h"
#include "keyring/keyring_api.h"
#include "nodes/pg_list.h"

#define MASTER_KEY_NAME_LEN TDE_KEY_NAME_LEN
#define MAX_MASTER_KEY_VERSION_NUM 1000

typedef struct TDEMasterKeyId
{
	uint32	version;
	char	name[MASTER_KEY_NAME_LEN];
	char	versioned_name[MASTER_KEY_NAME_LEN + 4];
} TDEMasterKeyId;

typedef struct TDEMasterKeyInfo
{
	Oid databaseId;
	Oid tablespaceId;
	Oid userId;
	Oid keyringId;
	struct timeval creationTime;
	TDEMasterKeyId keyId;
} TDEMasterKeyInfo;

typedef struct TDEMasterKey
{
	TDEMasterKeyInfo keyInfo;
	unsigned char keyData[MAX_KEY_DATA_SIZE];
	uint32 keyLength;
} TDEMasterKey;

typedef struct XLogMasterKeyRotate
{
	Oid databaseId;
	off_t map_size;
	off_t keydata_size;
	char  buff[FLEXIBLE_ARRAY_MEMBER];
} XLogMasterKeyRotate;

#define SizeoOfXLogMasterKeyRotate	offsetof(XLogMasterKeyRotate, buff)

typedef struct XLogMasterKeyCleanup
{
	Oid databaseId;
	Oid tablespaceId;
} XLogMasterKeyCleanup;

extern void InitializeMasterKeyInfo(void);
extern void cleanup_master_key_info(Oid databaseId, Oid tablespaceId);

extern bool save_master_key_info(TDEMasterKeyInfo *masterKeyInfo);

extern Oid GetMasterKeyProviderId(void);
extern TDEMasterKey* GetMasterKey(void);
extern bool SetMasterKey(const char *key_name, const char *provider_name);
extern bool RotateMasterKey(const char *new_key_name, const char *new_provider_name);

#endif /*PG_TDE_MASTER_KEY_H*/
