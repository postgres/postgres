/*-------------------------------------------------------------------------
 *
 * pg_aggregate.c
 *	  routines to support manipulation of the pg_aggregate relation
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/pg_aggregate.c,v 1.32 2000/04/16 04:16:09 tgl Exp $
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
 *		is created and inserted in the aggregate relation.	The fields
 *		of this tuple are aggregate name, owner id, 2 transition functions
 *		(called aggtransfn1 and aggtransfn2), final function (aggfinalfn),
 *		type of data on which aggtransfn1 operates (aggbasetype), return
 *		types of the two transition functions (aggtranstype1 and
 *		aggtranstype2), final return type (aggfinaltype), and initial values
 *		for the two state transition functions (agginitval1 and agginitval2).
 *		All types and functions must have been defined
 *		prior to defining the aggregate.
 *
 * ---------------
 */
void
AggregateCreate(char *aggName,
				char *aggtransfn1Name,
				char *aggtransfn2Name,
				char *aggfinalfnName,
				char *aggbasetypeName,
				char *aggtransfn1typeName,
				char *aggtransfn2typeName,
				char *agginitval1,
				char *agginitval2)
{
	int			i;
	Relation	aggdesc;
	HeapTuple	tup;
	char		nulls[Natts_pg_aggregate];
	Datum		values[Natts_pg_aggregate];
	Form_pg_proc proc;
	Oid			xfn1 = InvalidOid;
	Oid			xfn2 = InvalidOid;
	Oid			ffn = InvalidOid;
	Oid			xbase = InvalidOid;
	Oid			xret1 = InvalidOid;
	Oid			xret2 = InvalidOid;
	Oid			fret = InvalidOid;
	Oid			fnArgs[FUNC_MAX_ARGS];
	NameData	aname;
	TupleDesc	tupDesc;

	MemSet(fnArgs, 0, FUNC_MAX_ARGS * sizeof(Oid));

	/* sanity checks */
	if (!aggName)
		elog(ERROR, "AggregateCreate: no aggregate name supplied");

	if (!aggtransfn1Name && !aggtransfn2Name)
		elog(ERROR, "AggregateCreate: aggregate must have at least one transition function");

	if (aggtransfn1Name && aggtransfn2Name && !aggfinalfnName)
		elog(ERROR, "AggregateCreate: Aggregate must have final function with both transition functions");

	/* handle the aggregate's base type (input data type) */
	tup = SearchSysCacheTuple(TYPENAME,
							  PointerGetDatum(aggbasetypeName),
							  0, 0, 0);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "AggregateCreate: Type '%s' undefined", aggbasetypeName);
	xbase = tup->t_data->t_oid;

	/* make sure there is no existing agg of same name and base type */
	tup = SearchSysCacheTuple(AGGNAME,
							  PointerGetDatum(aggName),
							  ObjectIdGetDatum(xbase),
							  0, 0);
	if (HeapTupleIsValid(tup))
		elog(ERROR,
			 "AggregateCreate: aggregate '%s' with base type '%s' already exists",
			 aggName, aggbasetypeName);

	/* handle transfn1 and transtype1 */
	if (aggtransfn1Name)
	{
		tup = SearchSysCacheTuple(TYPENAME,
								  PointerGetDatum(aggtransfn1typeName),
								  0, 0, 0);
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "AggregateCreate: Type '%s' undefined",
				 aggtransfn1typeName);
		xret1 = tup->t_data->t_oid;

		fnArgs[0] = xret1;
		fnArgs[1] = xbase;
		tup = SearchSysCacheTuple(PROCNAME,
								  PointerGetDatum(aggtransfn1Name),
								  Int32GetDatum(2),
								  PointerGetDatum(fnArgs),
								  0);
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "AggregateCreate: '%s('%s', '%s') does not exist",
				 aggtransfn1Name, aggtransfn1typeName, aggbasetypeName);
		if (((Form_pg_proc) GETSTRUCT(tup))->prorettype != xret1)
			elog(ERROR, "AggregateCreate: return type of '%s' is not '%s'",
				 aggtransfn1Name, aggtransfn1typeName);
		xfn1 = tup->t_data->t_oid;
		if (!OidIsValid(xfn1) || !OidIsValid(xret1) ||
			!OidIsValid(xbase))
			elog(ERROR, "AggregateCreate: bogus function '%s'", aggtransfn1Name);
	}

	/* handle transfn2 and transtype2 */
	if (aggtransfn2Name)
	{
		tup = SearchSysCacheTuple(TYPENAME,
								  PointerGetDatum(aggtransfn2typeName),
								  0, 0, 0);
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "AggregateCreate: Type '%s' undefined",
				 aggtransfn2typeName);
		xret2 = tup->t_data->t_oid;

		fnArgs[0] = xret2;
		fnArgs[1] = 0;
		tup = SearchSysCacheTuple(PROCNAME,
								  PointerGetDatum(aggtransfn2Name),
								  Int32GetDatum(1),
								  PointerGetDatum(fnArgs),
								  0);
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "AggregateCreate: '%s'('%s') does not exist",
				 aggtransfn2Name, aggtransfn2typeName);
		if (((Form_pg_proc) GETSTRUCT(tup))->prorettype != xret2)
			elog(ERROR, "AggregateCreate: return type of '%s' is not '%s'",
				 aggtransfn2Name, aggtransfn2typeName);
		xfn2 = tup->t_data->t_oid;
		if (!OidIsValid(xfn2) || !OidIsValid(xret2))
			elog(ERROR, "AggregateCreate: bogus function '%s'", aggtransfn2Name);
	}

	/* handle finalfn */
	if (aggfinalfnName)
	{
		int			nargs = 0;

		if (OidIsValid(xret1))
			fnArgs[nargs++] = xret1;
		if (OidIsValid(xret2))
			fnArgs[nargs++] = xret2;
		fnArgs[nargs] = 0;		/* make sure slot 2 is empty if just 1 arg */
		tup = SearchSysCacheTuple(PROCNAME,
								  PointerGetDatum(aggfinalfnName),
								  Int32GetDatum(nargs),
								  PointerGetDatum(fnArgs),
								  0);
		if (!HeapTupleIsValid(tup))
		{
			if (nargs == 2)
				elog(ERROR, "AggregateCreate: '%s'('%s','%s') does not exist",
				aggfinalfnName, aggtransfn1typeName, aggtransfn2typeName);
			else if (OidIsValid(xret1))
				elog(ERROR, "AggregateCreate: '%s'('%s') does not exist",
					 aggfinalfnName, aggtransfn1typeName);
			else
				elog(ERROR, "AggregateCreate: '%s'('%s') does not exist",
					 aggfinalfnName, aggtransfn2typeName);
		}
		ffn = tup->t_data->t_oid;
		proc = (Form_pg_proc) GETSTRUCT(tup);
		fret = proc->prorettype;
		if (!OidIsValid(ffn) || !OidIsValid(fret))
			elog(ERROR, "AggregateCreate: bogus function '%s'", aggfinalfnName);
	}
	else
	{

		/*
		 * If no finalfn, aggregate result type is type of the sole state
		 * value (we already checked there is only one)
		 */
		if (OidIsValid(xret1))
			fret = xret1;
		else
			fret = xret2;
	}
	Assert(OidIsValid(fret));

	/*
	 * If transition function 2 is defined, it must have an initial value,
	 * whereas transition function 1 need not, which allows max and min
	 * aggregates to return NULL if they are evaluated on empty sets.
	 */
	if (OidIsValid(xfn2) && !agginitval2)
		elog(ERROR, "AggregateCreate: transition function 2 MUST have an initial value");

	/* initialize nulls and values */
	for (i = 0; i < Natts_pg_aggregate; i++)
	{
		nulls[i] = ' ';
		values[i] = (Datum) NULL;
	}
	namestrcpy(&aname, aggName);
	values[Anum_pg_aggregate_aggname - 1] = NameGetDatum(&aname);
	values[Anum_pg_aggregate_aggowner - 1] = Int32GetDatum(GetUserId());
	values[Anum_pg_aggregate_aggtransfn1 - 1] = ObjectIdGetDatum(xfn1);
	values[Anum_pg_aggregate_aggtransfn2 - 1] = ObjectIdGetDatum(xfn2);
	values[Anum_pg_aggregate_aggfinalfn - 1] = ObjectIdGetDatum(ffn);
	values[Anum_pg_aggregate_aggbasetype - 1] = ObjectIdGetDatum(xbase);
	values[Anum_pg_aggregate_aggtranstype1 - 1] = ObjectIdGetDatum(xret1);
	values[Anum_pg_aggregate_aggtranstype2 - 1] = ObjectIdGetDatum(xret2);
	values[Anum_pg_aggregate_aggfinaltype - 1] = ObjectIdGetDatum(fret);

	if (agginitval1)
		values[Anum_pg_aggregate_agginitval1 - 1] = PointerGetDatum(textin(agginitval1));
	else
		nulls[Anum_pg_aggregate_agginitval1 - 1] = 'n';

	if (agginitval2)
		values[Anum_pg_aggregate_agginitval2 - 1] = PointerGetDatum(textin(agginitval2));
	else
		nulls[Anum_pg_aggregate_agginitval2 - 1] = 'n';

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

