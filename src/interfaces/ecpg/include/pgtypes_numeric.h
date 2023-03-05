#ifndef PGTYPES_NUMERIC
#define PGTYPES_NUMERIC

#include <pgtypes.h>

#define NUMERIC_POS						0x0000
#define NUMERIC_NEG						0x4000
#define NUMERIC_NAN						0xC000
#define NUMERIC_NULL						0xF000
#define NUMERIC_MAX_PRECISION			1000
#define NUMERIC_MAX_DISPLAY_SCALE		NUMERIC_MAX_PRECISION
#define NUMERIC_MIN_DISPLAY_SCALE		0
#define NUMERIC_MIN_SIG_DIGITS			16

#define DECSIZE 30

typedef unsigned char NumericDigit;
typedef struct
{
	int			ndigits;		/* number of digits in digits[] - can be 0! */
	int			weight;			/* weight of first digit */
	int			rscale;			/* result scale */
	int			dscale;			/* display scale */
	int			sign;			/* NUMERIC_POS, NUMERIC_NEG, or NUMERIC_NAN */
	NumericDigit *buf;			/* start of alloc'd space for digits[] */
	NumericDigit *digits;		/* decimal digits */
} numeric;

typedef struct
{
	int			ndigits;		/* number of digits in digits[] - can be 0! */
	int			weight;			/* weight of first digit */
	int			rscale;			/* result scale */
	int			dscale;			/* display scale */
	int			sign;			/* NUMERIC_POS, NUMERIC_NEG, or NUMERIC_NAN */
	NumericDigit digits[DECSIZE];	/* decimal digits */
} decimal;

#ifdef __cplusplus
extern "C"
{
#endif

numeric    *PGTYPESnumeric_new(void);
decimal    *PGTYPESdecimal_new(void);
void		PGTYPESnumeric_free(numeric *var);
void		PGTYPESdecimal_free(decimal *var);
numeric    *PGTYPESnumeric_from_asc(char *str, char **endptr);
char	   *PGTYPESnumeric_to_asc(numeric *num, int dscale);
int			PGTYPESnumeric_add(numeric *var1, numeric *var2, numeric *result);
int			PGTYPESnumeric_sub(numeric *var1, numeric *var2, numeric *result);
int			PGTYPESnumeric_mul(numeric *var1, numeric *var2, numeric *result);
int			PGTYPESnumeric_div(numeric *var1, numeric *var2, numeric *result);
int			PGTYPESnumeric_cmp(numeric *var1, numeric *var2);
int			PGTYPESnumeric_from_int(signed int int_val, numeric *var);
int			PGTYPESnumeric_from_long(signed long int long_val, numeric *var);
int			PGTYPESnumeric_copy(numeric *src, numeric *dst);
int			PGTYPESnumeric_from_double(double d, numeric *dst);
int			PGTYPESnumeric_to_double(numeric *nv, double *dp);
int			PGTYPESnumeric_to_int(numeric *nv, int *ip);
int			PGTYPESnumeric_to_long(numeric *nv, long *lp);
int			PGTYPESnumeric_to_decimal(numeric *src, decimal *dst);
int			PGTYPESnumeric_from_decimal(decimal *src, numeric *dst);

#ifdef __cplusplus
}
#endif

#endif							/* PGTYPES_NUMERIC */
