/* ----------
 * numeric.c -
 *
 *	An exact numeric data type for the Postgres database system
 *
 *	1998 Jan Wieck
 *
 * $Header: /cvsroot/pgsql/src/backend/utils/adt/numeric.c,v 1.2 1998/12/30 20:46:05 wieck Exp $
 *
 * ----------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <float.h>
#include <math.h>
#include <nan.h>
#include <errno.h>
#include <sys/types.h>

#include "postgres.h"
#include "utils/builtins.h"
#include "utils/palloc.h"
#include "utils/numeric.h"


/* ----------
 * Uncomment the following to enable compilation of dump_numeric()
 * and dump_var() and to get a dump of any result produced by make_result().
 * ----------
#define NUMERIC_DEBUG
 */


/* ----------
 * Local definitions
 * ----------
 */
#define NUMERIC_MIN_BUFSIZE		2048
#define NUMERIC_MAX_FREEBUFS	20

#ifndef MIN
#  define MIN(a,b) (((a)<(b)) ? (a) : (b))
#endif
#ifndef MAX
#  define MAX(a,b) (((a)>(b)) ? (a) : (b))
#endif



/* ----------
 * Local data types
 * ----------
 */
typedef unsigned char	NumericDigit;

typedef struct NumericDigitBuf {
	struct NumericDigitBuf	*prev;
	struct NumericDigitBuf	*next;
	int						size;
} NumericDigitBuf;

typedef struct NumericVar {
	int						ndigits;
	int						weight;
	int						rscale;
	int						dscale;
	int						sign;
	NumericDigitBuf			*buf;
	NumericDigit			*digits;
} NumericVar;


/* ----------
 * Local data
 * ----------
 */
static NumericDigitBuf	*digitbuf_freelist	= NULL;
static NumericDigitBuf	*digitbuf_usedlist	= NULL;
static int				digitbuf_nfree = 0;
static int				global_rscale = NUMERIC_MIN_RESULT_SCALE;

/* ----------
 * Some preinitialized variables we need often
 * ----------
 */
static NumericDigit		const_zero_data[1] = {0};
static NumericVar		const_zero =
		{0, 0, 0, 0, NUMERIC_POS, NULL, const_zero_data};

static NumericDigit		const_one_data[1] = {1};
static NumericVar		const_one =
		{1, 0, 0, 0, NUMERIC_POS, NULL, const_one_data};

static NumericDigit		const_two_data[1] = {2};
static NumericVar		const_two =
		{1, 0, 0, 0, NUMERIC_POS, NULL, const_two_data};

static NumericVar		const_nan =
		{0, 0, 0, 0, NUMERIC_NAN, NULL, NULL};



/* ----------
 * Local functions
 * ----------
 */

#ifdef NUMERIC_DEBUG
static void dump_numeric(char *str, Numeric num);
static void dump_var(char *str, NumericVar *var);
#else
#define dump_numeric(s,n)
#define dump_var(s,v)
#endif

static NumericDigitBuf *digitbuf_alloc(int size);
static void digitbuf_free(NumericDigitBuf *buf);

#define init_var(v)		memset(v,0,sizeof(NumericVar))
static void free_var(NumericVar *var);
static void free_allvars(void);

static void set_var_from_str(char *str, NumericVar *dest);
static void set_var_from_num(Numeric value, NumericVar *dest);
static void set_var_from_var(NumericVar *value, NumericVar *dest);
static Numeric make_result(NumericVar *var);

static void apply_typmod(NumericVar *var, int32 typmod);

static int  cmp_var(NumericVar *var1, NumericVar *var2);
static void add_var(NumericVar *var1, NumericVar *var2, NumericVar *result);
static void sub_var(NumericVar *var1, NumericVar *var2, NumericVar *result);
static void mul_var(NumericVar *var1, NumericVar *var2, NumericVar *result);
static void div_var(NumericVar *var1, NumericVar *var2, NumericVar *result);
static void mod_var(NumericVar *var1, NumericVar *var2, NumericVar *result);
static void ceil_var(NumericVar *var, NumericVar *result);
static void floor_var(NumericVar *var, NumericVar *result);

static void sqrt_var(NumericVar *arg, NumericVar *result);
static void exp_var(NumericVar *arg, NumericVar *result);
static void ln_var(NumericVar *arg, NumericVar *result);
static void log_var(NumericVar *base, NumericVar *num, NumericVar *result);
static void power_var(NumericVar *base, NumericVar *exp, NumericVar *result);

static int  cmp_abs(NumericVar *var1, NumericVar *var2);
static void add_abs(NumericVar *var1, NumericVar *var2, NumericVar *result);
static void sub_abs(NumericVar *var1, NumericVar *var2, NumericVar *result);



/* ----------------------------------------------------------------------
 *
 * Input-, output- and rounding-functions
 *
 * ----------------------------------------------------------------------
 */


/* ----------
 * numeric_in() -
 *
 *	Input function for numeric data type
 * ----------
 */
Numeric
numeric_in(char *str, int dummy, int32 typmod)
{
	NumericVar	value;
	Numeric		res;

	/* ----------
	 * Check for NULL
	 * ----------
	 */
	if (str == NULL)
		return NULL;

	if (strcmp(str, "NULL") == 0)
		return NULL;

	/* ----------
	 * Check for NaN
	 * ----------
	 */
	if (strcmp(str, "NaN") == 0)
		return make_result(&const_nan);

	/* ----------
	 * Use set_var_from_str() to parse the input string
	 * and return it in the packed DB storage format
	 * ----------
	 */
	init_var(&value);
	set_var_from_str(str, &value);

	apply_typmod(&value, typmod);

	res = make_result(&value);
	free_var(&value);

	return res;
}


/* ----------
 * numeric_out() -
 *
 *	Output function for numeric data type
 * ----------
 */
char *
numeric_out(Numeric num)
{
	char		*str;
	char		*cp;
	NumericVar	x;
	int			i;
	int			d;

	/* ----------
	 * Handle NULL
	 * ----------
	 */
	if (num == NULL)
	{
		str = palloc(5);
		strcpy(str, "NULL");
		return str;
	}

	/* ----------
	 * Handle NaN
	 * ----------
	 */
	if (NUMERIC_IS_NAN(num))
	{
		str = palloc(4);
		strcpy(str, "NaN");
		return str;
	}

	/* ----------
	 * Get the number in the variable format
	 * ----------
	 */
	init_var(&x);
	set_var_from_num(num, &x);

	/* ----------
	 * Allocate space for the result
	 * ----------
	 */
	str = palloc(x.dscale + MAX(0, x.weight) + 5);
	cp = str;
	
	/* ----------
	 * Output a dash for negative values
	 * ----------
	 */
	if (x.sign == NUMERIC_NEG)
		*cp++ = '-';

	/* ----------
	 * Check if we must round up before printing the value and
	 * do so.
	 * ----------
	 */
	if (x.dscale < x.rscale && (x.dscale + x.weight + 1) < x.ndigits)
	{
		int		j;
		int		carry;

		j = x.dscale + x.weight + 1;
		carry = (x.digits[j] > 4) ? 1 : 0;

		while (carry)
		{
			j--;
			carry += x.digits[j];
			x.digits[j] = carry % 10;
			carry /= 10;
		}
		if (j < 0)
		{
			x.digits--;
			x.weight++;
		}
	}

	/* ----------
	 * Output all digits before the decimal point
	 * ----------
	 */
	i = MAX(x.weight, 0);
	d = 0;

	while (i >= 0)
	{
		if (i <= x.weight && d < x.ndigits)
			*cp++ = x.digits[d++] + '0';
		else
			*cp++ = '0';
		i--;
	}

	/* ----------
	 * If requested, output a decimal point and all the digits
	 * that follow it.
	 * ----------
	 */
	if (x.dscale > 0)
	{
		*cp++ = '.';
		while (i >= -x.dscale)
		{
			if (i <= x.weight && d < x.ndigits)
				*cp++ = x.digits[d++] + '0';
			else
				*cp++ = '0';
			i--;
		}
	}

	/* ----------
	 * Get rid of the variable, terminate the string and return it
	 * ----------
	 */
	free_var(&x);

	*cp = '\0';
	return str;
}


/* ----------
 * numeric() -
 *
 *	This is a special function called by the Postgres database system
 *	before a value is stored in a tuples attribute. The precision and
 *	scale of the attribute have to be applied on the value. 
 * ----------
 */
Numeric
numeric(Numeric num, int32 typmod)
{
	Numeric		new;
	int32		tmp_typmod;
	int			precision;
	int			scale;
	int			maxweight;
	NumericVar	var;

	/* ----------
	 * Handle NULL
	 * ----------
	 */
	if (num == NULL)
		return NULL;

	/* ----------
	 * Handle NaN
	 * ----------
	 */
	if (NUMERIC_IS_NAN(num))
		return make_result(&const_nan);

	/* ----------
	 * If the value isn't a valid type modifier, simply return a
	 * copy of the input value
	 * ----------
	 */
	if (typmod < (int32)(VARHDRSZ))
	{
		new = (Numeric)palloc(num->varlen);
		memcpy(new, num, num->varlen);
		return new;
	}

	/* ----------
	 * Get the precision and scale out of the typmod value
	 * ----------
	 */
	tmp_typmod = typmod - VARHDRSZ;
	precision = (tmp_typmod >> 16) & 0xffff;
	scale     = tmp_typmod & 0xffff;
	maxweight = precision - scale;

	/* ----------
	 * If the number is in bounds and due to the present result scale
	 * no rounding could be necessary, make a copy of the input and
	 * modify it's header fields.
	 * ----------
	 */
	if (num->n_weight < maxweight && scale >= num->n_rscale)
	{
		new = (Numeric)palloc(num->varlen);
		memcpy(new, num, num->varlen);
		new->n_rscale = scale;
		new->n_sign_dscale = NUMERIC_SIGN(new) | 
					((uint16)scale & ~NUMERIC_SIGN_MASK);
		return new;
	}

	/* ----------
	 * We really need to fiddle with things - unpack the number into
	 * a variable and let apply_typmod() do it.
	 * ----------
	 */
	init_var(&var);

	set_var_from_num(num, &var);
	apply_typmod(&var, typmod);
	new = make_result(&var);

	free_var(&var);

	return new;
}


/* ----------------------------------------------------------------------
 *
 * Rounding and the like
 *
 * ----------------------------------------------------------------------
 */


