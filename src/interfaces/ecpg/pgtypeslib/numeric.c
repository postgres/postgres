/* src/interfaces/ecpg/pgtypeslib/numeric.c */

#include "postgres_fe.h"

#include <ctype.h>
#include <float.h>
#include <limits.h>

#include "pgtypes_error.h"
#include "pgtypes_numeric.h"
#include "pgtypeslib_extern.h"

#define Max(x, y)				((x) > (y) ? (x) : (y))
#define Min(x, y)				((x) < (y) ? (x) : (y))

#define init_var(v)				memset(v,0,sizeof(numeric))

#define digitbuf_alloc(size) ((NumericDigit *) pgtypes_alloc(size))
#define digitbuf_free(buf)		\
	   do { \
				 if ((buf) != NULL) \
						  free(buf); \
		  } while (0)


/* ----------
 *	alloc_var() -
 *
 *	 Allocate a digit buffer of ndigits digits (plus a spare digit for rounding)
 * ----------
 */
static int
alloc_var(numeric *var, int ndigits)
{
	digitbuf_free(var->buf);
	var->buf = digitbuf_alloc(ndigits + 1);
	if (var->buf == NULL)
		return -1;
	var->buf[0] = 0;
	var->digits = var->buf + 1;
	var->ndigits = ndigits;
	return 0;
}

numeric *
PGTYPESnumeric_new(void)
{
	numeric    *var;

	if ((var = (numeric *) pgtypes_alloc(sizeof(numeric))) == NULL)
		return NULL;

	if (alloc_var(var, 0) < 0)
	{
		free(var);
		return NULL;
	}

	return var;
}

decimal *
PGTYPESdecimal_new(void)
{
	decimal    *var;

	if ((var = (decimal *) pgtypes_alloc(sizeof(decimal))) == NULL)
		return NULL;

	memset(var, 0, sizeof(decimal));

	return var;
}

/* ----------
 * set_var_from_str()
 *
 *	Parse a string and put the number into a variable
 * ----------
 */
