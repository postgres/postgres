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

const char *cash_out(Cash *value);
Cash *cash_in(const char *str);

bool cash_eq(Cash *c1, Cash *c2);
bool cash_ne(Cash *c1, Cash *c2);
bool cash_lt(Cash *c1, Cash *c2);
bool cash_le(Cash *c1, Cash *c2);
bool cash_gt(Cash *c1, Cash *c2);
bool cash_ge(Cash *c1, Cash *c2);

Cash *cash_pl(Cash *c1, Cash *c2);
Cash *cash_mi(Cash *c1, Cash *c2);
Cash *cash_mul(Cash *c, float8 *f);
Cash *cash_div(Cash *c, float8 *f);

Cash *cashlarger(Cash *c1, Cash *c2);
Cash *cashsmaller(Cash *c1, Cash *c2);

const char *cash_words_out(Cash *value);
static const char *num_word(Cash value);

#endif /* CASH_H */