Numeric
numeric_abs(Numeric num)
{
	Numeric		res;

	/* ----------
	 * Handle NULL
	 * ----------
	 */
	if (num == NULL)
		return NULL;

	/* ----------
	 * Handle NaN
	 * ----------
	 */
	if (NUMERIC_IS_NAN(num))
		return make_result(&const_nan);

	/* ----------
	 * Do it the easy way directly on the packed format
	 * ----------
	 */
	res = (Numeric)palloc(num->varlen);
	memcpy(res, num, num->varlen);

	res->n_sign_dscale = NUMERIC_POS | NUMERIC_DSCALE(num);

	return res;
}


Numeric
numeric_sign(Numeric num)
{
	Numeric		res;
	NumericVar	result;

	/* ----------
	 * Handle NULL
	 * ----------
	 */
	if (num == NULL)
		return NULL;

	/* ----------
	 * Handle NaN
	 * ----------
	 */
	if (NUMERIC_IS_NAN(num))
		return make_result(&const_nan);

	init_var(&result);

	/* ----------
	 * The packed format is known to be totally zero digit trimmed
	 * allways. So we can identify a ZERO by the fact that there
	 * are no digits at all.
	 * ----------
	 */
	if (num->varlen == NUMERIC_HDRSZ)
	{
		set_var_from_var(&const_zero, &result);
	}
	else
	{
		/* ----------
		 * And if there are some, we return a copy of ONE
		 * with the sign of our argument
		 * ----------
		 */
		set_var_from_var(&const_one, &result);
		result.sign = NUMERIC_SIGN(num);
	}

	res = make_result(&result);
	free_var(&result);

	return res;
}


/* ----------
 * numeric_round() -
 *
 *	Modify rscale and dscale of a number and round it if required.
 * ----------
 */
Numeric
numeric_round(Numeric num, int32 scale)
{
	int32		typmod;

	/* ----------
	 * Handle NULL
	 * ----------
	 */
	if (num == NULL)
		return NULL;

	/* ----------
	 * Handle NaN
	 * ----------
	 */
	if (NUMERIC_IS_NAN(num))
		return make_result(&const_nan);

	/* ----------
	 * Check that the requested scale is valid
	 * ----------
	 */
	if (scale < 0 || scale > NUMERIC_MAX_DISPLAY_SCALE)
	{
		free_allvars();
		elog(ERROR, "illegal numeric scale %d - must be between 0 and %d",
						scale, NUMERIC_MAX_DISPLAY_SCALE);
	}

	/* ----------
	 * Let numeric() and in turn apply_typmod() do the job
	 * ----------
	 */
	typmod = (((num->n_weight + scale + 1) << 16) | scale) + VARHDRSZ;
	return numeric(num, typmod);
}


/* ----------
 * numeric_trunc() -
 *
 *	Modify rscale and dscale of a number and cut it if required.
 * ----------
 */
Numeric
numeric_trunc(Numeric num, int32 scale)
{
	Numeric			res;
	NumericVar		arg;

	/* ----------
	 * Handle NULL
	 * ----------
	 */
	if (num == NULL)
		return NULL;

	/* ----------
	 * Handle NaN
	 * ----------
	 */
	if (NUMERIC_IS_NAN(num))
		return make_result(&const_nan);

	/* ----------
	 * Check that the requested scale is valid
	 * ----------
	 */
	if (scale < 0 || scale > NUMERIC_MAX_DISPLAY_SCALE)
	{
		free_allvars();
		elog(ERROR, "illegal numeric scale %d - must be between 0 and %d",
						scale, NUMERIC_MAX_DISPLAY_SCALE);
	}

	/* ----------
	 * Unpack the argument and truncate it
	 * ----------
	 */
	init_var(&arg);
	set_var_from_num(num, &arg);

	arg.rscale = scale;
	arg.dscale = scale;

	arg.ndigits = MIN(arg.ndigits, MAX(0, arg.weight + scale + 1));
	while (arg.ndigits > 0 && arg.digits[arg.ndigits - 1] == 0)
	{
		arg.ndigits--;
	}

	/* ----------
	 * Return the truncated result
	 * ----------
	 */
	res = make_result(&arg);

	free_var(&arg);
	return res;
}


/* ----------
 * numeric_ceil() -
 *
 *	Return the smallest integer greater than or equal to the argument
 * ----------
 */
Numeric
numeric_ceil(Numeric num)
{
	Numeric		res;
	NumericVar	result;

	if (num == NULL)
		return NULL;

	if (NUMERIC_IS_NAN(num))
		return make_result(&const_nan);

	init_var(&result);

	set_var_from_num(num, &result);
	ceil_var(&result, &result);

	result.dscale = 0;

	res = make_result(&result);
	free_var(&result);

	return res;
}


/* ----------
 * numeric_floor() -
 *
 *	Return the largest integer equal to or less than the argument
 * ----------
 */
Numeric
numeric_floor(Numeric num)
{
	Numeric		res;
	NumericVar	result;

	if (num == NULL)
		return NULL;

	if (NUMERIC_IS_NAN(num))
		return make_result(&const_nan);

	init_var(&result);

	set_var_from_num(num, &result);
	floor_var(&result, &result);

	result.dscale = 0;

	res = make_result(&result);
	free_var(&result);

	return res;
}


/* ----------------------------------------------------------------------
 *
 * Comparision functions
 *
 * ----------------------------------------------------------------------
 */


bool
numeric_eq(Numeric num1, Numeric num2)
{
	int			result;
	NumericVar	arg1;
	NumericVar	arg2;

	if (num1 == NULL || num2 == NULL)
		return FALSE;

	if (NUMERIC_IS_NAN(num1) || NUMERIC_IS_NAN(num2))
		return FALSE;

	init_var(&arg1);
	init_var(&arg2);

	set_var_from_num(num1, &arg1);
	set_var_from_num(num2, &arg2);

	result = cmp_var(&arg1, &arg2);

	free_var(&arg1);
	free_var(&arg2);

	return (result == 0);
}


bool
numeric_ne(Numeric num1, Numeric num2)
{
	int			result;
	NumericVar	arg1;
	NumericVar	arg2;

	if (num1 == NULL || num2 == NULL)
		return FALSE;

	if (NUMERIC_IS_NAN(num1) || NUMERIC_IS_NAN(num2))
		return FALSE;

	init_var(&arg1);
	init_var(&arg2);

	set_var_from_num(num1, &arg1);
	set_var_from_num(num2, &arg2);

	result = cmp_var(&arg1, &arg2);

	free_var(&arg1);
	free_var(&arg2);

	return (result != 0);
}


bool
numeric_gt(Numeric num1, Numeric num2)
{
	int			result;
	NumericVar	arg1;
	NumericVar	arg2;

	if (num1 == NULL || num2 == NULL)
		return FALSE;

	if (NUMERIC_IS_NAN(num1) || NUMERIC_IS_NAN(num2))
		return FALSE;

	init_var(&arg1);
	init_var(&arg2);

	set_var_from_num(num1, &arg1);
	set_var_from_num(num2, &arg2);

	result = cmp_var(&arg1, &arg2);

	free_var(&arg1);
	free_var(&arg2);

	return (result > 0);
}


bool
numeric_ge(Numeric num1, Numeric num2)
{
	int			result;
	NumericVar	arg1;
	NumericVar	arg2;

	if (num1 == NULL || num2 == NULL)
		return FALSE;

	if (NUMERIC_IS_NAN(num1) || NUMERIC_IS_NAN(num2))
		return FALSE;

	init_var(&arg1);
	init_var(&arg2);

	set_var_from_num(num1, &arg1);
	set_var_from_num(num2, &arg2);

	result = cmp_var(&arg1, &arg2);

	free_var(&arg1);
	free_var(&arg2);

	return (result >= 0);
}


bool
numeric_lt(Numeric num1, Numeric num2)
{
	int			result;
	NumericVar	arg1;
	NumericVar	arg2;

	if (num1 == NULL || num2 == NULL)
		return FALSE;

	if (NUMERIC_IS_NAN(num1) || NUMERIC_IS_NAN(num2))
		return FALSE;

	init_var(&arg1);
	init_var(&arg2);

	set_var_from_num(num1, &arg1);
	set_var_from_num(num2, &arg2);

	result = cmp_var(&arg1, &arg2);

	free_var(&arg1);
	free_var(&arg2);

	return (result < 0);
}


bool
numeric_le(Numeric num1, Numeric num2)
{
	int			result;
	NumericVar	arg1;
	NumericVar	arg2;

	if (num1 == NULL || num2 == NULL)
		return FALSE;

	if (NUMERIC_IS_NAN(num1) || NUMERIC_IS_NAN(num2))
		return FALSE;

	init_var(&arg1);
	init_var(&arg2);

	set_var_from_num(num1, &arg1);
	set_var_from_num(num2, &arg2);

	result = cmp_var(&arg1, &arg2);

	free_var(&arg1);
	free_var(&arg2);

	return (result <= 0);
}


/* ----------------------------------------------------------------------
 *
 * Arithmetic base functions
 *
 * ----------------------------------------------------------------------
 */


/* ----------
 * numeric_add() -
 *
 *	Add two numerics
 * ----------
 */
Numeric
numeric_add(Numeric num1, Numeric num2)
{
	NumericVar	arg1;
	NumericVar	arg2;
	NumericVar	result;
	Numeric		res;

	/* ----------
	 * Handle NULL
	 * ----------
	 */
	if (num1 == NULL || num2 == NULL)
		return NULL;

	/* ----------
	 * Handle NaN
	 * ----------
	 */
	if (NUMERIC_IS_NAN(num1) || NUMERIC_IS_NAN(num2))
		return make_result(&const_nan);

	/* ----------
	 * Unpack the values, let add_var() compute the result
	 * and return it. The internals of add_var() will automatically
	 * set the correct result and display scales in the result.
	 * ----------
	 */
	init_var(&arg1);
	init_var(&arg2);
	init_var(&result);

	set_var_from_num(num1, &arg1);
	set_var_from_num(num2, &arg2);

	add_var(&arg1, &arg2, &result);
	res = make_result(&result);

	free_var(&arg1);
	free_var(&arg2);
	free_var(&result);

	return res;
}


/* ----------
 * numeric_sub() -
 *
 *	Subtract one numeric from another
 * ----------
 */
Numeric
numeric_sub(Numeric num1, Numeric num2)
{
	NumericVar	arg1;
	NumericVar	arg2;
	NumericVar	result;
	Numeric		res;

	/* ----------
	 * Handle NULL
	 * ----------
	 */
	if (num1 == NULL || num2 == NULL)
		return NULL;

	/* ----------
	 * Handle NaN
	 * ----------
	 */
	if (NUMERIC_IS_NAN(num1) || NUMERIC_IS_NAN(num2))
		return make_result(&const_nan);

	/* ----------
	 * Unpack the two arguments, let sub_var() compute the
	 * result and return it.
	 * ----------
	 */
	init_var(&arg1);
	init_var(&arg2);
	init_var(&result);

	set_var_from_num(num1, &arg1);
	set_var_from_num(num2, &arg2);

	sub_var(&arg1, &arg2, &result);
	res = make_result(&result);

	free_var(&arg1);
	free_var(&arg2);
	free_var(&result);

	return res;
}


