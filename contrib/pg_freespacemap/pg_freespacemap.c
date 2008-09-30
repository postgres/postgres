/*-------------------------------------------------------------------------
 *
 * pg_freespacemap.c
 *	  display contents of a free space map
 *
 *	  $PostgreSQL: pgsql/contrib/pg_freespacemap/pg_freespacemap.c,v 1.11 2008/09/30 11:17:07 heikki Exp $
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "storage/freespace.h"
#include "utils/builtins.h"


PG_MODULE_MAGIC;

Datum		pg_freespace(PG_FUNCTION_ARGS);
Datum		pg_freespacedump(PG_FUNCTION_ARGS);

/*
 * Returns the amount of free space on a given page, according to the
 * free space map.
 */
PG_FUNCTION_INFO_V1(pg_freespace);

Datum
pg_freespace(PG_FUNCTION_ARGS)
{
	Oid		relid = PG_GETARG_OID(0);
	uint32	blkno = PG_GETARG_UINT32(1);
	int16	freespace;
	Relation rel;

	rel = relation_open(relid, AccessShareLock);

	if (!BlockNumberIsValid(blkno))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid block number")));

	freespace = GetRecordedFreeSpace(rel, blkno);

	relation_close(rel, AccessShareLock);
	PG_RETURN_INT16(freespace);
}
