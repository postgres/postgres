/*-------------------------------------------------------------------------
 *
 * pg_aggregate.c
 *	  routines to support manipulation of the pg_aggregate relation
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/pg_aggregate.c,v 1.37 2001/01/24 19:42:52 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "parser/parse_coerce.h"
#include "parser/parse_func.h"
#include "utils/builtins.h"
#include "utils/syscache.h"

/* ----------------
 * AggregateCreate
 *
 * aggregates overloading has been added.  Instead of the full
 * overload support we have for functions, aggregate overloading only
 * applies to exact basetype matches.  That is, we don't check the
 * inheritance hierarchy
 *
 * OLD COMMENTS:
 *		Currently, redefining aggregates using the same name is not
 *		supported.	In such a case, a warning is printed that the
 *		aggregate already exists.  If such is not the case, a new tuple
 *		is created and inserted in the aggregate relation.
 *		All types and functions must have been defined
 *		prior to defining the aggregate.
 *
 * ---------------
 */
void
AggregateCreate(char *aggName,
				char *aggtransfnName,
				char *aggfinalfnName,
				char *aggbasetypeName,
				char *aggtranstypeName,
				char *agginitval)
{
	Relation	aggdesc;
	HeapTuple	tup;
	char		nulls[Natts_pg_aggregate];
	Datum		values[Natts_pg_aggregate];
	Form_pg_proc proc;
	Oid			transfn;
	Oid			finalfn = InvalidOid; /* can be omitted */
	Oid			basetype;
	Oid			transtype;
	Oid			finaltype;
	Oid			fnArgs[FUNC_MAX_ARGS];
	int			nargs;
	NameData	aname;
	TupleDesc	tupDesc;
	int			i;

	MemSet(fnArgs, 0, FUNC_MAX_ARGS * sizeof(Oid));

	/* sanity checks */
	if (!aggName)
		elog(ERROR, "AggregateCreate: no aggregate name supplied");

	if (!aggtransfnName)
		elog(ERROR, "AggregateCreate: aggregate must have a transition function");

	/*
	 * Handle the aggregate's base type (input data type).  This can be
	 * specified as 'ANY' for a data-independent transition function,
	 * such as COUNT(*).
	 */
	basetype = GetSysCacheOid(TYPENAME,
							  PointerGetDatum(aggbasetypeName),
							  0, 0, 0);
	if (!OidIsValid(basetype))
	{
		if (strcasecmp(aggbasetypeName, "ANY") != 0)
			elog(ERROR, "AggregateCreate: Type '%s' undefined",
				 aggbasetypeName);
		basetype = InvalidOid;
	}

	/* make sure there is no existing agg of same name and base type */
	if (SearchSysCacheExists(AGGNAME,
							 PointerGetDatum(aggName),
							 ObjectIdGetDatum(basetype),
							 0, 0))
		elog(ERROR,
			 "AggregateCreate: aggregate '%s' with base type '%s' already exists",
			 aggName, aggbasetypeName);

	/* handle transtype */
	transtype = GetSysCacheOid(TYPENAME,
							   PointerGetDatum(aggtranstypeName),
							   0, 0, 0);
	if (!OidIsValid(transtype))
		elog(ERROR, "AggregateCreate: Type '%s' undefined",
			 aggtranstypeName);

	/* handle transfn */
	fnArgs[0] = transtype;
	if (OidIsValid(basetype))
	{
		fnArgs[1] = basetype;
		nargs = 2;
	}
	else
	{
		nargs = 1;
	}
	tup = SearchSysCache(PROCNAME,
						 PointerGetDatum(aggtransfnName),
						 Int32GetDatum(nargs),
						 PointerGetDatum(fnArgs),
						 0);
	if (!HeapTupleIsValid(tup))
		func_error("AggregateCreate", aggtransfnName, nargs, fnArgs, NULL);
	transfn = tup->t_data->t_oid;
	Assert(OidIsValid(transfn));
	proc = (Form_pg_proc) GETSTRUCT(tup);
	if (proc->prorettype != transtype)
		elog(ERROR, "AggregateCreate: return type of '%s' is not '%s'",
			 aggtransfnName, aggtranstypeName);
	/*
	 * If the transfn is strict and the initval is NULL, make sure
	 * input type and transtype are the same (or at least binary-
	 * compatible), so that it's OK to use the first input value
	 * as the initial transValue.
	 */
	if (proc->proisstrict && agginitval == NULL)
	{
		if (basetype != transtype &&
			! IS_BINARY_COMPATIBLE(basetype, transtype))
			elog(ERROR, "AggregateCreate: must not omit initval when transfn is strict and transtype is not compatible with input type");
	}
	ReleaseSysCache(tup);

	/* handle finalfn, if supplied */
	if (aggfinalfnName)
	{
		fnArgs[0] = transtype;
		fnArgs[1] = 0;
		tup = SearchSysCache(PROCNAME,
							 PointerGetDatum(aggfinalfnName),
							 Int32GetDatum(1),
							 PointerGetDatum(fnArgs),
							 0);
		if (!HeapTupleIsValid(tup))
			func_error("AggregateCreate", aggfinalfnName, 1, fnArgs, NULL);
		finalfn = tup->t_data->t_oid;
		Assert(OidIsValid(finalfn));
		proc = (Form_pg_proc) GETSTRUCT(tup);
		finaltype = proc->prorettype;
		ReleaseSysCache(tup);
	}
	else
	{
		/*
		 * If no finalfn, aggregate result type is type of the state value
		 */
		finaltype = transtype;
	}
	Assert(OidIsValid(finaltype));

	/* initialize nulls and values */
	for (i = 0; i < Natts_pg_aggregate; i++)
	{
		nulls[i] = ' ';
		values[i] = (Datum) NULL;
	}
	namestrcpy(&aname, aggName);
	values[Anum_pg_aggregate_aggname - 1] = NameGetDatum(&aname);
	values[Anum_pg_aggregate_aggowner - 1] = Int32GetDatum(GetUserId());
	values[Anum_pg_aggregate_aggtransfn - 1] = ObjectIdGetDatum(transfn);
	values[Anum_pg_aggregate_aggfinalfn - 1] = ObjectIdGetDatum(finalfn);
	values[Anum_pg_aggregate_aggbasetype - 1] = ObjectIdGetDatum(basetype);
	values[Anum_pg_aggregate_aggtranstype - 1] = ObjectIdGetDatum(transtype);
	values[Anum_pg_aggregate_aggfinaltype - 1] = ObjectIdGetDatum(finaltype);

	if (agginitval)
		values[Anum_pg_aggregate_agginitval - 1] =
			DirectFunctionCall1(textin, CStringGetDatum(agginitval));
	else
		nulls[Anum_pg_aggregate_agginitval - 1] = 'n';

	aggdesc = heap_openr(AggregateRelationName, RowExclusiveLock);
	tupDesc = aggdesc->rd_att;
	if (!HeapTupleIsValid(tup = heap_formtuple(tupDesc,
											   values,
											   nulls)))
		elog(ERROR, "AggregateCreate: heap_formtuple failed");
	if (!OidIsValid(heap_insert(aggdesc, tup)))
		elog(ERROR, "AggregateCreate: heap_insert failed");

	if (RelationGetForm(aggdesc)->relhasindex)
	{
		Relation	idescs[Num_pg_aggregate_indices];

		CatalogOpenIndices(Num_pg_aggregate_indices, Name_pg_aggregate_indices, idescs);
		CatalogIndexInsert(idescs, Num_pg_aggregate_indices, aggdesc, tup);
		CatalogCloseIndices(Num_pg_aggregate_indices, idescs);
	}

	heap_close(aggdesc, RowExclusiveLock);
}

