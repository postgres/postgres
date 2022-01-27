/*-------------------------------------------------------------------------
 *
 * xid.c
 *	  POSTGRES transaction identifier and command identifier datatypes.
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/xid.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/multixact.h"
#include "access/transam.h"
#include "access/xact.h"
#include "libpq/pqformat.h"
#include "utils/builtins.h"
#include "utils/xid8.h"

#define PG_GETARG_TRANSACTIONID(n)	DatumGetTransactionId(PG_GETARG_DATUM(n))
#define PG_RETURN_TRANSACTIONID(x)	return TransactionIdGetDatum(x)

#define PG_GETARG_COMMANDID(n)		DatumGetCommandId(PG_GETARG_DATUM(n))
#define PG_RETURN_COMMANDID(x)		return CommandIdGetDatum(x)


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
	char	   *result = (char *) palloc(16);

	snprintf(result, 16, "%lu", (unsigned long) transactionId);
	PG_RETURN_CSTRING(result);
}

/*
 *		xidrecv			- converts external binary format to xid
 */
Datum
xidrecv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

	PG_RETURN_TRANSACTIONID((TransactionId) pq_getmsgint(buf, sizeof(TransactionId)));
}

/*
 *		xidsend			- converts xid to binary format
 */
Datum
xidsend(PG_FUNCTION_ARGS)
{
	TransactionId arg1 = PG_GETARG_TRANSACTIONID(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint32(&buf, arg1);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
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
 *		xidneq			- are two xids different?
 */
Datum
xidneq(PG_FUNCTION_ARGS)
{
	TransactionId xid1 = PG_GETARG_TRANSACTIONID(0);
	TransactionId xid2 = PG_GETARG_TRANSACTIONID(1);

	PG_RETURN_BOOL(!TransactionIdEquals(xid1, xid2));
}

/*
 *		xid_age			- compute age of an XID (relative to latest stable xid)
 */
Datum
xid_age(PG_FUNCTION_ARGS)
{
	TransactionId xid = PG_GETARG_TRANSACTIONID(0);
	TransactionId now = GetStableLatestTransactionId();

	/* Permanent XIDs are always infinitely old */
	if (!TransactionIdIsNormal(xid))
		PG_RETURN_INT32(INT_MAX);

	PG_RETURN_INT32((int32) (now - xid));
}

/*
 *		mxid_age			- compute age of a multi XID (relative to latest stable mxid)
 */
Datum
mxid_age(PG_FUNCTION_ARGS)
{
	TransactionId xid = PG_GETARG_TRANSACTIONID(0);
	MultiXactId now = ReadNextMultiXactId();

	if (!MultiXactIdIsValid(xid))
		PG_RETURN_INT32(INT_MAX);

	PG_RETURN_INT32((int32) (now - xid));
}

/*
 * xidComparator
 *		qsort comparison function for XIDs
 *
 * We can't use wraparound comparison for XIDs because that does not respect
 * the triangle inequality!  Any old sort order will do.
 */
int
xidComparator(const void *arg1, const void *arg2)
{
	TransactionId xid1 = *(const TransactionId *) arg1;
	TransactionId xid2 = *(const TransactionId *) arg2;

	if (xid1 > xid2)
		return 1;
	if (xid1 < xid2)
		return -1;
	return 0;
}

/*
 * xidLogicalComparator
 *		qsort comparison function for XIDs
 *
 * This is used to compare only XIDs from the same epoch (e.g. for backends
 * running at the same time). So there must be only normal XIDs, so there's
 * no issue with triangle inequality.
 */
int
xidLogicalComparator(const void *arg1, const void *arg2)
{
	TransactionId xid1 = *(const TransactionId *) arg1;
	TransactionId xid2 = *(const TransactionId *) arg2;

	Assert(TransactionIdIsNormal(xid1));
	Assert(TransactionIdIsNormal(xid2));

	if (TransactionIdPrecedes(xid1, xid2))
		return -1;

	if (TransactionIdPrecedes(xid2, xid1))
		return 1;

	return 0;
}

Datum
xid8toxid(PG_FUNCTION_ARGS)
{
	FullTransactionId fxid = PG_GETARG_FULLTRANSACTIONID(0);

	PG_RETURN_TRANSACTIONID(XidFromFullTransactionId(fxid));
}

Datum
xid8in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);

	PG_RETURN_FULLTRANSACTIONID(FullTransactionIdFromU64(pg_strtouint64(str, NULL, 0)));
}

Datum
xid8out(PG_FUNCTION_ARGS)
{
	FullTransactionId fxid = PG_GETARG_FULLTRANSACTIONID(0);
	char	   *result = (char *) palloc(21);

	snprintf(result, 21, UINT64_FORMAT, U64FromFullTransactionId(fxid));
	PG_RETURN_CSTRING(result);
}

