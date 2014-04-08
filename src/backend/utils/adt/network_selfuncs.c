/*-------------------------------------------------------------------------
 *
 * network_selfuncs.c
 *	  Functions for selectivity estimation of inet/cidr operators
 *
 * Currently these are just stubs, but we hope to do better soon.
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/network_selfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/inet.h"


Datum
networksel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(0.001);
}

Datum
networkjoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(0.001);
}
