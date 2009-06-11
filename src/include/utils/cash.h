/*
 * $PostgreSQL: pgsql/src/include/utils/cash.h,v 1.27 2009/06/11 14:49:13 momjian Exp $
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
#define DatumGetCash(X)		((Cash) DatumGetInt64(X))
#define CashGetDatum(X)		Int64GetDatum(X)
#define PG_GETARG_CASH(n)	DatumGetCash(PG_GETARG_DATUM(n))
#define PG_RETURN_CASH(x)	return CashGetDatum(x)

extern Datum cash_in(PG_FUNCTION_ARGS);
extern Datum cash_out(PG_FUNCTION_ARGS);
extern Datum cash_recv(PG_FUNCTION_ARGS);
extern Datum cash_send(PG_FUNCTION_ARGS);

extern Datum cash_eq(PG_FUNCTION_ARGS);
extern Datum cash_ne(PG_FUNCTION_ARGS);
extern Datum cash_lt(PG_FUNCTION_ARGS);
extern Datum cash_le(PG_FUNCTION_ARGS);
extern Datum cash_gt(PG_FUNCTION_ARGS);
extern Datum cash_ge(PG_FUNCTION_ARGS);
extern Datum cash_cmp(PG_FUNCTION_ARGS);

extern Datum cash_pl(PG_FUNCTION_ARGS);
extern Datum cash_mi(PG_FUNCTION_ARGS);

extern Datum cash_mul_flt8(PG_FUNCTION_ARGS);
extern Datum flt8_mul_cash(PG_FUNCTION_ARGS);
extern Datum cash_div_flt8(PG_FUNCTION_ARGS);

extern Datum cash_mul_flt4(PG_FUNCTION_ARGS);
extern Datum flt4_mul_cash(PG_FUNCTION_ARGS);
extern Datum cash_div_flt4(PG_FUNCTION_ARGS);

extern Datum cash_mul_int8(PG_FUNCTION_ARGS);
extern Datum int8_mul_cash(PG_FUNCTION_ARGS);
extern Datum cash_div_int8(PG_FUNCTION_ARGS);

extern Datum cash_mul_int4(PG_FUNCTION_ARGS);
extern Datum int4_mul_cash(PG_FUNCTION_ARGS);
extern Datum cash_div_int4(PG_FUNCTION_ARGS);

extern Datum cash_mul_int2(PG_FUNCTION_ARGS);
extern Datum int2_mul_cash(PG_FUNCTION_ARGS);
extern Datum cash_div_int2(PG_FUNCTION_ARGS);

extern Datum cashlarger(PG_FUNCTION_ARGS);
extern Datum cashsmaller(PG_FUNCTION_ARGS);

extern Datum cash_words(PG_FUNCTION_ARGS);

#endif   /* CASH_H */