Datum
AggNameGetInitVal(char *aggName, Oid basetype, bool *isNull)
{
	HeapTuple	tup;
	Oid			transtype,
				typinput,
				typelem;
	Datum		textInitVal;
	char	   *strInitVal;
	Datum		initVal;

	Assert(PointerIsValid(aggName));
	Assert(PointerIsValid(isNull));

	tup = SearchSysCache(AGGNAME,
						 PointerGetDatum(aggName),
						 ObjectIdGetDatum(basetype),
						 0, 0);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "AggNameGetInitVal: cache lookup failed for aggregate '%s'",
			 aggName);
	transtype = ((Form_pg_aggregate) GETSTRUCT(tup))->aggtranstype;

	/*
	 * initval is potentially null, so don't try to access it as a struct
	 * field. Must do it the hard way with SysCacheGetAttr.
	 */
	textInitVal = SysCacheGetAttr(AGGNAME, tup,
								  Anum_pg_aggregate_agginitval,
								  isNull);
	if (*isNull)
	{
		ReleaseSysCache(tup);
		return (Datum) 0;
	}

	strInitVal = DatumGetCString(DirectFunctionCall1(textout, textInitVal));

	ReleaseSysCache(tup);

	tup = SearchSysCache(TYPEOID,
						 ObjectIdGetDatum(transtype),
						 0, 0, 0);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "AggNameGetInitVal: cache lookup failed on aggregate transition function return type %u", transtype);

	typinput = ((Form_pg_type) GETSTRUCT(tup))->typinput;
	typelem = ((Form_pg_type) GETSTRUCT(tup))->typelem;
	ReleaseSysCache(tup);

	initVal = OidFunctionCall3(typinput,
							   CStringGetDatum(strInitVal),
							   ObjectIdGetDatum(typelem),
							   Int32GetDatum(-1));

	pfree(strInitVal);
	return initVal;
}
