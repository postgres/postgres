/*-------------------------------------------------------------------------
 *
 * xid.c
 *	  POSTGRES transaction identifier type.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	$Id: xid.c,v 1.30 2001/03/22 03:59:18 momjian Exp $
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

/*
 * TransactionId is typedef'd as uint32, so...
 */
#define PG_GETARG_TRANSACTIONID(n)	PG_GETARG_UINT32(n)
#define PG_RETURN_TRANSACTIONID(x)	PG_RETURN_UINT32(x)


extern TransactionId NullTransactionId;
extern TransactionId DisabledTransactionId;
extern TransactionId AmiTransactionId;
extern TransactionId FirstTransactionId;

/* XXX name for catalogs */
Datum
xidin(PG_FUNCTION_ARGS)
{
	char	   *representation = PG_GETARG_CSTRING(0);

	PG_RETURN_TRANSACTIONID((TransactionId) atol(representation));
}

/* XXX name for catalogs */
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

	PG_RETURN_BOOL(xid1 == xid2);
}

/* ----------------------------------------------------------------
 *		TransactionIdAdd
 * ----------------------------------------------------------------
 */
void
TransactionIdAdd(TransactionId *xid, int value)
{
	*xid += value;
}
