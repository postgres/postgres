#ifndef PG_TDE_XLOG_KEYS_H
#define PG_TDE_XLOG_KEYS_H

#include "access/xlog_internal.h"

#include "access/pg_tde_tdemap.h"
#include "catalog/tde_principal_key.h"

typedef struct WalEncryptionKey
{
	uint8		key[INTERNAL_KEY_LEN];
	uint8		base_iv[INTERNAL_KEY_IV_LEN];
	uint32		type;

	XLogRecPtr	start_lsn;
} WalEncryptionKey;

/*
 * TODO: For now it's a simple linked list which is no good. So consider having
 * 		 dedicated WAL keys cache inside some proper data structure.
 */
typedef struct WALKeyCacheRec
{
	XLogRecPtr	start_lsn;
	XLogRecPtr	end_lsn;

	WalEncryptionKey key;
	void	   *crypt_ctx;

	struct WALKeyCacheRec *next;
} WALKeyCacheRec;

extern int	pg_tde_count_wal_keys_in_file(void);
extern void pg_tde_create_wal_key(WalEncryptionKey *rel_key_data, TDEMapEntryType entry_type);
extern void pg_tde_delete_server_key(void);
extern WALKeyCacheRec *pg_tde_fetch_wal_keys(XLogRecPtr start_lsn);
extern WALKeyCacheRec *pg_tde_get_last_wal_key(void);
extern TDESignedPrincipalKeyInfo *pg_tde_get_server_key_info(void);
extern WALKeyCacheRec *pg_tde_get_wal_cache_keys(void);
extern void pg_tde_perform_rotate_server_key(TDEPrincipalKey *principal_key, TDEPrincipalKey *new_principal_key, bool write_xlog);
extern WalEncryptionKey *pg_tde_read_last_wal_key(void);
extern void pg_tde_save_server_key(const TDEPrincipalKey *principal_key, bool write_xlog);
extern void pg_tde_save_server_key_redo(const TDESignedPrincipalKeyInfo *signed_key_info);
extern void pg_tde_wal_last_key_set_lsn(XLogRecPtr lsn);

#endif							/* PG_TDE_XLOG_KEYS_H */
