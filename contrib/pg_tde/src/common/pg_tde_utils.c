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

#include "common/pg_tde_utils.h"
#include "pg_tde.h"

#ifndef FRONTEND
#include "fmgr.h"
#include "smgr/pg_tde_smgr.h"
#include "access/relation.h"
#include "utils/rel.h"

PG_FUNCTION_INFO_V1(pg_tde_is_encrypted);
Datum
pg_tde_is_encrypted(PG_FUNCTION_ARGS)
{
	Oid			relationOid = PG_GETARG_OID(0);
	LOCKMODE	lockmode = AccessShareLock;
	Relation	rel = relation_open(relationOid, lockmode);
	bool		result;

	if (!RELKIND_HAS_STORAGE(rel->rd_rel->relkind))
	{
		relation_close(rel, lockmode);
		PG_RETURN_NULL();
	}

	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("we cannot check if temporary relations from other backends are encrypted"));

	result = tde_smgr_rel_is_encrypted(RelationGetSmgr(rel));

	relation_close(rel, lockmode);

	PG_RETURN_BOOL(result);
}

#endif							/* !FRONTEND */

static char tde_data_dir[MAXPGPATH] = PG_TDE_DATA_DIR;

void
pg_tde_set_data_dir(const char *dir)
{
	Assert(dir != NULL);
	strlcpy(tde_data_dir, dir, sizeof(tde_data_dir));
}

const char *
pg_tde_get_data_dir(void)
{
	return tde_data_dir;
}
