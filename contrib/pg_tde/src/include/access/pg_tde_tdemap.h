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

/* Map entry flags */
#define MAP_ENTRY_EMPTY					0x00
#define TDE_KEY_TYPE_SMGR				0x02
#define TDE_KEY_TYPE_GLOBAL				0x04
#define TDE_KEY_TYPE_WAL_UNENCRYPTED	0x08
#define TDE_KEY_TYPE_WAL_ENCRYPTED		0x10
#define MAP_ENTRY_VALID (TDE_KEY_TYPE_SMGR | TDE_KEY_TYPE_GLOBAL)

#define INTERNAL_KEY_LEN 16
#define INTERNAL_KEY_IV_LEN 16

typedef struct InternalKey
{
	/*
	 * DO NOT re-arrange fields! Any changes should be aligned with
	 * pg_tde_read/write_one_keydata()
	 */
	uint8		key[INTERNAL_KEY_LEN];
	uint8		base_iv[INTERNAL_KEY_IV_LEN];
	uint32		rel_type;

	XLogRecPtr	start_lsn;
} InternalKey;

#define WALKeySetInvalid(key) \
	((key)->rel_type &= ~(TDE_KEY_TYPE_WAL_ENCRYPTED | TDE_KEY_TYPE_WAL_UNENCRYPTED))
#define WALKeyIsValid(key) \
	(((key)->rel_type & TDE_KEY_TYPE_WAL_UNENCRYPTED) != 0 || \
	((key)->rel_type & TDE_KEY_TYPE_WAL_ENCRYPTED) != 0)

#define MAP_ENTRY_EMPTY_IV_SIZE 16
#define MAP_ENTRY_EMPTY_AEAD_TAG_SIZE 16

/* We do not need the dbOid since the entries are stored in a file per db */
typedef struct TDEMapEntry
{
	Oid			spcOid;
	RelFileNumber relNumber;
	uint32		flags;
	InternalKey enc_key;
	/* IV and tag used when encrypting the key itself */
	unsigned char entry_iv[MAP_ENTRY_EMPTY_IV_SIZE];
	unsigned char aead_tag[MAP_ENTRY_EMPTY_AEAD_TAG_SIZE];
} TDEMapEntry;

typedef struct XLogRelKey
{
	TDEMapEntry mapEntry;
	TDEPrincipalKeyInfo pkInfo;
} XLogRelKey;

/*
 * WALKeyCacheRec is built on top of the InternalKeys cache. We still don't
 * want to key data be swapped out to the disk (implemented in the InternalKeys
 * cache) but we need extra information and the ability to have and reference
 * a sequence of keys.
 *
 * TODO: For now it's a simple linked list which is no good. So consider having
 * 			dedicated WAL keys cache inside some proper data structure.
 */
typedef struct WALKeyCacheRec
{
	XLogRecPtr	start_lsn;
	XLogRecPtr	end_lsn;

	InternalKey *key;
	void	   *crypt_ctx;

	struct WALKeyCacheRec *next;
} WALKeyCacheRec;


extern InternalKey *pg_tde_read_last_wal_key(void);

extern WALKeyCacheRec *pg_tde_get_last_wal_key(void);
extern WALKeyCacheRec *pg_tde_fetch_wal_keys(XLogRecPtr start_lsn);
extern WALKeyCacheRec *pg_tde_get_wal_cache_keys(void);
extern void pg_tde_wal_last_key_set_lsn(XLogRecPtr lsn, const char *keyfile_path);

extern InternalKey *pg_tde_create_smgr_key(const RelFileLocatorBackend *newrlocator);
extern void pg_tde_create_wal_key(InternalKey *rel_key_data, const RelFileLocator *newrlocator, uint32 flags);
extern void pg_tde_free_key_map_entry(const RelFileLocator *rlocator, off_t offset);
extern void pg_tde_write_key_map_entry_redo(const TDEMapEntry *write_map_entry, TDEPrincipalKeyInfo *principal_key_info);

#define PG_TDE_MAP_FILENAME			"pg_tde_%d_map"

static inline void
pg_tde_set_db_file_path(Oid dbOid, char *path)
{
	join_path_components(path, pg_tde_get_tde_data_dir(), psprintf(PG_TDE_MAP_FILENAME, dbOid));
}

extern InternalKey *GetSMGRRelationKey(RelFileLocatorBackend rel);

extern void pg_tde_delete_tde_files(Oid dbOid);

extern TDEPrincipalKeyInfo *pg_tde_get_principal_key_info(Oid dbOid);
extern void pg_tde_save_principal_key(TDEPrincipalKeyInfo *principal_key_info);
extern void pg_tde_save_principal_key_redo(TDEPrincipalKeyInfo *principal_key_info);
extern void pg_tde_perform_rotate_key(TDEPrincipalKey *principal_key, TDEPrincipalKey *new_principal_key);
extern void pg_tde_write_map_keydata_file(off_t size, char *file_data);

const char *tde_sprint_key(InternalKey *k);

#endif							/* PG_TDE_MAP_H */