static int
set_var_from_str(char *str, char **ptr, numeric *dest)
{
	bool		have_dp = false;
	int			i = 0;

	errno = 0;
	*ptr = str;
	while (*(*ptr))
	{
		if (!isspace((unsigned char) *(*ptr)))
			break;
		(*ptr)++;
	}

	if (pg_strncasecmp(*ptr, "NaN", 3) == 0)
	{
		*ptr += 3;
		dest->sign = NUMERIC_NAN;

		/* Should be nothing left but spaces */
		while (*(*ptr))
		{
			if (!isspace((unsigned char) *(*ptr)))
			{
				errno = PGTYPES_NUM_BAD_NUMERIC;
				return -1;
			}
			(*ptr)++;
		}

		return 0;
	}

	if (alloc_var(dest, strlen((*ptr))) < 0)
		return -1;
	dest->weight = -1;
	dest->dscale = 0;
	dest->sign = NUMERIC_POS;

	switch (*(*ptr))
	{
		case '+':
			dest->sign = NUMERIC_POS;
			(*ptr)++;
			break;

		case '-':
			dest->sign = NUMERIC_NEG;
			(*ptr)++;
			break;
	}

	if (*(*ptr) == '.')
	{
		have_dp = true;
		(*ptr)++;
	}

	if (!isdigit((unsigned char) *(*ptr)))
	{
		errno = PGTYPES_NUM_BAD_NUMERIC;
		return -1;
	}

	while (*(*ptr))
	{
		if (isdigit((unsigned char) *(*ptr)))
		{
			dest->digits[i++] = *(*ptr)++ - '0';
			if (!have_dp)
				dest->weight++;
			else
				dest->dscale++;
		}
		else if (*(*ptr) == '.')
		{
			if (have_dp)
			{
				errno = PGTYPES_NUM_BAD_NUMERIC;
				return -1;
			}
			have_dp = true;
			(*ptr)++;
		}
		else
			break;
	}
	dest->ndigits = i;

	/* Handle exponent, if any */
	if (*(*ptr) == 'e' || *(*ptr) == 'E')
	{
		long		exponent;
		char	   *endptr;

		(*ptr)++;
		exponent = strtol(*ptr, &endptr, 10);
		if (endptr == (*ptr))
		{
			errno = PGTYPES_NUM_BAD_NUMERIC;
			return -1;
		}
		(*ptr) = endptr;
		if (exponent >= INT_MAX / 2 || exponent <= -(INT_MAX / 2))
		{
			errno = PGTYPES_NUM_BAD_NUMERIC;
			return -1;
		}
		dest->weight += (int) exponent;
		dest->dscale -= (int) exponent;
		if (dest->dscale < 0)
			dest->dscale = 0;
	}

	/* Should be nothing left but spaces */
	while (*(*ptr))
	{
		if (!isspace((unsigned char) *(*ptr)))
		{
			errno = PGTYPES_NUM_BAD_NUMERIC;
			return -1;
		}
		(*ptr)++;
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
	return 0;
}


/* ----------
 * get_str_from_var() -
 *
 *	Convert a var to text representation (guts of numeric_out).
 *	CAUTION: var's contents may be modified by rounding!
 * ----------
 */
static char *
get_str_from_var(numeric *var, int dscale)
{
	char	   *str;
	char	   *cp;
	int			i;
	int			d;

	if (var->sign == NUMERIC_NAN)
	{
		str = (char *) pgtypes_alloc(4);
		if (str == NULL)
			return NULL;
		sprintf(str, "NaN");
		return str;
	}

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
	if ((str = (char *) pgtypes_alloc(Max(0, dscale) + Max(0, var->weight) + 4)) == NULL)
		return NULL;
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
	 * If requested, output a decimal point and all the digits that follow it.
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

numeric *
PGTYPESnumeric_from_asc(char *str, char **endptr)
{
	numeric    *value = (numeric *) pgtypes_alloc(sizeof(numeric));
	int			ret;

	char	   *realptr;
	char	  **ptr = (endptr != NULL) ? endptr : &realptr;

	if (!value)
		return NULL;

	ret = set_var_from_str(str, ptr, value);
	if (ret)
	{
		PGTYPESnumeric_free(value);
		return NULL;
	}

	return value;
}

char *
PGTYPESnumeric_to_asc(numeric *num, int dscale)
{
	numeric    *numcopy = PGTYPESnumeric_new();
	char	   *s;

	if (numcopy == NULL)
		return NULL;

	if (PGTYPESnumeric_copy(num, numcopy) < 0)
	{
		PGTYPESnumeric_free(numcopy);
		return NULL;
	}

	if (dscale < 0)
		dscale = num->dscale;

	/* get_str_from_var may change its argument */
	s = get_str_from_var(numcopy, dscale);
	PGTYPESnumeric_free(numcopy);
	return s;
}

/* ----------
 * zero_var() -
 *
 *	Set a variable to ZERO.
 *	Note: rscale and dscale are not touched.
 * ----------
 */
static void
zero_var(numeric *var)
{
	digitbuf_free(var->buf);
	var->buf = NULL;
	var->digits = NULL;
	var->ndigits = 0;
	var->weight = 0;			/* by convention; doesn't really matter */
	var->sign = NUMERIC_POS;	/* anything but NAN... */
}

void
PGTYPESnumeric_free(numeric *var)
{
	digitbuf_free(var->buf);
	free(var);
}

void
PGTYPESdecimal_free(decimal *var)
{
	free(var);
}

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
cmp_abs(numeric *var1, numeric *var2)
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
static int
add_abs(numeric *var1, numeric *var2, numeric *result)
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

	if ((res_buf = digitbuf_alloc(res_ndigits)) == NULL)
		return -1;
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

	return 0;
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
static int
sub_abs(numeric *var1, numeric *var2, numeric *result)
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

	if ((res_buf = digitbuf_alloc(res_ndigits)) == NULL)
		return -1;
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

	return 0;
}

/* ----------
 * add_var() -
 *
 *	Full version of add functionality on variable level (handling signs).
 *	result might point to one of the operands too without danger.
 * ----------
 */
int
PGTYPESnumeric_add(numeric *var1, numeric *var2, numeric *result)
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
			if (add_abs(var1, var2, result) != 0)
				return -1;
			result->sign = NUMERIC_POS;
		}
		else
		{
			/*
			 * var1 is positive, var2 is negative Must compare absolute values
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
					if (sub_abs(var1, var2, result) != 0)
						return -1;
					result->sign = NUMERIC_POS;
					break;

				case -1:
					/* ----------
					 * ABS(var1) < ABS(var2)
					 * result = -(ABS(var2) - ABS(var1))
					 * ----------
					 */
					if (sub_abs(var2, var1, result) != 0)
						return -1;
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
					if (sub_abs(var1, var2, result) != 0)
						return -1;
					result->sign = NUMERIC_NEG;
					break;

				case -1:
					/* ----------
					 * ABS(var1) < ABS(var2)
					 * result = +(ABS(var2) - ABS(var1))
					 * ----------
					 */
					if (sub_abs(var2, var1, result) != 0)
						return -1;
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
			if (add_abs(var1, var2, result) != 0)
				return -1;
			result->sign = NUMERIC_NEG;
		}
	}

	return 0;
}


