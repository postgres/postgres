/*-------------------------------------------------------------------------
 *
 * tde_keyring.h
 *	  TDE catalog handling
 *
 * src/include/catalog/tde_keyring.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TDE_KEYRING_H
#define TDE_KEYRING_H

#include "postgres.h"
#include "nodes/pg_list.h"

#define PG_TDE_NAMESPACE_NAME "percona_tde"
#define PG_TDE_KEY_PROVIDER_CAT_NAME "pg_tde_key_provider"
/*
 * Keyring type name must be in sync with catalog table
 * defination in pg_tde--1.0 SQL
 */
#define FILE_KEYRING_TYPE "file"
#define VAULTV2_KEYRING_TYPE "vault-v2"

#define MAX_PROVIDER_NAME_LEN 128 /* pg_tde_key_provider's provider_name size*/
#define MAX_VAULT_V2_KEY_LEN 128 /* From hashi corp docs */
#define MAX_KEYRING_OPTION_LEN	1024
typedef enum ProviderType
{
	UNKNOWN_KEY_PROVIDER,
	FILE_KEY_PROVIDER,
	VAULT_V2_KEY_PROVIDER,
} ProviderType;

/* Base type for all keyring */
typedef struct GenericKeyring
{
	ProviderType type; /* Must be the first field */
	Oid key_id;
	char provider_name[MAX_PROVIDER_NAME_LEN];
	char options[MAX_KEYRING_OPTION_LEN]; /* User provided options string*/
} GenericKeyring;

typedef struct FileKeyring
{
	GenericKeyring keyring; /* Must be the first field */
	char file_name[MAXPGPATH];
} FileKeyring;

typedef struct VaultV2Keyring
{
	GenericKeyring keyring; /* Must be the first field */
	char vault_token[MAX_VAULT_V2_KEY_LEN];
	char vault_url[MAXPGPATH];
	char vault_ca_path[MAXPGPATH];
	char vault_mount_path[MAXPGPATH];
} VaultV2Keyring;

/* This record goes into key provider info file */
typedef struct KeyringProvideRecord
{
	int provider_id;
	char provider_name[MAX_PROVIDER_NAME_LEN];
	char options[MAX_KEYRING_OPTION_LEN];
	ProviderType provider_type;
} KeyringProvideRecord;
typedef struct KeyringProviderXLRecord
{
	Oid database_id;
	Oid tablespace_id;
	off_t offset_in_file;
	KeyringProvideRecord provider;
} KeyringProviderXLRecord;

extern List *GetAllKeyringProviders(Oid dbOid, Oid spcOid);
extern GenericKeyring *GetKeyProviderByName(const char *provider_name, Oid dbOid, Oid spcOid);
extern GenericKeyring *GetKeyProviderByID(int provider_id, Oid dbOid, Oid spcOid);
extern ProviderType get_keyring_provider_from_typename(char *provider_type);
extern void cleanup_key_provider_info(Oid databaseId, Oid tablespaceId);
extern void InitializeKeyProviderInfo(void);
extern uint32 save_new_key_provider_info(KeyringProvideRecord *provider, Oid databaseId, Oid tablespaceId, bool recovery);
extern uint32 redo_key_provider_info(KeyringProviderXLRecord *xlrec);

extern bool ParseKeyringJSONOptions(ProviderType provider_type, void *out_opts,
											char *in_buf, int buf_len);
#endif /*TDE_KEYRING_H*/
