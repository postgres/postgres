#ifndef PGTYPES_NUMERIC
#define PGTYPES_NUMERIC

typedef unsigned char NumericDigit;
typedef struct NumericVar
{
		int ndigits;		/* number of digits in digits[] - can be 0! */
		int weight;		/* weight of first digit */
		int rscale;		/* result scale */
		int dscale;		/* display scale */
		int sign;		/* NUMERIC_POS, NUMERIC_NEG, or NUMERIC_NAN */
		NumericDigit *buf;	/* start of alloc'd space for digits[] */
		NumericDigit *digits;	/* decimal digits */
} NumericVar;

NumericVar *PGTYPESnew(void);
void PGTYPESnumeric_free(NumericVar *);
NumericVar *PGTYPESnumeric_aton(char *, char **);
char *PGTYPESnumeric_ntoa(NumericVar *);
int PGTYPESnumeric_add(NumericVar *, NumericVar *, NumericVar *);
int PGTYPESnumeric_sub(NumericVar *, NumericVar *, NumericVar *);
int PGTYPESnumeric_mul(NumericVar *, NumericVar *, NumericVar *);
int PGTYPESnumeric_div(NumericVar *, NumericVar *, NumericVar *);
int PGTYPESnumeric_cmp(NumericVar *, NumericVar *);
int PGTYPESnumeric_iton(signed int, NumericVar *);
int PGTYPESnumeric_lton(signed long int, NumericVar *);
int PGTYPESnumeric_copy(NumericVar *, NumericVar *);
int PGTYPESnumeric_dton(double, NumericVar *);
int PGTYPESnumeric_ntod(NumericVar *, double *);
int PGTYPESnumeric_ntoi(NumericVar *, int *);
int PGTYPESnumeric_ntol(NumericVar *, long *);

int decadd(NumericVar *, NumericVar *, NumericVar *);
int deccmp(NumericVar *, NumericVar *);
void deccopy(NumericVar *, NumericVar *);
int deccvasc(char *, int, NumericVar *);
int deccvdbl(double, NumericVar *);
int deccvint(int, NumericVar *);
int deccvlong(long, NumericVar *);
int decdiv(NumericVar *, NumericVar *, NumericVar *);
int decmul(NumericVar *, NumericVar *, NumericVar *);
int decsub(NumericVar *, NumericVar *, NumericVar *);
int dectoasc(NumericVar *, char *, int, int);
int dectodbl(NumericVar *, double *);
int dectoint(NumericVar *, int *);
int dectolong(NumericVar *, long *);

#endif /* PGTYPES_NUMERIC */