/* ----------
 * numeric_mul() -
 *
 *	Calculate the product of two numerics
 * ----------
 */
Numeric
numeric_mul(Numeric num1, Numeric num2)
{
	NumericVar	arg1;
	NumericVar	arg2;
	NumericVar	result;
	Numeric		res;

	/* ----------
	 * Handle NULL
	 * ----------
	 */
	if (num1 == NULL || num2 == NULL)
		return NULL;

	/* ----------
	 * Handle NaN
	 * ----------
	 */
	if (NUMERIC_IS_NAN(num1) || NUMERIC_IS_NAN(num2))
		return make_result(&const_nan);

	/* ----------
	 * Unpack the arguments, let mul_var() compute the result
	 * and return it. Unlike add_var() and sub_var(), mul_var()
	 * will round the result to the scale stored in global_rscale.
	 * In the case of numeric_mul(), which is invoked for the *
	 * operator on numerics, we set it to the exact representation
	 * for the product (rscale = sum(rscale of arg1, rscale of arg2)
	 * and the same for the dscale).
	 * ----------
	 */
	init_var(&arg1);
	init_var(&arg2);
	init_var(&result);

	set_var_from_num(num1, &arg1);
	set_var_from_num(num2, &arg2);

	global_rscale = arg1.rscale + arg2.rscale;

	mul_var(&arg1, &arg2, &result);

	result.dscale = arg1.dscale + arg2.dscale;

	res = make_result(&result);

	free_var(&arg1);
	free_var(&arg2);
	free_var(&result);

	return res;
}


/* ----------
 * numeric_div() -
 *
 *	Divide one numeric into another
 * ----------
 */
Numeric
numeric_div(Numeric num1, Numeric num2)
{
	NumericVar	arg1;
	NumericVar	arg2;
	NumericVar	result;
	Numeric		res;
	int			res_dscale;

	/* ----------
	 * Handle NULL
	 * ----------
	 */
	if (num1 == NULL || num2 == NULL)
		return NULL;

	/* ----------
	 * Handle NaN
	 * ----------
	 */
	if (NUMERIC_IS_NAN(num1) || NUMERIC_IS_NAN(num2))
		return make_result(&const_nan);

	/* ----------
	 * Unpack the arguments
	 * ----------
	 */
	init_var(&arg1);
	init_var(&arg2);
	init_var(&result);

	set_var_from_num(num1, &arg1);
	set_var_from_num(num2, &arg2);

	/* ----------
	 * The result scale of a division isn't specified in any
	 * SQL standard. For Postgres it is the following (where
	 * SR, DR are the result- and display-scales of the returned
	 * value, S1, D1, S2 and D2 are the scales of the two arguments,
	 * The minimum and maximum scales are compile time options from
	 * numeric.h):
	 *
	 *	DR = MIN(MAX(D1 + D2, MIN_DISPLAY_SCALE))
	 *  SR = MIN(MAX(MAX(S1 + S2, MIN_RESULT_SCALE), DR + 4), MAX_RESULT_SCALE)
	 *
	 * By default, any result is computed with a minimum of 34 digits
	 * after the decimal point or at least with 4 digits more than
	 * displayed.
	 * ----------
	 */
	res_dscale = MAX(arg1.dscale + arg2.dscale, NUMERIC_MIN_DISPLAY_SCALE);
	res_dscale = MIN(res_dscale, NUMERIC_MAX_DISPLAY_SCALE);
	global_rscale = MAX(arg1.rscale + arg2.rscale, 
						NUMERIC_MIN_RESULT_SCALE);
	global_rscale = MAX(global_rscale, res_dscale + 4);
	global_rscale = MIN(global_rscale, NUMERIC_MAX_RESULT_SCALE);

	/* ----------
	 * Do the divide, set the display scale and return the result
	 * ----------
	 */
	div_var(&arg1, &arg2, &result);

	result.dscale = res_dscale;

	res = make_result(&result);

	free_var(&arg1);
	free_var(&arg2);
	free_var(&result);

	return res;
}


/* ----------
 * numeric_mod() -
 *
 *	Calculate the modulo of two numerics
 * ----------
 */
Numeric
numeric_mod(Numeric num1, Numeric num2)
{
	Numeric		res;
	NumericVar	arg1;
	NumericVar	arg2;
	NumericVar	result;

	if (num1 == NULL || num2 == NULL)
		return NULL;

	if (NUMERIC_IS_NAN(num1) || NUMERIC_IS_NAN(num2))
		return make_result(&const_nan);

	init_var(&arg1);
	init_var(&arg2);
	init_var(&result);

	set_var_from_num(num1, &arg1);
	set_var_from_num(num2, &arg2);

	mod_var(&arg1, &arg2, &result);

	result.dscale = result.rscale;
	res = make_result(&result);

	free_var(&result);
	free_var(&arg2);
	free_var(&arg1);

	return res;
}


/* ----------
 * numeric_inc() -
 *
 *	Increment a number by one
 * ----------
 */
Numeric
numeric_inc(Numeric num)
{
	NumericVar	arg;
	Numeric		res;

	/* ----------
	 * Handle NULL
	 * ----------
	 */
	if (num == NULL)
		return NULL;

	/* ----------
	 * Handle NaN
	 * ----------
	 */
	if (NUMERIC_IS_NAN(num))
		return make_result(&const_nan);

	/* ----------
	 * Compute the result and return it
	 * ----------
	 */
	init_var(&arg);

	set_var_from_num(num, &arg);

	add_var(&arg, &const_one, &arg);
	res = make_result(&arg);

	free_var(&arg);

	return res;
}


/* ----------
 * numeric_dec() -
 *
 *	Decrement a number by one
 * ----------
 */
Numeric
numeric_dec(Numeric num)
{
	NumericVar	arg;
	Numeric		res;

	/* ----------
	 * Handle NULL
	 * ----------
	 */
	if (num == NULL)
		return NULL;

	/* ----------
	 * Handle NaN
	 * ----------
	 */
	if (NUMERIC_IS_NAN(num))
		return make_result(&const_nan);

	/* ----------
	 * Compute the result and return it
	 * ----------
	 */
	init_var(&arg);

	set_var_from_num(num, &arg);

	sub_var(&arg, &const_one, &arg);
	res = make_result(&arg);

	free_var(&arg);

	return res;
}


/* ----------
 * numeric_smaller() -
 *
 *	Return the smaller of two numbers
 * ----------
 */
Numeric
numeric_smaller(Numeric num1, Numeric num2)
{
	NumericVar	arg1;
	NumericVar	arg2;
	Numeric		res;

	/* ----------
	 * Handle NULL
	 * ----------
	 */
	if (num1 == NULL || num2 == NULL)
		return NULL;

	/* ----------
	 * Handle NaN
	 * ----------
	 */
	if (NUMERIC_IS_NAN(num1) || NUMERIC_IS_NAN(num2))
		return make_result(&const_nan);

	/* ----------
	 * Unpack the values, and decide which is the smaller one
	 * ----------
	 */
	init_var(&arg1);
	init_var(&arg2);

	set_var_from_num(num1, &arg1);
	set_var_from_num(num2, &arg2);

	if (cmp_var(&arg1, &arg2) <= 0)
		res = make_result(&arg1);
	else
		res = make_result(&arg2);

	free_var(&arg1);
	free_var(&arg2);

	return res;
}


/* ----------
 * numeric_larger() -
 *
 *	Return the larger of two numbers
 * ----------
 */
Numeric
numeric_larger(Numeric num1, Numeric num2)
{
	NumericVar	arg1;
	NumericVar	arg2;
	Numeric		res;

	/* ----------
	 * Handle NULL
	 * ----------
	 */
	if (num1 == NULL || num2 == NULL)
		return NULL;

	/* ----------
	 * Handle NaN
	 * ----------
	 */
	if (NUMERIC_IS_NAN(num1) || NUMERIC_IS_NAN(num2))
		return make_result(&const_nan);

	/* ----------
	 * Unpack the values, and decide which is the larger one
	 * ----------
	 */
	init_var(&arg1);
	init_var(&arg2);

	set_var_from_num(num1, &arg1);
	set_var_from_num(num2, &arg2);

	if (cmp_var(&arg1, &arg2) >= 0)
		res = make_result(&arg1);
	else
		res = make_result(&arg2);

	free_var(&arg1);
	free_var(&arg2);

	return res;
}


/* ----------------------------------------------------------------------
 *
 * Complex math functions
 *
 * ----------------------------------------------------------------------
 */


/* ----------
 * numeric_sqrt() -
 *
 *	Compute the square root of a numeric.
 * ----------
 */
Numeric
numeric_sqrt(Numeric num)
{
	Numeric			res;
	NumericVar		arg;
	NumericVar		result;
	int				res_dscale;

	/* ----------
	 * Handle NULL
	 * ----------
	 */
	if (num == NULL)
		return NULL;

	/* ----------
	 * Handle NaN
	 * ----------
	 */
	if (NUMERIC_IS_NAN(num))
		return make_result(&const_nan);

	/* ----------
	 * Unpack the argument, determine the scales like for divide,
	 * let sqrt_var() do the calculation and return the result.
	 * ----------
	 */
	init_var(&arg);
	init_var(&result);

	set_var_from_num(num, &arg);

	res_dscale = MAX(arg.dscale, NUMERIC_MIN_DISPLAY_SCALE);
	res_dscale = MIN(res_dscale, NUMERIC_MAX_DISPLAY_SCALE);
	global_rscale = MAX(arg.rscale, NUMERIC_MIN_RESULT_SCALE);
	global_rscale = MAX(global_rscale, res_dscale + 4);
	global_rscale = MIN(global_rscale, NUMERIC_MAX_RESULT_SCALE);

	sqrt_var(&arg, &result);

	result.dscale = res_dscale;

	res = make_result(&result);

	free_var(&result);
	free_var(&arg);

	return res;
}


/* ----------
 * numeric_exp() -
 *
 *	Raise e to the power of x
 * ----------
 */
