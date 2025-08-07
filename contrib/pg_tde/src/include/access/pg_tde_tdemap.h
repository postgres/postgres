#ifndef PG_TDE_MAP_H
#define PG_TDE_MAP_H

#include "access/xlog_internal.h"
#include "storage/relfilelocator.h"
#include "catalog/tde_principal_key.h"
#include "common/pg_tde_utils.h"

typedef enum
{
	MAP_ENTRY_EMPTY = 0,
	TDE_KEY_TYPE_SMGR = 1,
	TDE_KEY_TYPE_WAL_UNENCRYPTED = 2,
	TDE_KEY_TYPE_WAL_ENCRYPTED = 3,
	TDE_KEY_TYPE_WAL_INVALID = 4,
} TDEMapEntryType;

#define INTERNAL_KEY_LEN 16
#define INTERNAL_KEY_IV_LEN 16

typedef struct InternalKey
{
	uint8		key[INTERNAL_KEY_LEN];
	uint8		base_iv[INTERNAL_KEY_IV_LEN];
	uint32		type;

	XLogRecPtr	start_lsn;
} InternalKey;

#define MAP_ENTRY_IV_SIZE 16
#define MAP_ENTRY_AEAD_TAG_SIZE 16

typedef struct
{
	TDEPrincipalKeyInfo data;
	unsigned char sign_iv[MAP_ENTRY_IV_SIZE];
	unsigned char aead_tag[MAP_ENTRY_AEAD_TAG_SIZE];
} TDESignedPrincipalKeyInfo;

extern void pg_tde_save_smgr_key(RelFileLocator rel, const InternalKey *key);
extern bool pg_tde_has_smgr_key(RelFileLocator rel);
extern InternalKey *pg_tde_get_smgr_key(RelFileLocator rel);
extern void pg_tde_free_key_map_entry(RelFileLocator rel);

extern int	pg_tde_count_encryption_keys(Oid dbOid);

extern void pg_tde_delete_tde_files(Oid dbOid);

extern TDESignedPrincipalKeyInfo *pg_tde_get_principal_key_info(Oid dbOid);
extern bool pg_tde_verify_principal_key_info(TDESignedPrincipalKeyInfo *signed_key_info, const KeyData *principal_key_data);
extern void pg_tde_save_principal_key(const TDEPrincipalKey *principal_key, bool write_xlog);
extern void pg_tde_save_principal_key_redo(const TDESignedPrincipalKeyInfo *signed_key_info);
extern void pg_tde_perform_rotate_key(TDEPrincipalKey *principal_key, TDEPrincipalKey *new_principal_key, bool write_xlog);
extern void pg_tde_delete_principal_key(Oid dbOid);
extern void pg_tde_delete_principal_key_redo(Oid dbOid);

extern void pg_tde_sign_principal_key_info(TDESignedPrincipalKeyInfo *signed_key_info, const TDEPrincipalKey *principal_key);

const char *tde_sprint_key(InternalKey *k);

#endif							/* PG_TDE_MAP_H */
