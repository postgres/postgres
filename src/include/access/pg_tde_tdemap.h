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
#include "storage/relfilelocator.h"
#include "access/xlog_internal.h"
#include "catalog/tde_master_key.h"

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

extern void pg_tde_write_key_map_entry(const RelFileLocator *rlocator, RelKeyData *enc_rel_key_data, bool fail_on_check);
extern void pg_tde_delete_key_map_entry(const RelFileLocator *rlocator);
extern void pg_tde_free_key_map_entry(const RelFileLocator *rlocator, off_t offset);
extern void pg_tde_create_key_map_entry(const RelFileLocator *newrlocator, Relation rel);
extern RelKeyData *pg_tde_get_key_from_fork(const RelFileLocator *rlocator);
extern RelKeyData *GetRelationKey(RelFileLocator rel);
extern void pg_tde_cleanup_path_vars(void);

const char * tde_sprint_key(InternalKey *k);

#endif /*PG_TDE_MAP_H*/
