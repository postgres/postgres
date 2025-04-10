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

#include "utils/snapmgr.h"
#include "commands/defrem.h"
#include "common/pg_tde_utils.h"
#include "miscadmin.h"
#include "catalog/tde_principal_key.h"
#include "access/pg_tde_tdemap.h"
#include "pg_tde.h"

#ifndef FRONTEND
#include "access/genam.h"
#include "access/heapam.h"

static Oid
get_tde_table_am_oid(void)
{
	return get_table_am_oid("tde_heap", false);
}

PG_FUNCTION_INFO_V1(pg_tde_is_encrypted);
Datum
pg_tde_is_encrypted(PG_FUNCTION_ARGS)
{
	Oid			relationOid = PG_GETARG_OID(0);
	LOCKMODE	lockmode = AccessShareLock;
	Relation	rel = relation_open(relationOid, lockmode);
	RelFileLocatorBackend rlocator = {.locator = rel->rd_locator,.backend = rel->rd_backend};
	InternalKey *key;

	if (!RELKIND_HAS_STORAGE(rel->rd_rel->relkind))
	{
		relation_close(rel, lockmode);
		PG_RETURN_NULL();
	}

	if (RelFileLocatorBackendIsTemp(rlocator) && !rel->rd_islocaltemp)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("we cannot check if temporary relations from other backends are encrypted"));

	key = GetSMGRRelationKey(rlocator);

	relation_close(rel, lockmode);

	PG_RETURN_BOOL(key != NULL);
}

/*
 * Returns the number of TDE tables in a database
 */
int
get_tde_tables_count(void)
{
	Relation	pg_class;
	SysScanDesc scan;
	HeapTuple	tuple;
	int			count = 0;
	Oid			am_oid = get_tde_table_am_oid();

	/* Open the pg_class table */
	pg_class = table_open(RelationRelationId, AccessShareLock);

	/* Start a scan */
	scan = systable_beginscan(pg_class, ClassOidIndexId, true,
							  SnapshotSelf, 0, NULL);

	/* Iterate over all tuples in the table */
	while ((tuple = systable_getnext(scan)) != NULL)
	{
		Form_pg_class classForm = (Form_pg_class) GETSTRUCT(tuple);

		/* Check if the table uses the specified access method */
		if (classForm->relam == am_oid)
			count++;
	}

	/* End the scan */
	systable_endscan(scan);

	/* Close the pg_class table */
	table_close(pg_class, AccessShareLock);
	return count;
}

#endif							/* !FRONTEND */

static char tde_data_dir[MAXPGPATH] = PG_TDE_DATA_DIR;

void
pg_tde_set_data_dir(const char *dir)
{
	Assert(dir != NULL);
	strlcpy(tde_data_dir, dir, sizeof(tde_data_dir));
}

/* returns the palloc'd string */
char *
pg_tde_get_tde_data_dir(void)
{
	return tde_data_dir;
}
