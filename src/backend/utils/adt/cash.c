/*
 * cash.c
 * Written by D'Arcy J.M. Cain
 * darcy@druid.net
 * http://www.druid.net/darcy/
 *
 * Functions to allow input and output of money normally but store
 * and handle it as 64 bit ints
 *
 * A slightly modified version of this file and a discussion of the
 * workings can be found in the book "Software Solutions in C" by
 * Dale Schumacher, Academic Press, ISBN: 0-12-632360-7 except that
 * this version handles 64 bit numbers and so can hold values up to
 * $92,233,720,368,547,758.07.
 *
 * src/backend/utils/adt/cash.c
 */

#include "postgres.h"

#include <limits.h>
#include <ctype.h>
#include <math.h>

#include "common/int.h"
#include "libpq/pqformat.h"
#include "utils/builtins.h"
#include "utils/cash.h"
#include "utils/int8.h"
#include "utils/numeric.h"
#include "utils/pg_locale.h"


/*************************************************************************
 * Private routines
 ************************************************************************/

static const char *
num_word(Cash value)
{
	static char buf[128];
	static const char *const small[] = {
		"zero", "one", "two", "three", "four", "five", "six", "seven",
		"eight", "nine", "ten", "eleven", "twelve", "thirteen", "fourteen",
		"fifteen", "sixteen", "seventeen", "eighteen", "nineteen", "twenty",
		"thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"
	};
	const char *const *big = small + 18;
	int			tu = value % 100;

	/* deal with the simple cases first */
	if (value <= 20)
		return small[value];

	/* is it an even multiple of 100? */
	if (!tu)
	{
		sprintf(buf, "%s hundred", small[value / 100]);
		return buf;
	}

	/* more than 99? */
	if (value > 99)
	{
		/* is it an even multiple of 10 other than 10? */
		if (value % 10 == 0 && tu > 10)
			sprintf(buf, "%s hundred %s",
					small[value / 100], big[tu / 10]);
		else if (tu < 20)
			sprintf(buf, "%s hundred and %s",
					small[value / 100], small[tu]);
		else
			sprintf(buf, "%s hundred %s %s",
					small[value / 100], big[tu / 10], small[tu % 10]);
	}
	else
	{
		/* is it an even multiple of 10 other than 10? */
		if (value % 10 == 0 && tu > 10)
			sprintf(buf, "%s", big[tu / 10]);
		else if (tu < 20)
			sprintf(buf, "%s", small[tu]);
		else
			sprintf(buf, "%s %s", big[tu / 10], small[tu % 10]);
	}

	return buf;
}								/* num_word() */

/* cash_in()
 * Convert a string to a cash data type.
 * Format is [$]###[,]###[.##]
 * Examples: 123.45 $123.45 $123,456.78
 *
 */
Datum
cash_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	Cash		result;
	Cash		value = 0;
	Cash		dec = 0;
	Cash		sgn = 1;
	bool		seen_dot = false;
	const char *s = str;
	int			fpoint;
	char		dsymbol;
	const char *ssymbol,
			   *psymbol,
			   *nsymbol,
			   *csymbol;
	struct lconv *lconvert = PGLC_localeconv();

	/*
	 * frac_digits will be CHAR_MAX in some locales, notably C.  However, just
	 * testing for == CHAR_MAX is risky, because of compilers like gcc that
	 * "helpfully" let you alter the platform-standard definition of whether
	 * char is signed or not.  If we are so unfortunate as to get compiled
	 * with a nonstandard -fsigned-char or -funsigned-char switch, then our
	 * idea of CHAR_MAX will not agree with libc's. The safest course is not
	 * to test for CHAR_MAX at all, but to impose a range check for plausible
	 * frac_digits values.
	 */
	fpoint = lconvert->frac_digits;
	if (fpoint < 0 || fpoint > 10)
		fpoint = 2;				/* best guess in this case, I think */

	/* we restrict dsymbol to be a single byte, but not the other symbols */
	if (*lconvert->mon_decimal_point != '\0' &&
		lconvert->mon_decimal_point[1] == '\0')
		dsymbol = *lconvert->mon_decimal_point;
	else
		dsymbol = '.';
	if (*lconvert->mon_thousands_sep != '\0')
		ssymbol = lconvert->mon_thousands_sep;
	else						/* ssymbol should not equal dsymbol */
		ssymbol = (dsymbol != ',') ? "," : ".";
	csymbol = (*lconvert->currency_symbol != '\0') ? lconvert->currency_symbol : "$";
	psymbol = (*lconvert->positive_sign != '\0') ? lconvert->positive_sign : "+";
	nsymbol = (*lconvert->negative_sign != '\0') ? lconvert->negative_sign : "-";