Numeric
numeric_exp(Numeric num)
{
	Numeric			res;
	NumericVar		arg;
	NumericVar		result;
	int				res_dscale;

	/* ----------
	 * Handle NULL
	 * ----------
	 */
	if (num == NULL)
		return NULL;

	/* ----------
	 * Handle NaN
	 * ----------
	 */
	if (NUMERIC_IS_NAN(num))
		return make_result(&const_nan);

	/* ----------
	 * Same procedure like for sqrt().
	 * ----------
	 */
	init_var(&arg);
	init_var(&result);
	set_var_from_num(num, &arg);

	res_dscale = MAX(arg.dscale, NUMERIC_MIN_DISPLAY_SCALE);
	res_dscale = MIN(res_dscale, NUMERIC_MAX_DISPLAY_SCALE);
	global_rscale = MAX(arg.rscale, NUMERIC_MIN_RESULT_SCALE);
	global_rscale = MAX(global_rscale, res_dscale + 4);
	global_rscale = MIN(global_rscale, NUMERIC_MAX_RESULT_SCALE);

	exp_var(&arg, &result);

	result.dscale = res_dscale;

	res = make_result(&result);

	free_var(&result);
	free_var(&arg);

	return res;
}


/* ----------
 * numeric_ln() -
 *
 *	Compute the natural logarithm of x
 * ----------
 */
Numeric
numeric_ln(Numeric num)
{
	Numeric			res;
	NumericVar		arg;
	NumericVar		result;
	int				res_dscale;

	/* ----------
	 * Handle NULL
	 * ----------
	 */
	if (num == NULL)
		return NULL;

	/* ----------
	 * Handle NaN
	 * ----------
	 */
	if (NUMERIC_IS_NAN(num))
		return make_result(&const_nan);

	/* ----------
	 * Same procedure like for sqrt()
	 * ----------
	 */
	init_var(&arg);
	init_var(&result);
	set_var_from_num(num, &arg);

	res_dscale = MAX(arg.dscale, NUMERIC_MIN_DISPLAY_SCALE);
	res_dscale = MIN(res_dscale, NUMERIC_MAX_DISPLAY_SCALE);
	global_rscale = MAX(arg.rscale, NUMERIC_MIN_RESULT_SCALE);
	global_rscale = MAX(global_rscale, res_dscale + 4);
	global_rscale = MIN(global_rscale, NUMERIC_MAX_RESULT_SCALE);

	ln_var(&arg, &result);

	result.dscale = res_dscale;

	res = make_result(&result);

	free_var(&result);
	free_var(&arg);

	return res;
}


/* ----------
 * numeric_ln() -
 *
 *	Compute the logarithm of x in a given base
 * ----------
 */
Numeric
numeric_log(Numeric num1, Numeric num2)
{
	Numeric			res;
	NumericVar		arg1;
	NumericVar		arg2;
	NumericVar		result;
	int				res_dscale;

	/* ----------
	 * Handle NULL
	 * ----------
	 */
	if (num1 == NULL || num2 == NULL)
		return NULL;

	/* ----------
	 * Handle NaN
	 * ----------
	 */
	if (NUMERIC_IS_NAN(num1) || NUMERIC_IS_NAN(num2))
		return make_result(&const_nan);

	/* ----------
	 * Initialize things and calculate scales
	 * ----------
	 */
	init_var(&arg1);
	init_var(&arg2);
	init_var(&result);
	set_var_from_num(num1, &arg1);
	set_var_from_num(num2, &arg2);

	res_dscale = MAX(arg1.dscale + arg2.dscale, NUMERIC_MIN_DISPLAY_SCALE);
	res_dscale = MIN(res_dscale, NUMERIC_MAX_DISPLAY_SCALE);
	global_rscale = MAX(arg1.rscale + arg2.rscale, NUMERIC_MIN_RESULT_SCALE);
	global_rscale = MAX(global_rscale, res_dscale + 4);
	global_rscale = MIN(global_rscale, NUMERIC_MAX_RESULT_SCALE);

	/* ----------
	 * Call log_var() to compute and return the result
	 * ----------
	 */
	log_var(&arg1, &arg2, &result);

	result.dscale = res_dscale;

	res = make_result(&result);

	free_var(&result);
	free_var(&arg2);
	free_var(&arg1);

	return res;
}


/* ----------
 * numeric_power() -
 *
 *	Raise m to the power of x
 * ----------
 */
Numeric
numeric_power(Numeric num1, Numeric num2)
{
	Numeric			res;
	NumericVar		arg1;
	NumericVar		arg2;
	NumericVar		result;
	int				res_dscale;

	/* ----------
	 * Handle NULL
	 * ----------
	 */
	if (num1 == NULL || num2 == NULL)
		return NULL;

	/* ----------
	 * Handle NaN
	 * ----------
	 */
	if (NUMERIC_IS_NAN(num1) || NUMERIC_IS_NAN(num2))
		return make_result(&const_nan);

	/* ----------
	 * Initialize things and calculate scales
	 * ----------
	 */
	init_var(&arg1);
	init_var(&arg2);
	init_var(&result);
	set_var_from_num(num1, &arg1);
	set_var_from_num(num2, &arg2);

	res_dscale = MAX(arg1.dscale + arg2.dscale, NUMERIC_MIN_DISPLAY_SCALE);
	res_dscale = MIN(res_dscale, NUMERIC_MAX_DISPLAY_SCALE);
	global_rscale = MAX(arg1.rscale + arg2.rscale, NUMERIC_MIN_RESULT_SCALE);
	global_rscale = MAX(global_rscale, res_dscale + 4);
	global_rscale = MIN(global_rscale, NUMERIC_MAX_RESULT_SCALE);

	/* ----------
	 * Call log_var() to compute and return the result
	 * ----------
	 */
	power_var(&arg1, &arg2, &result);

	result.dscale = res_dscale;

	res = make_result(&result);

	free_var(&result);
	free_var(&arg2);
	free_var(&arg1);

	return res;
}


/* ----------------------------------------------------------------------
 *
 * Type conversion functions
 *
 * ----------------------------------------------------------------------
 */
Numeric
int4_numeric(int32 val)
{
	Numeric		res;
	NumericVar	result;
	char		*tmp;

	init_var(&result);

	tmp = int4out(val);
	set_var_from_str(tmp, &result);
	res = make_result(&result);

	free_var(&result);
	pfree(tmp);

	return res;
}


int32
numeric_int4(Numeric num)
{
	char		*tmp;
	int32		result;

	if (num == NULL)
		return 0;

	if (NUMERIC_IS_NAN(num))
		return 0;

	tmp = numeric_out(num);
	result = int4in(tmp);
	pfree(tmp);

	return result;
}


Numeric
float8_numeric(float64 val)
{
	Numeric		res;
	NumericVar	result;
	char		*tmp;

	if (val == NULL)
		return NULL;

	if (isnan(*val))
		return make_result(&const_nan);

	init_var(&result);

	tmp = float8out(val);
	set_var_from_str(tmp, &result);
	res = make_result(&result);

	free_var(&result);
	pfree(tmp);

	return res;
}



float64
numeric_float8(Numeric num)
{
	char		*tmp;
	float64		result;

	if (num == NULL)
		return NULL;

	if (NUMERIC_IS_NAN(num))
	{
		result = (float64)palloc(sizeof(float64data));
		*result = NAN;
		return result;
	}

	tmp = numeric_out(num);
	result = float8in(tmp);
	pfree(tmp);

	return result;
}


Numeric
float4_numeric(float32 val)
{
	Numeric		res;
	NumericVar	result;
	char		*tmp;

	if (val == NULL)
		return NULL;

	if (isnan(*val))
		return make_result(&const_nan);

	init_var(&result);

	tmp = float4out(val);
	set_var_from_str(tmp, &result);
	res = make_result(&result);

	free_var(&result);
	pfree(tmp);

	return res;
}


float32
numeric_float4(Numeric num)
{
	char		*tmp;
	float32		result;

	if (num == NULL)
		return NULL;

	if (NUMERIC_IS_NAN(num))
	{
		result = (float32)palloc(sizeof(float32data));
		*result = NAN;
		return result;
	}

	tmp = numeric_out(num);
	result = float4in(tmp);
	pfree(tmp);

	return result;
}


/* ----------------------------------------------------------------------
 *
 * Local functions follow
 *
 * ----------------------------------------------------------------------
 */


#ifdef NUMERIC_DEBUG

/* ----------
 * dump_numeric() - Dump a value in the db storage format for debugging
 * ----------
 */
static void
dump_numeric(char *str, Numeric num)
{
	int		i;

	printf("%s: NUMERIC w=%d r=%d d=%d ", str, num->n_weight, num->n_rscale,
						NUMERIC_DSCALE(num));
	switch (NUMERIC_SIGN(num))
	{
		case NUMERIC_POS:	printf("POS");
							break;
		case NUMERIC_NEG:	printf("NEG");
							break;
		case NUMERIC_NAN:	printf("NaN");
							break;
		default:			printf("SIGN=0x%x", NUMERIC_SIGN(num));
							break;
	}

	for (i = 0; i < num->varlen - NUMERIC_HDRSZ; i++)
	{
		printf(" %d %d", (num->n_data[i] >> 4) & 0x0f, num->n_data[i] & 0x0f);
	}
	printf("\n");
}


/* ----------
 * dump_var() - Dump a value in the variable format for debugging
 * ----------
 */
static void
dump_var(char *str, NumericVar *var)
{
	int		i;

	printf("%s: VAR w=%d r=%d d=%d ", str, var->weight, var->rscale,
						var->dscale);
	switch (var->sign)
	{
		case NUMERIC_POS:	printf("POS");
							break;
		case NUMERIC_NEG:	printf("NEG");
							break;
		case NUMERIC_NAN:	printf("NaN");
							break;
		default:			printf("SIGN=0x%x", var->sign);
							break;
	}

	for (i = 0; i < var->ndigits; i++)
		printf(" %d", var->digits[i]);
	
	printf("\n");
}

#endif /* NUMERIC_DEBUG */


/* ----------
 * digitbuf_alloc() -
 *
 *	All variables used in the arithmetic functions hold some base
 *	information (sign, scales etc.) and a digit buffer for the
 *	value itself. All the variable level functions are written in
 *	a style that makes it possible to give one and the same variable
 *	as argument and result destination. 
 *
 *	The two functions below manage unused buffers in a free list
 *	as a try to reduce the number of malloc()/free() calls.
 * ----------
 */
