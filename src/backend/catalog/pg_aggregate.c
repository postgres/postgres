/*-------------------------------------------------------------------------
 *
 * pg_aggregate.c--
 *	  routines to support manipulation of the pg_aggregate relation
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/pg_aggregate.c,v 1.6 1997/09/07 04:40:24 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <postgres.h>

#include <access/heapam.h>
#include <utils/builtins.h>
#include <fmgr.h>
#include <catalog/catname.h>
#include <utils/syscache.h>
#include <catalog/pg_operator.h>
#include <catalog/pg_proc.h>
#include <catalog/pg_type.h>
#include <catalog/pg_aggregate.h>
#include <miscadmin.h>
#ifndef HAVE_MEMMOVE
#include <regex/utils.h>
#else
#include <string.h>
#endif

/* ----------------
 * AggregateCreate
 *
 * aggregates overloading has been added.  Instead of the full
 * overload support we have for functions, aggregate overloading only
 * applies to exact basetype matches.  That is, we don't check the
 * the inheritance hierarchy
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
	register		i;
	Relation		aggdesc;
	HeapTuple		tup;
	char			nulls[Natts_pg_aggregate];
	Datum			values[Natts_pg_aggregate];
	Form_pg_proc	proc;
	Oid				xfn1 = InvalidOid;
	Oid				xfn2 = InvalidOid;
	Oid				ffn = InvalidOid;
	Oid				xbase = InvalidOid;
	Oid				xret1 = InvalidOid;
	Oid				xret2 = InvalidOid;
	Oid				fret = InvalidOid;
	Oid				fnArgs[8];
	TupleDesc		tupDesc;

	memset(fnArgs, 0, 8 * sizeof(Oid));

	/* sanity checks */
	if (!aggName)
		elog(WARN, "AggregateCreate: no aggregate name supplied");

	if (!aggtransfn1Name && !aggtransfn2Name)
		elog(WARN, "AggregateCreate: aggregate must have at least one transition function");

	tup = SearchSysCacheTuple(TYPNAME,
							  PointerGetDatum(aggbasetypeName),
							  0, 0, 0);
	if (!HeapTupleIsValid(tup))
		elog(WARN, "AggregateCreate: Type '%s' undefined", aggbasetypeName);
	xbase = tup->t_oid;

	if (aggtransfn1Name)
	{
		tup = SearchSysCacheTuple(TYPNAME,
								  PointerGetDatum(aggtransfn1typeName),
								  0, 0, 0);
		if (!HeapTupleIsValid(tup))
			elog(WARN, "AggregateCreate: Type '%s' undefined",
				 aggtransfn1typeName);
		xret1 = tup->t_oid;

		fnArgs[0] = xret1;
		fnArgs[1] = xbase;
		tup = SearchSysCacheTuple(PRONAME,
								  PointerGetDatum(aggtransfn1Name),
								  Int32GetDatum(2),
								  PointerGetDatum(fnArgs),
								  0);
		if (!HeapTupleIsValid(tup))
			elog(WARN, "AggregateCreate: '%s('%s', '%s') does not exist",
				 aggtransfn1Name, aggtransfn1typeName, aggbasetypeName);
		if (((Form_pg_proc) GETSTRUCT(tup))->prorettype != xret1)
			elog(WARN, "AggregateCreate: return type of '%s' is not '%s'",
				 aggtransfn1Name,
				 aggtransfn1typeName);
		xfn1 = tup->t_oid;
		if (!OidIsValid(xfn1) || !OidIsValid(xret1) ||
			!OidIsValid(xbase))
			elog(WARN, "AggregateCreate: bogus function '%s'", aggfinalfnName);
	}

	if (aggtransfn2Name)
	{
		tup = SearchSysCacheTuple(TYPNAME,
								  PointerGetDatum(aggtransfn2typeName),
								  0, 0, 0);
		if (!HeapTupleIsValid(tup))
			elog(WARN, "AggregateCreate: Type '%s' undefined",
				 aggtransfn2typeName);
		xret2 = tup->t_oid;

		fnArgs[0] = xret2;
		fnArgs[1] = 0;
		tup = SearchSysCacheTuple(PRONAME,
								  PointerGetDatum(aggtransfn2Name),
								  Int32GetDatum(1),
								  PointerGetDatum(fnArgs),
								  0);
		if (!HeapTupleIsValid(tup))
			elog(WARN, "AggregateCreate: '%s'('%s') does not exist",
				 aggtransfn2Name, aggtransfn2typeName);
		if (((Form_pg_proc) GETSTRUCT(tup))->prorettype != xret2)
			elog(WARN, "AggregateCreate: return type of '%s' is not '%s'",
				 aggtransfn2Name, aggtransfn2typeName);
		xfn2 = tup->t_oid;
		if (!OidIsValid(xfn2) || !OidIsValid(xret2))
			elog(WARN, "AggregateCreate: bogus function '%s'", aggfinalfnName);
	}

	tup = SearchSysCacheTuple(AGGNAME, PointerGetDatum(aggName),
							  ObjectIdGetDatum(xbase),
							  0, 0);
	if (HeapTupleIsValid(tup))
		elog(WARN,
			 "AggregateCreate: aggregate '%s' with base type '%s' already exists",
			 aggName, aggbasetypeName);

	/* more sanity checks */
	if (aggtransfn1Name && aggtransfn2Name && !aggfinalfnName)
		elog(WARN, "AggregateCreate: Aggregate must have final function with both transition functions");

	if ((!aggtransfn1Name || !aggtransfn2Name) && aggfinalfnName)
		elog(WARN, "AggregateCreate: Aggregate cannot have final function without both transition functions");

	if (aggfinalfnName)
	{
		fnArgs[0] = xret1;
		fnArgs[1] = xret2;
		tup = SearchSysCacheTuple(PRONAME,
								  PointerGetDatum(aggfinalfnName),
								  Int32GetDatum(2),
								  PointerGetDatum(fnArgs),
								  0);
		if (!HeapTupleIsValid(tup))
			elog(WARN, "AggregateCreate: '%s'('%s','%s') does not exist",
			   aggfinalfnName, aggtransfn1typeName, aggtransfn2typeName);
		ffn = tup->t_oid;
		proc = (Form_pg_proc) GETSTRUCT(tup);
		fret = proc->prorettype;
		if (!OidIsValid(ffn) || !OidIsValid(fret))
			elog(WARN, "AggregateCreate: bogus function '%s'", aggfinalfnName);
	}

	/*
	 * If transition function 2 is defined, it must have an initial value,
	 * whereas transition function 1 does not, which allows man and min
	 * aggregates to return NULL if they are evaluated on empty sets.
	 */
	if (OidIsValid(xfn2) && !agginitval2)
		elog(WARN, "AggregateCreate: transition function 2 MUST have an initial value");

	/* initialize nulls and values */
	for (i = 0; i < Natts_pg_aggregate; i++)
	{
		nulls[i] = ' ';
		values[i] = (Datum) NULL;
	}
	values[Anum_pg_aggregate_aggname - 1] = PointerGetDatum(aggName);
	values[Anum_pg_aggregate_aggowner - 1] =
		Int32GetDatum(GetUserId());
	values[Anum_pg_aggregate_aggtransfn1 - 1] =
		ObjectIdGetDatum(xfn1);
	values[Anum_pg_aggregate_aggtransfn2 - 1] =
		ObjectIdGetDatum(xfn2);
	values[Anum_pg_aggregate_aggfinalfn - 1] =
		ObjectIdGetDatum(ffn);

	values[Anum_pg_aggregate_aggbasetype - 1] =
		ObjectIdGetDatum(xbase);
	if (!OidIsValid(xfn1))
	{
		values[Anum_pg_aggregate_aggtranstype1 - 1] =
			ObjectIdGetDatum(InvalidOid);
		values[Anum_pg_aggregate_aggtranstype2 - 1] =
			ObjectIdGetDatum(xret2);
		values[Anum_pg_aggregate_aggfinaltype - 1] =
			ObjectIdGetDatum(xret2);
	}
	else if (!OidIsValid(xfn2))
	{
		values[Anum_pg_aggregate_aggtranstype1 - 1] =
			ObjectIdGetDatum(xret1);
		values[Anum_pg_aggregate_aggtranstype2 - 1] =
			ObjectIdGetDatum(InvalidOid);
		values[Anum_pg_aggregate_aggfinaltype - 1] =
			ObjectIdGetDatum(xret1);
	}
	else
	{
		values[Anum_pg_aggregate_aggtranstype1 - 1] =
			ObjectIdGetDatum(xret1);
		values[Anum_pg_aggregate_aggtranstype2 - 1] =
			ObjectIdGetDatum(xret2);
		values[Anum_pg_aggregate_aggfinaltype - 1] =
			ObjectIdGetDatum(fret);
	}

	if (agginitval1)
		values[Anum_pg_aggregate_agginitval1 - 1] = PointerGetDatum(textin(agginitval1));
	else
		nulls[Anum_pg_aggregate_agginitval1 - 1] = 'n';

	if (agginitval2)
		values[Anum_pg_aggregate_agginitval2 - 1] = PointerGetDatum(textin(agginitval2));
	else
		nulls[Anum_pg_aggregate_agginitval2 - 1] = 'n';

	if (!RelationIsValid(aggdesc = heap_openr(AggregateRelationName)))
		elog(WARN, "AggregateCreate: could not open '%s'",
			 AggregateRelationName);

	tupDesc = aggdesc->rd_att;
	if (!HeapTupleIsValid(tup = heap_formtuple(tupDesc,
											   values,
											   nulls)))
		elog(WARN, "AggregateCreate: heap_formtuple failed");
	if (!OidIsValid(heap_insert(aggdesc, tup)))
		elog(WARN, "AggregateCreate: heap_insert failed");
	heap_close(aggdesc);

}