#ifdef CASHDEBUG
	printf("cashin- precision '%d'; decimal '%c'; thousands '%s'; currency '%s'; positive '%s'; negative '%s'\n",
		   fpoint, dsymbol, ssymbol, csymbol, psymbol, nsymbol);
#endif

	/* we need to add all sorts of checking here.  For now just */
	/* strip all leading whitespace and any leading currency symbol */
	while (isspace((unsigned char) *s))
		s++;
	if (strncmp(s, csymbol, strlen(csymbol)) == 0)
		s += strlen(csymbol);
	while (isspace((unsigned char) *s))
		s++;

#ifdef CASHDEBUG
	printf("cashin- string is '%s'\n", s);
#endif

	/* a leading minus or paren signifies a negative number */
	/* again, better heuristics needed */
	/* XXX - doesn't properly check for balanced parens - djmc */
	if (strncmp(s, nsymbol, strlen(nsymbol)) == 0)
	{
		sgn = -1;
		s += strlen(nsymbol);
	}
	else if (*s == '(')
	{
		sgn = -1;
		s++;
	}
	else if (strncmp(s, psymbol, strlen(psymbol)) == 0)
		s += strlen(psymbol);

#ifdef CASHDEBUG
	printf("cashin- string is '%s'\n", s);
#endif

	/* allow whitespace and currency symbol after the sign, too */
	while (isspace((unsigned char) *s))
		s++;
	if (strncmp(s, csymbol, strlen(csymbol)) == 0)
		s += strlen(csymbol);
	while (isspace((unsigned char) *s))
		s++;

#ifdef CASHDEBUG
	printf("cashin- string is '%s'\n", s);
#endif

	/*
	 * We accumulate the absolute amount in "value" and then apply the sign at
	 * the end.  (The sign can appear before or after the digits, so it would
	 * be more complicated to do otherwise.)  Because of the larger range of
	 * negative signed integers, we build "value" in the negative and then
	 * flip the sign at the end, catching most-negative-number overflow if
	 * necessary.
	 */

	for (; *s; s++)
	{
		/*
		 * We look for digits as long as we have found less than the required
		 * number of decimal places.
		 */
		if (isdigit((unsigned char) *s) && (!seen_dot || dec < fpoint))
		{
			int8		digit = *s - '0';

			if (pg_mul_s64_overflow(value, 10, &value) ||
				pg_sub_s64_overflow(value, digit, &value))
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("value \"%s\" is out of range for type %s",
								str, "money")));

			if (seen_dot)
				dec++;
		}
		/* decimal point? then start counting fractions... */
		else if (*s == dsymbol && !seen_dot)
		{
			seen_dot = true;
		}
		/* ignore if "thousands" separator, else we're done */
		else if (strncmp(s, ssymbol, strlen(ssymbol)) == 0)
			s += strlen(ssymbol) - 1;
		else
			break;
	}

	/* round off if there's another digit */
	if (isdigit((unsigned char) *s) && *s >= '5')
	{
		/* remember we build the value in the negative */
		if (pg_sub_s64_overflow(value, 1, &value))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("value \"%s\" is out of range for type %s",
							str, "money")));
	}

	/* adjust for less than required decimal places */
	for (; dec < fpoint; dec++)
	{
		if (pg_mul_s64_overflow(value, 10, &value))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("value \"%s\" is out of range for type %s",
							str, "money")));
	}

	/*
	 * should only be trailing digits followed by whitespace, right paren,
	 * trailing sign, and/or trailing currency symbol
	 */
	while (isdigit((unsigned char) *s))
		s++;

	while (*s)
	{
		if (isspace((unsigned char) *s) || *s == ')')
			s++;
		else if (strncmp(s, nsymbol, strlen(nsymbol)) == 0)
		{
			sgn = -1;
			s += strlen(nsymbol);
		}
		else if (strncmp(s, psymbol, strlen(psymbol)) == 0)
			s += strlen(psymbol);
		else if (strncmp(s, csymbol, strlen(csymbol)) == 0)
			s += strlen(csymbol);
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type %s: \"%s\"",
							"money", str)));
	}

	/*
	 * If the value is supposed to be positive, flip the sign, but check for
	 * the most negative number.
	 */
	if (sgn > 0)
	{
		if (value == PG_INT64_MIN)
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("value \"%s\" is out of range for type %s",
							str, "money")));
		result = -value;
	}
	else
		result = value;

