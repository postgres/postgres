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
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/pg_aggregate.c,v 1.57 2003/06/24 23:14:42 momjian Exp $
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
	int			nargs_transfn;
	int			nargs_finalfn;
	Oid			procOid;
	TupleDesc	tupDesc;
	int			i;
	Oid			rettype;
	Oid		   *true_oid_array_transfn;
	Oid		   *true_oid_array_finalfn;
	bool		retset;
	FuncDetailCode fdresult;
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
		nargs_transfn = 1;
	else
	{
		fnArgs[1] = aggBaseType;
		nargs_transfn = 2;
	}

	/*
	 * func_get_detail looks up the function in the catalogs, does
	 * disambiguation for polymorphic functions, handles inheritance, and
	 * returns the funcid and type and set or singleton status of the
	 * function's return value.  it also returns the true argument types
	 * to the function.
	 */
	fdresult = func_get_detail(aggtransfnName, NIL, nargs_transfn, fnArgs,
							   &transfn, &rettype, &retset,
							   &true_oid_array_transfn);

	/* only valid case is a normal function */
	if (fdresult != FUNCDETAIL_NORMAL)
		func_error("AggregateCreate", aggtransfnName, nargs_transfn, fnArgs, NULL);

	if (!OidIsValid(transfn))
		func_error("AggregateCreate", aggtransfnName, nargs_transfn, fnArgs, NULL);

	/*
	 * enforce consistency with ANYARRAY and ANYELEMENT argument
	 * and return types, possibly modifying return type along the way
	 */
	rettype = enforce_generic_type_consistency(fnArgs, true_oid_array_transfn,
													   nargs_transfn, rettype);

	if (rettype != aggTransType)
		elog(ERROR, "return type of transition function %s is not %s",
		 NameListToString(aggtransfnName), format_type_be(aggTransType));

	tup = SearchSysCache(PROCOID,
						 ObjectIdGetDatum(transfn),
						 0, 0, 0);
	if (!HeapTupleIsValid(tup))
		func_error("AggregateCreate", aggtransfnName,
						nargs_transfn, fnArgs, NULL);
	proc = (Form_pg_proc) GETSTRUCT(tup);

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
		nargs_finalfn = 1;

		fdresult = func_get_detail(aggfinalfnName, NIL, 1, fnArgs,
								   &finalfn, &rettype, &retset,
								   &true_oid_array_finalfn);

		/* only valid case is a normal function */
		if (fdresult != FUNCDETAIL_NORMAL)
			func_error("AggregateCreate", aggfinalfnName, 1, fnArgs, NULL);

		if (!OidIsValid(finalfn))
			func_error("AggregateCreate", aggfinalfnName, 1, fnArgs, NULL);

		/*
		 * enforce consistency with ANYARRAY and ANYELEMENT argument
		 * and return types, possibly modifying return type along the way
		 */
		finaltype = enforce_generic_type_consistency(fnArgs,
													 true_oid_array_finalfn,
													 nargs_finalfn, rettype);
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
	 * special disallowed cases:
	 * 1)	if finaltype is polymorphic, basetype cannot be ANY
	 * 2)	if finaltype is polymorphic, both args to transfn must be
	 *		polymorphic
	 */
	if (finaltype == ANYARRAYOID || finaltype == ANYELEMENTOID)
	{
		if (aggBaseType == ANYOID)
			elog(ERROR, "aggregate with base type ANY must have a " \
						"non-polymorphic return type");

		if (nargs_transfn > 1 && (
			(true_oid_array_transfn[0] != ANYARRAYOID &&
			 true_oid_array_transfn[0] != ANYELEMENTOID) ||
			(true_oid_array_transfn[1] != ANYARRAYOID &&
			 true_oid_array_transfn[1] != ANYELEMENTOID)))
			elog(ERROR, "aggregate with polymorphic return type requires " \
						"state function with both arguments polymorphic");
	}

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
