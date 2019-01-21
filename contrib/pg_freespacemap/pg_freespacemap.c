/*-------------------------------------------------------------------------
 *
 * pg_freespacemap.c
 *	  display contents of a free space map
 *
 *	  contrib/pg_freespacemap/pg_freespacemap.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relation.h"
#include "funcapi.h"
#include "storage/freespace.h"

PG_MODULE_MAGIC;

/*
 * Returns the amount of free space on a given page, according to the
 * free space map.
 */
PG_FUNCTION_INFO_V1(pg_freespace);

Datum
pg_freespace(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		blkno = PG_GETARG_INT64(1);
	int16		freespace;
	Relation	rel;

	rel = relation_open(relid, AccessShareLock);

	if (blkno < 0 || blkno > MaxBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid block number")));

	freespace = GetRecordedFreeSpace(rel, blkno);

	relation_close(rel, AccessShareLock);
	PG_RETURN_INT16(freespace);
}