/* ----------
 * sub_var() -
 *
 *	Full version of sub functionality on variable level (handling signs).
 *	result might point to one of the operands too without danger.
 * ----------
 */
int
PGTYPESnumeric_sub(numeric *var1, numeric *var2, numeric *result)
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
			if (add_abs(var1, var2, result) != 0)
				return -1;
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
					if (sub_abs(var1, var2, result) != 0)
						return -1;
					result->sign = NUMERIC_POS;
					break;

				case -1:
					/* ----------
					 * ABS(var1) < ABS(var2)
					 * result = -(ABS(var2) - ABS(var1))
					 * ----------
					 */
					if (sub_abs(var2, var1, result) != 0)
						return -1;
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
					if (sub_abs(var1, var2, result) != 0)
						return -1;
					result->sign = NUMERIC_NEG;
					break;

				case -1:
					/* ----------
					 * ABS(var1) < ABS(var2)
					 * result = +(ABS(var2) - ABS(var1))
					 * ----------
					 */
					if (sub_abs(var2, var1, result) != 0)
						return -1;
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
			if (add_abs(var1, var2, result) != 0)
				return -1;
			result->sign = NUMERIC_NEG;
		}
	}

	return 0;
}

/* ----------
 * mul_var() -
 *
 *	Multiplication on variable level. Product of var1 * var2 is stored
 *	in result.  Accuracy of result is determined by global_rscale.
 * ----------
 */
