/*-------------------------------------------------------------------------
 *
 * xid.c
 *	  POSTGRES transaction identifier datatype.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	$Id: xid.c,v 1.32 2001/08/23 23:06:37 tgl Exp $
 *
 * OLD COMMENTS
 * XXX WARNING
 *		Much of this file will change when we change our representation
 *		of transaction ids -cim 3/23/90
 *
 * It is time to make the switch from 5 byte to 4 byte transaction ids
 * This file was totally reworked. -mer 5/22/92
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xact.h"

#define PG_GETARG_TRANSACTIONID(n)	DatumGetTransactionId(PG_GETARG_DATUM(n))
#define PG_RETURN_TRANSACTIONID(x)	return TransactionIdGetDatum(x)


Datum
xidin(PG_FUNCTION_ARGS)
{
	char	   *representation = PG_GETARG_CSTRING(0);

	PG_RETURN_TRANSACTIONID((TransactionId) atol(representation));
}

Datum
xidout(PG_FUNCTION_ARGS)
{
	TransactionId transactionId = PG_GETARG_TRANSACTIONID(0);
	/* maximum 32 bit unsigned integer representation takes 10 chars */
	char	   *representation = palloc(11);

	snprintf(representation, 11, "%lu", (unsigned long) transactionId);

	PG_RETURN_CSTRING(representation);
}

/* ----------------------------------------------------------------
 *		xideq
 * ----------------------------------------------------------------
 */

/*
 *		xideq			- returns 1, iff xid1 == xid2
 *								  0  else;
 */
Datum
xideq(PG_FUNCTION_ARGS)
{
	TransactionId xid1 = PG_GETARG_TRANSACTIONID(0);
	TransactionId xid2 = PG_GETARG_TRANSACTIONID(1);

	PG_RETURN_BOOL(TransactionIdEquals(xid1, xid2));
}
