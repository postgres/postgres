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

#define TDE_FORK_EXT "tde"

#define INTERNAL_KEY_LEN 16
typedef struct InternalKey
{
    uint8   key[INTERNAL_KEY_LEN];
    /* a start and end range of the key
     * (start_loc == 0 && end_loc == 0) -> the key is for the whole file
     */
    Size    start_loc; 
    Size    end_loc;
} InternalKey;

#define MASTER_KEY_NAME_LEN 256
typedef struct RelKeysData
{
    char        master_key_name[MASTER_KEY_NAME_LEN];
    Size        internal_keys_len;
    InternalKey internal_key[FLEXIBLE_ARRAY_MEMBER];
} RelKeysData;

#define SizeOfRelKeysDataHeader offsetof(RelKeysData, internal_key)
#define SizeOfRelKeysData(keys_num) \
    (SizeOfRelKeysDataHeader + sizeof(InternalKey) * keys_num)

/* Relation keys cache.
 * 
 * TODO: For now it is just a linked list. Data can only be added w/o any
 * ability to remove or change it. Also consider usage of more efficient data
 * struct (hash map) in the shared memory(?) - currently allocated in the
 * TopMemoryContext of the process. 
 */
typedef struct RelKeys
{
    Oid     rel_id;
    RelKeysData    *keys;
    struct RelKeys *next;
} RelKeys;

extern void pg_tde_create_key_fork(const RelFileLocator *newrlocator, Relation rel);
extern RelKeysData *pg_tde_get_keys_from_fork(const RelFileLocator *rlocator);
extern RelKeysData *GetRelationKeys(Relation rel);
const char * tde_sprint_key(InternalKey *k);
#endif                            /* PG_TDE_MAP_H */