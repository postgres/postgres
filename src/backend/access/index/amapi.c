/*-------------------------------------------------------------------------
 *
 * amapi.c
 *	  Support routines for API for Postgres index access methods.
 *
 * Copyright (c) 2015-2025, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/index/amapi.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/htup_details.h"
#include "catalog/pg_am.h"
#include "catalog/pg_opclass.h"
#include "utils/fmgrprotos.h"
#include "utils/syscache.h"


/*
 * GetIndexAmRoutine - call the specified access method handler routine to get
 * its IndexAmRoutine struct, which will be palloc'd in the caller's context.
 *
 * Note that if the amhandler function is built-in, this will not involve
 * any catalog access.  It's therefore safe to use this while bootstrapping
 * indexes for the system catalogs.  relcache.c relies on that.
 */
IndexAmRoutine *
GetIndexAmRoutine(Oid amhandler)
{
	Datum		datum;
	IndexAmRoutine *routine;

	datum = OidFunctionCall0(amhandler);
	routine = (IndexAmRoutine *) DatumGetPointer(datum);

	if (routine == NULL || !IsA(routine, IndexAmRoutine))
		elog(ERROR, "index access method handler function %u did not return an IndexAmRoutine struct",
			 amhandler);

	return routine;
}

/*
 * GetIndexAmRoutineByAmId - look up the handler of the index access method
 * with the given OID, and get its IndexAmRoutine struct.
 *
 * If the given OID isn't a valid index access method, returns NULL if
 * noerror is true, else throws error.
 */
IndexAmRoutine *
GetIndexAmRoutineByAmId(Oid amoid, bool noerror)
{
	HeapTuple	tuple;
	Form_pg_am	amform;
	regproc		amhandler;

	/* Get handler function OID for the access method */
	tuple = SearchSysCache1(AMOID, ObjectIdGetDatum(amoid));
	if (!HeapTupleIsValid(tuple))
	{
		if (noerror)
			return NULL;
		elog(ERROR, "cache lookup failed for access method %u",
			 amoid);
	}
	amform = (Form_pg_am) GETSTRUCT(tuple);

	/* Check if it's an index access method as opposed to some other AM */
	if (amform->amtype != AMTYPE_INDEX)
	{
		if (noerror)
		{
			ReleaseSysCache(tuple);
			return NULL;
		}
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("access method \"%s\" is not of type %s",
						NameStr(amform->amname), "INDEX")));
	}

	amhandler = amform->amhandler;

	/* Complain if handler OID is invalid */
	if (!RegProcedureIsValid(amhandler))
	{
		if (noerror)
		{
			ReleaseSysCache(tuple);
			return NULL;
		}
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("index access method \"%s\" does not have a handler",
						NameStr(amform->amname))));
	}

	ReleaseSysCache(tuple);

	/* And finally, call the handler function to get the API struct. */
	return GetIndexAmRoutine(amhandler);
}


/*
 * IndexAmTranslateStrategy - given an access method and strategy, get the
 * corresponding compare type.
 *
 * If missing_ok is false, throw an error if no compare type is found.  If
 * true, just return COMPARE_INVALID.
 */
CompareType
IndexAmTranslateStrategy(StrategyNumber strategy, Oid amoid, Oid opfamily, bool missing_ok)
{
	CompareType result;
	IndexAmRoutine *amroutine;

	/* shortcut for common case */
	if (amoid == BTREE_AM_OID &&
		(strategy > InvalidStrategy && strategy <= BTMaxStrategyNumber))
		return (CompareType) strategy;

	amroutine = GetIndexAmRoutineByAmId(amoid, false);
	if (amroutine->amtranslatestrategy)
		result = amroutine->amtranslatestrategy(strategy, opfamily);
	else
		result = COMPARE_INVALID;

	if (!missing_ok && result == COMPARE_INVALID)
		elog(ERROR, "could not translate strategy number %d for index AM %u", strategy, amoid);

	return result;
}

/*
 * IndexAmTranslateCompareType - given an access method and compare type, get
 * the corresponding strategy number.
 *
 * If missing_ok is false, throw an error if no strategy is found correlating
 * to the given cmptype.  If true, just return InvalidStrategy.
 */
StrategyNumber
IndexAmTranslateCompareType(CompareType cmptype, Oid amoid, Oid opfamily, bool missing_ok)
{
	StrategyNumber result;
	IndexAmRoutine *amroutine;

	/* shortcut for common case */
	if (amoid == BTREE_AM_OID &&
		(cmptype > COMPARE_INVALID && cmptype <= COMPARE_GT))
		return (StrategyNumber) cmptype;

	amroutine = GetIndexAmRoutineByAmId(amoid, false);
	if (amroutine->amtranslatecmptype)
		result = amroutine->amtranslatecmptype(cmptype, opfamily);
	else
		result = InvalidStrategy;

	if (!missing_ok && result == InvalidStrategy)
		elog(ERROR, "could not translate compare type %u for index AM %u", cmptype, amoid);

	return result;
}

/*
 * Ask appropriate access method to validate the specified opclass.
 */
Datum
amvalidate(PG_FUNCTION_ARGS)
{
	Oid			opclassoid = PG_GETARG_OID(0);
	bool		result;
	HeapTuple	classtup;
	Form_pg_opclass classform;
	Oid			amoid;
	IndexAmRoutine *amroutine;

	classtup = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclassoid));
	if (!HeapTupleIsValid(classtup))
		elog(ERROR, "cache lookup failed for operator class %u", opclassoid);
	classform = (Form_pg_opclass) GETSTRUCT(classtup);

	amoid = classform->opcmethod;

	ReleaseSysCache(classtup);

	amroutine = GetIndexAmRoutineByAmId(amoid, false);

	if (amroutine->amvalidate == NULL)
		elog(ERROR, "function amvalidate is not defined for index access method %u",
			 amoid);

	result = amroutine->amvalidate(opclassoid);

	pfree(amroutine);

	PG_RETURN_BOOL(result);
}