#ifdef CASHDEBUG
	printf("cashin- result is " INT64_FORMAT "\n", result);
#endif

	PG_RETURN_CASH(result);
}


/* cash_out()
 * Function to convert cash to a dollars and cents representation, using
 * the lc_monetary locale's formatting.
 */
Datum
cash_out(PG_FUNCTION_ARGS)
{
	Cash		value = PG_GETARG_CASH(0);
	char	   *result;
	char		buf[128];
	char	   *bufptr;
	int			digit_pos;
	int			points,
				mon_group;
	char		dsymbol;
	const char *ssymbol,
			   *csymbol,
			   *signsymbol;
	char		sign_posn,
				cs_precedes,
				sep_by_space;
	struct lconv *lconvert = PGLC_localeconv();

	/* see comments about frac_digits in cash_in() */
	points = lconvert->frac_digits;
	if (points < 0 || points > 10)
		points = 2;				/* best guess in this case, I think */

	/*
	 * As with frac_digits, must apply a range check to mon_grouping to avoid
	 * being fooled by variant CHAR_MAX values.
	 */
	mon_group = *lconvert->mon_grouping;
	if (mon_group <= 0 || mon_group > 6)
		mon_group = 3;

	/* we restrict dsymbol to be a single byte, but not the other symbols */
	if (*lconvert->mon_decimal_point != '\0' &&
		lconvert->mon_decimal_point[1] == '\0')
		dsymbol = *lconvert->mon_decimal_point;
	else
		dsymbol = '.';
	if (*lconvert->mon_thousands_sep != '\0')
		ssymbol = lconvert->mon_thousands_sep;
	else						/* ssymbol should not equal dsymbol */
		ssymbol = (dsymbol != ',') ? "," : ".";
	csymbol = (*lconvert->currency_symbol != '\0') ? lconvert->currency_symbol : "$";

	if (value < 0)
	{
		/* make the amount positive for digit-reconstruction loop */
		value = -value;
		/* set up formatting data */
		signsymbol = (*lconvert->negative_sign != '\0') ? lconvert->negative_sign : "-";
		sign_posn = lconvert->n_sign_posn;
		cs_precedes = lconvert->n_cs_precedes;
		sep_by_space = lconvert->n_sep_by_space;
	}
	else
	{
		signsymbol = lconvert->positive_sign;
		sign_posn = lconvert->p_sign_posn;
		cs_precedes = lconvert->p_cs_precedes;
		sep_by_space = lconvert->p_sep_by_space;
	}

	/* we build the digits+decimal-point+sep string right-to-left in buf[] */
	bufptr = buf + sizeof(buf) - 1;
	*bufptr = '\0';

	/*
	 * Generate digits till there are no non-zero digits left and we emitted
	 * at least one to the left of the decimal point.  digit_pos is the
	 * current digit position, with zero as the digit just left of the decimal
	 * point, increasing to the right.
	 */
	digit_pos = points;
	do
	{
		if (points && digit_pos == 0)
		{
			/* insert decimal point, but not if value cannot be fractional */
			*(--bufptr) = dsymbol;
		}
		else if (digit_pos < 0 && (digit_pos % mon_group) == 0)
		{
			/* insert thousands sep, but only to left of radix point */
			bufptr -= strlen(ssymbol);
			memcpy(bufptr, ssymbol, strlen(ssymbol));
		}

		*(--bufptr) = ((uint64) value % 10) + '0';
		value = ((uint64) value) / 10;
		digit_pos--;
	} while (value || digit_pos >= 0);

	/*----------
	 * Now, attach currency symbol and sign symbol in the correct order.
	 *
	 * The POSIX spec defines these values controlling this code:
	 *
	 * p/n_sign_posn:
	 *	0	Parentheses enclose the quantity and the currency_symbol.
	 *	1	The sign string precedes the quantity and the currency_symbol.
	 *	2	The sign string succeeds the quantity and the currency_symbol.
	 *	3	The sign string precedes the currency_symbol.
	 *	4	The sign string succeeds the currency_symbol.
	 *
	 * p/n_cs_precedes: 0 means currency symbol after value, else before it.
	 *
	 * p/n_sep_by_space:
	 *	0	No <space> separates the currency symbol and value.
	 *	1	If the currency symbol and sign string are adjacent, a <space>
	 *		separates them from the value; otherwise, a <space> separates
	 *		the currency symbol from the value.
	 *	2	If the currency symbol and sign string are adjacent, a <space>
	 *		separates them; otherwise, a <space> separates the sign string
	 *		from the value.
	 *----------
	 */
	switch (sign_posn)
	{
		case 0:
			if (cs_precedes)
				result = psprintf("(%s%s%s)",
								  csymbol,
								  (sep_by_space == 1) ? " " : "",
								  bufptr);
			else
				result = psprintf("(%s%s%s)",
								  bufptr,
								  (sep_by_space == 1) ? " " : "",
								  csymbol);
			break;
		case 1:
		default:
			if (cs_precedes)
				result = psprintf("%s%s%s%s%s",
								  signsymbol,
								  (sep_by_space == 2) ? " " : "",
								  csymbol,
								  (sep_by_space == 1) ? " " : "",
								  bufptr);
			else
				result = psprintf("%s%s%s%s%s",
								  signsymbol,
								  (sep_by_space == 2) ? " " : "",
								  bufptr,
								  (sep_by_space == 1) ? " " : "",
								  csymbol);
			break;
		case 2:
			if (cs_precedes)
				result = psprintf("%s%s%s%s%s",
								  csymbol,
								  (sep_by_space == 1) ? " " : "",
								  bufptr,
								  (sep_by_space == 2) ? " " : "",
								  signsymbol);
			else
				result = psprintf("%s%s%s%s%s",
								  bufptr,
								  (sep_by_space == 1) ? " " : "",
								  csymbol,
								  (sep_by_space == 2) ? " " : "",
								  signsymbol);
			break;
		case 3:
			if (cs_precedes)
				result = psprintf("%s%s%s%s%s",
								  signsymbol,
								  (sep_by_space == 2) ? " " : "",
								  csymbol,
								  (sep_by_space == 1) ? " " : "",
								  bufptr);
			else
				result = psprintf("%s%s%s%s%s",
								  bufptr,
								  (sep_by_space == 1) ? " " : "",
								  signsymbol,
								  (sep_by_space == 2) ? " " : "",
								  csymbol);
			break;
		case 4:
			if (cs_precedes)
				result = psprintf("%s%s%s%s%s",
								  csymbol,
								  (sep_by_space == 2) ? " " : "",
								  signsymbol,
								  (sep_by_space == 1) ? " " : "",
								  bufptr);
			else
				result = psprintf("%s%s%s%s%s",
								  bufptr,
								  (sep_by_space == 1) ? " " : "",
								  csymbol,
								  (sep_by_space == 2) ? " " : "",
								  signsymbol);
			break;
	}

	PG_RETURN_CSTRING(result);
}

