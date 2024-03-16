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
#include "catalog/tde_master_key.h"
#include "storage/fd.h"
#include "storage/relfilelocator.h"

typedef struct InternalKey
{
    uint8   key[INTERNAL_KEY_LEN];
	void*   ctx; // TODO: shouldn't be here / written to the disk
} InternalKey;

typedef struct RelKeyData
{
    TDEMasterKeyId  master_key_id;
    InternalKey     internal_key;
} RelKeyData;

/* Relation key cache.
 * 
 * TODO: For now it is just a linked list. Data can only be added w/o any
 * ability to remove or change it. Also consider usage of more efficient data
 * struct (hash map) in the shared memory(?) - currently allocated in the
 * TopMemoryContext of the process. 
 */
typedef struct RelKey
{
    Oid     rel_id;
    RelKeyData    *key;
    struct RelKey *next;
} RelKey;

typedef struct XLogRelKey
{
	RelFileLocator  rlocator;
	RelKeyData      relKey;
} XLogRelKey;

extern void pg_tde_create_key_map_entry(const RelFileLocator *newrlocator, Relation rel);
extern void pg_tde_write_key_map_entry(const RelFileLocator *rlocator, RelKeyData *enc_rel_key_data, TDEMasterKeyInfo *master_key_info);
extern void pg_tde_delete_key_map_entry(const RelFileLocator *rlocator);
extern void pg_tde_free_key_map_entry(const RelFileLocator *rlocator, off_t offset);

extern RelKeyData *pg_tde_get_key_from_fork(const RelFileLocator *rlocator);
extern RelKeyData *GetRelationKey(RelFileLocator rel);

extern void pg_tde_cleanup_path_vars(void);
extern void pg_tde_delete_tde_files(Oid dbOid);

extern TDEMasterKeyInfo *pg_tde_get_master_key(Oid dbOid);
extern bool pg_tde_save_master_key(TDEMasterKeyInfo *master_key_info);
extern bool pg_tde_perform_rotate_key(TDEMasterKey *master_key, TDEMasterKey *new_master_key);
extern bool xl_tde_perform_rotate_key(XLogMasterKeyRotate *xlrec);

const char * tde_sprint_key(InternalKey *k);

#endif /*PG_TDE_MAP_H*/
