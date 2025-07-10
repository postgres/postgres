/*
 * TDE catalog handling
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
extern void KeyProviderShmemInit(void);
extern void key_provider_startup_cleanup(Oid databaseId);
extern bool get_keyring_info_file_record_by_name(char *provider_name,
												 Oid database_id,
												 KeyringProviderRecordInFile *record);
extern void write_key_provider_info(KeyringProviderRecordInFile *record,
									bool write_xlog);
extern void redo_key_provider_info(KeyringProviderRecordInFile *xlrec);

extern void ParseKeyringJSONOptions(ProviderType provider_type,
									GenericKeyring *out_opts,
									char *in_buf, int buf_len);
extern void free_keyring(GenericKeyring *keyring);

#endif							/* TDE_KEYRING_H */
