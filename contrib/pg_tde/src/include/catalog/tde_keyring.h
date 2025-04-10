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
#include "keyring/keyring_api.h"

/* This record goes into key provider info file */
typedef struct KeyringProviderRecord
{
	int			provider_id;
	char		provider_name[MAX_PROVIDER_NAME_LEN];
	char		options[MAX_KEYRING_OPTION_LEN];
	ProviderType provider_type;
} KeyringProviderRecord;

typedef struct KeyringProviderXLRecord
{
	Oid			database_id;
	off_t		offset_in_file;
	KeyringProviderRecord provider;
} KeyringProviderXLRecord;

extern GenericKeyring *GetKeyProviderByName(const char *provider_name, Oid dbOid);
extern GenericKeyring *GetKeyProviderByID(int provider_id, Oid dbOid);
extern ProviderType get_keyring_provider_from_typename(char *provider_type);
extern void InitializeKeyProviderInfo(void);
extern void save_new_key_provider_info(KeyringProviderRecord *provider,
									   Oid databaseId, bool write_xlog);
extern void modify_key_provider_info(KeyringProviderRecord *provider,
									 Oid databaseId, bool write_xlog);
extern void delete_key_provider_info(int provider_id,
									 Oid databaseId, bool write_xlog);
extern void redo_key_provider_info(KeyringProviderXLRecord *xlrec);

extern bool ParseKeyringJSONOptions(ProviderType provider_type, void *out_opts,
									char *in_buf, int buf_len);

#endif							/* TDE_KEYRING_H */
