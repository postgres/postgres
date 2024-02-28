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
#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "catalog/pg_class.h"
#include "catalog/pg_am.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "commands/defrem.h"
#include "common/pg_tde_utils.h"

Oid
get_tde_table_am_oid(void)
{
    return get_table_am_oid("pg_tde", false);
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
    Oid am_oid = get_tde_table_am_oid();

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
