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

extern const char *cash_out(Cash *value);
extern Cash *cash_in(const char *str);

extern bool cash_eq(Cash *c1, Cash *c2);
extern bool cash_ne(Cash *c1, Cash *c2);
extern bool cash_lt(Cash *c1, Cash *c2);
extern bool cash_le(Cash *c1, Cash *c2);
extern bool cash_gt(Cash *c1, Cash *c2);
extern bool cash_ge(Cash *c1, Cash *c2);

extern Cash *cash_pl(Cash *c1, Cash *c2);
extern Cash *cash_mi(Cash *c1, Cash *c2);

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

extern Cash *cashlarger(Cash *c1, Cash *c2);
extern Cash *cashsmaller(Cash *c1, Cash *c2);

extern Datum cash_words(PG_FUNCTION_ARGS);

#endif	 /* CASH_H */
