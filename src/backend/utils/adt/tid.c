/*-------------------------------------------------------------------------
 *
 * tid.c
 *	  Functions for the built-in type tuple id
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/tid.c,v 1.22 2000/08/03 16:34:23 tgl Exp $
 *
 * NOTES
 *	  input routine largely stolen from boxin().
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "utils/builtins.h"

#define DatumGetItemPointer(X)   ((ItemPointer) DatumGetPointer(X))
#define ItemPointerGetDatum(X)   PointerGetDatum(X)
#define PG_GETARG_ITEMPOINTER(n) DatumGetItemPointer(PG_GETARG_DATUM(n))
#define PG_RETURN_ITEMPOINTER(x) return ItemPointerGetDatum(x)

#define LDELIM			'('
#define RDELIM			')'
#define DELIM			','
#define NTIDARGS		2

/* ----------------------------------------------------------------
 *		tidin
 * ----------------------------------------------------------------
 */
Datum
tidin(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	char	   *p,
			   *coord[NTIDARGS];
	int			i;
	ItemPointer result;
	BlockNumber blockNumber;
	OffsetNumber offsetNumber;

	for (i = 0, p = str; *p && i < NTIDARGS && *p != RDELIM; p++)
		if (*p == DELIM || (*p == LDELIM && !i))
			coord[i++] = p + 1;

	if (i < NTIDARGS)
		elog(ERROR, "invalid tid format: '%s'", str);

	blockNumber = (BlockNumber) atoi(coord[0]);
	offsetNumber = (OffsetNumber) atoi(coord[1]);

	result = (ItemPointer) palloc(sizeof(ItemPointerData));

	ItemPointerSet(result, blockNumber, offsetNumber);

	PG_RETURN_ITEMPOINTER(result);
}

/* ----------------------------------------------------------------
 *		tidout
 * ----------------------------------------------------------------
 */
Datum
tidout(PG_FUNCTION_ARGS)
{
	ItemPointer	itemPtr = PG_GETARG_ITEMPOINTER(0);
	BlockId		blockId;
	BlockNumber blockNumber;
	OffsetNumber offsetNumber;
	char		buf[32];
	static char *invalidTid = "()";

	if (!ItemPointerIsValid(itemPtr))
		PG_RETURN_CSTRING(pstrdup(invalidTid));

	blockId = &(itemPtr->ip_blkid);

	blockNumber = BlockIdGetBlockNumber(blockId);
	offsetNumber = itemPtr->ip_posid;

	sprintf(buf, "(%d,%d)", (int) blockNumber, (int) offsetNumber);

	PG_RETURN_CSTRING(pstrdup(buf));
}

/*****************************************************************************
 *	 PUBLIC ROUTINES														 *
 *****************************************************************************/

Datum
tideq(PG_FUNCTION_ARGS)
{
	ItemPointer		arg1 = PG_GETARG_ITEMPOINTER(0);
	ItemPointer		arg2 = PG_GETARG_ITEMPOINTER(1);

	PG_RETURN_BOOL(BlockIdGetBlockNumber(&(arg1->ip_blkid)) ==
				   BlockIdGetBlockNumber(&(arg2->ip_blkid)) &&
				   arg1->ip_posid == arg2->ip_posid);
}

#ifdef NOT_USED
Datum
tidne(PG_FUNCTION_ARGS)
{
	ItemPointer		arg1 = PG_GETARG_ITEMPOINTER(0);
	ItemPointer		arg2 = PG_GETARG_ITEMPOINTER(1);

	PG_RETURN_BOOL(BlockIdGetBlockNumber(&(arg1->ip_blkid)) !=
				   BlockIdGetBlockNumber(&(arg2->ip_blkid)) ||
				   arg1->ip_posid != arg2->ip_posid);
}
#endif

/*
 *	Functions to get latest tid of a specified tuple.
 *
 *	Maybe these implementations should be moved to another place
 */
Datum
currtid_byreloid(PG_FUNCTION_ARGS)
{
	Oid				reloid = PG_GETARG_OID(0);
	ItemPointer		tid = PG_GETARG_ITEMPOINTER(1);
	ItemPointer		result,
					ret;
	Relation		rel;

	result = (ItemPointer) palloc(sizeof(ItemPointerData));
	ItemPointerSetInvalid(result);
	if ((rel = heap_open(reloid, AccessShareLock)) != NULL)
	{
		ret = heap_get_latest_tid(rel, SnapshotNow, tid);
		if (ret)
			ItemPointerCopy(ret, result);
		heap_close(rel, AccessShareLock);
	}
	else
		elog(ERROR, "Relation %u not found", reloid);

	PG_RETURN_ITEMPOINTER(result);
}

Datum
currtid_byrelname(PG_FUNCTION_ARGS)
{
	text		   *relname = PG_GETARG_TEXT_P(0);
	ItemPointer		tid = PG_GETARG_ITEMPOINTER(1);
	ItemPointer		result,
					ret;
	char		   *str;
	Relation		rel;

	str = DatumGetCString(DirectFunctionCall1(textout,
											  PointerGetDatum(relname)));

	result = (ItemPointer) palloc(sizeof(ItemPointerData));
	ItemPointerSetInvalid(result);
	if ((rel = heap_openr(str, AccessShareLock)) != NULL)
	{
		ret = heap_get_latest_tid(rel, SnapshotNow, tid);
		if (ret)
			ItemPointerCopy(ret, result);
		heap_close(rel, AccessShareLock);
	}
	else
		elog(ERROR, "Relation %s not found", str);

	pfree(str);

	PG_RETURN_ITEMPOINTER(result);
}
