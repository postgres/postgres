/*
 * cash.h
 * Written by D'Arcy J.M. Cain
 *
 * Functions to allow input and output of money normally but store
 *  and handle it as long integers.
 */

#ifndef CASH_H
#define CASH_H

typedef long int Cash;

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
extern Cash *cash_mul(Cash *c, float8 *f);
extern Cash *cash_div(Cash *c, float8 *f);

extern Cash *cashlarger(Cash *c1, Cash *c2);
extern Cash *cashsmaller(Cash *c1, Cash *c2);

extern const char *cash_words_out(Cash *value);

#endif /* CASH_H */
