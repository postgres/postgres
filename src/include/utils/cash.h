/*
cash.h
Written by D'Arcy J.M. Cain

functions to allow input and output of money normally but store
and handle it as long integers
*/

#ifndef		_CASH_H
#define		_CASH_H

const char	*cash_out(long value);
long		cash_in(const char *str);
const char	*cash_words_out(long value);

#endif		/* _CASH_H */