/*
 *		cash_recv			- converts external binary format to cash
 */
Datum
cash_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

	PG_RETURN_CASH((Cash) pq_getmsgint64(buf));
}

/*
 *		cash_send			- converts cash to binary format
 */
Datum
cash_send(PG_FUNCTION_ARGS)
{
	Cash		arg1 = PG_GETARG_CASH(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint64(&buf, arg1);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * Comparison functions
 */

Datum
cash_eq(PG_FUNCTION_ARGS)
{
	Cash		c1 = PG_GETARG_CASH(0);
	Cash		c2 = PG_GETARG_CASH(1);

	PG_RETURN_BOOL(c1 == c2);
}

Datum
cash_ne(PG_FUNCTION_ARGS)
{
	Cash		c1 = PG_GETARG_CASH(0);
	Cash		c2 = PG_GETARG_CASH(1);

	PG_RETURN_BOOL(c1 != c2);
}

Datum
cash_lt(PG_FUNCTION_ARGS)
{
	Cash		c1 = PG_GETARG_CASH(0);
	Cash		c2 = PG_GETARG_CASH(1);

	PG_RETURN_BOOL(c1 < c2);
}

Datum
cash_le(PG_FUNCTION_ARGS)
{
	Cash		c1 = PG_GETARG_CASH(0);
	Cash		c2 = PG_GETARG_CASH(1);

	PG_RETURN_BOOL(c1 <= c2);
}

Datum
cash_gt(PG_FUNCTION_ARGS)
{
	Cash		c1 = PG_GETARG_CASH(0);
	Cash		c2 = PG_GETARG_CASH(1);

	PG_RETURN_BOOL(c1 > c2);
}

Datum
cash_ge(PG_FUNCTION_ARGS)
{
	Cash		c1 = PG_GETARG_CASH(0);
	Cash		c2 = PG_GETARG_CASH(1);

	PG_RETURN_BOOL(c1 >= c2);
}

Datum
cash_cmp(PG_FUNCTION_ARGS)
{
	Cash		c1 = PG_GETARG_CASH(0);
	Cash		c2 = PG_GETARG_CASH(1);

	if (c1 > c2)
		PG_RETURN_INT32(1);
	else if (c1 == c2)
		PG_RETURN_INT32(0);
	else
		PG_RETURN_INT32(-1);
}


/* cash_pl()
 * Add two cash values.
 */
Datum
cash_pl(PG_FUNCTION_ARGS)
{
	Cash		c1 = PG_GETARG_CASH(0);
	Cash		c2 = PG_GETARG_CASH(1);
	Cash		result;

	result = c1 + c2;

	PG_RETURN_CASH(result);
}


/* cash_mi()
 * Subtract two cash values.
 */
Datum
cash_mi(PG_FUNCTION_ARGS)
{
	Cash		c1 = PG_GETARG_CASH(0);
	Cash		c2 = PG_GETARG_CASH(1);
	Cash		result;

	result = c1 - c2;

	PG_RETURN_CASH(result);
}


/* cash_div_cash()
 * Divide cash by cash, returning float8.
 */
Datum
cash_div_cash(PG_FUNCTION_ARGS)
{
	Cash		dividend = PG_GETARG_CASH(0);
	Cash		divisor = PG_GETARG_CASH(1);
	float8		quotient;

	if (divisor == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));

	quotient = (float8) dividend / (float8) divisor;
	PG_RETURN_FLOAT8(quotient);
}


/* cash_mul_flt8()
 * Multiply cash by float8.
 */
Datum
cash_mul_flt8(PG_FUNCTION_ARGS)
{
	Cash		c = PG_GETARG_CASH(0);
	float8		f = PG_GETARG_FLOAT8(1);
	Cash		result;

	result = rint(c * f);
	PG_RETURN_CASH(result);
}


/* flt8_mul_cash()
 * Multiply float8 by cash.
 */
Datum
flt8_mul_cash(PG_FUNCTION_ARGS)
{
	float8		f = PG_GETARG_FLOAT8(0);
	Cash		c = PG_GETARG_CASH(1);
	Cash		result;

	result = rint(f * c);
	PG_RETURN_CASH(result);
}


/* cash_div_flt8()
 * Divide cash by float8.
 */
Datum
cash_div_flt8(PG_FUNCTION_ARGS)
{
	Cash		c = PG_GETARG_CASH(0);
	float8		f = PG_GETARG_FLOAT8(1);
	Cash		result;

	if (f == 0.0)
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));

	result = rint(c / f);
	PG_RETURN_CASH(result);
}


