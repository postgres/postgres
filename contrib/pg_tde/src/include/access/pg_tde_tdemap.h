/*-------------------------------------------------------------------------
 *
 * pg_tde_tdemap.h
 *	  TDE relation fork manapulation.
 *
 *-------------------------------------------------------------------------
 */
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

/* We do not need the dbOid since the entries are stored in a file per db */
typedef struct TDEMapEntry
{
	Oid			spcOid;
	RelFileNumber relNumber;
	uint32		type;
	InternalKey enc_key;
	/* IV and tag used when encrypting the key itself */
	unsigned char entry_iv[MAP_ENTRY_IV_SIZE];
	unsigned char aead_tag[MAP_ENTRY_AEAD_TAG_SIZE];
} TDEMapEntry;

typedef struct XLogRelKey
{
	RelFileLocator rlocator;
} XLogRelKey;

/*
 * TODO: For now it's a simple linked list which is no good. So consider having
 * 		 dedicated WAL keys cache inside some proper data structure.
 */
typedef struct WALKeyCacheRec
{
	XLogRecPtr	start_lsn;
	XLogRecPtr	end_lsn;

	InternalKey key;
	void	   *crypt_ctx;

	struct WALKeyCacheRec *next;
} WALKeyCacheRec;

extern InternalKey *pg_tde_read_last_wal_key(void);

extern WALKeyCacheRec *pg_tde_get_last_wal_key(void);
extern WALKeyCacheRec *pg_tde_fetch_wal_keys(XLogRecPtr start_lsn);
extern WALKeyCacheRec *pg_tde_get_wal_cache_keys(void);
extern void pg_tde_wal_last_key_set_lsn(XLogRecPtr lsn, const char *keyfile_path);

extern void pg_tde_create_wal_key(InternalKey *rel_key_data, const RelFileLocator *newrlocator, TDEMapEntryType flags);

#define PG_TDE_MAP_FILENAME			"%d_keys"

static inline void
pg_tde_set_db_file_path(Oid dbOid, char *path)
{
	join_path_components(path, pg_tde_get_data_dir(), psprintf(PG_TDE_MAP_FILENAME, dbOid));
}

extern void pg_tde_save_smgr_key(RelFileLocator rel, const InternalKey *key, bool write_xlog);
extern bool pg_tde_has_smgr_key(RelFileLocator rel);
extern InternalKey *pg_tde_get_smgr_key(RelFileLocator rel);
extern void pg_tde_free_key_map_entry(RelFileLocator rel);

extern int	pg_tde_count_relations(Oid dbOid);

extern void pg_tde_delete_tde_files(Oid dbOid);

extern TDESignedPrincipalKeyInfo *pg_tde_get_principal_key_info(Oid dbOid);
extern bool pg_tde_verify_principal_key_info(TDESignedPrincipalKeyInfo *signed_key_info, const TDEPrincipalKey *principal_key);
extern void pg_tde_save_principal_key(const TDEPrincipalKey *principal_key, bool write_xlog);
extern void pg_tde_save_principal_key_redo(const TDESignedPrincipalKeyInfo *signed_key_info);
extern void pg_tde_perform_rotate_key(TDEPrincipalKey *principal_key, TDEPrincipalKey *new_principal_key, bool write_xlog);

const char *tde_sprint_key(InternalKey *k);

#endif							/* PG_TDE_MAP_H */
