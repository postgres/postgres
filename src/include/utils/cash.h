/*
 * src/include/utils/cash.h
 *
 *
 * cash.h
 * Written by D'Arcy J.M. Cain
 *
 * Functions to allow input and output of money normally but store
 *	and handle it as 64 bit integer.
 */

#ifndef CASH_H
#define CASH_H

#include "fmgr.h"

typedef int64 Cash;

/* Cash is pass-by-reference if and only if int64 is */
static inline Cash
DatumGetCash(Datum X)
{
	return DatumGetInt64(X);
}

static inline Datum
CashGetDatum(Cash X)
{
	return Int64GetDatum(X);
}

#define PG_GETARG_CASH(n)	DatumGetCash(PG_GETARG_DATUM(n))
#define PG_RETURN_CASH(x)	return CashGetDatum(x)

#endif							/* CASH_H */
