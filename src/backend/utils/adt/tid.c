/*-------------------------------------------------------------------------
 *
 * tid.c
 *	  Functions for the built-in type tuple id
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/tid.c,v 1.29 2002/03/30 01:02:41 tgl Exp $
 *
 * NOTES
 *	  input routine largely stolen from boxin().
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/namespace.h"
#include "utils/builtins.h"

#define DatumGetItemPointer(X)	 ((ItemPointer) DatumGetPointer(X))
#define ItemPointerGetDatum(X)	 PointerGetDatum(X)
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
	ItemPointer itemPtr = PG_GETARG_ITEMPOINTER(0);
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
	ItemPointer arg1 = PG_GETARG_ITEMPOINTER(0);
	ItemPointer arg2 = PG_GETARG_ITEMPOINTER(1);

	PG_RETURN_BOOL(BlockIdGetBlockNumber(&(arg1->ip_blkid)) ==
				   BlockIdGetBlockNumber(&(arg2->ip_blkid)) &&
				   arg1->ip_posid == arg2->ip_posid);
}

#ifdef NOT_USED
Datum
tidne(PG_FUNCTION_ARGS)
{
	ItemPointer arg1 = PG_GETARG_ITEMPOINTER(0);
	ItemPointer arg2 = PG_GETARG_ITEMPOINTER(1);

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

static ItemPointerData Current_last_tid = {{0, 0}, 0};

void
setLastTid(const ItemPointer tid)
{
	Current_last_tid = *tid;
}

Datum
currtid_byreloid(PG_FUNCTION_ARGS)
{
	Oid			reloid = PG_GETARG_OID(0);
	ItemPointer tid = PG_GETARG_ITEMPOINTER(1);
	ItemPointer result;
	Relation	rel;

	result = (ItemPointer) palloc(sizeof(ItemPointerData));
	if (!reloid)
	{
		*result = Current_last_tid;
		PG_RETURN_ITEMPOINTER(result);
	}

	rel = heap_open(reloid, AccessShareLock);

	ItemPointerCopy(tid, result);
	heap_get_latest_tid(rel, SnapshotNow, result);

	heap_close(rel, AccessShareLock);

	PG_RETURN_ITEMPOINTER(result);
}

Datum
currtid_byrelname(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_P(0);
	ItemPointer tid = PG_GETARG_ITEMPOINTER(1);
	ItemPointer result;
	RangeVar   *relrv;
	Relation	rel;

	relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname,
														"currtid_byrelname"));
	rel = heap_openrv(relrv, AccessShareLock);

	result = (ItemPointer) palloc(sizeof(ItemPointerData));
	ItemPointerCopy(tid, result);

	heap_get_latest_tid(rel, SnapshotNow, result);

	heap_close(rel, AccessShareLock);

	PG_RETURN_ITEMPOINTER(result);
}
