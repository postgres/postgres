/*-------------------------------------------------------------------------
 *
 * pg_aggregate.c
 *	  routines to support manipulation of the pg_aggregate relation
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/pg_aggregate.c,v 1.58 2003/06/25 21:30:25 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "optimizer/cost.h"
#include "parser/parse_coerce.h"
#include "parser/parse_func.h"
#include "utils/builtins.h"
#include "utils/syscache.h"


/*
 * AggregateCreate
 */
void
AggregateCreate(const char *aggName,
				Oid aggNamespace,
				List *aggtransfnName,
				List *aggfinalfnName,
				Oid aggBaseType,
				Oid aggTransType,
				const char *agginitval)
{
	Relation	aggdesc;
	HeapTuple	tup;
	char		nulls[Natts_pg_aggregate];
	Datum		values[Natts_pg_aggregate];
	Form_pg_proc proc;
	Oid			transfn;
	Oid			finalfn = InvalidOid;	/* can be omitted */
	Oid			finaltype;
	Oid			fnArgs[FUNC_MAX_ARGS];
	int			nargs;
	Oid			procOid;
	TupleDesc	tupDesc;
	int			i;
	ObjectAddress myself,
				referenced;

	/* sanity checks */
	if (!aggName)
		elog(ERROR, "no aggregate name supplied");

	if (!aggtransfnName)
		elog(ERROR, "aggregate must have a transition function");

	/* handle transfn */
	MemSet(fnArgs, 0, FUNC_MAX_ARGS * sizeof(Oid));
	fnArgs[0] = aggTransType;
	if (aggBaseType == ANYOID)
		nargs = 1;
	else
	{
		fnArgs[1] = aggBaseType;
		nargs = 2;
	}
	transfn = LookupFuncName(aggtransfnName, nargs, fnArgs);
	if (!OidIsValid(transfn))
		func_error("AggregateCreate", aggtransfnName, nargs, fnArgs, NULL);
	tup = SearchSysCache(PROCOID,
						 ObjectIdGetDatum(transfn),
						 0, 0, 0);
	if (!HeapTupleIsValid(tup))
		func_error("AggregateCreate", aggtransfnName, nargs, fnArgs, NULL);
	proc = (Form_pg_proc) GETSTRUCT(tup);
	if (proc->prorettype != aggTransType)
		elog(ERROR, "return type of transition function %s is not %s",
		 NameListToString(aggtransfnName), format_type_be(aggTransType));

	/*
	 * If the transfn is strict and the initval is NULL, make sure input
	 * type and transtype are the same (or at least binary-compatible), so
	 * that it's OK to use the first input value as the initial
	 * transValue.
	 */
	if (proc->proisstrict && agginitval == NULL)
	{
		if (!IsBinaryCoercible(aggBaseType, aggTransType))
			elog(ERROR, "must not omit initval when transfn is strict and transtype is not compatible with input type");
	}
	ReleaseSysCache(tup);

	/* handle finalfn, if supplied */
	if (aggfinalfnName)
	{
		MemSet(fnArgs, 0, FUNC_MAX_ARGS * sizeof(Oid));
		fnArgs[0] = aggTransType;
		finalfn = LookupFuncName(aggfinalfnName, 1, fnArgs);
		if (!OidIsValid(finalfn))
			func_error("AggregateCreate", aggfinalfnName, 1, fnArgs, NULL);
		tup = SearchSysCache(PROCOID,
							 ObjectIdGetDatum(finalfn),
							 0, 0, 0);
		if (!HeapTupleIsValid(tup))
			func_error("AggregateCreate", aggfinalfnName, 1, fnArgs, NULL);
		proc = (Form_pg_proc) GETSTRUCT(tup);
		finaltype = proc->prorettype;
		ReleaseSysCache(tup);
	}
	else
	{
		/*
		 * If no finalfn, aggregate result type is type of the state value
		 */
		finaltype = aggTransType;
	}
	Assert(OidIsValid(finaltype));

	/*
	 * Everything looks okay.  Try to create the pg_proc entry for the
	 * aggregate.  (This could fail if there's already a conflicting
	 * entry.)
	 */
	MemSet(fnArgs, 0, FUNC_MAX_ARGS * sizeof(Oid));
	fnArgs[0] = aggBaseType;

	procOid = ProcedureCreate(aggName,
							  aggNamespace,
							  false,	/* no replacement */
							  false,	/* doesn't return a set */
							  finaltype,		/* returnType */
							  INTERNALlanguageId,		/* languageObjectId */
							  0,
							  "aggregate_dummy",		/* placeholder proc */
							  "-",		/* probin */
							  true,		/* isAgg */
							  false,	/* security invoker (currently not
										 * definable for agg) */
							  false,	/* isStrict (not needed for agg) */
							  PROVOLATILE_IMMUTABLE,	/* volatility (not
														 * needed for agg) */
							  1,	/* parameterCount */
							  fnArgs);	/* parameterTypes */

	/*
	 * Okay to create the pg_aggregate entry.
	 */

	/* initialize nulls and values */
	for (i = 0; i < Natts_pg_aggregate; i++)
	{
		nulls[i] = ' ';
		values[i] = (Datum) NULL;
	}
	values[Anum_pg_aggregate_aggfnoid - 1] = ObjectIdGetDatum(procOid);
	values[Anum_pg_aggregate_aggtransfn - 1] = ObjectIdGetDatum(transfn);
	values[Anum_pg_aggregate_aggfinalfn - 1] = ObjectIdGetDatum(finalfn);
	values[Anum_pg_aggregate_aggtranstype - 1] = ObjectIdGetDatum(aggTransType);
	if (agginitval)
		values[Anum_pg_aggregate_agginitval - 1] =
			DirectFunctionCall1(textin, CStringGetDatum(agginitval));
	else
		nulls[Anum_pg_aggregate_agginitval - 1] = 'n';

	aggdesc = heap_openr(AggregateRelationName, RowExclusiveLock);
	tupDesc = aggdesc->rd_att;

	tup = heap_formtuple(tupDesc, values, nulls);
	simple_heap_insert(aggdesc, tup);

	CatalogUpdateIndexes(aggdesc, tup);

	heap_close(aggdesc, RowExclusiveLock);

	/*
	 * Create dependencies for the aggregate (above and beyond those
	 * already made by ProcedureCreate).  Note: we don't need an explicit
	 * dependency on aggTransType since we depend on it indirectly through
	 * transfn.
	 */
	myself.classId = RelOid_pg_proc;
	myself.objectId = procOid;
	myself.objectSubId = 0;

	/* Depends on transition function */
	referenced.classId = RelOid_pg_proc;
	referenced.objectId = transfn;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* Depends on final function, if any */
	if (OidIsValid(finalfn))
	{
		referenced.classId = RelOid_pg_proc;
		referenced.objectId = finalfn;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}
}