static NumericDigitBuf *
digitbuf_alloc(int size)
{
	NumericDigitBuf		*buf;
	int					asize;

	/* ----------
	 * Lookup the free list if there is a digit buffer of
	 * the required size available
	 * ----------
	 */
	for (buf = digitbuf_freelist; buf != NULL; buf = buf->next)
	{
		if (buf->size < size) continue;

		/* ----------
		 * We found a free buffer that is big enough - remove it from
		 * the free list
		 * ----------
		 */
		if (buf->prev == NULL)
		{
			digitbuf_freelist = buf->next;
			if (buf->next != NULL)
				buf->next->prev = NULL;
		}
		else
		{
			buf->prev->next = buf->next;
			if (buf->next != NULL)
				buf->next->prev = buf->prev;
		}
		digitbuf_nfree--;

		/* ----------
		 * Put it onto the used list
		 * ----------
		 */
		buf->prev = NULL;
		buf->next = digitbuf_usedlist;
		if (digitbuf_usedlist != NULL)
			digitbuf_usedlist->prev = buf;
		digitbuf_usedlist = buf;

		/* ----------
		 * Return this buffer
		 * ----------
		 */
		return buf;
	}

	/* ----------
	 * There is no free buffer big enough - allocate a new one
	 * ----------
	 */
	for (asize = NUMERIC_MIN_BUFSIZE; asize < size; asize *= 2);
	buf = (NumericDigitBuf *)malloc(sizeof(NumericDigitBuf) + asize);
	buf->size = asize;

	/* ----------
	 * Put it onto the used list
	 * ----------
	 */
	buf->prev = NULL;
	buf->next = digitbuf_usedlist;
	if (digitbuf_usedlist != NULL)
		digitbuf_usedlist->prev = buf;
	digitbuf_usedlist = buf;

	/* ----------
	 * Return the new buffer
	 * ----------
	 */
	return buf;
}


/* ----------
 * digitbuf_free() -
 * ----------
 */
static void
digitbuf_free(NumericDigitBuf *buf)
{
	NumericDigitBuf		*smallest;

	if (buf == NULL)
		return;

	/* ----------
	 * Remove the buffer from the used list
	 * ----------
	 */
	if (buf->prev == NULL)
	{
		digitbuf_usedlist = buf->next;
		if (buf->next != NULL)
			buf->next->prev = NULL;
	}
	else
	{
		buf->prev->next = buf->next;
		if (buf->next != NULL)
			buf->next->prev = buf->prev;
	}

	/* ----------
	 * Put it onto the free list
	 * ----------
	 */
	if (digitbuf_freelist != NULL)
		digitbuf_freelist->prev = buf;
	buf->prev = NULL;
	buf->next = digitbuf_freelist;
	digitbuf_freelist = buf;
	digitbuf_nfree++;

	/* ----------
	 * Check for maximum free buffers
	 * ----------
	 */
	if (digitbuf_nfree <= NUMERIC_MAX_FREEBUFS)
		return;

	/* ----------
	 * Have too many free buffers - destroy the smallest one
	 * ----------
	 */
	smallest = buf;
	for (buf = digitbuf_freelist->next; buf != NULL; buf = buf->next)
	{
		if (buf->size < smallest->size)
			smallest = buf;
	}

	/* ----------
	 * Remove it from the free list
	 * ----------
	 */
	if (smallest->prev == NULL)
	{
		digitbuf_freelist = smallest->next;
		if (smallest->next != NULL)
			smallest->next->prev = NULL;
	}
	else
	{
		smallest->prev->next = smallest->next;
		if (smallest->next != NULL)
			smallest->next->prev = smallest->prev;
	}
	digitbuf_nfree--;

	/* ----------
	 * And destroy it
	 * ----------
	 */
	free(smallest);
}


/* ----------
 * free_var() -
 *
 *	Return the digit buffer of a variable to the pool
 * ----------
 */
static void
free_var(NumericVar *var)
{
	if (var->buf != NULL)
	{
		digitbuf_free(var->buf);
		var->buf    = NULL;
		var->digits = NULL;
	}
	var->sign = NUMERIC_NAN;
}


/* ----------
 * free_allvars() -
 *
 *	Put all the currently used buffers back into the pool.
 *
 *	Warning: the variables currently holding the buffers aren't
 *	cleaned! This function should only be used directly before
 *	a call to elog(ERROR,...) or if it is totally impossible that
 *	any other call to free_var() will occur. None of the variable
 *	level functions should call it if it might return later without
 *	an error.
 * ----------
 */
static void
free_allvars(void)
{
	NumericDigitBuf		*buf;
	NumericDigitBuf		*next;

	buf = digitbuf_usedlist;
	while (buf != NULL)
	{
		next = buf->next;
		digitbuf_free(buf);
		buf = next;
	}
}


/* ----------
 * set_var_from_str()
 *
 *	Parse a string and put the number into a variable
 * ----------
 */
static void
set_var_from_str(char *str, NumericVar *dest)
{
	char			*cp = str;
	bool			have_dp = FALSE;
	int				i = 1;

	while(*cp)
	{
		if (!isspace(*cp)) break;
		cp++;
	}

	digitbuf_free(dest->buf);

	dest->buf       = digitbuf_alloc(strlen(cp) + 2);
	dest->digits    = (NumericDigit *)(dest->buf) + sizeof(NumericDigitBuf);
	dest->digits[0] = 0;
	dest->weight    = 0;
	dest->dscale    = 0;

	switch (*cp)
	{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':		dest->sign = NUMERIC_POS;
						break;

		case '+':		dest->sign = NUMERIC_POS;
						cp++;
						break;

		case '-':		dest->sign = NUMERIC_NEG;
						cp++;
						break;

		case '.':		dest->sign = NUMERIC_POS;
						have_dp = TRUE;
						cp++;
						break;

		default:		free_allvars();
						elog(ERROR, "Bad numeric input format '%s'", str);
	}

	if (*cp == '.')
	{
		if (have_dp)
		{
			free_allvars();
			elog(ERROR, "Bad numeric input format '%s'", str);
		}

		have_dp = TRUE;
		cp++;
	}

	if (*cp < '0' && *cp > '9')
	{
		free_allvars();
		elog(ERROR, "Bad numeric input format '%s'", str);
	}

	while (*cp)
	{
		if (isspace(*cp))
			break;

		if (*cp == 'e' || *cp == 'E')
			break;

		switch (*cp)
		{
			case '0':
			case '1':
			case '2':
			case '3':
			case '4':
			case '5':
			case '6':
			case '7':
			case '8':
			case '9':	dest->digits[i++] = *cp++ - '0';
						if (!have_dp)
							dest->weight++;
						else
							dest->dscale++;
						break;

			case '.':	if (have_dp)
						{
							free_allvars();
							elog(ERROR, "Bad numeric input format '%s'", str);
						}
						have_dp = TRUE;
						cp++;
						break;

			default:	free_allvars();
						elog(ERROR, "Bad numeric input format '%s'", str);
		}
	}
	dest->ndigits = i;

	if (*cp == 'e' || *cp == 'E')
	{
		/* Handle ...Ennn */
	}

	while (dest->ndigits > 0 && *(dest->digits) == 0)
	{
		(dest->digits)++;
		(dest->weight)--;
		(dest->ndigits)--;
	}

	dest->rscale = dest->dscale;
}


/*
 * set_var_from_num() -
 *
 *	Parse back the packed db format into a variable
 *
 */
static void
set_var_from_num(Numeric num, NumericVar *dest)
{
	NumericDigit		*digit;
	int					i;
	int					n;

	n = num->varlen - NUMERIC_HDRSZ;

	digitbuf_free(dest->buf);
	dest->buf = digitbuf_alloc(n * 2 + 2);

	digit = ((NumericDigit *)(dest->buf)) + sizeof(NumericDigitBuf);
	*digit++ = 0;
	*digit++ = 0;
	dest->digits = digit;
	dest->ndigits = n * 2;

	dest->weight = num->n_weight;
	dest->rscale = num->n_rscale;
	dest->dscale = NUMERIC_DSCALE(num);
	dest->sign   = NUMERIC_SIGN(num);

	for (i = 0; i < n; i++)
	{
		*digit++ = (num->n_data[i] >> 4) & 0x0f;
		*digit++ = num->n_data[i] & 0x0f;
	}
}


/* ----------
 * set_var_from_var() -
 *
 *	Copy one variable into another
 * ----------
 */
static void
set_var_from_var(NumericVar *value, NumericVar *dest)
{
	NumericDigitBuf		*newbuf;
	NumericDigit		*newdigits;

	newbuf = digitbuf_alloc(value->ndigits);
	newdigits = ((NumericDigit *)newbuf) + sizeof(NumericDigitBuf);
	memcpy(newdigits, value->digits, value->ndigits);

	digitbuf_free(dest->buf);
	memcpy(dest, value, sizeof(NumericVar));
	dest->buf    = newbuf;
	dest->digits = newdigits;
}


/* ----------
 * make_result() -
 *
 *	Create the packed db numeric format in palloc()'d memory from
 *	a variable.
 * ----------
 */
static Numeric
make_result(NumericVar *var)
{
	Numeric				result;
	NumericDigit		*digit = var->digits;
	int					n;
	int					weight = var->weight;
	int					sign   = var->sign;
	int					i, j;

	if (sign == NUMERIC_NAN)
	{
		result = (Numeric)palloc(NUMERIC_HDRSZ);

		result->varlen        = NUMERIC_HDRSZ;
		result->n_weight      = 0;
		result->n_rscale      = 0;
		result->n_sign_dscale = NUMERIC_NAN;

		dump_numeric("make_result()", result);
		return result;
	}

	n = MAX(0, MIN(var->ndigits, var->weight + var->rscale + 1));

	while (n > 0 && *digit == 0)
	{
		digit++;
		weight--;
		n--;
	}
	while (n > 0 && digit[n - 1] == 0)
		n--;

	if (n == 0)
	{
		weight = 0;
		sign   = NUMERIC_POS;
	}

	result = (Numeric)palloc(NUMERIC_HDRSZ + (n + 1) / 2);
	result->varlen = NUMERIC_HDRSZ + (n + 1) / 2;
	result->n_weight = weight;
	result->n_rscale = var->rscale;
	result->n_sign_dscale = sign | ((uint16)(var->dscale) & ~NUMERIC_SIGN_MASK);

	i = 0; j = 0;
	while (j < n)
	{
		result->n_data[i] = digit[j++] << 4;
		if (j < n)
			result->n_data[i] |= digit[j++];
		i++;
	}

	dump_numeric("make_result()", result);
	return result;
}


/* ----------
 * apply_typmod() -
 *
 *	Do bounds checking and rounding according to the attributes
 *	typmod field.
 * ----------
 */