/* cash_mul_flt4()
 * Multiply cash by float4.
 */
Datum
cash_mul_flt4(PG_FUNCTION_ARGS)
{
	Cash		c = PG_GETARG_CASH(0);
	float4		f = PG_GETARG_FLOAT4(1);
	Cash		result;

	result = rint(c * (float8) f);
	PG_RETURN_CASH(result);
}


/* flt4_mul_cash()
 * Multiply float4 by cash.
 */
Datum
flt4_mul_cash(PG_FUNCTION_ARGS)
{
	float4		f = PG_GETARG_FLOAT4(0);
	Cash		c = PG_GETARG_CASH(1);
	Cash		result;

	result = rint((float8) f * c);
	PG_RETURN_CASH(result);
}


/* cash_div_flt4()
 * Divide cash by float4.
 *
 */
Datum
cash_div_flt4(PG_FUNCTION_ARGS)
{
	Cash		c = PG_GETARG_CASH(0);
	float4		f = PG_GETARG_FLOAT4(1);
	Cash		result;

	if (f == 0.0)
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));

	result = rint(c / (float8) f);
	PG_RETURN_CASH(result);
}


/* cash_mul_int8()
 * Multiply cash by int8.
 */
Datum
cash_mul_int8(PG_FUNCTION_ARGS)
{
	Cash		c = PG_GETARG_CASH(0);
	int64		i = PG_GETARG_INT64(1);
	Cash		result;

	result = c * i;
	PG_RETURN_CASH(result);
}


