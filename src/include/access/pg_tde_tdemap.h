/*-------------------------------------------------------------------------
 *
 * pg_tde_tdemap.h
 *	  TDE relation fork manapulation.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TDE_MAP_H
#define PG_TDE_MAP_H

#include "utils/rel.h"
#include "access/xlog_internal.h"
#include "catalog/pg_tablespace_d.h"
#include "catalog/tde_principal_key.h"
#include "storage/fd.h"
#include "storage/relfilelocator.h"

typedef struct InternalKey
{
    uint8   key[INTERNAL_KEY_LEN];
	void*   ctx; // TODO: shouldn't be here / written to the disk
} InternalKey;

typedef struct RelKeyData
{
    TDEPrincipalKeyId  principal_key_id;
    InternalKey     internal_key;
} RelKeyData;


typedef struct XLogRelKey
{
	RelFileLocator  rlocator;
	RelKeyData      relKey;
} XLogRelKey;

extern RelKeyData* pg_tde_create_key_map_entry(const RelFileLocator *newrlocator);
extern void pg_tde_write_key_map_entry(const RelFileLocator *rlocator, RelKeyData *enc_rel_key_data, TDEPrincipalKeyInfo *principal_key_info);
extern void pg_tde_delete_key_map_entry(const RelFileLocator *rlocator);
extern void pg_tde_free_key_map_entry(const RelFileLocator *rlocator, off_t offset);

extern RelKeyData *GetRelationKey(RelFileLocator rel);

extern void pg_tde_delete_tde_files(Oid dbOid, Oid spcOid);

extern TDEPrincipalKeyInfo *pg_tde_get_principal_key(Oid dbOid, Oid spcOid);
extern bool pg_tde_save_principal_key(TDEPrincipalKeyInfo *principal_key_info);
extern bool pg_tde_perform_rotate_key(TDEPrincipalKey *principal_key, TDEPrincipalKey *new_principal_key);
extern bool pg_tde_write_map_keydata_files(off_t map_size, char *m_file_data, off_t keydata_size, char *k_file_data);
extern RelKeyData* tde_create_rel_key(Oid rel_id, InternalKey *key, TDEPrincipalKeyInfo *principal_key_info);
extern RelKeyData *tde_encrypt_rel_key(TDEPrincipalKey *principal_key, RelKeyData *rel_key_data, const RelFileLocator *rlocator);
extern RelKeyData *tde_decrypt_rel_key(TDEPrincipalKey *principal_key, RelKeyData *enc_rel_key_data, const RelFileLocator *rlocator);
extern RelKeyData *pg_tde_get_key_from_file(const RelFileLocator *rlocator);

extern void pg_tde_set_db_file_paths(const RelFileLocator *rlocator, char *map_path, char *keydata_path);

const char * tde_sprint_key(InternalKey *k);

extern RelKeyData *pg_tde_put_key_into_map(Oid rel_id, RelKeyData *key);

#endif /*PG_TDE_MAP_H*/
