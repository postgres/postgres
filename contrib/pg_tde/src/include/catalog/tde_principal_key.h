/*
 * TDE principal key handling
 */

#ifndef PG_TDE_PRINCIPAL_KEY_H
#define PG_TDE_PRINCIPAL_KEY_H

#include "postgres.h"
#include "catalog/tde_keyring.h"
#ifndef FRONTEND
#include "storage/lwlock.h"
#endif

#define PRINCIPAL_KEY_NAME_LEN TDE_KEY_NAME_LEN

typedef struct TDEPrincipalKeyInfo
{
	Oid			databaseId;
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
	Oid			keyringId;
	char		keyName[PRINCIPAL_KEY_NAME_LEN];
} XLogPrincipalKeyRotate;

#define SizeoOfXLogPrincipalKeyRotate	offsetof(XLogPrincipalKeyRotate, buff)

extern void PrincipalKeyShmemInit(void);
extern Size PrincipalKeyShmemSize(void);

#ifndef FRONTEND
extern void principal_key_startup_cleanup(Oid databaseId);
extern LWLock *tde_lwlock_enc_keys(void);
extern bool pg_tde_principal_key_configured(Oid databaseId);
extern TDEPrincipalKey *GetPrincipalKey(Oid dbOid, LWLockMode lockMode);
#else
extern TDEPrincipalKey *GetPrincipalKey(Oid dbOid, void *lockMode);
#endif

extern void xl_tde_perform_rotate_key(XLogPrincipalKeyRotate *xlrec);
extern bool pg_tde_is_provider_used(Oid databaseOid, Oid providerId);
extern void pg_tde_verify_provider_keys_in_use(GenericKeyring *proposed_provider);

#endif							/* PG_TDE_PRINCIPAL_KEY_H */
