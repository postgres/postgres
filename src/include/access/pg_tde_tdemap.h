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

#define INTERNAL_KEY_LEN 16
typedef struct InternalKey
{
    uint8   key[INTERNAL_KEY_LEN];
    /* a start and end range of the key
     * (start_loc == 0 && end_loc == 0) -> the key is for the whole file
     */
    Size    start_loc; 
    Size    end_loc;
	void*   ctx; // TODO: shouldn't be here / written to the disk
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

extern void pg_tde_delete_key_map_entry(const RelFileLocator *rlocator);
extern void pg_tde_free_key_map_entry(const RelFileLocator *rlocator, off_t offset);
extern void pg_tde_create_key_map_entry(const RelFileLocator *newrlocator, Relation rel);
extern RelKeysData *pg_tde_get_keys_from_fork(const RelFileLocator *rlocator);
extern RelKeysData *GetRelationKeys(RelFileLocator rel);
extern void pg_tde_cleanup_path_vars(void);

const char * tde_sprint_key(InternalKey *k);

/* TDE XLOG resource manager */
#define XLOG_TDE_RELATION_KEY   0x00
/* TODO: ID has to be registedred and changed: https://wiki.postgresql.org/wiki/CustomWALResourceManagers */
#define RM_TDERMGR_ID          RM_EXPERIMENTAL_ID
#define RM_TDERMGR_NAME        "test_pg_tde_custom_rmgr"

extern void            pg_tde_rmgr_redo(XLogReaderState *record);
extern void            pg_tde_rmgr_desc(StringInfo buf, XLogReaderState *record);
extern const char *    pg_tde_rmgr_identify(uint8 info);


/* Move this to pg_tde.c file */
static const RmgrData pg_tde_rmgr = {
	.rm_name = RM_TDERMGR_NAME,
	.rm_redo = pg_tde_rmgr_redo,
	.rm_desc = pg_tde_rmgr_desc,
	.rm_identify = pg_tde_rmgr_identify
};

#endif                            /* PG_TDE_MAP_H */
