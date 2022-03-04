/*-------------------------------------------------------------------------
 *
 * tid.c
 *	  Functions for the built-in type tuple id
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/tid.c
 *
 * NOTES
 *	  input routine largely stolen from boxin().
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>
#include <limits.h>

#include "access/heapam.h"
#include "access/sysattr.h"
#include "access/tableam.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "parser/parsetree.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/varlena.h"


#define DatumGetItemPointer(X)	 ((ItemPointer) DatumGetPointer(X))
#define ItemPointerGetDatum(X)	 PointerGetDatum(X)
#define PG_GETARG_ITEMPOINTER(n) DatumGetItemPointer(PG_GETARG_DATUM(n))
#define PG_RETURN_ITEMPOINTER(x) return ItemPointerGetDatum(x)

#define LDELIM			'('
#define RDELIM			')'
#define DELIM			','
#define NTIDARGS		2

static ItemPointer currtid_for_view(Relation viewrel, ItemPointer tid);

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
	unsigned long cvt;

	for (i = 0, p = str; *p && i < NTIDARGS && *p != RDELIM; p++)
		if (*p == DELIM || (*p == LDELIM && i == 0))
			coord[i++] = p + 1;

	if (i < NTIDARGS)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s: \"%s\"",
						"tid", str)));

	errno = 0;
	cvt = strtoul(coord[0], &badp, 10);
	if (errno || *badp != DELIM)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s: \"%s\"",
						"tid", str)));
	blockNumber = (BlockNumber) cvt;

	/*
	 * Cope with possibility that unsigned long is wider than BlockNumber, in
	 * which case strtoul will not raise an error for some values that are out
	 * of the range of BlockNumber.  (See similar code in oidin().)
	 */
#if SIZEOF_LONG > 4
	if (cvt != (unsigned long) blockNumber &&
		cvt != (unsigned long) ((int32) blockNumber))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s: \"%s\"",
						"tid", str)));
#endif

	cvt = strtoul(coord[1], &badp, 10);
	if (errno || *badp != RDELIM ||
		cvt > USHRT_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s: \"%s\"",
						"tid", str)));
	offsetNumber = (OffsetNumber) cvt;

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
	BlockNumber blockNumber;
	OffsetNumber offsetNumber;
	char		buf[32];

	blockNumber = ItemPointerGetBlockNumberNoCheck(itemPtr);
	offsetNumber = ItemPointerGetOffsetNumberNoCheck(itemPtr);

	/* Perhaps someday we should output this as a record. */
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
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint32(&buf, ItemPointerGetBlockNumberNoCheck(itemPtr));
	pq_sendint16(&buf, ItemPointerGetOffsetNumberNoCheck(itemPtr));
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

	PG_RETURN_BOOL(ItemPointerCompare(arg1, arg2) == 0);
}

Datum
tidne(PG_FUNCTION_ARGS)
{
	ItemPointer arg1 = PG_GETARG_ITEMPOINTER(0);
	ItemPointer arg2 = PG_GETARG_ITEMPOINTER(1);

	PG_RETURN_BOOL(ItemPointerCompare(arg1, arg2) != 0);
}

Datum
tidlt(PG_FUNCTION_ARGS)
{
	ItemPointer arg1 = PG_GETARG_ITEMPOINTER(0);
	ItemPointer arg2 = PG_GETARG_ITEMPOINTER(1);

	PG_RETURN_BOOL(ItemPointerCompare(arg1, arg2) < 0);
}

Datum
tidle(PG_FUNCTION_ARGS)
{
	ItemPointer arg1 = PG_GETARG_ITEMPOINTER(0);
	ItemPointer arg2 = PG_GETARG_ITEMPOINTER(1);

	PG_RETURN_BOOL(ItemPointerCompare(arg1, arg2) <= 0);
}

Datum
tidgt(PG_FUNCTION_ARGS)
{
	ItemPointer arg1 = PG_GETARG_ITEMPOINTER(0);
	ItemPointer arg2 = PG_GETARG_ITEMPOINTER(1);

	PG_RETURN_BOOL(ItemPointerCompare(arg1, arg2) > 0);
}

Datum
tidge(PG_FUNCTION_ARGS)
{
	ItemPointer arg1 = PG_GETARG_ITEMPOINTER(0);
	ItemPointer arg2 = PG_GETARG_ITEMPOINTER(1);

	PG_RETURN_BOOL(ItemPointerCompare(arg1, arg2) >= 0);
}

Datum
bttidcmp(PG_FUNCTION_ARGS)
{
	ItemPointer arg1 = PG_GETARG_ITEMPOINTER(0);
	ItemPointer arg2 = PG_GETARG_ITEMPOINTER(1);

	PG_RETURN_INT32(ItemPointerCompare(arg1, arg2));
}

