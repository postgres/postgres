#include <pgtypes_numeric.h>

#ifndef dec_t
#define dec_t Numeric
#endif /* dec_t */

int decadd(dec_t *, Numeric *, Numeric *);
int deccmp(dec_t *, Numeric *);
void deccopy(dec_t *, Numeric *);
int deccvasc(char *, int, dec_t *);
int deccvdbl(double, dec_t *);
int deccvint(int, dec_t *);
int deccvlong(long, dec_t *);
int decdiv(dec_t *, Numeric *, Numeric *);
int decmul(dec_t *, Numeric *, Numeric *);
int decsub(dec_t *, Numeric *, Numeric *);
int dectoasc(dec_t *, char *, int, int);
int dectodbl(dec_t *, double *);
int dectoint(dec_t *, int *);
int dectolong(dec_t *, long *);

