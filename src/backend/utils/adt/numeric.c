/* ----------
 * numeric.c
 *
 *	An exact numeric data type for the Postgres database system
 *
 *	1998 Jan Wieck
 *
 * $Header: /cvsroot/pgsql/src/backend/utils/adt/numeric.c,v 1.57 2003/03/11 21:01:33 tgl Exp $
 *
 * ----------
 */

#include "postgres.h"

#include <ctype.h>
#include <float.h>
#include <math.h>
#include <errno.h>

#include "catalog/pg_type.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/int8.h"
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
#ifndef NAN
#define NAN		(0.0/0.0)
#endif


/* ----------
 * Local data types
 *
 * Note: the first digit of a NumericVar's value is assumed to be multiplied
 * by 10 ** weight.  Another way to say it is that there are weight+1 digits
 * before the decimal point.  It is possible to have weight < 0.
 *
 * The value represented by a NumericVar is determined by the sign, weight,
 * ndigits, and digits[] array.  The rscale and dscale are carried along,
 * but they are just auxiliary information until rounding is done before
 * final storage or display.  (Scales are the number of digits wanted
 * *after* the decimal point.  Scales are always >= 0.)
 *
 * buf points at the physical start of the palloc'd digit buffer for the
 * NumericVar.	digits points at the first digit in actual use (the one
 * with the specified weight).	We normally leave an unused byte or two
 * (preset to zeroes) between buf and digits, so that there is room to store
 * a carry out of the top digit without special pushups.  We just need to
 * decrement digits (and increment weight) to make room for the carry digit.
 *
 * If buf is NULL then the digit buffer isn't actually palloc'd and should
 * not be freed --- see the constants below for an example.
 *
 * NB: All the variable-level functions are written in a style that makes it
 * possible to give one and the same variable as argument and destination.
 * This is feasible because the digit buffer is separate from the variable.
 * ----------
 */
typedef unsigned char NumericDigit;

typedef struct NumericVar
{
	int			ndigits;		/* number of digits in digits[] - can be
								 * 0! */
	int			weight;			/* weight of first digit */
	int			rscale;			/* result scale */
	int			dscale;			/* display scale */
	int			sign;			/* NUMERIC_POS, NUMERIC_NEG, or
								 * NUMERIC_NAN */
	NumericDigit *buf;			/* start of palloc'd space for digits[] */
	NumericDigit *digits;		/* decimal digits */
} NumericVar;


/* ----------
 * Local data
 * ----------
 */
static int	global_rscale = 0;

/* ----------
 * Some preinitialized variables we need often
 * ----------
 */
static NumericDigit const_zero_data[1] = {0};
static NumericVar const_zero =
{0, 0, 0, 0, NUMERIC_POS, NULL, const_zero_data};

static NumericDigit const_one_data[1] = {1};
static NumericVar const_one =
{1, 0, 0, 0, NUMERIC_POS, NULL, const_one_data};

static NumericDigit const_two_data[1] = {2};
static NumericVar const_two =
{1, 0, 0, 0, NUMERIC_POS, NULL, const_two_data};

static NumericDigit const_zero_point_one_data[1] = {1};
static NumericVar const_zero_point_one =
{1, -1, 1, 1, NUMERIC_POS, NULL, const_zero_point_one_data};

static NumericDigit const_zero_point_nine_data[1] = {9};
static NumericVar const_zero_point_nine =
{1, -1, 1, 1, NUMERIC_POS, NULL, const_zero_point_nine_data};

static NumericDigit const_one_point_one_data[2] = {1, 1};
static NumericVar const_one_point_one =
{2, 0, 1, 1, NUMERIC_POS, NULL, const_one_point_one_data};

static NumericVar const_nan =
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

#define digitbuf_alloc(size)  ((NumericDigit *) palloc(size))
#define digitbuf_free(buf)	\
	do { \
		 if ((buf) != NULL) \
			 pfree(buf); \
	} while (0)

#define init_var(v)		memset(v,0,sizeof(NumericVar))
static void alloc_var(NumericVar *var, int ndigits);
static void free_var(NumericVar *var);
static void zero_var(NumericVar *var);

static void set_var_from_str(char *str, NumericVar *dest);
static void set_var_from_num(Numeric value, NumericVar *dest);
static void set_var_from_var(NumericVar *value, NumericVar *dest);
static char *get_str_from_var(NumericVar *var, int dscale);

static Numeric make_result(NumericVar *var);

static void apply_typmod(NumericVar *var, int32 typmod);

static double numeric_to_double_no_overflow(Numeric num);
static double numericvar_to_double_no_overflow(NumericVar *var);

static int	cmp_numerics(Numeric num1, Numeric num2);
static int	cmp_var(NumericVar *var1, NumericVar *var2);
static void add_var(NumericVar *var1, NumericVar *var2, NumericVar *result);
static void sub_var(NumericVar *var1, NumericVar *var2, NumericVar *result);
static void mul_var(NumericVar *var1, NumericVar *var2, NumericVar *result);
static void div_var(NumericVar *var1, NumericVar *var2, NumericVar *result);
static int	select_div_scale(NumericVar *var1, NumericVar *var2);
static void mod_var(NumericVar *var1, NumericVar *var2, NumericVar *result);
static void ceil_var(NumericVar *var, NumericVar *result);
static void floor_var(NumericVar *var, NumericVar *result);

static void sqrt_var(NumericVar *arg, NumericVar *result);
static void exp_var(NumericVar *arg, NumericVar *result);
static void ln_var(NumericVar *arg, NumericVar *result);
static void log_var(NumericVar *base, NumericVar *num, NumericVar *result);
static void power_var(NumericVar *base, NumericVar *exp, NumericVar *result);

static int	cmp_abs(NumericVar *var1, NumericVar *var2);
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
Datum
numeric_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);

#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		typmod = PG_GETARG_INT32(2);
	NumericVar	value;
	Numeric		res;

	/*
	 * Check for NaN
	 */
	if (strcmp(str, "NaN") == 0)
		PG_RETURN_NUMERIC(make_result(&const_nan));

	/*
	 * Use set_var_from_str() to parse the input string and return it in
	 * the packed DB storage format
	 */
	init_var(&value);
	set_var_from_str(str, &value);

	apply_typmod(&value, typmod);

	res = make_result(&value);
	free_var(&value);

	PG_RETURN_NUMERIC(res);
}


/* ----------
 * numeric_out() -
 *
 *	Output function for numeric data type
 * ----------
 */
Datum
numeric_out(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	NumericVar	x;
	char	   *str;

	/*
	 * Handle NaN
	 */
	if (NUMERIC_IS_NAN(num))
		PG_RETURN_CSTRING(pstrdup("NaN"));

	/*
	 * Get the number in the variable format.
	 *
	 * Even if we didn't need to change format, we'd still need to copy the
	 * value to have a modifiable copy for rounding.  set_var_from_num()
	 * also guarantees there is extra digit space in case we produce a
	 * carry out from rounding.
	 */
	init_var(&x);
	set_var_from_num(num, &x);

	str = get_str_from_var(&x, x.dscale);

	free_var(&x);

	PG_RETURN_CSTRING(str);
}


/* ----------
 * numeric() -
 *
 *	This is a special function called by the Postgres database system
 *	before a value is stored in a tuples attribute. The precision and
 *	scale of the attribute have to be applied on the value.
 * ----------
 */
Datum
numeric(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	int32		typmod = PG_GETARG_INT32(1);
	Numeric		new;
	int32		tmp_typmod;
	int			precision;
	int			scale;
	int			maxweight;
	NumericVar	var;

	/*
	 * Handle NaN
	 */
	if (NUMERIC_IS_NAN(num))
		PG_RETURN_NUMERIC(make_result(&const_nan));

	/*
	 * If the value isn't a valid type modifier, simply return a copy of
	 * the input value
	 */
	if (typmod < (int32) (VARHDRSZ))
	{
		new = (Numeric) palloc(num->varlen);
		memcpy(new, num, num->varlen);
		PG_RETURN_NUMERIC(new);
	}

	/*
	 * Get the precision and scale out of the typmod value
	 */
	tmp_typmod = typmod - VARHDRSZ;
	precision = (tmp_typmod >> 16) & 0xffff;
	scale = tmp_typmod & 0xffff;
	maxweight = precision - scale;

	/*
	 * If the number is in bounds and due to the present result scale no
	 * rounding could be necessary, just make a copy of the input and
	 * modify its scale fields.
	 */
	if (num->n_weight < maxweight && scale >= num->n_rscale)
	{
		new = (Numeric) palloc(num->varlen);
		memcpy(new, num, num->varlen);
		new->n_rscale = scale;
		new->n_sign_dscale = NUMERIC_SIGN(new) |
			((uint16) scale & NUMERIC_DSCALE_MASK);
		PG_RETURN_NUMERIC(new);
	}

	/*
	 * We really need to fiddle with things - unpack the number into a
	 * variable and let apply_typmod() do it.
	 */
	init_var(&var);

	set_var_from_num(num, &var);
	apply_typmod(&var, typmod);
	new = make_result(&var);

	free_var(&var);

	PG_RETURN_NUMERIC(new);
}


/* ----------------------------------------------------------------------
 *
 * Sign manipulation, rounding and the like
 *
 * ----------------------------------------------------------------------
 */

Datum
numeric_abs(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	Numeric		res;

	/*
	 * Handle NaN
	 */
	if (NUMERIC_IS_NAN(num))
		PG_RETURN_NUMERIC(make_result(&const_nan));

	/*
	 * Do it the easy way directly on the packed format
	 */
	res = (Numeric) palloc(num->varlen);
	memcpy(res, num, num->varlen);

	res->n_sign_dscale = NUMERIC_POS | NUMERIC_DSCALE(num);

	PG_RETURN_NUMERIC(res);
}


Datum
numeric_uminus(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	Numeric		res;

	/*
	 * Handle NaN
	 */
	if (NUMERIC_IS_NAN(num))
		PG_RETURN_NUMERIC(make_result(&const_nan));

	/*
	 * Do it the easy way directly on the packed format
	 */
	res = (Numeric) palloc(num->varlen);
	memcpy(res, num, num->varlen);

	/*
	 * The packed format is known to be totally zero digit trimmed always.
	 * So we can identify a ZERO by the fact that there are no digits at
	 * all.  Do nothing to a zero.
	 */
	if (num->varlen != NUMERIC_HDRSZ)
	{
		/* Else, flip the sign */
		if (NUMERIC_SIGN(num) == NUMERIC_POS)
			res->n_sign_dscale = NUMERIC_NEG | NUMERIC_DSCALE(num);
		else
			res->n_sign_dscale = NUMERIC_POS | NUMERIC_DSCALE(num);
	}

	PG_RETURN_NUMERIC(res);
}


Datum
numeric_uplus(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	Numeric		res;

	res = (Numeric) palloc(num->varlen);
	memcpy(res, num, num->varlen);

	PG_RETURN_NUMERIC(res);
}

/* ----------
 * numeric_sign() -
 *
 * returns -1 if the argument is less than 0, 0 if the argument is equal
 * to 0, and 1 if the argument is greater than zero.
 * ----------
 */
Datum
numeric_sign(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	Numeric		res;
	NumericVar	result;

	/*
	 * Handle NaN
	 */
	if (NUMERIC_IS_NAN(num))
		PG_RETURN_NUMERIC(make_result(&const_nan));

	init_var(&result);

	/*
	 * The packed format is known to be totally zero digit trimmed always.
	 * So we can identify a ZERO by the fact that there are no digits at
	 * all.
	 */
	if (num->varlen == NUMERIC_HDRSZ)
		set_var_from_var(&const_zero, &result);
	else
	{
		/*
		 * And if there are some, we return a copy of ONE with the sign of
		 * our argument
		 */
		set_var_from_var(&const_one, &result);
		result.sign = NUMERIC_SIGN(num);
	}

	res = make_result(&result);
	free_var(&result);

	PG_RETURN_NUMERIC(res);
}


