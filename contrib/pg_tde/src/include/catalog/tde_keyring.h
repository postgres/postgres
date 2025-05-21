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

/* This struct also keeps some context of where the record belongs */
typedef struct KeyringProviderRecordInFile
{
	Oid			database_id;
	off_t		offset_in_file;
	KeyringProviderRecord provider;
} KeyringProviderRecordInFile;

extern GenericKeyring *GetKeyProviderByName(const char *provider_name, Oid dbOid);
extern GenericKeyring *GetKeyProviderByID(int provider_id, Oid dbOid);
extern ProviderType get_keyring_provider_from_typename(char *provider_type);
extern void InitializeKeyProviderInfo(void);
extern void key_provider_startup_cleanup(Oid databaseId);
extern void save_new_key_provider_info(KeyringProviderRecord *provider,
									   Oid databaseId, bool write_xlog);
extern void modify_key_provider_info(KeyringProviderRecord *provider,
									 Oid databaseId, bool write_xlog);
extern void delete_key_provider_info(char *provider_name,
									 Oid databaseId, bool write_xlog);
extern bool get_keyring_info_file_record_by_name(char *provider_name,
												 Oid database_id,
												 KeyringProviderRecordInFile *record);
extern void write_key_provider_info(KeyringProviderRecordInFile *record,
									bool write_xlog);
extern void redo_key_provider_info(KeyringProviderRecordInFile *xlrec);

extern void ParseKeyringJSONOptions(ProviderType provider_type,
									GenericKeyring *out_opts,
									char *in_buf, int buf_len);

#endif							/* TDE_KEYRING_H */
