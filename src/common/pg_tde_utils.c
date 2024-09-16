/*-------------------------------------------------------------------------
 *
 * pg_tde_utils.c
 *      Utility functions.
 *
 * IDENTIFICATION
 *    contrib/pg_tde/src/pg_tde_utils.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_tablespace_d.h"
#include "utils/snapmgr.h"
#include "commands/defrem.h"
#include "common/pg_tde_utils.h"

#ifndef FRONTEND
#include "access/genam.h"
#include "access/heapam.h"

Oid
get_tde_basic_table_am_oid(void)
{
    return get_table_am_oid("tde_heap_basic", false);
}

Oid
get_tde_table_am_oid(void)
{
    return get_table_am_oid("tde_heap", false);
}

/*
 * Returns the list of OIDs for all TDE tables in a database
 */
List*
get_all_tde_tables(void)
{
    Relation pg_class;
    SysScanDesc scan;
    HeapTuple tuple;
    List* tde_tables = NIL;
    Oid am_oid = get_tde_basic_table_am_oid();

    /* Open the pg_class table */
    pg_class = table_open(RelationRelationId, AccessShareLock);

    /* Start a scan */
    scan = systable_beginscan(pg_class, ClassOidIndexId, true,
                              SnapshotSelf, 0, NULL);

    /* Iterate over all tuples in the table */
    while ((tuple = systable_getnext(scan)) != NULL)
    {
        Form_pg_class classForm = (Form_pg_class)GETSTRUCT(tuple);

        /* Check if the table uses the specified access method */
        if (classForm->relam == am_oid)
        {
            /* Print the name of the table */
            tde_tables = lappend_oid(tde_tables, classForm->oid);
            elog(DEBUG2, "Table %s uses the TDE access method.", NameStr(classForm->relname));
        }
    }

    /* End the scan */
    systable_endscan(scan);

    /* Close the pg_class table */
    table_close(pg_class, AccessShareLock);
    return tde_tables;
}

int
get_tde_tables_count(void)
{
    List* tde_tables = get_all_tde_tables();
    int count = list_length(tde_tables);
    list_free(tde_tables);
    return count;
}

#endif          /* !FRONTEND */

static char globalspace_dir[MAXPGPATH] = {0};

void
pg_tde_set_globalspace_dir(const char *dir)
{
    Assert(dir != NULL);
    strncpy(globalspace_dir, dir, sizeof(globalspace_dir));
}

/* returns the palloc'd string */
char *
pg_tde_get_tde_file_dir(Oid dbOid, Oid spcOid)
{
	/* `dbOid` is set to a value for the XLog keys caching but GetDatabasePath() 
	 * expects it (`dbOid`) to be `0` if this is a global space.
	 */
	if (spcOid == GLOBALTABLESPACE_OID)
    {
        if (strlen(globalspace_dir) > 0)
            return pstrdup(globalspace_dir);

		return pstrdup("global");
    }
	return GetDatabasePath(dbOid, spcOid);
}