/* ----------
 * numeric_round() -
 *
 *	Round a value to have 'scale' digits after the decimal point.
 *	We allow negative 'scale', implying rounding before the decimal
 *	point --- Oracle interprets rounding that way.
 * ----------
 */
Datum
numeric_round(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	int32		scale = PG_GETARG_INT32(1);
	Numeric		res;
	NumericVar	arg;
	int			i;

	/*
	 * Handle NaN
	 */
	if (NUMERIC_IS_NAN(num))
		PG_RETURN_NUMERIC(make_result(&const_nan));

	/*
	 * Limit the scale value to avoid possible overflow in calculations
	 * below.
	 */
	scale = Max(scale, -NUMERIC_MAX_RESULT_SCALE);
	scale = Min(scale, NUMERIC_MAX_RESULT_SCALE);

	/*
	 * Unpack the argument and round it at the proper digit position
	 */
	init_var(&arg);
	set_var_from_num(num, &arg);

	i = arg.weight + scale + 1;

	if (i < arg.ndigits)
	{
		/*
		 * If i = 0, the value loses all digits, but could round up if its
		 * first digit is more than 4.	If i < 0 the result must be 0.
		 */
		if (i < 0)
			arg.ndigits = 0;
		else
		{
			int			carry = (arg.digits[i] > 4) ? 1 : 0;

			arg.ndigits = i;

			while (carry)
			{
				carry += arg.digits[--i];
				arg.digits[i] = carry % 10;
				carry /= 10;
			}

			if (i < 0)
			{
				Assert(i == -1);	/* better not have added more than 1 digit */
				Assert(arg.digits > arg.buf);
				arg.digits--;
				arg.ndigits++;
				arg.weight++;
			}
		}
	}

	/*
	 * Set result's scale to something reasonable.
	 */
	scale = Min(NUMERIC_MAX_DISPLAY_SCALE, Max(0, scale));
	arg.rscale = scale;
	arg.dscale = scale;

	/*
	 * Return the rounded result
	 */
	res = make_result(&arg);

	free_var(&arg);
	PG_RETURN_NUMERIC(res);
}


/* ----------
 * numeric_trunc() -
 *
 *	Truncate a value to have 'scale' digits after the decimal point.
 *	We allow negative 'scale', implying a truncation before the decimal
 *	point --- Oracle interprets truncation that way.
 * ----------
 */
Datum
numeric_trunc(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	int32		scale = PG_GETARG_INT32(1);
	Numeric		res;
	NumericVar	arg;

	/*
	 * Handle NaN
	 */
	if (NUMERIC_IS_NAN(num))
		PG_RETURN_NUMERIC(make_result(&const_nan));

	/*
	 * Limit the scale value to avoid possible overflow in calculations
	 * below.
	 */
	scale = Max(scale, -NUMERIC_MAX_RESULT_SCALE);
	scale = Min(scale, NUMERIC_MAX_RESULT_SCALE);

	/*
	 * Unpack the argument and truncate it at the proper digit position
	 */
	init_var(&arg);
	set_var_from_num(num, &arg);

	arg.ndigits = Min(arg.ndigits, Max(0, arg.weight + scale + 1));

	/*
	 * Set result's scale to something reasonable.
	 */
	scale = Min(NUMERIC_MAX_DISPLAY_SCALE, Max(0, scale));
	arg.rscale = scale;
	arg.dscale = scale;

	/*
	 * Return the truncated result
	 */
	res = make_result(&arg);

	free_var(&arg);
	PG_RETURN_NUMERIC(res);
}


/* ----------
 * numeric_ceil() -
 *
 *	Return the smallest integer greater than or equal to the argument
 * ----------
 */
Datum
numeric_ceil(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	Numeric		res;
	NumericVar	result;

	if (NUMERIC_IS_NAN(num))
		PG_RETURN_NUMERIC(make_result(&const_nan));

	init_var(&result);

	set_var_from_num(num, &result);
	ceil_var(&result, &result);

	result.dscale = 0;

	res = make_result(&result);
	free_var(&result);

	PG_RETURN_NUMERIC(res);
}


/* ----------
 * numeric_floor() -
 *
 *	Return the largest integer equal to or less than the argument
 * ----------
 */
Datum
numeric_floor(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	Numeric		res;
	NumericVar	result;

	if (NUMERIC_IS_NAN(num))
		PG_RETURN_NUMERIC(make_result(&const_nan));

	init_var(&result);

	set_var_from_num(num, &result);
	floor_var(&result, &result);

	result.dscale = 0;

	res = make_result(&result);
	free_var(&result);

	PG_RETURN_NUMERIC(res);
}


/* ----------------------------------------------------------------------
 *
 * Comparison functions
 *
 * Note: btree indexes need these routines not to leak memory; therefore,
 * be careful to free working copies of toasted datums.  Most places don't
 * need to be so careful.
 * ----------------------------------------------------------------------
 */


Datum
numeric_cmp(PG_FUNCTION_ARGS)
{
	Numeric		num1 = PG_GETARG_NUMERIC(0);
	Numeric		num2 = PG_GETARG_NUMERIC(1);
	int			result;

	result = cmp_numerics(num1, num2);

	PG_FREE_IF_COPY(num1, 0);
	PG_FREE_IF_COPY(num2, 1);

	PG_RETURN_INT32(result);
}


Datum
numeric_eq(PG_FUNCTION_ARGS)
{
	Numeric		num1 = PG_GETARG_NUMERIC(0);
	Numeric		num2 = PG_GETARG_NUMERIC(1);
	bool		result;

	result = cmp_numerics(num1, num2) == 0;

	PG_FREE_IF_COPY(num1, 0);
	PG_FREE_IF_COPY(num2, 1);

	PG_RETURN_BOOL(result);
}

Datum
numeric_ne(PG_FUNCTION_ARGS)
{
	Numeric		num1 = PG_GETARG_NUMERIC(0);
	Numeric		num2 = PG_GETARG_NUMERIC(1);
	bool		result;

	result = cmp_numerics(num1, num2) != 0;

	PG_FREE_IF_COPY(num1, 0);
	PG_FREE_IF_COPY(num2, 1);

	PG_RETURN_BOOL(result);
}

Datum
numeric_gt(PG_FUNCTION_ARGS)
{
	Numeric		num1 = PG_GETARG_NUMERIC(0);
	Numeric		num2 = PG_GETARG_NUMERIC(1);
	bool		result;

	result = cmp_numerics(num1, num2) > 0;

	PG_FREE_IF_COPY(num1, 0);
	PG_FREE_IF_COPY(num2, 1);

	PG_RETURN_BOOL(result);
}

Datum
numeric_ge(PG_FUNCTION_ARGS)
{
	Numeric		num1 = PG_GETARG_NUMERIC(0);
	Numeric		num2 = PG_GETARG_NUMERIC(1);
	bool		result;

	result = cmp_numerics(num1, num2) >= 0;

	PG_FREE_IF_COPY(num1, 0);
	PG_FREE_IF_COPY(num2, 1);

	PG_RETURN_BOOL(result);
}

Datum
numeric_lt(PG_FUNCTION_ARGS)
{
	Numeric		num1 = PG_GETARG_NUMERIC(0);
	Numeric		num2 = PG_GETARG_NUMERIC(1);
	bool		result;

	result = cmp_numerics(num1, num2) < 0;

	PG_FREE_IF_COPY(num1, 0);
	PG_FREE_IF_COPY(num2, 1);

	PG_RETURN_BOOL(result);
}

Datum
numeric_le(PG_FUNCTION_ARGS)
{
	Numeric		num1 = PG_GETARG_NUMERIC(0);
	Numeric		num2 = PG_GETARG_NUMERIC(1);
	bool		result;

	result = cmp_numerics(num1, num2) <= 0;

	PG_FREE_IF_COPY(num1, 0);
	PG_FREE_IF_COPY(num2, 1);

	PG_RETURN_BOOL(result);
}