/* int8_mul_cash()
 * Multiply int8 by cash.
 */
Datum
int8_mul_cash(PG_FUNCTION_ARGS)
{
	int64		i = PG_GETARG_INT64(0);
	Cash		c = PG_GETARG_CASH(1);
	Cash		result;

	result = i * c;
	PG_RETURN_CASH(result);
}

/* cash_div_int8()
 * Divide cash by 8-byte integer.
 */
Datum
cash_div_int8(PG_FUNCTION_ARGS)
{
	Cash		c = PG_GETARG_CASH(0);
	int64		i = PG_GETARG_INT64(1);
	Cash		result;

	if (i == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));

	result = c / i;

	PG_RETURN_CASH(result);
}


/* cash_mul_int4()
 * Multiply cash by int4.
 */
Datum
cash_mul_int4(PG_FUNCTION_ARGS)
{
	Cash		c = PG_GETARG_CASH(0);
	int32		i = PG_GETARG_INT32(1);
	Cash		result;

	result = c * i;
	PG_RETURN_CASH(result);
}


/* int4_mul_cash()
 * Multiply int4 by cash.
 */
Datum
int4_mul_cash(PG_FUNCTION_ARGS)
{
	int32		i = PG_GETARG_INT32(0);
	Cash		c = PG_GETARG_CASH(1);
	Cash		result;

	result = i * c;
	PG_RETURN_CASH(result);
}


/* cash_div_int4()
 * Divide cash by 4-byte integer.
 *
 */
Datum
cash_div_int4(PG_FUNCTION_ARGS)
{
	Cash		c = PG_GETARG_CASH(0);
	int32		i = PG_GETARG_INT32(1);
	Cash		result;

	if (i == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));

	result = c / i;

	PG_RETURN_CASH(result);
}


/* cash_mul_int2()
 * Multiply cash by int2.
 */
Datum
cash_mul_int2(PG_FUNCTION_ARGS)
{
	Cash		c = PG_GETARG_CASH(0);
	int16		s = PG_GETARG_INT16(1);
	Cash		result;

	result = c * s;
	PG_RETURN_CASH(result);
}

/* int2_mul_cash()
 * Multiply int2 by cash.
 */
Datum
int2_mul_cash(PG_FUNCTION_ARGS)
{
	int16		s = PG_GETARG_INT16(0);
	Cash		c = PG_GETARG_CASH(1);
	Cash		result;

	result = s * c;
	PG_RETURN_CASH(result);
}