int
PGTYPESnumeric_mul(numeric *var1, numeric *var2, numeric *result)
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
	int			global_rscale = var1->rscale + var2->rscale;

	res_weight = var1->weight + var2->weight + 2;
	res_ndigits = var1->ndigits + var2->ndigits + 1;
	if (var1->sign == var2->sign)
		res_sign = NUMERIC_POS;
	else
		res_sign = NUMERIC_NEG;

	if ((res_buf = digitbuf_alloc(res_ndigits)) == NULL)
		return -1;
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
	result->dscale = var1->dscale + var2->dscale;

	return 0;
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
select_div_scale(numeric *var1, numeric *var2, int *rscale)
{
	int			weight1,
				weight2,
				qweight,
				i;
	NumericDigit firstdigit1,
				firstdigit2;
	int			res_dscale;

	/*
	 * The result scale of a division isn't specified in any SQL standard. For
	 * PostgreSQL we select a display scale that will give at least
	 * NUMERIC_MIN_SIG_DIGITS significant digits, so that numeric gives a
	 * result no less accurate than float8; but use a scale not less than
	 * either input's display scale.
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
	 * Estimate weight of quotient.  If the two first digits are equal, we
	 * can't be sure, but assume that var1 is less than var2.
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
	*rscale = res_dscale + 4;

	return res_dscale;
}

int
PGTYPESnumeric_div(numeric *var1, numeric *var2, numeric *result)
{
	NumericDigit *res_digits;
	int			res_ndigits;
	int			res_sign;
	int			res_weight;
	numeric		dividend;
	numeric		divisor[10];
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
	int			rscale;
	int			res_dscale = select_div_scale(var1, var2, &rscale);
	int			err = -1;
	NumericDigit *tmp_buf;

	/*
	 * First of all division by zero check
	 */
	ndigits_tmp = var2->ndigits + 1;
	if (ndigits_tmp == 1)
	{
		errno = PGTYPES_NUM_DIVIDE_ZERO;
		return -1;
	}

	/*
	 * Determine the result sign, weight and number of digits to calculate
	 */
	if (var1->sign == var2->sign)
		res_sign = NUMERIC_POS;
	else
		res_sign = NUMERIC_NEG;
	res_weight = var1->weight - var2->weight + 1;
	res_ndigits = rscale + res_weight;
	if (res_ndigits <= 0)
		res_ndigits = 1;

	/*
	 * Now result zero check
	 */
	if (var1->ndigits == 0)
	{
		zero_var(result);
		result->rscale = rscale;
		return 0;
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
	if (divisor[1].buf == NULL)
		goto done;
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
	if (dividend.buf == NULL)
		goto done;
	dividend.digits = dividend.buf;
	memcpy(dividend.digits, var1->digits, var1->ndigits);

	/*
	 * Setup the result. Do the allocation in a temporary buffer first, so we
	 * don't free result->buf unless we have successfully allocated a buffer
	 * to replace it with.
	 */
	tmp_buf = digitbuf_alloc(res_ndigits + 2);
	if (tmp_buf == NULL)
		goto done;
	digitbuf_free(result->buf);
	result->buf = tmp_buf;
	res_digits = result->buf;
	result->digits = res_digits;
	result->ndigits = res_ndigits;
	result->weight = res_weight;
	result->rscale = rscale;
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

				memcpy(&divisor[guess], &divisor[1], sizeof(numeric));
				divisor[guess].buf = digitbuf_alloc(divisor[guess].ndigits);
				if (divisor[guess].buf == NULL)
					goto done;
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

		if (sub_abs(&dividend, &divisor[guess], &dividend) != 0)
			goto done;

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

	result->dscale = res_dscale;
	err = 0;					/* if we've made it this far, return success */

done:

	/*
	 * Tidy up
	 */
	if (dividend.buf != NULL)
		digitbuf_free(dividend.buf);

	for (i = 1; i < 10; i++)
	{
		if (divisor[i].buf != NULL)
			digitbuf_free(divisor[i].buf);
	}

	return err;
}


int
PGTYPESnumeric_cmp(numeric *var1, numeric *var2)
{
	/* use cmp_abs function to calculate the result */

	/* both are positive: normal comparison with cmp_abs */
	if (var1->sign == NUMERIC_POS && var2->sign == NUMERIC_POS)
		return cmp_abs(var1, var2);

	/* both are negative: return the inverse of the normal comparison */
	if (var1->sign == NUMERIC_NEG && var2->sign == NUMERIC_NEG)
	{
		/*
		 * instead of inverting the result, we invert the parameter ordering
		 */
		return cmp_abs(var2, var1);
	}

	/* one is positive, one is negative: trivial */
	if (var1->sign == NUMERIC_POS && var2->sign == NUMERIC_NEG)
		return 1;
	if (var1->sign == NUMERIC_NEG && var2->sign == NUMERIC_POS)
		return -1;

	errno = PGTYPES_NUM_BAD_NUMERIC;
	return INT_MAX;

}

int
PGTYPESnumeric_from_int(signed int int_val, numeric *var)
{
	/* implicit conversion */
	signed long int long_int = int_val;

	return PGTYPESnumeric_from_long(long_int, var);
}

int
PGTYPESnumeric_from_long(signed long int long_val, numeric *var)
{
	/* calculate the size of the long int number */
	/* a number n needs log_10 n digits */

	/*
	 * however we multiply by 10 each time and compare instead of calculating
	 * the logarithm
	 */

	int			size = 0;
	int			i;
	signed long int abs_long_val = long_val;
	signed long int extract;
	signed long int reach_limit;

	if (abs_long_val < 0)
	{
		abs_long_val *= -1;
		var->sign = NUMERIC_NEG;
	}
	else
		var->sign = NUMERIC_POS;

	reach_limit = 1;
	do
	{
		size++;
		reach_limit *= 10;
	} while (reach_limit - 1 < abs_long_val && reach_limit <= LONG_MAX / 10);

	if (reach_limit > LONG_MAX / 10)
	{
		/* add the first digit and a .0 */
		size += 2;
	}
	else
	{
		/* always add a .0 */
		size++;
		reach_limit /= 10;
	}

	if (alloc_var(var, size) < 0)
		return -1;

	var->rscale = 1;
	var->dscale = 1;
	var->weight = size - 2;

	i = 0;
	do
	{
		extract = abs_long_val - (abs_long_val % reach_limit);
		var->digits[i] = extract / reach_limit;
		abs_long_val -= extract;
		i++;
		reach_limit /= 10;

		/*
		 * we can abandon if abs_long_val reaches 0, because the memory is
		 * initialized properly and filled with '0', so converting 10000 in
		 * only one step is no problem
		 */
	} while (abs_long_val > 0);

	return 0;
}

int
PGTYPESnumeric_copy(numeric *src, numeric *dst)
{
	int			i;

	if (dst == NULL)
		return -1;
	zero_var(dst);

	dst->weight = src->weight;
	dst->rscale = src->rscale;
	dst->dscale = src->dscale;
	dst->sign = src->sign;

	if (alloc_var(dst, src->ndigits) != 0)
		return -1;

	for (i = 0; i < src->ndigits; i++)
		dst->digits[i] = src->digits[i];

	return 0;
}

int
PGTYPESnumeric_from_double(double d, numeric *dst)
{
	char		buffer[DBL_DIG + 100];
	numeric    *tmp;
	int			i;

	if (sprintf(buffer, "%.*g", DBL_DIG, d) <= 0)
		return -1;

	if ((tmp = PGTYPESnumeric_from_asc(buffer, NULL)) == NULL)
		return -1;
	i = PGTYPESnumeric_copy(tmp, dst);
	PGTYPESnumeric_free(tmp);
	if (i != 0)
		return -1;

	errno = 0;
	return 0;
}

static int
numericvar_to_double(numeric *var, double *dp)
{
	char	   *tmp;
	double		val;
	char	   *endptr;
	numeric    *varcopy = PGTYPESnumeric_new();

	if (varcopy == NULL)
		return -1;

	if (PGTYPESnumeric_copy(var, varcopy) < 0)
	{
		PGTYPESnumeric_free(varcopy);
		return -1;
	}

	tmp = get_str_from_var(varcopy, varcopy->dscale);
	PGTYPESnumeric_free(varcopy);

	if (tmp == NULL)
		return -1;

	/*
	 * strtod does not reset errno to 0 in case of success.
	 */
	errno = 0;
	val = strtod(tmp, &endptr);
	if (errno == ERANGE)
	{
		free(tmp);
		if (val == 0)
			errno = PGTYPES_NUM_UNDERFLOW;
		else
			errno = PGTYPES_NUM_OVERFLOW;
		return -1;
	}

	/* can't free tmp yet, endptr points still into it */
	if (*endptr != '\0')
	{
		/* shouldn't happen ... */
		free(tmp);
		errno = PGTYPES_NUM_BAD_NUMERIC;
		return -1;
	}
	free(tmp);
	*dp = val;
	return 0;
}

int
PGTYPESnumeric_to_double(numeric *nv, double *dp)
{
	double		tmp;

	if (numericvar_to_double(nv, &tmp) != 0)
		return -1;
	*dp = tmp;
	return 0;
}

int
PGTYPESnumeric_to_int(numeric *nv, int *ip)
{
	long		l;
	int			i;

	if ((i = PGTYPESnumeric_to_long(nv, &l)) != 0)
		return i;

	if (l < -INT_MAX || l > INT_MAX)
	{
		errno = PGTYPES_NUM_OVERFLOW;
		return -1;
	}

	*ip = (int) l;
	return 0;
}

int
PGTYPESnumeric_to_long(numeric *nv, long *lp)
{
	char	   *s = PGTYPESnumeric_to_asc(nv, 0);
	char	   *endptr;

	if (s == NULL)
		return -1;

	errno = 0;
	*lp = strtol(s, &endptr, 10);
	if (endptr == s)
	{
		/* this should not happen actually */
		free(s);
		return -1;
	}
	free(s);
	if (errno == ERANGE)
	{
		if (*lp == LONG_MIN)
			errno = PGTYPES_NUM_UNDERFLOW;
		else
			errno = PGTYPES_NUM_OVERFLOW;
		return -1;
	}
	return 0;
}

int
PGTYPESnumeric_to_decimal(numeric *src, decimal *dst)
{
	int			i;

	if (src->ndigits > DECSIZE)
	{
		errno = PGTYPES_NUM_OVERFLOW;
		return -1;
	}

	dst->weight = src->weight;
	dst->rscale = src->rscale;
	dst->dscale = src->dscale;
	dst->sign = src->sign;
	dst->ndigits = src->ndigits;

	for (i = 0; i < src->ndigits; i++)
		dst->digits[i] = src->digits[i];

	return 0;
}

int
PGTYPESnumeric_from_decimal(decimal *src, numeric *dst)
{
	int			i;

	zero_var(dst);

	dst->weight = src->weight;
	dst->rscale = src->rscale;
	dst->dscale = src->dscale;
	dst->sign = src->sign;

	if (alloc_var(dst, src->ndigits) != 0)
		return -1;

	for (i = 0; i < src->ndigits; i++)
		dst->digits[i] = src->digits[i];

	return 0;
}
