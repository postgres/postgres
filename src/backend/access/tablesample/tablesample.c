/*-------------------------------------------------------------------------
 *
 * tablesample.c
 *		  Support functions for TABLESAMPLE feature
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *		  src/backend/access/tablesample/tablesample.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/tsmapi.h"


/*
 * GetTsmRoutine --- get a TsmRoutine struct by invoking the handler.
 *
 * This is a convenience routine that's just meant to check for errors.
 */
TsmRoutine *
GetTsmRoutine(Oid tsmhandler)
{
	Datum		datum;
	TsmRoutine *routine;

	datum = OidFunctionCall1(tsmhandler, PointerGetDatum(NULL));
	routine = (TsmRoutine *) DatumGetPointer(datum);

	if (routine == NULL || !IsA(routine, TsmRoutine))
		elog(ERROR, "tablesample handler function %u did not return a TsmRoutine struct",
			 tsmhandler);

	return routine;
}