/* cash_div_int2()
 * Divide cash by int2.
 *
 */
Datum
cash_div_int2(PG_FUNCTION_ARGS)
{
	Cash		c = PG_GETARG_CASH(0);
	int16		s = PG_GETARG_INT16(1);
	Cash		result;

	if (s == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));

	result = c / s;
	PG_RETURN_CASH(result);
}

/* cashlarger()
 * Return larger of two cash values.
 */
Datum
cashlarger(PG_FUNCTION_ARGS)
{
	Cash		c1 = PG_GETARG_CASH(0);
	Cash		c2 = PG_GETARG_CASH(1);
	Cash		result;

	result = (c1 > c2) ? c1 : c2;

	PG_RETURN_CASH(result);
}

/* cashsmaller()
 * Return smaller of two cash values.
 */
Datum
cashsmaller(PG_FUNCTION_ARGS)
{
	Cash		c1 = PG_GETARG_CASH(0);
	Cash		c2 = PG_GETARG_CASH(1);
	Cash		result;

	result = (c1 < c2) ? c1 : c2;

	PG_RETURN_CASH(result);
}

/* cash_words()
 * This converts an int4 as well but to a representation using words
 * Obviously way North American centric - sorry
 */
Datum
cash_words(PG_FUNCTION_ARGS)
{
	Cash		value = PG_GETARG_CASH(0);
	uint64		val;
	char		buf[256];
	char	   *p = buf;
	Cash		m0;
	Cash		m1;
	Cash		m2;
	Cash		m3;
	Cash		m4;
	Cash		m5;
	Cash		m6;

	/* work with positive numbers */
	if (value < 0)
	{
		value = -value;
		strcpy(buf, "minus ");
		p += 6;
	}
	else
		buf[0] = '\0';

	/* Now treat as unsigned, to avoid trouble at INT_MIN */
	val = (uint64) value;

	m0 = val % INT64CONST(100); /* cents */
	m1 = (val / INT64CONST(100)) % 1000;	/* hundreds */
	m2 = (val / INT64CONST(100000)) % 1000; /* thousands */
	m3 = (val / INT64CONST(100000000)) % 1000;	/* millions */
	m4 = (val / INT64CONST(100000000000)) % 1000;	/* billions */
	m5 = (val / INT64CONST(100000000000000)) % 1000;	/* trillions */
	m6 = (val / INT64CONST(100000000000000000)) % 1000; /* quadrillions */

	if (m6)
	{
		strcat(buf, num_word(m6));
		strcat(buf, " quadrillion ");
	}

	if (m5)
	{
		strcat(buf, num_word(m5));
		strcat(buf, " trillion ");
	}

	if (m4)
	{
		strcat(buf, num_word(m4));
		strcat(buf, " billion ");
	}

	if (m3)
	{
		strcat(buf, num_word(m3));
		strcat(buf, " million ");
	}

	if (m2)
	{
		strcat(buf, num_word(m2));
		strcat(buf, " thousand ");
	}

	if (m1)
		strcat(buf, num_word(m1));

	if (!*p)
		strcat(buf, "zero");

	strcat(buf, (val / 100) == 1 ? " dollar and " : " dollars and ");
	strcat(buf, num_word(m0));
	strcat(buf, m0 == 1 ? " cent" : " cents");

	/* capitalize output */
	buf[0] = pg_toupper((unsigned char) buf[0]);

	/* return as text datum */
	PG_RETURN_TEXT_P(cstring_to_text(buf));
}


/* cash_numeric()
 * Convert cash to numeric.
 */
