/*
 * cash.h
 * Written by D'Arcy J.M. Cain
 *
 * Functions to allow input and output of money normally but store
 *	and handle it as int4.
 */

#ifndef CASH_H
#define CASH_H

/* if we store this as 4 bytes, we better make it int, not long, bjm */
typedef signed int Cash;

extern Datum cash_in(PG_FUNCTION_ARGS);
extern Datum cash_out(PG_FUNCTION_ARGS);

extern Datum cash_eq(PG_FUNCTION_ARGS);
extern Datum cash_ne(PG_FUNCTION_ARGS);
extern Datum cash_lt(PG_FUNCTION_ARGS);
extern Datum cash_le(PG_FUNCTION_ARGS);
extern Datum cash_gt(PG_FUNCTION_ARGS);
extern Datum cash_ge(PG_FUNCTION_ARGS);

extern Datum cash_pl(PG_FUNCTION_ARGS);
extern Datum cash_mi(PG_FUNCTION_ARGS);

extern Datum cash_mul_flt8(PG_FUNCTION_ARGS);
extern Datum cash_div_flt8(PG_FUNCTION_ARGS);
extern Datum flt8_mul_cash(PG_FUNCTION_ARGS);

extern Datum cash_mul_flt4(PG_FUNCTION_ARGS);
extern Datum cash_div_flt4(PG_FUNCTION_ARGS);
extern Datum flt4_mul_cash(PG_FUNCTION_ARGS);

extern Datum cash_mul_int4(PG_FUNCTION_ARGS);
extern Datum cash_div_int4(PG_FUNCTION_ARGS);
extern Datum int4_mul_cash(PG_FUNCTION_ARGS);

extern Datum cash_mul_int2(PG_FUNCTION_ARGS);
extern Datum int2_mul_cash(PG_FUNCTION_ARGS);
extern Datum cash_div_int2(PG_FUNCTION_ARGS);

extern Datum cashlarger(PG_FUNCTION_ARGS);
extern Datum cashsmaller(PG_FUNCTION_ARGS);

extern Datum cash_words(PG_FUNCTION_ARGS);

#endif	 /* CASH_H */
