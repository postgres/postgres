/*-------------------------------------------------------------------------
 *
 * pg_tde_tdemap.h
 *	  TDE relation fork manapulation.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TDE_MAP_H
#define PG_TDE_MAP_H

#include "pg_tde.h"
#include "utils/rel.h"
#include "access/xlog_internal.h"
#include "catalog/tde_principal_key.h"
#include "common/pg_tde_utils.h"
#include "storage/relfilelocator.h"

/* Map entry flags */
#define MAP_ENTRY_EMPTY         0x00
#define TDE_KEY_TYPE_HEAP_BASIC 0x01
#define TDE_KEY_TYPE_SMGR       0x02
#define TDE_KEY_TYPE_GLOBAL     0x04
#define MAP_ENTRY_VALID (TDE_KEY_TYPE_HEAP_BASIC | TDE_KEY_TYPE_SMGR | TDE_KEY_TYPE_GLOBAL)

typedef struct InternalKey
{
	/*
	 * DO NOT re-arrange fields! Any changes should be aligned with
	 * pg_tde_read/write_one_keydata()
	 */
	uint8		key[INTERNAL_KEY_LEN];
	uint32		rel_type;

	void	   *ctx;
} InternalKey;

#define INTERNAL_KEY_DAT_LEN	offsetof(InternalKey, ctx)

typedef struct RelKeyData
{
	TDEPrincipalKeyId principal_key_id;
	InternalKey internal_key;
} RelKeyData;


typedef struct XLogRelKey
{
	RelFileLocator rlocator;
	RelKeyData	relKey;
	TDEPrincipalKeyInfo pkInfo;
} XLogRelKey;

extern RelKeyData *pg_tde_create_smgr_key(const RelFileLocator *newrlocator);
extern RelKeyData *pg_tde_create_global_key(const RelFileLocator *newrlocator);
extern RelKeyData *pg_tde_create_heap_basic_key(const RelFileLocator *newrlocator);
extern void pg_tde_write_key_map_entry(const RelFileLocator *rlocator, RelKeyData *enc_rel_key_data, TDEPrincipalKeyInfo *principal_key_info);
extern void pg_tde_delete_key_map_entry(const RelFileLocator *rlocator, uint32 key_type);
extern void pg_tde_free_key_map_entry(const RelFileLocator *rlocator, uint32 key_type, off_t offset);

extern RelKeyData *GetRelationKey(RelFileLocator rel, uint32 entry_type, bool no_map_ok);
extern RelKeyData *GetSMGRRelationKey(RelFileLocator rel);
extern RelKeyData *GetHeapBaiscRelationKey(RelFileLocator rel);
extern RelKeyData *GetTdeGlobaleRelationKey(RelFileLocator rel);

extern void pg_tde_delete_tde_files(Oid dbOid);

extern TDEPrincipalKeyInfo *pg_tde_get_principal_key_info(Oid dbOid);
extern bool pg_tde_save_principal_key(TDEPrincipalKeyInfo *principal_key_info, bool truncate_existing, bool update_header);
extern bool pg_tde_perform_rotate_key(TDEPrincipalKey *principal_key, TDEPrincipalKey *new_principal_key);
extern bool pg_tde_write_map_keydata_files(off_t map_size, char *m_file_data, off_t keydata_size, char *k_file_data);
extern RelKeyData *tde_create_rel_key(const RelFileLocator *locator, InternalKey *key, TDEPrincipalKeyInfo *principal_key_info);
extern RelKeyData *tde_encrypt_rel_key(TDEPrincipalKey *principal_key, RelKeyData *rel_key_data, Oid dbOid);
extern RelKeyData *tde_decrypt_rel_key(TDEPrincipalKey *principal_key, RelKeyData *enc_rel_key_data, Oid dbOid);
extern RelKeyData *pg_tde_get_key_from_file(const RelFileLocator *rlocator, uint32 key_type, bool no_map_ok);
extern void pg_tde_move_rel_key(const RelFileLocator *newrlocator, const RelFileLocator *oldrlocator);

const char *tde_sprint_key(InternalKey *k);

extern RelKeyData *pg_tde_put_key_into_cache(const RelFileLocator *locator, RelKeyData *key);

#endif							/* PG_TDE_MAP_H */