Datum
cash_numeric(PG_FUNCTION_ARGS)
{
	Cash		money = PG_GETARG_CASH(0);
	Datum		result;
	int			fpoint;
	struct lconv *lconvert = PGLC_localeconv();

	/* see comments about frac_digits in cash_in() */
	fpoint = lconvert->frac_digits;
	if (fpoint < 0 || fpoint > 10)
		fpoint = 2;

	/* convert the integral money value to numeric */
	result = DirectFunctionCall1(int8_numeric, Int64GetDatum(money));

	/* scale appropriately, if needed */
	if (fpoint > 0)
	{
		int64		scale;
		int			i;
		Datum		numeric_scale;
		Datum		quotient;

		/* compute required scale factor */
		scale = 1;
		for (i = 0; i < fpoint; i++)
			scale *= 10;
		numeric_scale = DirectFunctionCall1(int8_numeric,
											Int64GetDatum(scale));

		/*
		 * Given integral inputs approaching INT64_MAX, select_div_scale()
		 * might choose a result scale of zero, causing loss of fractional
		 * digits in the quotient.  We can ensure an exact result by setting
		 * the dscale of either input to be at least as large as the desired
		 * result scale.  numeric_round() will do that for us.
		 */
		numeric_scale = DirectFunctionCall2(numeric_round,
											numeric_scale,
											Int32GetDatum(fpoint));

		/* Now we can safely divide ... */
		quotient = DirectFunctionCall2(numeric_div, result, numeric_scale);

		/* ... and forcibly round to exactly the intended number of digits */
		result = DirectFunctionCall2(numeric_round,
									 quotient,
									 Int32GetDatum(fpoint));
	}

	PG_RETURN_DATUM(result);
}

/* numeric_cash()
 * Convert numeric to cash.
 */
Datum
numeric_cash(PG_FUNCTION_ARGS)
{
	Datum		amount = PG_GETARG_DATUM(0);
	Cash		result;
	int			fpoint;
	int64		scale;
	int			i;
	Datum		numeric_scale;
	struct lconv *lconvert = PGLC_localeconv();

	/* see comments about frac_digits in cash_in() */
	fpoint = lconvert->frac_digits;
	if (fpoint < 0 || fpoint > 10)
		fpoint = 2;

	/* compute required scale factor */
	scale = 1;
	for (i = 0; i < fpoint; i++)
		scale *= 10;

	/* multiply the input amount by scale factor */
	numeric_scale = DirectFunctionCall1(int8_numeric, Int64GetDatum(scale));
	amount = DirectFunctionCall2(numeric_mul, amount, numeric_scale);

	/* note that numeric_int8 will round to nearest integer for us */
	result = DatumGetInt64(DirectFunctionCall1(numeric_int8, amount));

	PG_RETURN_CASH(result);
}

/* int4_cash()
 * Convert int4 (int) to cash
 */
Datum
int4_cash(PG_FUNCTION_ARGS)
{
	int32		amount = PG_GETARG_INT32(0);
	Cash		result;
	int			fpoint;
	int64		scale;
	int			i;
	struct lconv *lconvert = PGLC_localeconv();

	/* see comments about frac_digits in cash_in() */
	fpoint = lconvert->frac_digits;
	if (fpoint < 0 || fpoint > 10)
		fpoint = 2;

	/* compute required scale factor */
	scale = 1;
	for (i = 0; i < fpoint; i++)
		scale *= 10;

	/* compute amount * scale, checking for overflow */
	result = DatumGetInt64(DirectFunctionCall2(int8mul, Int64GetDatum(amount),
											   Int64GetDatum(scale)));

	PG_RETURN_CASH(result);
}

/* int8_cash()
 * Convert int8 (bigint) to cash
 */
Datum
int8_cash(PG_FUNCTION_ARGS)
{
	int64		amount = PG_GETARG_INT64(0);
	Cash		result;
	int			fpoint;
	int64		scale;
	int			i;
	struct lconv *lconvert = PGLC_localeconv();

	/* see comments about frac_digits in cash_in() */
	fpoint = lconvert->frac_digits;
	if (fpoint < 0 || fpoint > 10)
		fpoint = 2;

	/* compute required scale factor */
	scale = 1;
	for (i = 0; i < fpoint; i++)
		scale *= 10;

	/* compute amount * scale, checking for overflow */
	result = DatumGetInt64(DirectFunctionCall2(int8mul, Int64GetDatum(amount),
											   Int64GetDatum(scale)));

	PG_RETURN_CASH(result);
}
