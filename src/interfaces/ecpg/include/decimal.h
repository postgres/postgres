#include <pgtypes_numeric.h>

#ifndef dec_t
#define dec_t NumericVar
#endif /* dec_t */

int decadd(dec_t *, NumericVar *, NumericVar *);
int deccmp(dec_t *, NumericVar *);
void deccopy(dec_t *, NumericVar *);
int deccvasc(char *, int, dec_t *);
int deccvdbl(double, dec_t *);
int deccvint(int, dec_t *);
int deccvlong(long, dec_t *);
int decdiv(dec_t *, NumericVar *, NumericVar *);
int decmul(dec_t *, NumericVar *, NumericVar *);
int decsub(dec_t *, NumericVar *, NumericVar *);
int dectoasc(dec_t *, char *, int, int);
int dectodbl(dec_t *, double *);
int dectoint(dec_t *, int *);
int dectolong(dec_t *, long *);