static void
apply_typmod(NumericVar *var, int32 typmod)
{
	int			precision;
	int			scale;
	int			maxweight;
	int			i;

	if (typmod < (int32)(VARHDRSZ))
		return;

	typmod -= VARHDRSZ;
	precision = (typmod >> 16) & 0xffff;
	scale     = typmod & 0xffff;
	maxweight = precision - scale;

	if (var->weight >= maxweight)
	{
		free_allvars();
		elog(ERROR, "overflow on numeric
        ABS(value) >= 10^%d for field with precision %d scale %d",
							var->weight, precision, scale);
	}

	i = scale + var->weight + 1;
	if (var->ndigits > i)
	{
		long	carry = (var->digits[i] > 4) ? 1 : 0;

		var->ndigits = i;
		while (carry)
		{
			carry += var->digits[--i];
			var->digits[i] = carry % 10;
			carry /= 10;
		}

		if (i < 0)
		{
			var->digits--;
			var->ndigits++;
			var->weight++;
		}
	}

	var->rscale = scale;
	var->dscale = scale;
}


/* ----------
 * cmp_var() -
 *
 *	Compare two values on variable level
 * ----------
 */
static int
cmp_var(NumericVar *var1, NumericVar *var2)
{
	if (var1->ndigits == 0)
	{
		if (var2->ndigits == 0)
			return 0;
		if (var2->sign == NUMERIC_NEG)
			return 1;
		return -1;
	}
	if (var2->ndigits == 0)
	{
		if (var1->sign == NUMERIC_POS)
			return 1;
		return -1;
	}

	if (var1->sign == NUMERIC_POS)
	{
		if (var2->sign == NUMERIC_NEG)
			return 1;
		return cmp_abs(var1, var2);
	}

	if (var2->sign == NUMERIC_POS)
		return -1;

	return cmp_abs(var2, var1);
}


/* ----------
 * add_var() -
 *
 *	Full version of add functionality on variable level (handling signs).
 *	result might point to one of the operands too without danger.
 * ----------
 */
static void
add_var(NumericVar *var1, NumericVar *var2, NumericVar *result)
{
	/* ----------
	 * Decide on the signs of the two variables what to do
	 * ----------
	 */
	if (var1->sign == NUMERIC_POS)
	{
		if (var2->sign == NUMERIC_POS)
		{
			/* ----------
			 * Both are positive 
			 * result = +(ABS(var1) + ABS(var2))
			 * ----------
			 */
			add_abs(var1, var2, result);
			result->sign = NUMERIC_POS;
		}
		else
		{
			/* ----------
			 * var1 is positive, var2 is negative
			 * Must compare absolute values
			 * ----------
			 */
			switch (cmp_abs(var1, var2))
			{
				case 0:		/* ----------
							 * ABS(var1) == ABS(var2)
							 * result = ZERO
							 * ----------
							 */
							digitbuf_free(result->buf);
							result->buf = digitbuf_alloc(0);
							result->ndigits = 0;
							result->digits = ((NumericDigit *)(result->buf)) +
											sizeof(NumericDigitBuf);
							result->weight = 0;
							result->rscale = MAX(var1->rscale, var2->rscale);
							result->dscale = MAX(var1->dscale, var2->dscale);
							result->sign   = NUMERIC_POS;
							break;

				case 1:		/* ----------
							 * ABS(var1) > ABS(var2)
							 * result = +(ABS(var1) - ABS(var2))
							 * ----------
							 */
							sub_abs(var1, var2, result);
							result->sign = NUMERIC_POS;
							break;

				case -1:	/* ----------
							 * ABS(var1) < ABS(var2)
							 * result = -(ABS(var2) - ABS(var1))
							 * ----------
							 */
							sub_abs(var2, var1, result);
							result->sign = NUMERIC_NEG;
							break;
			}
		}
	}
	else
	{
		if (var2->sign == NUMERIC_POS)
		{
			/* ----------
			 * var1 is negative, var2 is positive
			 * Must compare absolute values
			 * ----------
			 */
			switch (cmp_abs(var1, var2))
			{
				case 0:		/* ----------
							 * ABS(var1) == ABS(var2)
							 * result = ZERO
							 * ----------
							 */
							digitbuf_free(result->buf);
							result->buf = digitbuf_alloc(0);
							result->ndigits = 0;
							result->digits = ((NumericDigit *)(result->buf)) +
											sizeof(NumericDigitBuf);
							result->weight = 0;
							result->rscale = MAX(var1->rscale, var2->rscale);
							result->dscale = MAX(var1->dscale, var2->dscale);
							result->sign   = NUMERIC_POS;
							break;

				case 1:		/* ----------
							 * ABS(var1) > ABS(var2)
							 * result = -(ABS(var1) - ABS(var2))
							 * ----------
							 */
							sub_abs(var1, var2, result);
							result->sign = NUMERIC_NEG;
							break;

				case -1:	/* ----------
							 * ABS(var1) < ABS(var2)
							 * result = +(ABS(var2) - ABS(var1))
							 * ----------
							 */
							sub_abs(var2, var1, result);
							result->sign = NUMERIC_POS;
							break;
			}
		}
		else
		{
			/* ----------
			 * Both are negative
			 * result = -(ABS(var1) + ABS(var2))
			 * ----------
			 */
			add_abs(var1, var2, result);
			result->sign = NUMERIC_NEG;
		}
	}
}


/* ----------
 * sub_var() -
 *
 *	Full version of sub functionality on variable level (handling signs).
 *	result might point to one of the operands too without danger.
 * ----------
 */
static void
sub_var(NumericVar *var1, NumericVar *var2, NumericVar *result)
{
	/* ----------
	 * Decide on the signs of the two variables what to do
	 * ----------
	 */
	if (var1->sign == NUMERIC_POS)
	{
		if (var2->sign == NUMERIC_NEG)
		{
			/* ----------
			 * var1 is positive, var2 is negative
			 * result = +(ABS(var1) + ABS(var2))
			 * ----------
			 */
			add_abs(var1, var2, result);
			result->sign = NUMERIC_POS;
		}
		else
		{
			/* ----------
			 * Both are positive
			 * Must compare absolute values
			 * ----------
			 */
			switch (cmp_abs(var1, var2))
			{
				case 0:		/* ----------
							 * ABS(var1) == ABS(var2)
							 * result = ZERO
							 * ----------
							 */
							digitbuf_free(result->buf);
							result->buf = digitbuf_alloc(0);
							result->ndigits = 0;
							result->digits = ((NumericDigit *)(result->buf)) +
											sizeof(NumericDigitBuf);
							result->weight = 0;
							result->rscale = MAX(var1->rscale, var2->rscale);
							result->dscale = MAX(var1->dscale, var2->dscale);
							result->sign   = NUMERIC_POS;
							break;

				case 1:		/* ----------
							 * ABS(var1) > ABS(var2)
							 * result = +(ABS(var1) - ABS(var2))
							 * ----------
							 */
							sub_abs(var1, var2, result);
							result->sign = NUMERIC_POS;
							break;

				case -1:	/* ----------
							 * ABS(var1) < ABS(var2)
							 * result = -(ABS(var2) - ABS(var1))
							 * ----------
							 */
							sub_abs(var2, var1, result);
							result->sign = NUMERIC_NEG;
							break;
			}
		}
	}
	else
	{
		if (var2->sign == NUMERIC_NEG)
		{
			/* ----------
			 * Both are negative
			 * Must compare absolute values
			 * ----------
			 */
			switch (cmp_abs(var1, var2))
			{
				case 0:		/* ----------
							 * ABS(var1) == ABS(var2)
							 * result = ZERO
							 * ----------
							 */
							digitbuf_free(result->buf);
							result->buf = digitbuf_alloc(0);
							result->ndigits = 0;
							result->digits = ((NumericDigit *)(result->buf)) +
											sizeof(NumericDigitBuf);
							result->weight = 0;
							result->rscale = MAX(var1->rscale, var2->rscale);
							result->dscale = MAX(var1->dscale, var2->dscale);
							result->sign   = NUMERIC_POS;
							break;

				case 1:		/* ----------
							 * ABS(var1) > ABS(var2)
							 * result = -(ABS(var1) - ABS(var2))
							 * ----------
							 */
							sub_abs(var1, var2, result);
							result->sign = NUMERIC_NEG;
							break;

				case -1:	/* ----------
							 * ABS(var1) < ABS(var2)
							 * result = +(ABS(var2) - ABS(var1))
							 * ----------
							 */
							sub_abs(var2, var1, result);
							result->sign = NUMERIC_POS;
							break;
			}
		}
		else
		{
			/* ----------
			 * var1 is negative, var2 is positive
			 * result = -(ABS(var1) + ABS(var2))
			 * ----------
			 */
			add_abs(var1, var2, result);
			result->sign = NUMERIC_NEG;
		}
	}
}


/* ----------
 * mul_var() -
 *
 *	Multiplication on variable level. Product of var1 * var2 is stored
 *	in result.
 * ----------
 */
static void
mul_var(NumericVar *var1, NumericVar *var2, NumericVar *result)
{
	NumericDigitBuf		*res_buf;
	NumericDigit		*res_digits;
	int					res_ndigits;
	int					res_weight;
	int					res_sign;
	int					i, ri, i1, i2;
	long				sum = 0;

	res_weight  = var1->weight + var2->weight + 2;
	res_ndigits = var1->ndigits + var2->ndigits + 1;
	if (var1->sign == var2->sign)
		res_sign = NUMERIC_POS;
	else
		res_sign = NUMERIC_NEG;

	res_buf = digitbuf_alloc(res_ndigits);
	res_digits = ((NumericDigit *)res_buf) + sizeof(NumericDigitBuf);
	memset(res_digits, 0, res_ndigits);

	ri = res_ndigits;
	for (i1 = var1->ndigits - 1; i1 >= 0; i1--)
	{
		sum = 0;
		i = --ri;

		for (i2 = var2->ndigits - 1; i2 >= 0; i2--)
		{
			sum = sum + res_digits[i] + var1->digits[i1] * var2->digits[i2];
			res_digits[i--] = sum % 10;
			sum /= 10;
		}
		res_digits[i] = sum;
	}

	i = res_weight + global_rscale + 2;
	if (i >= 0 && i < res_ndigits)
	{
		sum = (res_digits[i] > 4) ? 1 : 0;
		res_ndigits = i;
		i--;
		while (sum)
		{
			sum += res_digits[i];
			res_digits[i--] = sum % 10;
			sum /= 10;
		}
	}

	while (res_ndigits > 0 && *res_digits == 0)
	{
		res_digits++;
		res_weight--;
		res_ndigits--;
	}
	while (res_ndigits > 0 && res_digits[res_ndigits - 1] == 0)
	{
		res_ndigits--;
	}

	if (res_ndigits == 0)
	{
		res_sign   = NUMERIC_POS;
		res_weight = 0;
	}

	digitbuf_free(result->buf);
	result->buf     = res_buf;
	result->digits  = res_digits;
	result->ndigits = res_ndigits;
	result->weight  = res_weight;
	result->rscale  = global_rscale;
	result->sign    = res_sign;
}


