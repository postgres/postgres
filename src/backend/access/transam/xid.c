/*-------------------------------------------------------------------------
 *
 * xid.c
 *	  POSTGRES transaction identifier datatype.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	$Id: xid.c,v 1.34 2001/10/25 05:49:22 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "access/xact.h"


#define PG_GETARG_TRANSACTIONID(n)	DatumGetTransactionId(PG_GETARG_DATUM(n))
#define PG_RETURN_TRANSACTIONID(x)	return TransactionIdGetDatum(x)


Datum
xidin(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);

	PG_RETURN_TRANSACTIONID((TransactionId) strtoul(str, NULL, 0));
}

Datum
xidout(PG_FUNCTION_ARGS)
{
	TransactionId transactionId = PG_GETARG_TRANSACTIONID(0);

	/* maximum 32 bit unsigned integer representation takes 10 chars */
	char	   *str = palloc(11);

	snprintf(str, 11, "%lu", (unsigned long) transactionId);

	PG_RETURN_CSTRING(str);
}

/*
 *		xideq			- are two xids equal?
 */
Datum
xideq(PG_FUNCTION_ARGS)
{
	TransactionId xid1 = PG_GETARG_TRANSACTIONID(0);
	TransactionId xid2 = PG_GETARG_TRANSACTIONID(1);

	PG_RETURN_BOOL(TransactionIdEquals(xid1, xid2));
}

/*
 *		xid_age			- compute age of an XID (relative to current xact)
 */
Datum
xid_age(PG_FUNCTION_ARGS)
{
	TransactionId xid = PG_GETARG_TRANSACTIONID(0);
	TransactionId now = GetCurrentTransactionId();

	/* Permanent XIDs are always infinitely old */
	if (!TransactionIdIsNormal(xid))
		PG_RETURN_INT32(INT_MAX);

	PG_RETURN_INT32((int32) (now - xid));
}
