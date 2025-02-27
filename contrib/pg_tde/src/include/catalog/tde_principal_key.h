/*-------------------------------------------------------------------------
 *
 * tde_principal_key.h
 *	  TDE principal key handling
 *
 * src/include/catalog/tde_principal_key.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TDE_PRINCIPAL_KEY_H
#define PG_TDE_PRINCIPAL_KEY_H


#include "postgres.h"
#include "catalog/tde_keyring.h"
#include "keyring/keyring_api.h"
#include "nodes/pg_list.h"
#ifndef FRONTEND
#include "storage/lwlock.h"
#endif

#define PRINCIPAL_KEY_NAME_LEN TDE_KEY_NAME_LEN

typedef struct TDEPrincipalKeyInfo
{
	Oid			databaseId;
	Oid			userId;
	Oid			keyringId;
	struct timeval creationTime;
	char		name[PRINCIPAL_KEY_NAME_LEN];
} TDEPrincipalKeyInfo;

typedef struct TDEPrincipalKey
{
	TDEPrincipalKeyInfo keyInfo;
	unsigned char keyData[MAX_KEY_DATA_SIZE];
	uint32		keyLength;
} TDEPrincipalKey;

typedef struct XLogPrincipalKeyRotate
{
	Oid			databaseId;
	off_t		map_size;
	off_t		keydata_size;
	char		buff[FLEXIBLE_ARRAY_MEMBER];
} XLogPrincipalKeyRotate;

#define SizeoOfXLogPrincipalKeyRotate	offsetof(XLogPrincipalKeyRotate, buff)

extern void InitializePrincipalKeyInfo(void);
extern void cleanup_principal_key_info(Oid databaseId);

#ifndef FRONTEND
extern LWLock *tde_lwlock_enc_keys(void);
extern TDEPrincipalKey *GetPrincipalKey(Oid dbOid, LWLockMode lockMode);
extern TDEPrincipalKey *GetPrincipalKeyNoDefault(Oid dbOid, LWLockMode lockMode);
#else
extern TDEPrincipalKey *GetPrincipalKey(Oid dbOid, void *lockMode);
extern TDEPrincipalKey *GetPrincipalKeyNoDefault(Oid dbOid, void *lockMode);
#endif

extern bool create_principal_key_info(TDEPrincipalKeyInfo *principalKeyInfo);
extern bool update_principal_key_info(TDEPrincipalKeyInfo *principal_key_info);

extern Oid	GetPrincipalKeyProviderId(void);
extern bool AlterPrincipalKeyKeyring(const char *provider_name);
extern bool xl_tde_perform_rotate_key(XLogPrincipalKeyRotate *xlrec);

extern void PrincipalKeyGucInit(void);

extern TDEPrincipalKey *get_principal_key_from_keyring(Oid dbOid, bool pushToCache);

#endif							/* PG_TDE_PRINCIPAL_KEY_H */