/* ----------
 * div_var() -
 *
 *	Division on variable level.
 * ----------
 */
static void
div_var(NumericVar *var1, NumericVar *var2, NumericVar *result)
{
	NumericDigit	*res_digits;
	int				res_ndigits;
	int				res_sign;
	int				res_weight;
	NumericVar		dividend;
	NumericVar		divisor[10];
	int				ndigits_tmp;
	int				weight_tmp;
	int				rscale_tmp;
	int				ri;
	int				i;
	long			guess;
	long			first_have;
	long			first_div;
	int				first_nextdigit;
	int				stat = 0;

	/* ----------
	 * First of all division by zero check
	 * ----------
	 */
	ndigits_tmp = var2->ndigits + 1;
	if (ndigits_tmp == 1)
	{
		free_allvars();
		elog(ERROR, "division by zero on numeric");
	}

	/* ----------
	 * Determine the result sign, weight and number of digits to calculate
	 * ----------
	 */
	if (var1->sign == var2->sign)
		res_sign = NUMERIC_POS;
	else
		res_sign = NUMERIC_NEG;
	res_weight = var1->weight - var2->weight + 1;
	res_ndigits = global_rscale + res_weight;

	/* ----------
	 * Now result zero check
	 * ----------
	 */
	if (var1->ndigits == 0)
	{
		digitbuf_free(result->buf);
		result->buf = digitbuf_alloc(0);
		result->digits = ((NumericDigit *)(result->buf)) + sizeof(NumericDigitBuf);
		result->ndigits = 0;
		result->weight  = 0;
		result->rscale  = global_rscale;
		result->sign    = NUMERIC_POS;
		return;
	}

	/* ----------
	 * Initialize local variables
	 * ----------
	 */
	init_var(&dividend);
	for (i = 1; i < 10; i++)
	{
		init_var(&divisor[i]);
	}


	/* ----------
	 * Make a copy of the divisor which has one leading zero digit
	 * ----------
	 */
	divisor[1].ndigits   = ndigits_tmp;
	divisor[1].rscale    = var2->ndigits;
	divisor[1].sign      = NUMERIC_POS;
	divisor[1].buf       = digitbuf_alloc(ndigits_tmp);
	divisor[1].digits    = ((NumericDigit *)(divisor[1].buf)) +
									sizeof(NumericDigitBuf);
	divisor[1].digits[0] = 0;
	memcpy(&(divisor[1].digits[1]), var2->digits, ndigits_tmp - 1);

	/* ----------
	 * Make a copy of the dividend
	 * ----------
	 */
	dividend.ndigits = var1->ndigits;
	dividend.weight  = 0;
	dividend.rscale  = var1->ndigits;
	dividend.sign    = NUMERIC_POS;
	dividend.buf     = digitbuf_alloc(var1->ndigits);
	dividend.digits  = ((NumericDigit *)(dividend.buf)) + sizeof(NumericDigitBuf);
	memcpy(dividend.digits, var1->digits, var1->ndigits);

	/* ----------
	 * Setup the result
	 * ----------
	 */
	digitbuf_free(result->buf);
	result->buf = digitbuf_alloc(res_ndigits + 2);
	res_digits = ((NumericDigit *)(result->buf)) + sizeof(NumericDigitBuf);
	result->digits = res_digits;
	result->ndigits = res_ndigits;
	result->weight  = res_weight;
	result->rscale  = global_rscale;
	result->sign    = res_sign;
	res_digits[0] = 0;

	first_div = divisor[1].digits[1] * 10;
	if (ndigits_tmp > 2)
		first_div += divisor[1].digits[2];
	
	first_have = 0;
	first_nextdigit = 0;

	weight_tmp = 1;
	rscale_tmp = divisor[1].rscale;

	for (ri = 0; ri < res_ndigits + 1; ri++)
	{
		first_have = first_have * 10;
		if (first_nextdigit >= 0 && first_nextdigit < dividend.ndigits)
			first_have += dividend.digits[first_nextdigit];
		first_nextdigit++;

		guess = (first_have * 10) / first_div + 1;
		if (guess > 9)
			guess = 9;

		while (guess > 0)
		{
			if (divisor[guess].buf == NULL)
			{
				int		i;
				long	sum = 0;

				memcpy(&divisor[guess], &divisor[1], sizeof(NumericVar));
				divisor[guess].buf = digitbuf_alloc(divisor[guess].ndigits);
				divisor[guess].digits = ((NumericDigit *)(divisor[guess].buf) +
								sizeof(NumericDigitBuf));
				for (i = divisor[1].ndigits - 1; i >= 0; i--)
				{
					sum += divisor[1].digits[i] * guess;
					divisor[guess].digits[i] = sum % 10;
					sum /= 10;
				}
			}

			divisor[guess].weight = weight_tmp;
			divisor[guess].rscale = rscale_tmp;

			stat = cmp_abs(&dividend, &divisor[guess]);
			if (stat >= 0) break;

			guess--;
		}

		res_digits[ri + 1] = guess;
		if (stat == 0)
		{
			ri++;
			break;
		}

		weight_tmp--;
		rscale_tmp++;

		if (guess == 0)
			continue;

		sub_abs(&dividend, &divisor[guess], &dividend);

		first_nextdigit = dividend.weight - weight_tmp;
		first_have = 0;
		if (first_nextdigit >= 0 && first_nextdigit < dividend.ndigits)
			first_have = dividend.digits[first_nextdigit];
		first_nextdigit++;
	}

	result->ndigits = ri + 1;
	if (ri == res_ndigits + 1)
	{
		long	carry = (res_digits[ri] > 4) ? 1 : 0;

		result->ndigits = ri;
		res_digits[ri] = 0;

		while(carry && ri > 0)
		{
			carry += res_digits[--ri];
			res_digits[ri] = carry % 10;
			carry /= 10;
		}
	}

	while (result->ndigits > 0 && *(result->digits) == 0)
	{
		(result->digits)++;
		(result->weight)--;
		(result->ndigits)--;
	}
	while (result->ndigits > 0 && result->digits[result->ndigits - 1] == 0)
	{
		(result->ndigits)--;
	}
	if (result->ndigits == 0)
		result->sign = NUMERIC_POS;

	/*
	 * Tidy up
	 *
	 */
	digitbuf_free(dividend.buf);
	for (i = 1; i < 10; i++)
		digitbuf_free(divisor[i].buf);
}


/* ----------
 * mod_var() -
 *
 *	Calculate the modulo of two numerics at variable level
 * ----------
 */
static void
mod_var(NumericVar *var1, NumericVar *var2, NumericVar *result)
{
	NumericVar		tmp;
	int				save_global_rscale;

	init_var(&tmp);

	/* ----------
	 * We do it by fiddling around with global_rscale and truncating
	 * the result of the division.
	 * ----------
	 */
	save_global_rscale = global_rscale;
	global_rscale = var2->rscale + 2;

	div_var(var1, var2, &tmp);
	tmp.rscale = var2->rscale;
	tmp.ndigits = MAX(0, MIN(tmp.ndigits, tmp.weight + tmp.rscale + 1));

	global_rscale = var2->rscale;
	mul_var(var2, &tmp, &tmp);

	sub_var(var1, &tmp, result);

	global_rscale = save_global_rscale;
	free_var(&tmp);
}


/* ----------
 * ceil_var() -
 *
 *	Return the smallest integer greater than or equal to the argument
 *	on variable level
 * ----------
 */
static void
ceil_var(NumericVar *var, NumericVar *result)
{
	NumericVar		tmp;

	init_var(&tmp);
	set_var_from_var(var, &tmp);

	tmp.rscale = 0;
	tmp.ndigits = MAX(0, tmp.weight + 1);
	if (tmp.sign == NUMERIC_POS && cmp_var(var, &tmp) != 0)
		add_var(&tmp, &const_one, &tmp);

	set_var_from_var(&tmp, result);
	free_var(&tmp);
}


/* ----------
 * floor_var() -
 *
 *	Return the largest integer equal to or less than the argument
 *	on variable level
 * ----------
 */
static void
floor_var(NumericVar *var, NumericVar *result)
{
	NumericVar		tmp;

	init_var(&tmp);
	set_var_from_var(var, &tmp);

	tmp.rscale = 0;
	tmp.ndigits = MAX(0, tmp.weight + 1);
	if (tmp.sign == NUMERIC_NEG && cmp_var(var, &tmp) != 0)
		sub_var(&tmp, &const_one, &tmp);

	set_var_from_var(&tmp, result);
	free_var(&tmp);
}


/* ----------
 * sqrt_var() -
 *
 *	Compute the square root of x using Newtons algorithm
 * ----------
 */
static void
sqrt_var(NumericVar *arg, NumericVar *result)
{
	NumericVar		tmp_arg;
	NumericVar		tmp_val;
	NumericVar		last_val;
	int				res_rscale;
	int				save_global_rscale;
	int				stat;

	save_global_rscale = global_rscale;
	global_rscale += 8;
	res_rscale = global_rscale;

	stat = cmp_var(arg, &const_zero);
	if (stat == 0)
	{
		set_var_from_var(&const_zero, result);
		result->rscale = res_rscale;
		result->sign   = NUMERIC_POS;
		return;
	}

	if (stat < 0)
	{
		free_allvars();
		elog(ERROR, "math error on numeric - cannot compute SQRT of negative value");
	}

	init_var(&tmp_arg);
	init_var(&tmp_val);
	init_var(&last_val);

	set_var_from_var(arg, &tmp_arg);
	set_var_from_var(result, &last_val);

	/* ----------
	 * Initialize the result to the first guess
	 * ----------
	 */
	digitbuf_free(result->buf);
	result->buf = digitbuf_alloc(1);
	result->digits = ((NumericDigit *)(result->buf)) + sizeof(NumericDigitBuf);
	result->digits[0] = tmp_arg.digits[0] / 2;
	if (result->digits[0] == 0)
		result->digits[0] = 1;
	result->ndigits = 1;
	result->weight = tmp_arg.weight / 2;
	result->rscale = res_rscale;
	result->sign   = NUMERIC_POS;

	for (;;)
	{
		div_var(&tmp_arg, result, &tmp_val);

		add_var(result, &tmp_val, result);
		div_var(result, &const_two, result);

		if (cmp_var(&last_val, result) == 0) break;
		set_var_from_var(result, &last_val);
	}

	free_var(&last_val);
	free_var(&tmp_val);
	free_var(&tmp_arg);

	global_rscale = save_global_rscale;
	div_var(result, &const_one, result);
}


/* ----------
 * exp_var() -
 *
 *	Raise e to the power of x
 * ----------
 */