char *
AggNameGetInitVal(char *aggName, Oid basetype, int xfuncno, bool *isNull)
{
	HeapTuple	tup;
	Relation	aggRel;
	int			initValAttno;
	Oid			transtype,
				typinput,
				typelem;
	text	   *textInitVal;
	char	   *strInitVal,
			   *initVal;

	Assert(PointerIsValid(aggName));
	Assert(PointerIsValid(isNull));
	Assert(xfuncno == 1 || xfuncno == 2);

	/*
	 * since we will have to use fastgetattr (in case one or both init
	 * vals are NULL), we will need to open the relation.  Do that first
	 * to ensure we don't get a stale tuple from the cache.
	 */

	aggRel = heap_openr(AggregateRelationName, AccessShareLock);

	tup = SearchSysCacheTuple(AGGNAME,
							  PointerGetDatum(aggName),
							  ObjectIdGetDatum(basetype),
							  0, 0);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "AggNameGetInitVal: cache lookup failed for aggregate '%s'",
			 aggName);
	if (xfuncno == 1)
	{
		transtype = ((Form_pg_aggregate) GETSTRUCT(tup))->aggtranstype1;
		initValAttno = Anum_pg_aggregate_agginitval1;
	}
	else
	{
		/* can only be 1 or 2 */
		transtype = ((Form_pg_aggregate) GETSTRUCT(tup))->aggtranstype2;
		initValAttno = Anum_pg_aggregate_agginitval2;
	}

	textInitVal = (text *) fastgetattr(tup, initValAttno,
									   RelationGetDescr(aggRel),
									   isNull);
	if (!PointerIsValid(textInitVal))
		*isNull = true;
	if (*isNull)
	{
		heap_close(aggRel, AccessShareLock);
		return (char *) NULL;
	}
	strInitVal = textout(textInitVal);

	heap_close(aggRel, AccessShareLock);

	tup = SearchSysCacheTuple(TYPEOID,
							  ObjectIdGetDatum(transtype),
							  0, 0, 0);
	if (!HeapTupleIsValid(tup))
	{
		pfree(strInitVal);
		elog(ERROR, "AggNameGetInitVal: cache lookup failed on aggregate transition function return type %u", transtype);
	}
	typinput = ((Form_pg_type) GETSTRUCT(tup))->typinput;
	typelem = ((Form_pg_type) GETSTRUCT(tup))->typelem;

	initVal = fmgr(typinput, strInitVal, typelem, -1);

	pfree(strInitVal);
	return initVal;
}
