#include <pgtypes_numeric.h>

#ifndef dec_t
#define dec_t decimal
#endif   /* dec_t */

int			decadd(dec_t *, dec_t *, dec_t *);
int			deccmp(dec_t *, dec_t *);
void		deccopy(dec_t *, dec_t *);
int			deccvasc(char *, int, dec_t *);
int			deccvdbl(double, dec_t *);
int			deccvint(int, dec_t *);
int			deccvlong(long, dec_t *);
int			decdiv(dec_t *, dec_t *, dec_t *);
int			decmul(dec_t *, dec_t *, dec_t *);
int			decsub(dec_t *, dec_t *, dec_t *);
int			dectoasc(dec_t *, char *, int, int);
int			dectodbl(dec_t *, double *);
int			dectoint(dec_t *, int *);
int			dectolong(dec_t *, long *);
