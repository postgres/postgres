
/*-------------------------------------------------------------------------
 *
 * tdeheap_ddl.c
 *      Handles the DDL operation on TDE relations.
 *
 * IDENTIFICATION
 *    contrib/pg_tde/src/access/pg_tde_ddl.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "catalog/objectaccess.h"
#include "access/pg_tde_ddl.h"
#include "access/pg_tdeam.h"
#include "access/pg_tde_tdemap.h"


static object_access_hook_type prev_object_access_hook = NULL;

static void tdeheap_object_access_hook(ObjectAccessType access, Oid classId,
                                         Oid objectId, int subId, void *arg);

void SetupTdeDDLHooks(void)
{
    prev_object_access_hook = object_access_hook;
    object_access_hook = tdeheap_object_access_hook;
}

static void
 tdeheap_object_access_hook(ObjectAccessType access, Oid classId, Oid objectId,
                             int subId, void *arg)
 {
    Relation    rel = NULL;

    if (prev_object_access_hook)
        prev_object_access_hook(access, classId, objectId, subId, arg);

     if (access == OAT_DROP && classId == RelationRelationId)
     {
        ObjectAccessDrop *drop_arg = (ObjectAccessDrop *) arg;
        rel = relation_open(objectId, AccessShareLock);
    }
    if (rel != NULL)
    {
        if ((rel->rd_rel->relkind == RELKIND_RELATION ||
            rel->rd_rel->relkind == RELKIND_TOASTVALUE ||
            rel->rd_rel->relkind == RELKIND_MATVIEW) &&
            (subId == 0) && is_tdeheap_rel(rel))
            {
                pg_tde_delete_key_map_entry(&rel->rd_locator);
            }
        relation_close(rel, AccessShareLock);
    }
}