Datum
xid8recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	uint64		value;

	value = (uint64) pq_getmsgint64(buf);
	PG_RETURN_FULLTRANSACTIONID(FullTransactionIdFromU64(value));
}

Datum
xid8send(PG_FUNCTION_ARGS)
{
	FullTransactionId arg1 = PG_GETARG_FULLTRANSACTIONID(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint64(&buf, (uint64) U64FromFullTransactionId(arg1));
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

Datum
xid8eq(PG_FUNCTION_ARGS)
{
	FullTransactionId fxid1 = PG_GETARG_FULLTRANSACTIONID(0);
	FullTransactionId fxid2 = PG_GETARG_FULLTRANSACTIONID(1);

	PG_RETURN_BOOL(FullTransactionIdEquals(fxid1, fxid2));
}

Datum
xid8ne(PG_FUNCTION_ARGS)
{
	FullTransactionId fxid1 = PG_GETARG_FULLTRANSACTIONID(0);
	FullTransactionId fxid2 = PG_GETARG_FULLTRANSACTIONID(1);

	PG_RETURN_BOOL(!FullTransactionIdEquals(fxid1, fxid2));
}

Datum
xid8lt(PG_FUNCTION_ARGS)
{
	FullTransactionId fxid1 = PG_GETARG_FULLTRANSACTIONID(0);
	FullTransactionId fxid2 = PG_GETARG_FULLTRANSACTIONID(1);

	PG_RETURN_BOOL(FullTransactionIdPrecedes(fxid1, fxid2));
}

Datum
xid8gt(PG_FUNCTION_ARGS)
{
	FullTransactionId fxid1 = PG_GETARG_FULLTRANSACTIONID(0);
	FullTransactionId fxid2 = PG_GETARG_FULLTRANSACTIONID(1);

	PG_RETURN_BOOL(FullTransactionIdFollows(fxid1, fxid2));
}

Datum
xid8le(PG_FUNCTION_ARGS)
{
	FullTransactionId fxid1 = PG_GETARG_FULLTRANSACTIONID(0);
	FullTransactionId fxid2 = PG_GETARG_FULLTRANSACTIONID(1);

	PG_RETURN_BOOL(FullTransactionIdPrecedesOrEquals(fxid1, fxid2));
}

Datum
xid8ge(PG_FUNCTION_ARGS)
{
	FullTransactionId fxid1 = PG_GETARG_FULLTRANSACTIONID(0);
	FullTransactionId fxid2 = PG_GETARG_FULLTRANSACTIONID(1);

	PG_RETURN_BOOL(FullTransactionIdFollowsOrEquals(fxid1, fxid2));
}

Datum
xid8cmp(PG_FUNCTION_ARGS)
{
	FullTransactionId fxid1 = PG_GETARG_FULLTRANSACTIONID(0);
	FullTransactionId fxid2 = PG_GETARG_FULLTRANSACTIONID(1);

	if (FullTransactionIdFollows(fxid1, fxid2))
		PG_RETURN_INT32(1);
	else if (FullTransactionIdEquals(fxid1, fxid2))
		PG_RETURN_INT32(0);
	else
		PG_RETURN_INT32(-1);
}

/*****************************************************************************
 *	 COMMAND IDENTIFIER ROUTINES											 *
 *****************************************************************************/

/*
 *		cidin	- converts CommandId to internal representation.
 */
Datum
cidin(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);

	PG_RETURN_COMMANDID((CommandId) strtoul(str, NULL, 0));
}

/*
 *		cidout	- converts a cid to external representation.
 */
Datum
cidout(PG_FUNCTION_ARGS)
{
	CommandId	c = PG_GETARG_COMMANDID(0);
	char	   *result = (char *) palloc(16);

	snprintf(result, 16, "%lu", (unsigned long) c);
	PG_RETURN_CSTRING(result);
}

/*
 *		cidrecv			- converts external binary format to cid
 */
Datum
cidrecv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

	PG_RETURN_COMMANDID((CommandId) pq_getmsgint(buf, sizeof(CommandId)));
}

/*
 *		cidsend			- converts cid to binary format
 */
Datum
cidsend(PG_FUNCTION_ARGS)
{
	CommandId	arg1 = PG_GETARG_COMMANDID(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint32(&buf, arg1);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

Datum
cideq(PG_FUNCTION_ARGS)
{
	CommandId	arg1 = PG_GETARG_COMMANDID(0);
	CommandId	arg2 = PG_GETARG_COMMANDID(1);

	PG_RETURN_BOOL(arg1 == arg2);
}
