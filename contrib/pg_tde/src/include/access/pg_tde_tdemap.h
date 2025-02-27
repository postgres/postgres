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
#include "port.h"
#include "storage/relfilelocator.h"

#include "pg_tde.h"
#include "catalog/tde_principal_key.h"
#include "common/pg_tde_utils.h"

/* Map entry flags */
#define MAP_ENTRY_EMPTY					0x00
#define TDE_KEY_TYPE_HEAP_BASIC			0x01
#define TDE_KEY_TYPE_SMGR				0x02
#define TDE_KEY_TYPE_GLOBAL				0x04
#define TDE_KEY_TYPE_WAL_UNENCRYPTED	0x08
#define TDE_KEY_TYPE_WAL_ENCRYPTED		0x10
#define MAP_ENTRY_VALID (TDE_KEY_TYPE_HEAP_BASIC | TDE_KEY_TYPE_SMGR | TDE_KEY_TYPE_GLOBAL)

typedef struct InternalKey
{
	/*
	 * DO NOT re-arrange fields! Any changes should be aligned with
	 * pg_tde_read/write_one_keydata()
	 */
	uint8		key[INTERNAL_KEY_LEN];
	uint32		rel_type;

	XLogRecPtr	start_lsn;

	void	   *ctx;
} InternalKey;

#define INTERNAL_KEY_DAT_LEN	offsetof(InternalKey, ctx)

#define WALKeySetInvalid(key) \
	((key)->rel_type &= ~(TDE_KEY_TYPE_WAL_ENCRYPTED | TDE_KEY_TYPE_WAL_UNENCRYPTED))
#define WALKeyIsValid(key) \
	(((key)->rel_type & TDE_KEY_TYPE_WAL_UNENCRYPTED) != 0 || \
	((key)->rel_type & TDE_KEY_TYPE_WAL_ENCRYPTED) != 0)

typedef struct XLogRelKey
{
	RelFileLocator rlocator;
	InternalKey relKey;
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

	struct WALKeyCacheRec *next;
} WALKeyCacheRec;


extern InternalKey *pg_tde_read_last_wal_key(void);

extern WALKeyCacheRec *pg_tde_get_last_wal_key(void);
extern WALKeyCacheRec *pg_tde_fetch_wal_keys(XLogRecPtr start_lsn);
extern WALKeyCacheRec *pg_tde_get_wal_cache_keys(void);
extern void pg_tde_wal_last_key_set_lsn(XLogRecPtr lsn, const char *keyfile_path);

extern InternalKey *pg_tde_create_smgr_key(const RelFileLocatorBackend *newrlocator);
extern InternalKey *pg_tde_create_heap_basic_key(const RelFileLocator *newrlocator);
extern void pg_tde_create_wal_key(InternalKey *rel_key_data, const RelFileLocator *newrlocator, uint32 flags);
extern void pg_tde_write_key_map_entry(const RelFileLocator *rlocator, InternalKey *enc_rel_key_data, TDEPrincipalKeyInfo *principal_key_info);
extern void pg_tde_delete_key_map_entry(const RelFileLocator *rlocator, uint32 key_type);
extern void pg_tde_free_key_map_entry(const RelFileLocator *rlocator, uint32 key_type, off_t offset);

#define PG_TDE_MAP_FILENAME			"pg_tde_%d_map"
#define PG_TDE_KEYDATA_FILENAME		"pg_tde_%d_dat"

static inline void
pg_tde_set_db_file_paths(Oid dbOid, char *map_path, char *keydata_path)
{
	if (map_path)
		join_path_components(map_path, pg_tde_get_tde_data_dir(), psprintf(PG_TDE_MAP_FILENAME, dbOid));
	if (keydata_path)
		join_path_components(keydata_path, pg_tde_get_tde_data_dir(), psprintf(PG_TDE_KEYDATA_FILENAME, dbOid));
}

extern InternalKey *GetRelationKey(RelFileLocator rel, uint32 entry_type, bool no_map_ok);
extern InternalKey *GetSMGRRelationKey(RelFileLocatorBackend rel);
extern InternalKey *GetHeapBaiscRelationKey(RelFileLocator rel);
extern InternalKey *GetTdeGlobaleRelationKey(RelFileLocator rel);

extern void pg_tde_delete_tde_files(Oid dbOid);

extern TDEPrincipalKeyInfo *pg_tde_get_principal_key_info(Oid dbOid);
extern bool pg_tde_save_principal_key(TDEPrincipalKeyInfo *principal_key_info, bool truncate_existing, bool update_header);
extern bool pg_tde_perform_rotate_key(TDEPrincipalKey *principal_key, TDEPrincipalKey *new_principal_key);
extern bool pg_tde_write_map_keydata_files(off_t map_size, char *m_file_data, off_t keydata_size, char *k_file_data);
extern InternalKey *pg_tde_get_key_from_file(const RelFileLocator *rlocator, uint32 key_type, bool no_map_ok);
extern void pg_tde_move_rel_key(const RelFileLocator *newrlocator, const RelFileLocator *oldrlocator);

const char *tde_sprint_key(InternalKey *k);

extern InternalKey *pg_tde_put_key_into_cache(const RelFileLocator *locator, InternalKey *key);

#endif							/* PG_TDE_MAP_H */