Datum
tidlarger(PG_FUNCTION_ARGS)
{
	ItemPointer arg1 = PG_GETARG_ITEMPOINTER(0);
	ItemPointer arg2 = PG_GETARG_ITEMPOINTER(1);

	PG_RETURN_ITEMPOINTER(ItemPointerCompare(arg1, arg2) >= 0 ? arg1 : arg2);
}

Datum
tidsmaller(PG_FUNCTION_ARGS)
{
	ItemPointer arg1 = PG_GETARG_ITEMPOINTER(0);
	ItemPointer arg2 = PG_GETARG_ITEMPOINTER(1);

	PG_RETURN_ITEMPOINTER(ItemPointerCompare(arg1, arg2) <= 0 ? arg1 : arg2);
}

Datum
hashtid(PG_FUNCTION_ARGS)
{
	ItemPointer key = PG_GETARG_ITEMPOINTER(0);

	/*
	 * While you'll probably have a lot of trouble with a compiler that
	 * insists on appending pad space to struct ItemPointerData, we can at
	 * least make this code work, by not using sizeof(ItemPointerData).
	 * Instead rely on knowing the sizes of the component fields.
	 */
	return hash_any((unsigned char *) key,
					sizeof(BlockIdData) + sizeof(OffsetNumber));
}

Datum
hashtidextended(PG_FUNCTION_ARGS)
{
	ItemPointer key = PG_GETARG_ITEMPOINTER(0);
	uint64		seed = PG_GETARG_INT64(1);

	/* As above */
	return hash_any_extended((unsigned char *) key,
							 sizeof(BlockIdData) + sizeof(OffsetNumber),
							 seed);
}


/*
 *	Functions to get latest tid of a specified tuple.
 *
 *	Maybe these implementations should be moved to another place
 */

/*
 * Utility wrapper for current CTID functions.
 *		Returns the latest version of a tuple pointing at "tid" for
 *		relation "rel".
 */
static ItemPointer
currtid_internal(Relation rel, ItemPointer tid)
{
	ItemPointer result;
	AclResult	aclresult;
	Snapshot	snapshot;
	TableScanDesc scan;

	result = (ItemPointer) palloc(sizeof(ItemPointerData));

	aclresult = pg_class_aclcheck(RelationGetRelid(rel), GetUserId(),
								  ACL_SELECT);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, get_relkind_objtype(rel->rd_rel->relkind),
					   RelationGetRelationName(rel));

	if (rel->rd_rel->relkind == RELKIND_VIEW)
		return currtid_for_view(rel, tid);

	if (!RELKIND_HAS_STORAGE(rel->rd_rel->relkind))
		elog(ERROR, "cannot look at latest visible tid for relation \"%s.%s\"",
			 get_namespace_name(RelationGetNamespace(rel)),
			 RelationGetRelationName(rel));

	ItemPointerCopy(tid, result);

	snapshot = RegisterSnapshot(GetLatestSnapshot());
	scan = table_beginscan_tid(rel, snapshot);
	table_tuple_get_latest_tid(scan, result);
	table_endscan(scan);
	UnregisterSnapshot(snapshot);

	return result;
}

/*
 *	Handle CTIDs of views.
 *		CTID should be defined in the view and it must
 *		correspond to the CTID of a base relation.
 */
static ItemPointer
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
		Form_pg_attribute attr = TupleDescAttr(att, i);

		if (strcmp(NameStr(attr->attname), "ctid") == 0)
		{
			if (attr->atttypid != TIDOID)
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

			if (list_length(rewrite->actions) != 1)
				elog(ERROR, "only one select rule is allowed in views");
			query = (Query *) linitial(rewrite->actions);
			tle = get_tle_by_resno(query->targetList, tididx + 1);
			if (tle && tle->expr && IsA(tle->expr, Var))
			{
				Var		   *var = (Var *) tle->expr;
				RangeTblEntry *rte;

				if (!IS_SPECIAL_VARNO(var->varno) &&
					var->varattno == SelfItemPointerAttributeNumber)
				{
					rte = rt_fetch(var->varno, query->rtable);
					if (rte)
					{
						ItemPointer result;
						Relation	rel;

						rel = table_open(rte->relid, AccessShareLock);
						result = currtid_internal(rel, tid);
						table_close(rel, AccessShareLock);
						return result;
					}
				}
			}
			break;
		}
	}
	elog(ERROR, "currtid cannot handle this view");
	return NULL;
}

/*
 * currtid_byrelname
 *		Get the latest tuple version of the tuple pointing at a CTID, for a
 *		given relation name.
 */
Datum
currtid_byrelname(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_PP(0);
	ItemPointer tid = PG_GETARG_ITEMPOINTER(1);
	ItemPointer result;
	RangeVar   *relrv;
	Relation	rel;

	relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname));
	rel = table_openrv(relrv, AccessShareLock);

	/* grab the latest tuple version associated to this CTID */
	result = currtid_internal(rel, tid);

	table_close(rel, AccessShareLock);

	PG_RETURN_ITEMPOINTER(result);
}