char		   *
AggNameGetInitVal(char *aggName, Oid basetype, int xfuncno, bool * isNull)
{
	HeapTuple		tup;
	Relation		aggRel;
	int				initValAttno;
	Oid				transtype;
	text		   *textInitVal;
	char		   *strInitVal,
				   *initVal;

	Assert(PointerIsValid(aggName));
	Assert(PointerIsValid(isNull));
	Assert(xfuncno == 1 || xfuncno == 2);

	tup = SearchSysCacheTuple(AGGNAME,
							  PointerGetDatum(aggName),
							  PointerGetDatum(basetype),
							  0, 0);
	if (!HeapTupleIsValid(tup))
		elog(WARN, "AggNameGetInitVal: cache lookup failed for aggregate '%s'",
			 aggName);
	if (xfuncno == 1)
	{
		transtype = ((Form_pg_aggregate) GETSTRUCT(tup))->aggtranstype1;
		initValAttno = Anum_pg_aggregate_agginitval1;
	}
	else
		 /* can only be 1 or 2 */
	{
		transtype = ((Form_pg_aggregate) GETSTRUCT(tup))->aggtranstype2;
		initValAttno = Anum_pg_aggregate_agginitval2;
	}

	aggRel = heap_openr(AggregateRelationName);
	if (!RelationIsValid(aggRel))
		elog(WARN, "AggNameGetInitVal: could not open \"%-.*s\"",
			 AggregateRelationName);

	/*
	 * must use fastgetattr in case one or other of the init values is
	 * NULL
	 */
	textInitVal = (text *) fastgetattr(tup, initValAttno,
									   RelationGetTupleDescriptor(aggRel),
									   isNull);
	if (!PointerIsValid(textInitVal))
		*isNull = true;
	if (*isNull)
	{
		heap_close(aggRel);
		return ((char *) NULL);
	}
	strInitVal = textout(textInitVal);
	heap_close(aggRel);

	tup = SearchSysCacheTuple(TYPOID, ObjectIdGetDatum(transtype),
							  0, 0, 0);
	if (!HeapTupleIsValid(tup))
	{
		pfree(strInitVal);
		elog(WARN, "AggNameGetInitVal: cache lookup failed on aggregate transition function return type");
	}
	initVal = fmgr(((TypeTupleForm) GETSTRUCT(tup))->typinput, strInitVal, -1);
	pfree(strInitVal);
	return (initVal);
}
