#ifndef PGTYPES_NUMERIC
#define PGTYPES_NUMERIC

#define NUMERIC_POS                     0x0000
#define NUMERIC_NEG                     0x4000
#define NUMERIC_MAX_PRECISION           1000
#define NUMERIC_MAX_DISPLAY_SCALE       NUMERIC_MAX_PRECISION
#define NUMERIC_MIN_DISPLAY_SCALE       0
#define NUMERIC_MIN_SIG_DIGITS          16

typedef unsigned char NumericDigit;
typedef struct 
{
		int ndigits;		/* number of digits in digits[] - can be 0! */
		int weight;		/* weight of first digit */
		int rscale;		/* result scale */
		int dscale;		/* display scale */
		int sign;		/* NUMERIC_POS, NUMERIC_NEG, or NUMERIC_NAN */
		NumericDigit *buf;	/* start of alloc'd space for digits[] */
		NumericDigit *digits;	/* decimal digits */
} Numeric;

Numeric *PGTYPESnew(void);
void PGTYPESnumeric_free(Numeric *);
Numeric *PGTYPESnumeric_aton(char *, char **);
char *PGTYPESnumeric_ntoa(Numeric *, int);
int PGTYPESnumeric_add(Numeric *, Numeric *, Numeric *);
int PGTYPESnumeric_sub(Numeric *, Numeric *, Numeric *);
int PGTYPESnumeric_mul(Numeric *, Numeric *, Numeric *);
int PGTYPESnumeric_div(Numeric *, Numeric *, Numeric *);
int PGTYPESnumeric_cmp(Numeric *, Numeric *);
int PGTYPESnumeric_iton(signed int, Numeric *);
int PGTYPESnumeric_lton(signed long int, Numeric *);
int PGTYPESnumeric_copy(Numeric *, Numeric *);
int PGTYPESnumeric_dton(double, Numeric *);
int PGTYPESnumeric_ntod(Numeric *, double *);
int PGTYPESnumeric_ntoi(Numeric *, int *);
int PGTYPESnumeric_ntol(Numeric *, long *);

#endif /* PGTYPES_NUMERIC */