static int
cmp_numerics(Numeric num1, Numeric num2)
{
	int			result;

	/*
	 * We consider all NANs to be equal and larger than any non-NAN. This
	 * is somewhat arbitrary; the important thing is to have a consistent
	 * sort order.
	 */
	if (NUMERIC_IS_NAN(num1))
	{
		if (NUMERIC_IS_NAN(num2))
			result = 0;			/* NAN = NAN */
		else
			result = 1;			/* NAN > non-NAN */
	}
	else if (NUMERIC_IS_NAN(num2))
	{
		result = -1;			/* non-NAN < NAN */
	}
	else
	{
		NumericVar	arg1;
		NumericVar	arg2;

		init_var(&arg1);
		init_var(&arg2);

		set_var_from_num(num1, &arg1);
		set_var_from_num(num2, &arg2);

		result = cmp_var(&arg1, &arg2);

		free_var(&arg1);
		free_var(&arg2);
	}

	return result;
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
Datum
numeric_add(PG_FUNCTION_ARGS)
{
	Numeric		num1 = PG_GETARG_NUMERIC(0);
	Numeric		num2 = PG_GETARG_NUMERIC(1);
	NumericVar	arg1;
	NumericVar	arg2;
	NumericVar	result;
	Numeric		res;

	/*
	 * Handle NaN
	 */
	if (NUMERIC_IS_NAN(num1) || NUMERIC_IS_NAN(num2))
		PG_RETURN_NUMERIC(make_result(&const_nan));

	/*
	 * Unpack the values, let add_var() compute the result and return it.
	 * The internals of add_var() will automatically set the correct
	 * result and display scales in the result.
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

	PG_RETURN_NUMERIC(res);
}


/* ----------
 * numeric_sub() -
 *
 *	Subtract one numeric from another
 * ----------
 */
Datum
numeric_sub(PG_FUNCTION_ARGS)
{
	Numeric		num1 = PG_GETARG_NUMERIC(0);
	Numeric		num2 = PG_GETARG_NUMERIC(1);
	NumericVar	arg1;
	NumericVar	arg2;
	NumericVar	result;
	Numeric		res;

	/*
	 * Handle NaN
	 */
	if (NUMERIC_IS_NAN(num1) || NUMERIC_IS_NAN(num2))
		PG_RETURN_NUMERIC(make_result(&const_nan));

	/*
	 * Unpack the two arguments, let sub_var() compute the result and
	 * return it.
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

	PG_RETURN_NUMERIC(res);
}


/* ----------
 * numeric_mul() -
 *
 *	Calculate the product of two numerics
 * ----------
 */
Datum
numeric_mul(PG_FUNCTION_ARGS)
{
	Numeric		num1 = PG_GETARG_NUMERIC(0);
	Numeric		num2 = PG_GETARG_NUMERIC(1);
	NumericVar	arg1;
	NumericVar	arg2;
	NumericVar	result;
	Numeric		res;

	/*
	 * Handle NaN
	 */
	if (NUMERIC_IS_NAN(num1) || NUMERIC_IS_NAN(num2))
		PG_RETURN_NUMERIC(make_result(&const_nan));

	/*
	 * Unpack the arguments, let mul_var() compute the result and return
	 * it. Unlike add_var() and sub_var(), mul_var() will round the result
	 * to the scale stored in global_rscale. In the case of numeric_mul(),
	 * which is invoked for the * operator on numerics, we set it to the
	 * exact representation for the product (rscale = sum(rscale of arg1,
	 * rscale of arg2) and the same for the dscale).
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

	PG_RETURN_NUMERIC(res);
}


/* ----------
 * numeric_div() -
 *
 *	Divide one numeric into another
 * ----------
 */
Datum
numeric_div(PG_FUNCTION_ARGS)
{
	Numeric		num1 = PG_GETARG_NUMERIC(0);
	Numeric		num2 = PG_GETARG_NUMERIC(1);
	NumericVar	arg1;
	NumericVar	arg2;
	NumericVar	result;
	Numeric		res;
	int			res_dscale;

	/*
	 * Handle NaN
	 */
	if (NUMERIC_IS_NAN(num1) || NUMERIC_IS_NAN(num2))
		PG_RETURN_NUMERIC(make_result(&const_nan));

	/*
	 * Unpack the arguments
	 */
	init_var(&arg1);
	init_var(&arg2);
	init_var(&result);

	set_var_from_num(num1, &arg1);
	set_var_from_num(num2, &arg2);

	res_dscale = select_div_scale(&arg1, &arg2);

	/*
	 * Do the divide, set the display scale and return the result
	 */
	div_var(&arg1, &arg2, &result);

	result.dscale = res_dscale;

	res = make_result(&result);

	free_var(&arg1);
	free_var(&arg2);
	free_var(&result);

	PG_RETURN_NUMERIC(res);
}


/* ----------
 * numeric_mod() -
 *
 *	Calculate the modulo of two numerics
 * ----------
 */
Datum
numeric_mod(PG_FUNCTION_ARGS)
{
	Numeric		num1 = PG_GETARG_NUMERIC(0);
	Numeric		num2 = PG_GETARG_NUMERIC(1);
	Numeric		res;
	NumericVar	arg1;
	NumericVar	arg2;
	NumericVar	result;

	if (NUMERIC_IS_NAN(num1) || NUMERIC_IS_NAN(num2))
		PG_RETURN_NUMERIC(make_result(&const_nan));

	init_var(&arg1);
	init_var(&arg2);
	init_var(&result);

	set_var_from_num(num1, &arg1);
	set_var_from_num(num2, &arg2);

	mod_var(&arg1, &arg2, &result);

	res = make_result(&result);

	free_var(&result);
	free_var(&arg2);
	free_var(&arg1);

	PG_RETURN_NUMERIC(res);
}


/* ----------
 * numeric_inc() -
 *
 *	Increment a number by one
 * ----------
 */
Datum
numeric_inc(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	NumericVar	arg;
	Numeric		res;

	/*
	 * Handle NaN
	 */
	if (NUMERIC_IS_NAN(num))
		PG_RETURN_NUMERIC(make_result(&const_nan));

	/*
	 * Compute the result and return it
	 */
	init_var(&arg);

	set_var_from_num(num, &arg);

	add_var(&arg, &const_one, &arg);
	res = make_result(&arg);

	free_var(&arg);

	PG_RETURN_NUMERIC(res);
}


/* ----------
 * numeric_smaller() -
 *
 *	Return the smaller of two numbers
 * ----------
 */
Datum
numeric_smaller(PG_FUNCTION_ARGS)
{
	Numeric		num1 = PG_GETARG_NUMERIC(0);
	Numeric		num2 = PG_GETARG_NUMERIC(1);
	NumericVar	arg1;
	NumericVar	arg2;
	Numeric		res;

	/*
	 * Handle NaN
	 */
	if (NUMERIC_IS_NAN(num1) || NUMERIC_IS_NAN(num2))
		PG_RETURN_NUMERIC(make_result(&const_nan));

	/*
	 * Unpack the values, and decide which is the smaller one
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

	PG_RETURN_NUMERIC(res);
}


/* ----------
 * numeric_larger() -
 *
 *	Return the larger of two numbers
 * ----------
 */
Datum
numeric_larger(PG_FUNCTION_ARGS)
{
	Numeric		num1 = PG_GETARG_NUMERIC(0);
	Numeric		num2 = PG_GETARG_NUMERIC(1);
	NumericVar	arg1;
	NumericVar	arg2;
	Numeric		res;

	/*
	 * Handle NaN
	 */
	if (NUMERIC_IS_NAN(num1) || NUMERIC_IS_NAN(num2))
		PG_RETURN_NUMERIC(make_result(&const_nan));

	/*
	 * Unpack the values, and decide which is the larger one
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

	PG_RETURN_NUMERIC(res);
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
Datum
numeric_sqrt(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	Numeric		res;
	NumericVar	arg;
	NumericVar	result;
	int			sweight;
	int			res_dscale;

	/*
	 * Handle NaN
	 */
	if (NUMERIC_IS_NAN(num))
		PG_RETURN_NUMERIC(make_result(&const_nan));

	/*
	 * Unpack the argument and determine the scales.  We choose a display
	 * scale to give at least NUMERIC_MIN_SIG_DIGITS significant digits;
	 * but in any case not less than the input's dscale.
	 */
	init_var(&arg);
	init_var(&result);

	set_var_from_num(num, &arg);

	/* Assume the input was normalized, so arg.weight is accurate */
	sweight = (arg.weight / 2) - 1;

	res_dscale = NUMERIC_MIN_SIG_DIGITS - sweight;
	res_dscale = Max(res_dscale, arg.dscale);
	res_dscale = Max(res_dscale, NUMERIC_MIN_DISPLAY_SCALE);
	res_dscale = Min(res_dscale, NUMERIC_MAX_DISPLAY_SCALE);

	global_rscale = res_dscale + 8;

	/*
	 * Let sqrt_var() do the calculation and return the result.
	 */
	sqrt_var(&arg, &result);

	result.dscale = res_dscale;

	res = make_result(&result);

	free_var(&result);
	free_var(&arg);

	PG_RETURN_NUMERIC(res);
}


/* ----------
 * numeric_exp() -
 *
 *	Raise e to the power of x
 * ----------
 */
Datum
numeric_exp(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	Numeric		res;
	NumericVar	arg;
	NumericVar	result;
	int			res_dscale;
	double		val;

	/*
	 * Handle NaN
	 */
	if (NUMERIC_IS_NAN(num))
		PG_RETURN_NUMERIC(make_result(&const_nan));

	/*
	 * Unpack the argument and determine the scales.  We choose a display
	 * scale to give at least NUMERIC_MIN_SIG_DIGITS significant digits;
	 * but in any case not less than the input's dscale.
	 */
	init_var(&arg);
	init_var(&result);

	set_var_from_num(num, &arg);

	/* convert input to float8, ignoring overflow */
	val = numeric_to_double_no_overflow(num);

	/* log10(result) = num * log10(e), so this is approximately the weight: */
	val *= 0.434294481903252;

	/* limit to something that won't cause integer overflow */
	val = Max(val, -NUMERIC_MAX_RESULT_SCALE);
	val = Min(val, NUMERIC_MAX_RESULT_SCALE);

	res_dscale = NUMERIC_MIN_SIG_DIGITS - (int) val;
	res_dscale = Max(res_dscale, arg.dscale);
	res_dscale = Max(res_dscale, NUMERIC_MIN_DISPLAY_SCALE);
	res_dscale = Min(res_dscale, NUMERIC_MAX_DISPLAY_SCALE);

	global_rscale = res_dscale + NUMERIC_EXTRA_DIGITS;

	/*
	 * Let exp_var() do the calculation and return the result.
	 */
	exp_var(&arg, &result);

	result.dscale = res_dscale;

	res = make_result(&result);

	free_var(&result);
	free_var(&arg);

	PG_RETURN_NUMERIC(res);
}


/* ----------
 * numeric_ln() -
 *
 *	Compute the natural logarithm of x
 * ----------
 */
Datum
numeric_ln(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	Numeric		res;
	NumericVar	arg;
	NumericVar	result;
	int			res_dscale;

	/*
	 * Handle NaN
	 */
	if (NUMERIC_IS_NAN(num))
		PG_RETURN_NUMERIC(make_result(&const_nan));

	init_var(&arg);
	init_var(&result);

	set_var_from_num(num, &arg);

	if (arg.weight > 0)
		res_dscale = NUMERIC_MIN_SIG_DIGITS - (int) log10(arg.weight);
	else if (arg.weight < 0)
		res_dscale = NUMERIC_MIN_SIG_DIGITS - (int) log10(- arg.weight);
	else
		res_dscale = NUMERIC_MIN_SIG_DIGITS;

	res_dscale = Max(res_dscale, arg.dscale);
	res_dscale = Max(res_dscale, NUMERIC_MIN_DISPLAY_SCALE);
	res_dscale = Min(res_dscale, NUMERIC_MAX_DISPLAY_SCALE);

	global_rscale = res_dscale + NUMERIC_EXTRA_DIGITS;

	ln_var(&arg, &result);

	result.dscale = res_dscale;

	res = make_result(&result);

	free_var(&result);
	free_var(&arg);

	PG_RETURN_NUMERIC(res);
}


/* ----------
 * numeric_log() -
 *
 *	Compute the logarithm of x in a given base
 * ----------
 */
Datum
numeric_log(PG_FUNCTION_ARGS)
{
	Numeric		num1 = PG_GETARG_NUMERIC(0);
	Numeric		num2 = PG_GETARG_NUMERIC(1);
	Numeric		res;
	NumericVar	arg1;
	NumericVar	arg2;
	NumericVar	result;

	/*
	 * Handle NaN
	 */
	if (NUMERIC_IS_NAN(num1) || NUMERIC_IS_NAN(num2))
		PG_RETURN_NUMERIC(make_result(&const_nan));

	/*
	 * Initialize things
	 */
	init_var(&arg1);
	init_var(&arg2);
	init_var(&result);

	set_var_from_num(num1, &arg1);
	set_var_from_num(num2, &arg2);

	/*
	 * Call log_var() to compute and return the result; note it handles
	 * rscale/dscale itself.
	 */
	log_var(&arg1, &arg2, &result);

	res = make_result(&result);

	free_var(&result);
	free_var(&arg2);
	free_var(&arg1);

	PG_RETURN_NUMERIC(res);
}


/* ----------
 * numeric_power() -
 *
 *	Raise b to the power of x
 * ----------
 */
Datum
numeric_power(PG_FUNCTION_ARGS)
{
	Numeric		num1 = PG_GETARG_NUMERIC(0);
	Numeric		num2 = PG_GETARG_NUMERIC(1);
	Numeric		res;
	NumericVar	arg1;
	NumericVar	arg2;
	NumericVar	result;

	/*
	 * Handle NaN
	 */
	if (NUMERIC_IS_NAN(num1) || NUMERIC_IS_NAN(num2))
		PG_RETURN_NUMERIC(make_result(&const_nan));

	/*
	 * Initialize things
	 */
	init_var(&arg1);
	init_var(&arg2);
	init_var(&result);

	set_var_from_num(num1, &arg1);
	set_var_from_num(num2, &arg2);

	/*
	 * Call power_var() to compute and return the result; note it handles
	 * rscale/dscale itself.
	 */
	power_var(&arg1, &arg2, &result);

	res = make_result(&result);

	free_var(&result);
	free_var(&arg2);
	free_var(&arg1);

	PG_RETURN_NUMERIC(res);
}


/* ----------------------------------------------------------------------
 *
 * Type conversion functions
 *
 * ----------------------------------------------------------------------
 */


Datum
int4_numeric(PG_FUNCTION_ARGS)
{
	int32		val = PG_GETARG_INT32(0);
	Numeric		res;
	NumericVar	result;
	char	   *tmp;

	init_var(&result);

	tmp = DatumGetCString(DirectFunctionCall1(int4out,
											  Int32GetDatum(val)));
	set_var_from_str(tmp, &result);
	res = make_result(&result);

	free_var(&result);
	pfree(tmp);

	PG_RETURN_NUMERIC(res);
}


Datum
numeric_int4(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	NumericVar	x;
	char	   *str;
	Datum		result;

	/* XXX would it be better to return NULL? */
	if (NUMERIC_IS_NAN(num))
		elog(ERROR, "Cannot convert NaN to int4");

	/*
	 * Get the number in the variable format so we can round to integer.
	 */
	init_var(&x);
	set_var_from_num(num, &x);

	str = get_str_from_var(&x, 0);		/* dscale = 0 produces rounding */

	free_var(&x);

	result = DirectFunctionCall1(int4in, CStringGetDatum(str));
	pfree(str);

	PG_RETURN_DATUM(result);
}


Datum
int8_numeric(PG_FUNCTION_ARGS)
{
	Datum		val = PG_GETARG_DATUM(0);
	Numeric		res;
	NumericVar	result;
	char	   *tmp;

	init_var(&result);

	tmp = DatumGetCString(DirectFunctionCall1(int8out, val));
	set_var_from_str(tmp, &result);
	res = make_result(&result);

	free_var(&result);
	pfree(tmp);

	PG_RETURN_NUMERIC(res);
}


Datum
numeric_int8(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	NumericVar	x;
	char	   *str;
	Datum		result;

	/* XXX would it be better to return NULL? */
	if (NUMERIC_IS_NAN(num))
		elog(ERROR, "Cannot convert NaN to int8");

	/*
	 * Get the number in the variable format so we can round to integer.
	 */
	init_var(&x);
	set_var_from_num(num, &x);

	str = get_str_from_var(&x, 0);		/* dscale = 0 produces rounding */

	free_var(&x);

	result = DirectFunctionCall1(int8in, CStringGetDatum(str));
	pfree(str);

	PG_RETURN_DATUM(result);
}


Datum
int2_numeric(PG_FUNCTION_ARGS)
{
	int16		val = PG_GETARG_INT16(0);
	Numeric		res;
	NumericVar	result;
	char	   *tmp;

	init_var(&result);

	tmp = DatumGetCString(DirectFunctionCall1(int2out,
											  Int16GetDatum(val)));
	set_var_from_str(tmp, &result);
	res = make_result(&result);

	free_var(&result);
	pfree(tmp);

	PG_RETURN_NUMERIC(res);
}


Datum
numeric_int2(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	NumericVar	x;
	char	   *str;
	Datum		result;

	/* XXX would it be better to return NULL? */
	if (NUMERIC_IS_NAN(num))
		elog(ERROR, "Cannot convert NaN to int2");

	/*
	 * Get the number in the variable format so we can round to integer.
	 */
	init_var(&x);
	set_var_from_num(num, &x);

	str = get_str_from_var(&x, 0);		/* dscale = 0 produces rounding */

	free_var(&x);

	result = DirectFunctionCall1(int2in, CStringGetDatum(str));
	pfree(str);

	return result;
}


Datum
float8_numeric(PG_FUNCTION_ARGS)
{
	float8		val = PG_GETARG_FLOAT8(0);
	Numeric		res;
	NumericVar	result;
	char		buf[DBL_DIG + 100];

	if (isnan(val))
		PG_RETURN_NUMERIC(make_result(&const_nan));

	sprintf(buf, "%.*g", DBL_DIG, val);

	init_var(&result);

	set_var_from_str(buf, &result);
	res = make_result(&result);

	free_var(&result);

	PG_RETURN_NUMERIC(res);
}


Datum
numeric_float8(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	char	   *tmp;
	Datum		result;

	if (NUMERIC_IS_NAN(num))
		PG_RETURN_FLOAT8(NAN);

	tmp = DatumGetCString(DirectFunctionCall1(numeric_out,
											  NumericGetDatum(num)));

	result = DirectFunctionCall1(float8in, CStringGetDatum(tmp));

	pfree(tmp);

	PG_RETURN_DATUM(result);
}


/* Convert numeric to float8; if out of range, return +/- HUGE_VAL */
Datum
numeric_float8_no_overflow(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	double		val;

	if (NUMERIC_IS_NAN(num))
		PG_RETURN_FLOAT8(NAN);

	val = numeric_to_double_no_overflow(num);

	PG_RETURN_FLOAT8(val);
}

Datum
float4_numeric(PG_FUNCTION_ARGS)
{
	float4		val = PG_GETARG_FLOAT4(0);
	Numeric		res;
	NumericVar	result;
	char		buf[FLT_DIG + 100];

	if (isnan(val))
		PG_RETURN_NUMERIC(make_result(&const_nan));

	sprintf(buf, "%.*g", FLT_DIG, val);

	init_var(&result);

	set_var_from_str(buf, &result);
	res = make_result(&result);

	free_var(&result);

	PG_RETURN_NUMERIC(res);
}


Datum
numeric_float4(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	char	   *tmp;
	Datum		result;

	if (NUMERIC_IS_NAN(num))
		PG_RETURN_FLOAT4((float4) NAN);

	tmp = DatumGetCString(DirectFunctionCall1(numeric_out,
											  NumericGetDatum(num)));

	result = DirectFunctionCall1(float4in, CStringGetDatum(tmp));

	pfree(tmp);

	PG_RETURN_DATUM(result);
}


Datum
text_numeric(PG_FUNCTION_ARGS)
{
	text	   *str = PG_GETARG_TEXT_P(0);
	int			len;
	char	   *s;
	Datum		result;

	len = (VARSIZE(str) - VARHDRSZ);
	s = palloc(len + 1);
	memcpy(s, VARDATA(str), len);
	*(s + len) = '\0';

	result = DirectFunctionCall3(numeric_in, CStringGetDatum(s),
								 ObjectIdGetDatum(0), Int32GetDatum(-1));

	pfree(s);

	return result;
}

Datum
numeric_text(PG_FUNCTION_ARGS)
{
	/* val is numeric, but easier to leave it as Datum */
	Datum		val = PG_GETARG_DATUM(0);
	char	   *s;
	int			len;
	text	   *result;

	s = DatumGetCString(DirectFunctionCall1(numeric_out, val));
	len = strlen(s);

	result = (text *) palloc(VARHDRSZ + len);

	VARATT_SIZEP(result) = len + VARHDRSZ;
	memcpy(VARDATA(result), s, len);

	pfree(s);

	PG_RETURN_TEXT_P(result);
}


/* ----------------------------------------------------------------------
 *
 * Aggregate functions
 *
 * The transition datatype for all these aggregates is a 3-element array
 * of Numeric, holding the values N, sum(X), sum(X*X) in that order.
 *
 * We represent N as a numeric mainly to avoid having to build a special
 * datatype; it's unlikely it'd overflow an int4, but ...
 *
 * ----------------------------------------------------------------------
 */

static ArrayType *
do_numeric_accum(ArrayType *transarray, Numeric newval)
{
	Datum	   *transdatums;
	int			ndatums;
	Datum		N,
				sumX,
				sumX2;
	ArrayType  *result;

	/* We assume the input is array of numeric */
	deconstruct_array(transarray,
					  NUMERICOID, -1, false, 'i',
					  &transdatums, &ndatums);
	if (ndatums != 3)
		elog(ERROR, "do_numeric_accum: expected 3-element numeric array");
	N = transdatums[0];
	sumX = transdatums[1];
	sumX2 = transdatums[2];

	N = DirectFunctionCall1(numeric_inc, N);
	sumX = DirectFunctionCall2(numeric_add, sumX,
							   NumericGetDatum(newval));
	sumX2 = DirectFunctionCall2(numeric_add, sumX2,
								DirectFunctionCall2(numeric_mul,
												 NumericGetDatum(newval),
											   NumericGetDatum(newval)));

	transdatums[0] = N;
	transdatums[1] = sumX;
	transdatums[2] = sumX2;

	result = construct_array(transdatums, 3,
							 NUMERICOID, -1, false, 'i');

	return result;
}

Datum
numeric_accum(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	Numeric		newval = PG_GETARG_NUMERIC(1);

	PG_RETURN_ARRAYTYPE_P(do_numeric_accum(transarray, newval));
}

/*
 * Integer data types all use Numeric accumulators to share code and
 * avoid risk of overflow.	For int2 and int4 inputs, Numeric accumulation
 * is overkill for the N and sum(X) values, but definitely not overkill
 * for the sum(X*X) value.	Hence, we use int2_accum and int4_accum only
 * for stddev/variance --- there are faster special-purpose accumulator
 * routines for SUM and AVG of these datatypes.
 */

Datum
int2_accum(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	Datum		newval2 = PG_GETARG_DATUM(1);
	Numeric		newval;

	newval = DatumGetNumeric(DirectFunctionCall1(int2_numeric, newval2));

	PG_RETURN_ARRAYTYPE_P(do_numeric_accum(transarray, newval));
}

Datum
int4_accum(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	Datum		newval4 = PG_GETARG_DATUM(1);
	Numeric		newval;

	newval = DatumGetNumeric(DirectFunctionCall1(int4_numeric, newval4));

	PG_RETURN_ARRAYTYPE_P(do_numeric_accum(transarray, newval));
}

Datum
int8_accum(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	Datum		newval8 = PG_GETARG_DATUM(1);
	Numeric		newval;

	newval = DatumGetNumeric(DirectFunctionCall1(int8_numeric, newval8));

	PG_RETURN_ARRAYTYPE_P(do_numeric_accum(transarray, newval));
}

Datum
numeric_avg(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	Datum	   *transdatums;
	int			ndatums;
	Numeric		N,
				sumX;

	/* We assume the input is array of numeric */
	deconstruct_array(transarray,
					  NUMERICOID, -1, false, 'i',
					  &transdatums, &ndatums);
	if (ndatums != 3)
		elog(ERROR, "numeric_avg: expected 3-element numeric array");
	N = DatumGetNumeric(transdatums[0]);
	sumX = DatumGetNumeric(transdatums[1]);
	/* ignore sumX2 */

	/* SQL92 defines AVG of no values to be NULL */
	/* N is zero iff no digits (cf. numeric_uminus) */
	if (N->varlen == NUMERIC_HDRSZ)
		PG_RETURN_NULL();

	PG_RETURN_DATUM(DirectFunctionCall2(numeric_div,
										NumericGetDatum(sumX),
										NumericGetDatum(N)));
}

Datum
numeric_variance(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	Datum	   *transdatums;
	int			ndatums;
	Numeric		N,
				sumX,
				sumX2,
				res;
	NumericVar	vN,
				vsumX,
				vsumX2,
				vNminus1;
	int			div_dscale;

	/* We assume the input is array of numeric */
	deconstruct_array(transarray,
					  NUMERICOID, -1, false, 'i',
					  &transdatums, &ndatums);
	if (ndatums != 3)
		elog(ERROR, "numeric_variance: expected 3-element numeric array");
	N = DatumGetNumeric(transdatums[0]);
	sumX = DatumGetNumeric(transdatums[1]);
	sumX2 = DatumGetNumeric(transdatums[2]);

	if (NUMERIC_IS_NAN(N) || NUMERIC_IS_NAN(sumX) || NUMERIC_IS_NAN(sumX2))
		PG_RETURN_NUMERIC(make_result(&const_nan));

	/* We define VARIANCE of no values to be NULL, of 1 value to be 0 */
	/* N is zero iff no digits (cf. numeric_uminus) */
	if (N->varlen == NUMERIC_HDRSZ)
		PG_RETURN_NULL();

	init_var(&vN);
	set_var_from_num(N, &vN);

	init_var(&vNminus1);
	sub_var(&vN, &const_one, &vNminus1);

	if (cmp_var(&vNminus1, &const_zero) <= 0)
	{
		free_var(&vN);
		free_var(&vNminus1);
		PG_RETURN_NUMERIC(make_result(&const_zero));
	}

	init_var(&vsumX);
	set_var_from_num(sumX, &vsumX);
	init_var(&vsumX2);
	set_var_from_num(sumX2, &vsumX2);

	/* set rscale for mul_var calls */
	global_rscale = vsumX.rscale * 2;

	mul_var(&vsumX, &vsumX, &vsumX);	/* now vsumX contains sumX * sumX */
	mul_var(&vN, &vsumX2, &vsumX2);		/* now vsumX2 contains N * sumX2 */
	sub_var(&vsumX2, &vsumX, &vsumX2);	/* N * sumX2 - sumX * sumX */

	if (cmp_var(&vsumX2, &const_zero) <= 0)
	{
		/* Watch out for roundoff error producing a negative numerator */
		res = make_result(&const_zero);
	}
	else
	{
		mul_var(&vN, &vNminus1, &vNminus1);		/* N * (N - 1) */
		div_dscale = select_div_scale(&vsumX2, &vNminus1);
		div_var(&vsumX2, &vNminus1, &vsumX);	/* variance */
		vsumX.dscale = div_dscale;

		res = make_result(&vsumX);
	}

	free_var(&vN);
	free_var(&vNminus1);
	free_var(&vsumX);
	free_var(&vsumX2);

	PG_RETURN_NUMERIC(res);
}

Datum
numeric_stddev(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	Datum	   *transdatums;
	int			ndatums;
	Numeric		N,
				sumX,
				sumX2,
				res;
	NumericVar	vN,
				vsumX,
				vsumX2,
				vNminus1;
	int			div_dscale;

	/* We assume the input is array of numeric */
	deconstruct_array(transarray,
					  NUMERICOID, -1, false, 'i',
					  &transdatums, &ndatums);
	if (ndatums != 3)
		elog(ERROR, "numeric_stddev: expected 3-element numeric array");
	N = DatumGetNumeric(transdatums[0]);
	sumX = DatumGetNumeric(transdatums[1]);
	sumX2 = DatumGetNumeric(transdatums[2]);

	if (NUMERIC_IS_NAN(N) || NUMERIC_IS_NAN(sumX) || NUMERIC_IS_NAN(sumX2))
		PG_RETURN_NUMERIC(make_result(&const_nan));

	/* We define STDDEV of no values to be NULL, of 1 value to be 0 */
	/* N is zero iff no digits (cf. numeric_uminus) */
	if (N->varlen == NUMERIC_HDRSZ)
		PG_RETURN_NULL();

	init_var(&vN);
	set_var_from_num(N, &vN);

	init_var(&vNminus1);
	sub_var(&vN, &const_one, &vNminus1);

	if (cmp_var(&vNminus1, &const_zero) <= 0)
	{
		free_var(&vN);
		free_var(&vNminus1);
		PG_RETURN_NUMERIC(make_result(&const_zero));
	}

	init_var(&vsumX);
	set_var_from_num(sumX, &vsumX);
	init_var(&vsumX2);
	set_var_from_num(sumX2, &vsumX2);

	/* set rscale for mul_var calls */
	global_rscale = vsumX.rscale * 2;

	mul_var(&vsumX, &vsumX, &vsumX);	/* now vsumX contains sumX * sumX */
	mul_var(&vN, &vsumX2, &vsumX2);		/* now vsumX2 contains N * sumX2 */
	sub_var(&vsumX2, &vsumX, &vsumX2);	/* N * sumX2 - sumX * sumX */

	if (cmp_var(&vsumX2, &const_zero) <= 0)
	{
		/* Watch out for roundoff error producing a negative numerator */
		res = make_result(&const_zero);
	}
	else
	{
		mul_var(&vN, &vNminus1, &vNminus1);		/* N * (N - 1) */
		div_dscale = select_div_scale(&vsumX2, &vNminus1);
		div_var(&vsumX2, &vNminus1, &vsumX);	/* variance */
		vsumX.dscale = div_dscale;
		sqrt_var(&vsumX, &vsumX);		/* stddev */

		res = make_result(&vsumX);
	}

	free_var(&vN);
	free_var(&vNminus1);
	free_var(&vsumX);
	free_var(&vsumX2);

	PG_RETURN_NUMERIC(res);
}


/*
 * SUM transition functions for integer datatypes.
 *
 * To avoid overflow, we use accumulators wider than the input datatype.
 * A Numeric accumulator is needed for int8 input; for int4 and int2
 * inputs, we use int8 accumulators which should be sufficient for practical
 * purposes.  (The latter two therefore don't really belong in this file,
 * but we keep them here anyway.)
 *
 * Because SQL92 defines the SUM() of no values to be NULL, not zero,
 * the initial condition of the transition data value needs to be NULL. This
 * means we can't rely on ExecAgg to automatically insert the first non-null
 * data value into the transition data: it doesn't know how to do the type
 * conversion.	The upshot is that these routines have to be marked non-strict
 * and handle substitution of the first non-null input themselves.
 */

Datum
int2_sum(PG_FUNCTION_ARGS)
{
	int64		oldsum;
	int64		newval;

	if (PG_ARGISNULL(0))
	{
		/* No non-null input seen so far... */
		if (PG_ARGISNULL(1))
			PG_RETURN_NULL();	/* still no non-null */
		/* This is the first non-null input. */
		newval = (int64) PG_GETARG_INT16(1);
		PG_RETURN_INT64(newval);
	}

	oldsum = PG_GETARG_INT64(0);

	/* Leave sum unchanged if new input is null. */
	if (PG_ARGISNULL(1))
		PG_RETURN_INT64(oldsum);

	/* OK to do the addition. */
	newval = oldsum + (int64) PG_GETARG_INT16(1);

	PG_RETURN_INT64(newval);
}

Datum
int4_sum(PG_FUNCTION_ARGS)
{
	int64		oldsum;
	int64		newval;

	if (PG_ARGISNULL(0))
	{
		/* No non-null input seen so far... */
		if (PG_ARGISNULL(1))
			PG_RETURN_NULL();	/* still no non-null */
		/* This is the first non-null input. */
		newval = (int64) PG_GETARG_INT32(1);
		PG_RETURN_INT64(newval);
	}

	oldsum = PG_GETARG_INT64(0);

	/* Leave sum unchanged if new input is null. */
	if (PG_ARGISNULL(1))
		PG_RETURN_INT64(oldsum);

	/* OK to do the addition. */
	newval = oldsum + (int64) PG_GETARG_INT32(1);

	PG_RETURN_INT64(newval);
}

Datum
int8_sum(PG_FUNCTION_ARGS)
{
	Numeric		oldsum;
	Datum		newval;

	if (PG_ARGISNULL(0))
	{
		/* No non-null input seen so far... */
		if (PG_ARGISNULL(1))
			PG_RETURN_NULL();	/* still no non-null */
		/* This is the first non-null input. */
		newval = DirectFunctionCall1(int8_numeric, PG_GETARG_DATUM(1));
		PG_RETURN_DATUM(newval);
	}

	oldsum = PG_GETARG_NUMERIC(0);

	/* Leave sum unchanged if new input is null. */
	if (PG_ARGISNULL(1))
		PG_RETURN_NUMERIC(oldsum);

	/* OK to do the addition. */
	newval = DirectFunctionCall1(int8_numeric, PG_GETARG_DATUM(1));

	PG_RETURN_DATUM(DirectFunctionCall2(numeric_add,
										NumericGetDatum(oldsum), newval));
}


/*
 * Routines for avg(int2) and avg(int4).  The transition datatype
 * is a two-element int8 array, holding count and sum.
 */

typedef struct Int8TransTypeData
{
#ifndef INT64_IS_BUSTED
	int64		count;
	int64		sum;
#else
	/* "int64" isn't really 64 bits, so fake up properly-aligned fields */
	int32		count;
	int32		pad1;
	int32		sum;
	int32		pad2;
#endif
} Int8TransTypeData;

Datum
int2_avg_accum(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P_COPY(0);
	int16		newval = PG_GETARG_INT16(1);
	Int8TransTypeData *transdata;

	/*
	 * We copied the input array, so it's okay to scribble on it directly.
	 */
	if (ARR_SIZE(transarray) != ARR_OVERHEAD(1) + sizeof(Int8TransTypeData))
		elog(ERROR, "int2_avg_accum: expected 2-element int8 array");
	transdata = (Int8TransTypeData *) ARR_DATA_PTR(transarray);

	transdata->count++;
	transdata->sum += newval;

	PG_RETURN_ARRAYTYPE_P(transarray);
}

Datum
int4_avg_accum(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P_COPY(0);
	int32		newval = PG_GETARG_INT32(1);
	Int8TransTypeData *transdata;

	/*
	 * We copied the input array, so it's okay to scribble on it directly.
	 */
	if (ARR_SIZE(transarray) != ARR_OVERHEAD(1) + sizeof(Int8TransTypeData))
		elog(ERROR, "int4_avg_accum: expected 2-element int8 array");
	transdata = (Int8TransTypeData *) ARR_DATA_PTR(transarray);

	transdata->count++;
	transdata->sum += newval;

	PG_RETURN_ARRAYTYPE_P(transarray);
}

Datum
int8_avg(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	Int8TransTypeData *transdata;
	Datum		countd,
				sumd;

	if (ARR_SIZE(transarray) != ARR_OVERHEAD(1) + sizeof(Int8TransTypeData))
		elog(ERROR, "int8_avg: expected 2-element int8 array");
	transdata = (Int8TransTypeData *) ARR_DATA_PTR(transarray);

	/* SQL92 defines AVG of no values to be NULL */
	if (transdata->count == 0)
		PG_RETURN_NULL();

	countd = DirectFunctionCall1(int8_numeric,
								 Int64GetDatumFast(transdata->count));
	sumd = DirectFunctionCall1(int8_numeric,
							   Int64GetDatumFast(transdata->sum));

	PG_RETURN_DATUM(DirectFunctionCall2(numeric_div, sumd, countd));
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
	int			i;

	printf("%s: NUMERIC w=%d r=%d d=%d ", str, num->n_weight, num->n_rscale,
		   NUMERIC_DSCALE(num));
	switch (NUMERIC_SIGN(num))
	{
		case NUMERIC_POS:
			printf("POS");
			break;
		case NUMERIC_NEG:
			printf("NEG");
			break;
		case NUMERIC_NAN:
			printf("NaN");
			break;
		default:
			printf("SIGN=0x%x", NUMERIC_SIGN(num));
			break;
	}

	for (i = 0; i < num->varlen - NUMERIC_HDRSZ; i++)
		printf(" %d %d", (num->n_data[i] >> 4) & 0x0f, num->n_data[i] & 0x0f);
	printf("\n");
}


/* ----------
 * dump_var() - Dump a value in the variable format for debugging
 * ----------
 */
static void
dump_var(char *str, NumericVar *var)
{
	int			i;

	printf("%s: VAR w=%d r=%d d=%d ", str, var->weight, var->rscale,
		   var->dscale);
	switch (var->sign)
	{
		case NUMERIC_POS:
			printf("POS");
			break;
		case NUMERIC_NEG:
			printf("NEG");
			break;
		case NUMERIC_NAN:
			printf("NaN");
			break;
		default:
			printf("SIGN=0x%x", var->sign);
			break;
	}

	for (i = 0; i < var->ndigits; i++)
		printf(" %d", var->digits[i]);

	printf("\n");
}
#endif   /* NUMERIC_DEBUG */


/* ----------
 * alloc_var() -
 *
 *	Allocate a digit buffer of ndigits digits (plus a spare digit for rounding)
 * ----------
 */
static void
alloc_var(NumericVar *var, int ndigits)
{
	digitbuf_free(var->buf);
	var->buf = digitbuf_alloc(ndigits + 1);
	var->buf[0] = 0;
	var->digits = var->buf + 1;
	var->ndigits = ndigits;
}


/* ----------
 * free_var() -
 *
 *	Return the digit buffer of a variable to the free pool
 * ----------
 */
static void
free_var(NumericVar *var)
{
	digitbuf_free(var->buf);
	var->buf = NULL;
	var->digits = NULL;
	var->sign = NUMERIC_NAN;
}


/* ----------
 * zero_var() -
 *
 *	Set a variable to ZERO.
 *	Note: rscale and dscale are not touched.
 * ----------
 */
static void
zero_var(NumericVar *var)
{
	digitbuf_free(var->buf);
	var->buf = NULL;
	var->digits = NULL;
	var->ndigits = 0;
	var->weight = 0;			/* by convention; doesn't really matter */
	var->sign = NUMERIC_POS;	/* anything but NAN... */
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
	char	   *cp = str;
	bool		have_dp = FALSE;
	int			i = 0;

	while (*cp)
	{
		if (!isspace((unsigned char) *cp))
			break;
		cp++;
	}

	alloc_var(dest, strlen(cp));
	dest->weight = -1;
	dest->dscale = 0;
	dest->sign = NUMERIC_POS;

	switch (*cp)
	{
		case '+':
			dest->sign = NUMERIC_POS;
			cp++;
			break;

		case '-':
			dest->sign = NUMERIC_NEG;
			cp++;
			break;
	}

	if (*cp == '.')
	{
		have_dp = TRUE;
		cp++;
	}

	if (!isdigit((unsigned char) *cp))
		elog(ERROR, "Bad numeric input format '%s'", str);

	while (*cp)
	{
		if (isdigit((unsigned char) *cp))
		{
			dest->digits[i++] = *cp++ - '0';
			if (!have_dp)
				dest->weight++;
			else
				dest->dscale++;
		}
		else if (*cp == '.')
		{
			if (have_dp)
				elog(ERROR, "Bad numeric input format '%s'", str);
			have_dp = TRUE;
			cp++;
		}
		else
			break;
	}
	dest->ndigits = i;

	/* Handle exponent, if any */
	if (*cp == 'e' || *cp == 'E')
	{
		long		exponent;
		char	   *endptr;

		cp++;
		exponent = strtol(cp, &endptr, 10);
		if (endptr == cp)
			elog(ERROR, "Bad numeric input format '%s'", str);
		cp = endptr;
		if (exponent > NUMERIC_MAX_PRECISION ||
			exponent < -NUMERIC_MAX_PRECISION)
			elog(ERROR, "Bad numeric input format '%s'", str);
		dest->weight += (int) exponent;
		dest->dscale -= (int) exponent;
		if (dest->dscale < 0)
			dest->dscale = 0;
	}

	/* Should be nothing left but spaces */
	while (*cp)
	{
		if (!isspace((unsigned char) *cp))
			elog(ERROR, "Bad numeric input format '%s'", str);
		cp++;
	}

	/* Strip any leading zeroes */
	while (dest->ndigits > 0 && *(dest->digits) == 0)
	{
		(dest->digits)++;
		(dest->weight)--;
		(dest->ndigits)--;
	}
	if (dest->ndigits == 0)
		dest->weight = 0;

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
	NumericDigit *digit;
	int			i;
	int			n;

	n = num->varlen - NUMERIC_HDRSZ;	/* number of digit-pairs in packed
										 * fmt */

	alloc_var(dest, n * 2);

	dest->weight = num->n_weight;
	dest->rscale = num->n_rscale;
	dest->dscale = NUMERIC_DSCALE(num);
	dest->sign = NUMERIC_SIGN(num);

	digit = dest->digits;

	for (i = 0; i < n; i++)
	{
		unsigned char digitpair = num->n_data[i];

		*digit++ = (digitpair >> 4) & 0x0f;
		*digit++ = digitpair & 0x0f;
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
	NumericDigit *newbuf;

	newbuf = digitbuf_alloc(value->ndigits + 1);
	newbuf[0] = 0;				/* spare digit for rounding */
	memcpy(newbuf + 1, value->digits, value->ndigits);

	digitbuf_free(dest->buf);

	memcpy(dest, value, sizeof(NumericVar));
	dest->buf = newbuf;
	dest->digits = newbuf + 1;
}


/* ----------
 * get_str_from_var() -
 *
 *	Convert a var to text representation (guts of numeric_out).
 *	CAUTION: var's contents may be modified by rounding!
 *	Caller must have checked for NaN case.
 *	Returns a palloc'd string.
 * ----------
 */
static char *
get_str_from_var(NumericVar *var, int dscale)
{
	char	   *str;
	char	   *cp;
	int			i;
	int			d;

	/*
	 * Check if we must round up before printing the value and do so.
	 */
	i = dscale + var->weight + 1;
	if (i >= 0 && var->ndigits > i)
	{
		int			carry = (var->digits[i] > 4) ? 1 : 0;

		var->ndigits = i;

		while (carry)
		{
			carry += var->digits[--i];
			var->digits[i] = carry % 10;
			carry /= 10;
		}

		if (i < 0)
		{
			Assert(i == -1);	/* better not have added more than 1 digit */
			Assert(var->digits > var->buf);
			var->digits--;
			var->ndigits++;
			var->weight++;
		}
	}
	else
		var->ndigits = Max(0, Min(i, var->ndigits));

	/*
	 * Allocate space for the result
	 */
	str = palloc(Max(0, dscale) + Max(0, var->weight) + 4);
	cp = str;

	/*
	 * Output a dash for negative values
	 */
	if (var->sign == NUMERIC_NEG)
		*cp++ = '-';

	/*
	 * Output all digits before the decimal point
	 */
	i = Max(var->weight, 0);
	d = 0;

	while (i >= 0)
	{
		if (i <= var->weight && d < var->ndigits)
			*cp++ = var->digits[d++] + '0';
		else
			*cp++ = '0';
		i--;
	}

	/*
	 * If requested, output a decimal point and all the digits that follow
	 * it.
	 */
	if (dscale > 0)
	{
		*cp++ = '.';
		while (i >= -dscale)
		{
			if (i <= var->weight && d < var->ndigits)
				*cp++ = var->digits[d++] + '0';
			else
				*cp++ = '0';
			i--;
		}
	}

	/*
	 * terminate the string and return it
	 */
	*cp = '\0';
	return str;
}


/* ----------
 * make_result() -
 *
 *	Create the packed db numeric format in palloc()'d memory from
 *	a variable.  The var's rscale determines the number of digits kept.
 * ----------
 */
static Numeric
make_result(NumericVar *var)
{
	Numeric		result;
	NumericDigit *digit = var->digits;
	int			weight = var->weight;
	int			sign = var->sign;
	int			n;
	int			i,
				j;

	if (sign == NUMERIC_NAN)
	{
		result = (Numeric) palloc(NUMERIC_HDRSZ);

		result->varlen = NUMERIC_HDRSZ;
		result->n_weight = 0;
		result->n_rscale = 0;
		result->n_sign_dscale = NUMERIC_NAN;

		dump_numeric("make_result()", result);
		return result;
	}

	n = Max(0, Min(var->ndigits, var->weight + var->rscale + 1));

	/* truncate leading zeroes */
	while (n > 0 && *digit == 0)
	{
		digit++;
		weight--;
		n--;
	}
	/* truncate trailing zeroes */
	while (n > 0 && digit[n - 1] == 0)
		n--;

	/* If zero result, force to weight=0 and positive sign */
	if (n == 0)
	{
		weight = 0;
		sign = NUMERIC_POS;
	}

	result = (Numeric) palloc(NUMERIC_HDRSZ + (n + 1) / 2);
	result->varlen = NUMERIC_HDRSZ + (n + 1) / 2;
	result->n_weight = weight;
	result->n_rscale = var->rscale;
	result->n_sign_dscale = sign |
		((uint16) var->dscale & NUMERIC_DSCALE_MASK);

	i = 0;
	j = 0;
	while (j < n)
	{
		unsigned char digitpair = digit[j++] << 4;

		if (j < n)
			digitpair |= digit[j++];
		result->n_data[i++] = digitpair;
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

	/* Do nothing if we have a default typmod (-1) */
	if (typmod < (int32) (VARHDRSZ))
		return;

	typmod -= VARHDRSZ;
	precision = (typmod >> 16) & 0xffff;
	scale = typmod & 0xffff;
	maxweight = precision - scale;

	/* Round to target scale */
	i = scale + var->weight + 1;
	if (i >= 0 && var->ndigits > i)
	{
		int			carry = (var->digits[i] > 4) ? 1 : 0;

		var->ndigits = i;

		while (carry)
		{
			carry += var->digits[--i];
			var->digits[i] = carry % 10;
			carry /= 10;
		}

		if (i < 0)
		{
			Assert(i == -1);	/* better not have added more than 1 digit */
			Assert(var->digits > var->buf);
			var->digits--;
			var->ndigits++;
			var->weight++;
		}
	}
	else
		var->ndigits = Max(0, Min(i, var->ndigits));

	/*
	 * Check for overflow - note we can't do this before rounding, because
	 * rounding could raise the weight.  Also note that the var's weight
	 * could be inflated by leading zeroes, which will be stripped before
	 * storage but perhaps might not have been yet. In any case, we must
	 * recognize a true zero, whose weight doesn't mean anything.
	 */
	if (var->weight >= maxweight)
	{
		/* Determine true weight; and check for all-zero result */
		int			tweight = var->weight;

		for (i = 0; i < var->ndigits; i++)
		{
			if (var->digits[i])
				break;
			tweight--;
		}

		if (tweight >= maxweight && i < var->ndigits)
			elog(ERROR, "overflow on numeric "
			  "ABS(value) >= 10^%d for field with precision %d scale %d",
				 tweight, precision, scale);
	}

	var->rscale = scale;
	var->dscale = scale;
}

/* Convert numeric to float8; if out of range, return +/- HUGE_VAL */
/* Caller should have eliminated the possibility of NAN */
static double
numeric_to_double_no_overflow(Numeric num)
{
	char	   *tmp;
	double		val;
	char	   *endptr;

	tmp = DatumGetCString(DirectFunctionCall1(numeric_out,
											  NumericGetDatum(num)));

	/* unlike float8in, we ignore ERANGE from strtod */
	val = strtod(tmp, &endptr);
	if (*endptr != '\0')
	{
		/* shouldn't happen ... */
		elog(ERROR, "Bad float8 input format '%s'", tmp);
	}

	pfree(tmp);

	return val;
}

/* As above, but work from a NumericVar */
static double
numericvar_to_double_no_overflow(NumericVar *var)
{
	char	   *tmp;
	double		val;
	char	   *endptr;

	tmp = get_str_from_var(var, var->dscale);

	/* unlike float8in, we ignore ERANGE from strtod */
	val = strtod(tmp, &endptr);
	if (*endptr != '\0')
	{
		/* shouldn't happen ... */
		elog(ERROR, "Bad float8 input format '%s'", tmp);
	}

	pfree(tmp);

	return val;
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
	/*
	 * Decide on the signs of the two variables what to do
	 */
	if (var1->sign == NUMERIC_POS)
	{
		if (var2->sign == NUMERIC_POS)
		{
			/*
			 * Both are positive result = +(ABS(var1) + ABS(var2))
			 */
			add_abs(var1, var2, result);
			result->sign = NUMERIC_POS;
		}
		else
		{
			/*
			 * var1 is positive, var2 is negative Must compare absolute
			 * values
			 */
			switch (cmp_abs(var1, var2))
			{
				case 0:
					/* ----------
					 * ABS(var1) == ABS(var2)
					 * result = ZERO
					 * ----------
					 */
					zero_var(result);
					result->rscale = Max(var1->rscale, var2->rscale);
					result->dscale = Max(var1->dscale, var2->dscale);
					break;

				case 1:
					/* ----------
					 * ABS(var1) > ABS(var2)
					 * result = +(ABS(var1) - ABS(var2))
					 * ----------
					 */
					sub_abs(var1, var2, result);
					result->sign = NUMERIC_POS;
					break;

				case -1:
					/* ----------
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
				case 0:
					/* ----------
					 * ABS(var1) == ABS(var2)
					 * result = ZERO
					 * ----------
					 */
					zero_var(result);
					result->rscale = Max(var1->rscale, var2->rscale);
					result->dscale = Max(var1->dscale, var2->dscale);
					break;

				case 1:
					/* ----------
					 * ABS(var1) > ABS(var2)
					 * result = -(ABS(var1) - ABS(var2))
					 * ----------
					 */
					sub_abs(var1, var2, result);
					result->sign = NUMERIC_NEG;
					break;

				case -1:
					/* ----------
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
	/*
	 * Decide on the signs of the two variables what to do
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
				case 0:
					/* ----------
					 * ABS(var1) == ABS(var2)
					 * result = ZERO
					 * ----------
					 */
					zero_var(result);
					result->rscale = Max(var1->rscale, var2->rscale);
					result->dscale = Max(var1->dscale, var2->dscale);
					break;

				case 1:
					/* ----------
					 * ABS(var1) > ABS(var2)
					 * result = +(ABS(var1) - ABS(var2))
					 * ----------
					 */
					sub_abs(var1, var2, result);
					result->sign = NUMERIC_POS;
					break;

				case -1:
					/* ----------
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
				case 0:
					/* ----------
					 * ABS(var1) == ABS(var2)
					 * result = ZERO
					 * ----------
					 */
					zero_var(result);
					result->rscale = Max(var1->rscale, var2->rscale);
					result->dscale = Max(var1->dscale, var2->dscale);
					break;

				case 1:
					/* ----------
					 * ABS(var1) > ABS(var2)
					 * result = -(ABS(var1) - ABS(var2))
					 * ----------
					 */
					sub_abs(var1, var2, result);
					result->sign = NUMERIC_NEG;
					break;

				case -1:
					/* ----------
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
 *	in result.  Accuracy of result is determined by global_rscale.
 * ----------
 */
static void
mul_var(NumericVar *var1, NumericVar *var2, NumericVar *result)
{
	NumericDigit *res_buf;
	NumericDigit *res_digits;
	int			res_ndigits;
	int			res_weight;
	int			res_sign;
	int			i,
				ri,
				i1,
				i2;
	long		sum = 0;

	res_weight = var1->weight + var2->weight + 2;
	res_ndigits = var1->ndigits + var2->ndigits + 1;
	if (var1->sign == var2->sign)
		res_sign = NUMERIC_POS;
	else
		res_sign = NUMERIC_NEG;

	res_buf = digitbuf_alloc(res_ndigits);
	res_digits = res_buf;
	memset(res_digits, 0, res_ndigits);

	ri = res_ndigits;
	for (i1 = var1->ndigits - 1; i1 >= 0; i1--)
	{
		sum = 0;
		i = --ri;

		for (i2 = var2->ndigits - 1; i2 >= 0; i2--)
		{
			sum += res_digits[i] + var1->digits[i1] * var2->digits[i2];
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
		res_ndigits--;

	if (res_ndigits == 0)
	{
		res_sign = NUMERIC_POS;
		res_weight = 0;
	}

	digitbuf_free(result->buf);
	result->buf = res_buf;
	result->digits = res_digits;
	result->ndigits = res_ndigits;
	result->weight = res_weight;
	result->rscale = global_rscale;
	result->sign = res_sign;
}


/* ----------
 * div_var() -
 *
 *	Division on variable level.  Accuracy of result is determined by
 *	global_rscale.
 * ----------
 */
static void
div_var(NumericVar *var1, NumericVar *var2, NumericVar *result)
{
	NumericDigit *res_digits;
	int			res_ndigits;
	int			res_sign;
	int			res_weight;
	NumericVar	dividend;
	NumericVar	divisor[10];
	int			ndigits_tmp;
	int			weight_tmp;
	int			rscale_tmp;
	int			ri;
	int			i;
	long		guess;
	long		first_have;
	long		first_div;
	int			first_nextdigit;
	int			stat = 0;

	/*
	 * First of all division by zero check
	 */
	ndigits_tmp = var2->ndigits + 1;
	if (ndigits_tmp == 1)
		elog(ERROR, "division by zero");

	/*
	 * Determine the result sign, weight and number of digits to calculate
	 */
	if (var1->sign == var2->sign)
		res_sign = NUMERIC_POS;
	else
		res_sign = NUMERIC_NEG;
	res_weight = var1->weight - var2->weight + 1;
	res_ndigits = global_rscale + res_weight;
	if (res_ndigits <= 0)
		res_ndigits = 1;

	/*
	 * Now result zero check
	 */
	if (var1->ndigits == 0)
	{
		zero_var(result);
		result->rscale = global_rscale;
		return;
	}

	/*
	 * Initialize local variables
	 */
	init_var(&dividend);
	for (i = 1; i < 10; i++)
		init_var(&divisor[i]);

	/*
	 * Make a copy of the divisor which has one leading zero digit
	 */
	divisor[1].ndigits = ndigits_tmp;
	divisor[1].rscale = var2->ndigits;
	divisor[1].sign = NUMERIC_POS;
	divisor[1].buf = digitbuf_alloc(ndigits_tmp);
	divisor[1].digits = divisor[1].buf;
	divisor[1].digits[0] = 0;
	memcpy(&(divisor[1].digits[1]), var2->digits, ndigits_tmp - 1);

	/*
	 * Make a copy of the dividend
	 */
	dividend.ndigits = var1->ndigits;
	dividend.weight = 0;
	dividend.rscale = var1->ndigits;
	dividend.sign = NUMERIC_POS;
	dividend.buf = digitbuf_alloc(var1->ndigits);
	dividend.digits = dividend.buf;
	memcpy(dividend.digits, var1->digits, var1->ndigits);

	/*
	 * Setup the result
	 */
	digitbuf_free(result->buf);
	result->buf = digitbuf_alloc(res_ndigits + 2);
	res_digits = result->buf;
	result->digits = res_digits;
	result->ndigits = res_ndigits;
	result->weight = res_weight;
	result->rscale = global_rscale;
	result->sign = res_sign;
	res_digits[0] = 0;

	first_div = divisor[1].digits[1] * 10;
	if (ndigits_tmp > 2)
		first_div += divisor[1].digits[2];

	first_have = 0;
	first_nextdigit = 0;

	weight_tmp = 1;
	rscale_tmp = divisor[1].rscale;

	for (ri = 0; ri <= res_ndigits; ri++)
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
				int			i;
				long		sum = 0;

				memcpy(&divisor[guess], &divisor[1], sizeof(NumericVar));
				divisor[guess].buf = digitbuf_alloc(divisor[guess].ndigits);
				divisor[guess].digits = divisor[guess].buf;
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
			if (stat >= 0)
				break;

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
		int			carry = (res_digits[ri] > 4) ? 1 : 0;

		result->ndigits = ri;
		res_digits[ri] = 0;

		while (carry && ri > 0)
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
		(result->ndigits)--;
	if (result->ndigits == 0)
		result->sign = NUMERIC_POS;

	/*
	 * Tidy up
	 */
	digitbuf_free(dividend.buf);
	for (i = 1; i < 10; i++)
		digitbuf_free(divisor[i].buf);
}


/*
 * Default scale selection for division
 *
 * Returns the appropriate display scale for the division result,
 * and sets global_rscale to the result scale to use during div_var.
 *
 * Note that this must be called before div_var.
 */
static int
select_div_scale(NumericVar *var1, NumericVar *var2)
{
	int			weight1,
				weight2,
				qweight,
				i;
	NumericDigit firstdigit1,
				firstdigit2;
	int			res_dscale;
	int			res_rscale;

	/*
	 * The result scale of a division isn't specified in any SQL standard.
	 * For PostgreSQL we select a display scale that will give at least
	 * NUMERIC_MIN_SIG_DIGITS significant digits, so that numeric gives a
	 * result no less accurate than float8; but use a scale not less than
	 * either input's display scale.
	 *
	 * The result scale is NUMERIC_EXTRA_DIGITS more than the display scale,
	 * to provide some guard digits in the calculation.
	 */

	/* Get the actual (normalized) weight and first digit of each input */

	weight1 = 0;				/* values to use if var1 is zero */
	firstdigit1 = 0;
	for (i = 0; i < var1->ndigits; i++)
	{
		firstdigit1 = var1->digits[i];
		if (firstdigit1 != 0)
		{
			weight1 = var1->weight - i;
			break;
		}
	}

	weight2 = 0;				/* values to use if var2 is zero */
	firstdigit2 = 0;
	for (i = 0; i < var2->ndigits; i++)
	{
		firstdigit2 = var2->digits[i];
		if (firstdigit2 != 0)
		{
			weight2 = var2->weight - i;
			break;
		}
	}

	/*
	 * Estimate weight of quotient.  If the two first digits are equal,
	 * we can't be sure, but assume that var1 is less than var2.
	 */
	qweight = weight1 - weight2;
	if (firstdigit1 <= firstdigit2)
		qweight--;

	/* Select display scale */
	res_dscale = NUMERIC_MIN_SIG_DIGITS - qweight;
	res_dscale = Max(res_dscale, var1->dscale);
	res_dscale = Max(res_dscale, var2->dscale);
	res_dscale = Max(res_dscale, NUMERIC_MIN_DISPLAY_SCALE);
	res_dscale = Min(res_dscale, NUMERIC_MAX_DISPLAY_SCALE);

	/* Select result scale */
	res_rscale = res_dscale + NUMERIC_EXTRA_DIGITS;
	global_rscale = res_rscale;

	return res_dscale;
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
	NumericVar	tmp;
	int			save_global_rscale;
	int			div_dscale;

	init_var(&tmp);

	/* ---------
	 * We do this using the equation
	 *		mod(x,y) = x - trunc(x/y)*y
	 * We set global_rscale the same way numeric_div and numeric_mul do
	 * to get the right answer from the equation.  The final result,
	 * however, need not be displayed to more precision than the inputs.
	 * ----------
	 */
	save_global_rscale = global_rscale;

	div_dscale = select_div_scale(var1, var2);

	div_var(var1, var2, &tmp);

	tmp.dscale = div_dscale;

	/* do trunc() by forgetting digits to the right of the decimal point */
	tmp.ndigits = Max(0, Min(tmp.ndigits, tmp.weight + 1));

	global_rscale = var2->rscale + tmp.rscale;

	mul_var(var2, &tmp, &tmp);

	sub_var(var1, &tmp, result);

	result->dscale = Max(var1->dscale, var2->dscale);

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
	NumericVar	tmp;

	init_var(&tmp);
	set_var_from_var(var, &tmp);

	tmp.rscale = 0;
	tmp.ndigits = Min(tmp.ndigits, Max(0, tmp.weight + 1));
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
	NumericVar	tmp;

	init_var(&tmp);
	set_var_from_var(var, &tmp);

	tmp.rscale = 0;
	tmp.ndigits = Min(tmp.ndigits, Max(0, tmp.weight + 1));
	if (tmp.sign == NUMERIC_NEG && cmp_var(var, &tmp) != 0)
		sub_var(&tmp, &const_one, &tmp);

	set_var_from_var(&tmp, result);
	free_var(&tmp);
}


/* ----------
 * sqrt_var() -
 *
 *	Compute the square root of x using Newton's algorithm
 * ----------
 */
static void
sqrt_var(NumericVar *arg, NumericVar *result)
{
	NumericVar	tmp_arg;
	NumericVar	tmp_val;
	NumericVar	last_val;
	int			res_rscale;
	int			save_global_rscale;
	int			stat;

	save_global_rscale = global_rscale;
	global_rscale += 8;
	res_rscale = global_rscale;

	stat = cmp_var(arg, &const_zero);
	if (stat == 0)
	{
		set_var_from_var(&const_zero, result);
		result->rscale = res_rscale;
		result->sign = NUMERIC_POS;
		global_rscale = save_global_rscale;
		return;
	}

	if (stat < 0)
		elog(ERROR, "math error on numeric - cannot compute SQRT of negative value");

	init_var(&tmp_arg);
	init_var(&tmp_val);
	init_var(&last_val);

	/* Copy arg in case it is the same var as result */
	set_var_from_var(arg, &tmp_arg);

	/*
	 * Initialize the result to the first guess
	 */
	digitbuf_free(result->buf);
	result->buf = digitbuf_alloc(1);
	result->digits = result->buf;
	result->digits[0] = tmp_arg.digits[0] / 2;
	if (result->digits[0] == 0)
		result->digits[0] = 1;
	result->ndigits = 1;
	result->weight = tmp_arg.weight / 2;
	result->rscale = res_rscale;
	result->sign = NUMERIC_POS;

	set_var_from_var(result, &last_val);

	for (;;)
	{
		div_var(&tmp_arg, result, &tmp_val);

		add_var(result, &tmp_val, result);
		div_var(result, &const_two, result);

		if (cmp_var(&last_val, result) == 0)
			break;
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
	NumericVar	x;
	NumericVar	xpow;
	NumericVar	ifac;
	NumericVar	elem;
	NumericVar	ni;
	int			d;
	int			i;
	int			xintval;
	int			ndiv2 = 0;
	bool		xneg = FALSE;
	int			save_global_rscale;

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

	/* Select an appropriate scale for internal calculation */
	xintval = 0;
	for (i = x.weight, d = 0; i >= 0; i--, d++)
	{
		xintval *= 10;
		if (d < x.ndigits)
			xintval += x.digits[d];
		if (xintval >= NUMERIC_MAX_RESULT_SCALE)
			elog(ERROR, "argument for EXP() too big");
	}

	save_global_rscale = global_rscale;
	global_rscale += xintval / 2 + 8;

	/* Reduce input into range 0 <= x <= 0.1 */
	while (cmp_var(&x, &const_zero_point_one) > 0)
	{
		ndiv2++;
		global_rscale++;
		div_var(&x, &const_two, &x);
	}

	/*
	 * Use the Taylor series
	 *
	 *		exp(x) = 1 + x + x^2/2! + x^3/3! + ...
	 *
	 * Given the limited range of x, this should converge reasonably quickly.
	 * We run the series until the terms fall below the global_rscale limit.
	 */
	add_var(&const_one, &x, result);
	set_var_from_var(&x, &xpow);
	set_var_from_var(&const_one, &ifac);
	set_var_from_var(&const_one, &ni);

	for (;;)
	{
		add_var(&ni, &const_one, &ni);
		mul_var(&xpow, &x, &xpow);
		mul_var(&ifac, &ni, &ifac);
		div_var(&xpow, &ifac, &elem);

		if (elem.ndigits == 0)
			break;

		add_var(result, &elem, result);
	}

	/* Compensate for argument range reduction */
	while (ndiv2-- > 0)
		mul_var(result, result, result);

	/* Compensate for input sign, and round to caller's global_rscale */
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
	NumericVar	x;
	NumericVar	xx;
	NumericVar	ni;
	NumericVar	elem;
	NumericVar	fact;
	int			save_global_rscale;

	if (cmp_var(arg, &const_zero) <= 0)
		elog(ERROR, "math error on numeric - cannot compute LN of value <= zero");

	save_global_rscale = global_rscale;
	global_rscale += 8;

	init_var(&x);
	init_var(&xx);
	init_var(&ni);
	init_var(&elem);
	init_var(&fact);

	set_var_from_var(&const_two, &fact);
	set_var_from_var(arg, &x);

	/* Reduce input into range 0.9 < x < 1.1 */
	while (cmp_var(&x, &const_zero_point_nine) <= 0)
	{
		global_rscale++;
		sqrt_var(&x, &x);
		mul_var(&fact, &const_two, &fact);
	}
	while (cmp_var(&x, &const_one_point_one) >= 0)
	{
		global_rscale++;
		sqrt_var(&x, &x);
		mul_var(&fact, &const_two, &fact);
	}

	/*
	 * We use the Taylor series for 0.5 * ln((1+z)/(1-z)),
	 *
	 *		z + z^3/3 + z^5/5 + ...
	 *
	 * where z = (x-1)/(x+1) is in the range (approximately) -0.053 .. 0.048
	 * due to the above range-reduction of x.
	 *
	 * The convergence of this is not as fast as one would like, but is
	 * tolerable given that z is small.
	 */
	sub_var(&x, &const_one, result);
	add_var(&x, &const_one, &elem);
	div_var(result, &elem, result);
	set_var_from_var(result, &xx);
	mul_var(result, result, &x);

	set_var_from_var(&const_one, &ni);

	for (;;)
	{
		add_var(&ni, &const_two, &ni);
		mul_var(&xx, &x, &xx);
		div_var(&xx, &ni, &elem);

		if (elem.ndigits == 0)
			break;

		add_var(result, &elem, result);
	}

	/* Compensate for argument range reduction, round to caller's rscale */
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
 *	Compute the logarithm of num in a given base.
 *
 *	Note: this routine chooses rscale and dscale of the result.
 * ----------
 */
static void
log_var(NumericVar *base, NumericVar *num, NumericVar *result)
{
	NumericVar	ln_base;
	NumericVar	ln_num;
	int			save_global_rscale = global_rscale;
	int			res_dscale;

	init_var(&ln_base);
	init_var(&ln_num);

	/* Set scale for ln() calculations */
	if (num->weight > 0)
		res_dscale = NUMERIC_MIN_SIG_DIGITS - (int) log10(num->weight);
	else if (num->weight < 0)
		res_dscale = NUMERIC_MIN_SIG_DIGITS - (int) log10(- num->weight);
	else
		res_dscale = NUMERIC_MIN_SIG_DIGITS;

	res_dscale = Max(res_dscale, base->dscale);
	res_dscale = Max(res_dscale, num->dscale);
	res_dscale = Max(res_dscale, NUMERIC_MIN_DISPLAY_SCALE);
	res_dscale = Min(res_dscale, NUMERIC_MAX_DISPLAY_SCALE);

	global_rscale = res_dscale + 8;

	/* Form natural logarithms */
	ln_var(base, &ln_base);
	ln_var(num, &ln_num);

	ln_base.dscale = res_dscale;
	ln_num.dscale = res_dscale;

	/* Select scale for division result */
	res_dscale = select_div_scale(&ln_num, &ln_base);

	div_var(&ln_num, &ln_base, result);

	result->dscale = res_dscale;

	global_rscale = save_global_rscale;

	free_var(&ln_num);
	free_var(&ln_base);
}


/* ----------
 * power_var() -
 *
 *	Raise base to the power of exp
 *
 *	Note: this routine chooses rscale and dscale of the result.
 * ----------
 */
static void
power_var(NumericVar *base, NumericVar *exp, NumericVar *result)
{
	NumericVar	ln_base;
	NumericVar	ln_num;
	int			save_global_rscale = global_rscale;
	int			res_dscale;
	double		val;

	init_var(&ln_base);
	init_var(&ln_num);

	/* Set scale for ln() calculation --- need extra accuracy here */
	if (base->weight > 0)
		res_dscale = NUMERIC_MIN_SIG_DIGITS*2 - (int) log10(base->weight);
	else if (base->weight < 0)
		res_dscale = NUMERIC_MIN_SIG_DIGITS*2 - (int) log10(- base->weight);
	else
		res_dscale = NUMERIC_MIN_SIG_DIGITS*2;

	res_dscale = Max(res_dscale, base->dscale * 2);
	res_dscale = Max(res_dscale, exp->dscale * 2);
	res_dscale = Max(res_dscale, NUMERIC_MIN_DISPLAY_SCALE);
	res_dscale = Min(res_dscale, NUMERIC_MAX_DISPLAY_SCALE);

	global_rscale = res_dscale + 8;

	ln_var(base, &ln_base);

	ln_base.dscale = res_dscale;

	mul_var(&ln_base, exp, &ln_num);

	ln_num.dscale = res_dscale;

	/* Set scale for exp() */

	/* convert input to float8, ignoring overflow */
	val = numericvar_to_double_no_overflow(&ln_num);

	/* log10(result) = num * log10(e), so this is approximately the weight: */
	val *= 0.434294481903252;

	/* limit to something that won't cause integer overflow */
	val = Max(val, -NUMERIC_MAX_RESULT_SCALE);
	val = Min(val, NUMERIC_MAX_RESULT_SCALE);

	res_dscale = NUMERIC_MIN_SIG_DIGITS - (int) val;
	res_dscale = Max(res_dscale, base->dscale);
	res_dscale = Max(res_dscale, exp->dscale);
	res_dscale = Max(res_dscale, NUMERIC_MIN_DISPLAY_SCALE);
	res_dscale = Min(res_dscale, NUMERIC_MAX_DISPLAY_SCALE);

	global_rscale = res_dscale + 8;

	exp_var(&ln_num, result);

	result->dscale = res_dscale;

	global_rscale = save_global_rscale;

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
	int			i1 = 0;
	int			i2 = 0;
	int			w1 = var1->weight;
	int			w2 = var2->weight;
	int			stat;

	while (w1 > w2 && i1 < var1->ndigits)
	{
		if (var1->digits[i1++] != 0)
			return 1;
		w1--;
	}
	while (w2 > w1 && i2 < var2->ndigits)
	{
		if (var2->digits[i2++] != 0)
			return -1;
		w2--;
	}

	if (w1 == w2)
	{
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
	NumericDigit *res_buf;
	NumericDigit *res_digits;
	int			res_ndigits;
	int			res_weight;
	int			res_rscale;
	int			res_dscale;
	int			i,
				i1,
				i2;
	int			carry = 0;

	/* copy these values into local vars for speed in inner loop */
	int			var1ndigits = var1->ndigits;
	int			var2ndigits = var2->ndigits;
	NumericDigit *var1digits = var1->digits;
	NumericDigit *var2digits = var2->digits;

	res_weight = Max(var1->weight, var2->weight) + 1;
	res_rscale = Max(var1->rscale, var2->rscale);
	res_dscale = Max(var1->dscale, var2->dscale);
	res_ndigits = res_rscale + res_weight + 1;
	if (res_ndigits <= 0)
		res_ndigits = 1;

	res_buf = digitbuf_alloc(res_ndigits);
	res_digits = res_buf;

	i1 = res_rscale + var1->weight + 1;
	i2 = res_rscale + var2->weight + 1;
	for (i = res_ndigits - 1; i >= 0; i--)
	{
		i1--;
		i2--;
		if (i1 >= 0 && i1 < var1ndigits)
			carry += var1digits[i1];
		if (i2 >= 0 && i2 < var2ndigits)
			carry += var2digits[i2];

		if (carry >= 10)
		{
			res_digits[i] = carry - 10;
			carry = 1;
		}
		else
		{
			res_digits[i] = carry;
			carry = 0;
		}
	}

	Assert(carry == 0);			/* else we failed to allow for carry out */

	while (res_ndigits > 0 && *res_digits == 0)
	{
		res_digits++;
		res_weight--;
		res_ndigits--;
	}
	while (res_ndigits > 0 && res_digits[res_ndigits - 1] == 0)
		res_ndigits--;

	if (res_ndigits == 0)
		res_weight = 0;

	digitbuf_free(result->buf);
	result->ndigits = res_ndigits;
	result->buf = res_buf;
	result->digits = res_digits;
	result->weight = res_weight;
	result->rscale = res_rscale;
	result->dscale = res_dscale;
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
	NumericDigit *res_buf;
	NumericDigit *res_digits;
	int			res_ndigits;
	int			res_weight;
	int			res_rscale;
	int			res_dscale;
	int			i,
				i1,
				i2;
	int			borrow = 0;

	/* copy these values into local vars for speed in inner loop */
	int			var1ndigits = var1->ndigits;
	int			var2ndigits = var2->ndigits;
	NumericDigit *var1digits = var1->digits;
	NumericDigit *var2digits = var2->digits;

	res_weight = var1->weight;
	res_rscale = Max(var1->rscale, var2->rscale);
	res_dscale = Max(var1->dscale, var2->dscale);
	res_ndigits = res_rscale + res_weight + 1;
	if (res_ndigits <= 0)
		res_ndigits = 1;

	res_buf = digitbuf_alloc(res_ndigits);
	res_digits = res_buf;

	i1 = res_rscale + var1->weight + 1;
	i2 = res_rscale + var2->weight + 1;
	for (i = res_ndigits - 1; i >= 0; i--)
	{
		i1--;
		i2--;
		if (i1 >= 0 && i1 < var1ndigits)
			borrow += var1digits[i1];
		if (i2 >= 0 && i2 < var2ndigits)
			borrow -= var2digits[i2];

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

	Assert(borrow == 0);		/* else caller gave us var1 < var2 */

	while (res_ndigits > 0 && *res_digits == 0)
	{
		res_digits++;
		res_weight--;
		res_ndigits--;
	}
	while (res_ndigits > 0 && res_digits[res_ndigits - 1] == 0)
		res_ndigits--;

	if (res_ndigits == 0)
		res_weight = 0;

	digitbuf_free(result->buf);
	result->ndigits = res_ndigits;
	result->buf = res_buf;
	result->digits = res_digits;
	result->weight = res_weight;
	result->rscale = res_rscale;
	result->dscale = res_dscale;
}