static void
exp_var(NumericVar *arg, NumericVar *result)
{
	NumericVar		x;
	NumericVar		xpow;
	NumericVar		ifac;
	NumericVar		elem;
	NumericVar		ni;
	int				d;
	int				i;
	int				ndiv2 = 0;
	bool			xneg = FALSE;
	int				save_global_rscale;

	init_var(&x);
	init_var(&xpow);
	init_var(&ifac);
	init_var(&elem);
	init_var(&ni);

	set_var_from_var(arg, &x);

	if (x.sign == NUMERIC_NEG)
	{
		xneg = TRUE;
		x.sign = NUMERIC_POS;
	}

	save_global_rscale = global_rscale;
	global_rscale = 0;
	for (i = x.weight, d = 0; i >= 0; i--, d++)
	{
		global_rscale *= 10;
		if (d < x.ndigits)
			global_rscale += x.digits[d];
		if (global_rscale >= 1000)
		{
			free_allvars();
			elog(ERROR, "argument for EXP() too big");
		}
	}

	global_rscale = global_rscale / 2 + save_global_rscale + 8;

	while(cmp_var(&x, &const_one) > 0)
	{
		ndiv2++;
		global_rscale++;
		div_var(&x, &const_two, &x);
	}

	add_var(&const_one, &x, result);
	set_var_from_var(&x, &xpow);
	set_var_from_var(&const_one, &ifac);
	set_var_from_var(&const_one, &ni);

	for (i = 2; TRUE; i++)
	{
		add_var(&ni, &const_one, &ni);
		mul_var(&xpow, &x, &xpow);
		mul_var(&ifac, &ni, &ifac);
		div_var(&xpow, &ifac, &elem);

		if (elem.ndigits == 0)
			break;

		add_var(result, &elem, result);
	}

	while (ndiv2-- > 0)
		mul_var(result, result, result);

	global_rscale = save_global_rscale;
	if (xneg)
		div_var(&const_one, result, result);
	else
		div_var(result, &const_one, result);

	result->sign = NUMERIC_POS;

	free_var(&x);
	free_var(&xpow);
	free_var(&ifac);
	free_var(&elem);
	free_var(&ni);
}


/* ----------
 * ln_var() -
 *
 *	Compute the natural log of x
 * ----------
 */
static void
ln_var(NumericVar *arg, NumericVar *result)
{
	NumericVar		x;
	NumericVar		xx;
	NumericVar		ni;
	NumericVar		elem;
	NumericVar		fact;
	int				i;
	int				save_global_rscale;

	if (cmp_var(arg, &const_zero) <= 0)
	{
		free_allvars();
		elog(ERROR, "math error on numeric - cannot compute LN of value <= zero");
	}

	save_global_rscale = global_rscale;
	global_rscale += 8;

	init_var(&x);
	init_var(&xx);
	init_var(&ni);
	init_var(&elem);
	init_var(&fact);

	set_var_from_var(&const_two, &fact);
	set_var_from_var(arg, &x);

	while (cmp_var(&x, &const_two) >= 0)
	{
		sqrt_var(&x, &x);
		mul_var(&fact, &const_two, &fact);
	}
	set_var_from_str("0.5", &elem);
	while (cmp_var(&x, &elem) <= 0)
	{
		sqrt_var(&x, &x);
		mul_var(&fact, &const_two, &fact);
	}

	sub_var(&x, &const_one, result);
	add_var(&x, &const_one, &elem);
	div_var(result, &elem, result);
	set_var_from_var(result, &xx);
	mul_var(result, result, &x);

	set_var_from_var(&const_one, &ni);

	for (i = 2; TRUE; i++)
	{
		add_var(&ni, &const_two, &ni);
		mul_var(&xx, &x, &xx);
		div_var(&xx, &ni, &elem);

		if (cmp_var(&elem, &const_zero) == 0)
			break;

		add_var(result, &elem, result);
	}

	global_rscale = save_global_rscale;
	mul_var(result, &fact, result);

	free_var(&x);
	free_var(&xx);
	free_var(&ni);
	free_var(&elem);
	free_var(&fact);
}


/* ----------
 * log_var() -
 *
 *	Compute the logarithm of x in a given base
 * ----------
 */
static void
log_var(NumericVar *base, NumericVar *num, NumericVar *result)
{
	NumericVar	ln_base;
	NumericVar	ln_num;

	global_rscale += 8;

	init_var(&ln_base);
	init_var(&ln_num);

	ln_var(base, &ln_base);
	ln_var(num,  &ln_num);

	global_rscale -= 8;
	
	div_var(&ln_num, &ln_base, result);

	free_var(&ln_num);
	free_var(&ln_base);
}


/* ----------
 * power_var() -
 *
 *	Raise base to the power of exp
 * ----------
 */
static void
power_var(NumericVar *base, NumericVar *exp, NumericVar *result)
{
	NumericVar	ln_base;
	NumericVar	ln_num;
	int			save_global_rscale;

	save_global_rscale = global_rscale;
	global_rscale += global_rscale / 3 + 8;

	init_var(&ln_base);
	init_var(&ln_num);

	ln_var(base, &ln_base);
	mul_var(&ln_base, exp, &ln_num);

	global_rscale = save_global_rscale;

	exp_var(&ln_num, result);
	
	free_var(&ln_num);
	free_var(&ln_base);

}


/* ----------------------------------------------------------------------
 *
 * Following are the lowest level functions that operate unsigned
 * on the variable level
 *
 * ----------------------------------------------------------------------
 */


/* ----------
 * cmp_abs() -
 *
 *	Compare the absolute values of var1 and var2
 *	Returns:	-1 for ABS(var1) < ABS(var2)
 *				0  for ABS(var1) == ABS(var2)
 *				1  for ABS(var1) > ABS(var2)
 * ----------
 */
static int
cmp_abs(NumericVar *var1, NumericVar *var2)
{
	int		i1 = 0;
	int		i2 = 0;
	int		w1 = var1->weight;
	int		w2 = var2->weight;
	int		stat;

	while (w1 > w2)
	{
		if (var1->digits[i1++] != 0) return 1;
		w1--;
	}
	while (w2 > w1)
	{
		if (var2->digits[i2++] != 0) return -1;
		w2--;
	}

	while (i1 < var1->ndigits && i2 < var2->ndigits)
	{
		stat = var1->digits[i1++] - var2->digits[i2++];
		if (stat)
		{
			if (stat > 0)
				return 1;
			return -1;
		}
	}

	while (i1 < var1->ndigits)
	{
		if (var1->digits[i1++] != 0)
			return 1;
	}
	while (i2 < var2->ndigits)
	{
		if (var2->digits[i2++] != 0)
			return -1;
	}

	return 0;
}


/* ----------
 * add_abs() -
 *
 *	Add the absolute values of two variables into result.
 *	result might point to one of the operands without danger.
 * ----------
 */
static void
add_abs(NumericVar *var1, NumericVar *var2, NumericVar *result)
{
	NumericDigitBuf		*res_buf;
	NumericDigit		*res_digits;
	int					res_ndigits;
	int					res_weight;
	int					res_rscale;
	int					res_dscale;
	int					i, i1, i2;
	int					carry = 0;

	res_weight = MAX(var1->weight, var2->weight) + 1;
	res_rscale = MAX(var1->rscale, var2->rscale);
	res_dscale = MAX(var1->dscale, var2->dscale);
	res_ndigits = res_rscale + res_weight + 1;

	res_buf = digitbuf_alloc(res_ndigits);
	res_digits = ((NumericDigit *)res_buf) + sizeof(NumericDigitBuf);

	i1 = res_rscale + var1->weight + 1;
	i2 = res_rscale + var2->weight + 1;
	for (i = res_ndigits - 1; i >= 0; i--)
	{
		i1--;
		i2--;
		if (i1 >= 0 && i1 < var1->ndigits)
			carry += var1->digits[i1];
		if (i2 >= 0 && i2 < var2->ndigits)
			carry += var2->digits[i2];

		res_digits[i] = carry % 10;
		carry /= 10;
	}

	while (res_ndigits > 0 && *res_digits == 0)
	{
		res_digits++;
		res_weight--;
		res_ndigits--;
	}
	while (res_ndigits > 0 && res_digits[res_ndigits - 1] == 0)
	{
		res_ndigits--;
	}

	if (res_ndigits == 0)
		res_weight = 0;

	digitbuf_free(result->buf);
	result->ndigits = res_ndigits;
	result->buf     = res_buf;
	result->digits  = res_digits;
	result->weight  = res_weight;
	result->rscale  = res_rscale;
	result->dscale  = res_dscale;
}


/* ----------
 * sub_abs() -
 *
 *	Subtract the absolute value of var2 from the absolute value of var1
 *	and store in result. result might point to one of the operands
 *	without danger.
 *
 *	ABS(var1) MUST BE GREATER OR EQUAL ABS(var2) !!!
 * ----------
 */
static void
sub_abs(NumericVar *var1, NumericVar *var2, NumericVar *result)
{
	NumericDigitBuf		*res_buf;
	NumericDigit		*res_digits;
	int					res_ndigits;
	int					res_weight;
	int					res_rscale;
	int					res_dscale;
	int					i, i1, i2;
	int					borrow = 0;

	res_weight = var1->weight;
	res_rscale = MAX(var1->rscale, var2->rscale);
	res_dscale = MAX(var1->dscale, var2->dscale);
	res_ndigits = res_rscale + res_weight + 1;

	res_buf = digitbuf_alloc(res_ndigits);
	res_digits = ((NumericDigit *)res_buf) + sizeof(NumericDigitBuf);

	i1 = res_rscale + var1->weight + 1;
	i2 = res_rscale + var2->weight + 1;
	for (i = res_ndigits - 1; i >= 0; i--)
	{
		i1--;
		i2--;
		if (i1 >= 0 && i1 < var1->ndigits)
			borrow += var1->digits[i1];
		if (i2 >= 0 && i2 < var2->ndigits)
			borrow -= var2->digits[i2];

		if (borrow < 0)
		{
			res_digits[i] = borrow + 10;
			borrow = -1;
		}
		else
		{
			res_digits[i] = borrow;
			borrow = 0;
		}
	}

	while (res_ndigits > 0 && *res_digits == 0)
	{
		res_digits++;
		res_weight--;
		res_ndigits--;
	}
	while (res_ndigits > 0 && res_digits[res_ndigits - 1] == 0)
	{
		res_ndigits--;
	}

	if (res_ndigits == 0)
		res_weight = 0;

	digitbuf_free(result->buf);
	result->ndigits = res_ndigits;
	result->buf     = res_buf;
	result->digits  = res_digits;
	result->weight  = res_weight;
	result->rscale  = res_rscale;
	result->dscale  = res_dscale;
}


