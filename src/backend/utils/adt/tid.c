/*-------------------------------------------------------------------------
 *
 * tid.c
 *	  Functions for the built-in type tuple id
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/tid.c,v 1.41 2003/09/25 06:58:04 petere Exp $
 *
 * NOTES
 *	  input routine largely stolen from boxin().
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <errno.h>
#include <math.h>
#include <limits.h>

#include "access/heapam.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "libpq/pqformat.h"
#include "parser/parsetree.h"
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
	char	   *badp;
	int			hold_offset;

	for (i = 0, p = str; *p && i < NTIDARGS && *p != RDELIM; p++)
		if (*p == DELIM || (*p == LDELIM && !i))
			coord[i++] = p + 1;

	if (i < NTIDARGS)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type tid: \"%s\"",
						str)));

	errno = 0;
	blockNumber = strtoul(coord[0], &badp, 10);
	if (errno || *badp != DELIM)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type tid: \"%s\"",
						str)));

	hold_offset = strtol(coord[1], &badp, 10);
	if (errno || *badp != RDELIM ||
		hold_offset > USHRT_MAX || hold_offset < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type tid: \"%s\"",
						str)));

	offsetNumber = hold_offset;

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

	if (!ItemPointerIsValid(itemPtr))
		PG_RETURN_CSTRING(pstrdup("()"));

	blockId = &(itemPtr->ip_blkid);
	blockNumber = BlockIdGetBlockNumber(blockId);
	offsetNumber = itemPtr->ip_posid;

	snprintf(buf, sizeof(buf), "(%u,%u)", blockNumber, offsetNumber);

	PG_RETURN_CSTRING(pstrdup(buf));
}

/*
 *		tidrecv			- converts external binary format to tid
 */
Datum
tidrecv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	ItemPointer result;
	BlockNumber blockNumber;
	OffsetNumber offsetNumber;

	blockNumber = pq_getmsgint(buf, sizeof(blockNumber));
	offsetNumber = pq_getmsgint(buf, sizeof(offsetNumber));

	result = (ItemPointer) palloc(sizeof(ItemPointerData));

	ItemPointerSet(result, blockNumber, offsetNumber);

	PG_RETURN_ITEMPOINTER(result);
}

/*
 *		tidsend			- converts tid to binary format
 */
Datum
tidsend(PG_FUNCTION_ARGS)
{
	ItemPointer itemPtr = PG_GETARG_ITEMPOINTER(0);
	BlockId		blockId;
	BlockNumber blockNumber;
	OffsetNumber offsetNumber;
	StringInfoData buf;

	blockId = &(itemPtr->ip_blkid);
	blockNumber = BlockIdGetBlockNumber(blockId);
	offsetNumber = itemPtr->ip_posid;

	pq_begintypsend(&buf);
	pq_sendint(&buf, blockNumber, sizeof(blockNumber));
	pq_sendint(&buf, offsetNumber, sizeof(offsetNumber));
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
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

/*
 *	Handle CTIDs of views.
 *		CTID should be defined in the view and it must
 *		correspond to the CTID of a base relation.
 */
static Datum
currtid_for_view(Relation viewrel, ItemPointer tid)
{
	TupleDesc	att = RelationGetDescr(viewrel);
	RuleLock   *rulelock;
	RewriteRule *rewrite;
	int			i,
				natts = att->natts,
				tididx = -1;

	for (i = 0; i < natts; i++)
	{
		if (strcasecmp(NameStr(att->attrs[i]->attname), "ctid") == 0)
		{
			if (att->attrs[i]->atttypid != TIDOID)
				elog(ERROR, "ctid isn't of type TID");
			tididx = i;
			break;
		}
	}
	if (tididx < 0)
		elog(ERROR, "currtid cannot handle views with no CTID");
	rulelock = viewrel->rd_rules;
	if (!rulelock)
		elog(ERROR, "the view has no rules");
	for (i = 0; i < rulelock->numLocks; i++)
	{
		rewrite = rulelock->rules[i];
		if (rewrite->event == CMD_SELECT)
		{
			Query	   *query;
			TargetEntry *tle;

			if (length(rewrite->actions) != 1)
				elog(ERROR, "only one select rule is allowed in views");
			query = (Query *) lfirst(rewrite->actions);
			tle = get_tle_by_resno(query->targetList, tididx+1);
			if (tle && tle->expr && IsA(tle->expr, Var))
			{
				Var		   *var = (Var *) tle->expr;
				RangeTblEntry *rte;

				if (var->varno > 0 && var->varno < INNER &&
					var->varattno == SelfItemPointerAttributeNumber)
				{
					rte = rt_fetch(var->varno, query->rtable);
					if (rte)
					{
						heap_close(viewrel, AccessShareLock);
						return DirectFunctionCall2(currtid_byreloid, ObjectIdGetDatum(rte->relid), PointerGetDatum(tid));
					}
				}
			}
			break;
		}
	}
	elog(ERROR, "currtid cannot handle this view");
	return (Datum) 0;
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
	if (rel->rd_rel->relkind == RELKIND_VIEW)
		return currtid_for_view(rel, tid);

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
	if (rel->rd_rel->relkind == RELKIND_VIEW)
		return currtid_for_view(rel, tid);

	result = (ItemPointer) palloc(sizeof(ItemPointerData));
	ItemPointerCopy(tid, result);

	heap_get_latest_tid(rel, SnapshotNow, result);

	heap_close(rel, AccessShareLock);

	PG_RETURN_ITEMPOINTER(result);
}
