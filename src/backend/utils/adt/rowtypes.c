/*-------------------------------------------------------------------------
 *
 * rowtypes.c
 *	  I/O functions for generic composite types.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/rowtypes.c,v 1.1 2004/04/01 21:28:45 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "libpq/pqformat.h"
#include "utils/builtins.h"


/*
 * record_in		- input routine for any composite type.
 */
Datum
record_in(PG_FUNCTION_ARGS)
{
	/* Need to decide on external format before we can write this */
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("input of composite types not implemented yet")));

	PG_RETURN_VOID();			/* keep compiler quiet */
}

/*
 * record_out		- output routine for any composite type.
 */
Datum
record_out(PG_FUNCTION_ARGS)
{
	/* Need to decide on external format before we can write this */
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("output of composite types not implemented yet")));

	PG_RETURN_VOID();			/* keep compiler quiet */
}

/*
 * record_recv		- binary input routine for any composite type.
 */
Datum
record_recv(PG_FUNCTION_ARGS)
{
	/* Need to decide on external format before we can write this */
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("input of composite types not implemented yet")));

	PG_RETURN_VOID();			/* keep compiler quiet */
}

/*
 * record_send		- binary output routine for any composite type.
 */
Datum
record_send(PG_FUNCTION_ARGS)
{
	/* Need to decide on external format before we can write this */
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("output of composite types not implemented yet")));

	PG_RETURN_VOID();			/* keep compiler quiet */
}
