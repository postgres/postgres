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
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/tid.c,v 1.21 2000/07/05 23:11:35 tgl Exp $
 *
 * NOTES
 *	  input routine largely stolen from boxin().
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "utils/builtins.h"

#define LDELIM			'('
#define RDELIM			')'
#define DELIM			','
#define NTIDARGS		2

/* ----------------------------------------------------------------
 *		tidin
 * ----------------------------------------------------------------
 */
ItemPointer
tidin(const char *str)
{
	const char *p,
			   *coord[NTIDARGS];
	int			i;
	ItemPointer result;

	BlockNumber blockNumber;
	OffsetNumber offsetNumber;

	if (str == NULL)
		return NULL;

	for (i = 0, p = str; *p && i < NTIDARGS && *p != RDELIM; p++)
		if (*p == DELIM || (*p == LDELIM && !i))
			coord[i++] = p + 1;

	/* if (i < NTIDARGS - 1) */
	if (i < NTIDARGS)
	{
		elog(ERROR, "%s invalid tid format", str);
		return NULL;
	}

	blockNumber = (BlockNumber) atoi(coord[0]);
	offsetNumber = (OffsetNumber) atoi(coord[1]);

	result = (ItemPointer) palloc(sizeof(ItemPointerData));

	ItemPointerSet(result, blockNumber, offsetNumber);

	return result;
}

/* ----------------------------------------------------------------
 *		tidout
 * ----------------------------------------------------------------
 */
char *
tidout(ItemPointer itemPtr)
{
	BlockNumber blockNumber;
	OffsetNumber offsetNumber;
	BlockId		blockId;
	char		buf[32];
	char	   *str;
	static char *invalidTid = "()";

	if (!itemPtr || !ItemPointerIsValid(itemPtr))
	{
		str = palloc(strlen(invalidTid));
		strcpy(str, invalidTid);
		return str;
	}

	blockId = &(itemPtr->ip_blkid);

	blockNumber = BlockIdGetBlockNumber(blockId);
	offsetNumber = itemPtr->ip_posid;

	sprintf(buf, "(%d,%d)", blockNumber, offsetNumber);

	str = (char *) palloc(strlen(buf) + 1);
	strcpy(str, buf);

	return str;
}

/*****************************************************************************
 *	 PUBLIC ROUTINES														 *
 *****************************************************************************/

bool
tideq(ItemPointer arg1, ItemPointer arg2)
{
	if ((!arg1) || (!arg2))
		return false;

	return (BlockIdGetBlockNumber(&(arg1->ip_blkid)) ==
			BlockIdGetBlockNumber(&(arg2->ip_blkid)) &&
			arg1->ip_posid == arg2->ip_posid);
}

#ifdef NOT_USED
bool
tidne(ItemPointer arg1, ItemPointer arg2)
{
	if ((!arg1) || (!arg2))
		return false;
	return (BlockIdGetBlockNumber(&(arg1->ip_blkid)) !=
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
	ItemPointer		tid = (ItemPointer) PG_GETARG_POINTER(1);
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

	PG_RETURN_POINTER(result);
}

Datum
currtid_byrelname(PG_FUNCTION_ARGS)
{
	text		   *relname = PG_GETARG_TEXT_P(0);
	ItemPointer		tid = (ItemPointer) PG_GETARG_POINTER(1);
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

	PG_RETURN_POINTER(result);
}
