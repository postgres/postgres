/*
 *	PostgreSQL type definitions for MAC addresses.
 *
 *	$Header: /cvsroot/pgsql/src/backend/utils/adt/mac.c,v 1.18 2000/08/23 06:04:33 thomas Exp $
 */

#include "postgres.h"

#include "utils/builtins.h"
#include "utils/inet.h"

/*
 *	Utility macros used for sorting and comparing:
 */

#define hibits(addr) \
  ((unsigned long)(((addr)->a<<16)|((addr)->b<<8)|((addr)->c)))

#define lobits(addr) \
  ((unsigned long)(((addr)->d<<16)|((addr)->e<<8)|((addr)->f)))

/*
 *	MAC address reader.  Accepts several common notations.
 */

Datum
macaddr_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	int			a,
				b,
				c,
				d,
				e,
				f;
	macaddr    *result;
	int			count;

	if (strlen(str) > 0)
	{
		count = sscanf(str, "%x:%x:%x:%x:%x:%x", &a, &b, &c, &d, &e, &f);
		if (count != 6)
			count = sscanf(str, "%x-%x-%x-%x-%x-%x", &a, &b, &c, &d, &e, &f);
		if (count != 6)
			count = sscanf(str, "%2x%2x%2x:%2x%2x%2x", &a, &b, &c, &d, &e, &f);
		if (count != 6)
			count = sscanf(str, "%2x%2x%2x-%2x%2x%2x", &a, &b, &c, &d, &e, &f);
		if (count != 6)
			count = sscanf(str, "%2x%2x.%2x%2x.%2x%2x", &a, &b, &c, &d, &e, &f);

		if (count != 6)
			elog(ERROR, "macaddr_in: error in parsing \"%s\"", str);

		if ((a < 0) || (a > 255) || (b < 0) || (b > 255) ||
			(c < 0) || (c > 255) || (d < 0) || (d > 255) ||
			(e < 0) || (e > 255) || (f < 0) || (f > 255))
			elog(ERROR, "macaddr_in: illegal address \"%s\"", str);
	}
	else
	{
		a = b = c = d = e = f = 0;		/* special case for missing
										 * address */
	}

	result = (macaddr *) palloc(sizeof(macaddr));

	result->a = a;
	result->b = b;
	result->c = c;
	result->d = d;
	result->e = e;
	result->f = f;

	PG_RETURN_MACADDR_P(result);
}

/*
 *	MAC address output function.  Fixed format.
 */

Datum
macaddr_out(PG_FUNCTION_ARGS)
{
	macaddr	   *addr = PG_GETARG_MACADDR_P(0);
	char	   *result;

	result = (char *) palloc(32);

	if ((hibits(addr) > 0) || (lobits(addr) > 0))
	{
		sprintf(result, "%02x:%02x:%02x:%02x:%02x:%02x",
				addr->a, addr->b, addr->c, addr->d, addr->e, addr->f);
	}
	else
	{
		result[0] = '\0';		/* special case for missing address */
	}

	PG_RETURN_CSTRING(result);
}

/* macaddr_text()
 * Convert macaddr to text data type.
 */

Datum
macaddr_text(PG_FUNCTION_ARGS)
{
	/* Input is a macaddr, but may as well leave it in Datum form */
	Datum		addr = PG_GETARG_DATUM(0);
	text	   *result;
	char	   *str;
	int			len;

	str = DatumGetCString(DirectFunctionCall1(macaddr_out, addr));

	len = (strlen(str) + VARHDRSZ);

	result = palloc(len);

	VARATT_SIZEP(result) = len;
	memmove(VARDATA(result), str, (len - VARHDRSZ));

	pfree(str);

	PG_RETURN_TEXT_P(result);
}

/* text_macaddr()
 * Convert text to macaddr data type.
 */

Datum
text_macaddr(PG_FUNCTION_ARGS)
{
	Datum		result;
	text	   *addr = PG_GETARG_TEXT_P(0);
	char		str[18];
	int			len;

	len = (VARSIZE(addr)-VARHDRSZ);
	if (len >= 18)
		elog(ERROR, "Text is too long to convert to MAC address");

	memmove(str, VARDATA(addr), len);
	*(str+len) = '\0';

    result = DirectFunctionCall1(macaddr_in, CStringGetDatum(str));

	return(result);
}

/*
 *	Comparison function for sorting:
 */

static int32
macaddr_cmp_internal(macaddr *a1, macaddr *a2)
{
	if (hibits(a1) < hibits(a2))
		return -1;
	else if (hibits(a1) > hibits(a2))
		return 1;
	else if (lobits(a1) < lobits(a2))
		return -1;
	else if (lobits(a1) > lobits(a2))
		return 1;
	else
		return 0;
}

Datum
macaddr_cmp(PG_FUNCTION_ARGS)
{
	macaddr	   *a1 = PG_GETARG_MACADDR_P(0);
	macaddr	   *a2 = PG_GETARG_MACADDR_P(1);

	PG_RETURN_INT32(macaddr_cmp_internal(a1, a2));
}

/*
 *	Boolean comparisons.
 */

Datum
macaddr_lt(PG_FUNCTION_ARGS)
{
	macaddr	   *a1 = PG_GETARG_MACADDR_P(0);
	macaddr	   *a2 = PG_GETARG_MACADDR_P(1);

	PG_RETURN_BOOL(macaddr_cmp_internal(a1, a2) < 0);
}

Datum
macaddr_le(PG_FUNCTION_ARGS)
{
	macaddr	   *a1 = PG_GETARG_MACADDR_P(0);
	macaddr	   *a2 = PG_GETARG_MACADDR_P(1);

	PG_RETURN_BOOL(macaddr_cmp_internal(a1, a2) <= 0);
}

Datum
macaddr_eq(PG_FUNCTION_ARGS)
{
	macaddr	   *a1 = PG_GETARG_MACADDR_P(0);
	macaddr	   *a2 = PG_GETARG_MACADDR_P(1);

	PG_RETURN_BOOL(macaddr_cmp_internal(a1, a2) == 0);
}

Datum
macaddr_ge(PG_FUNCTION_ARGS)
{
	macaddr	   *a1 = PG_GETARG_MACADDR_P(0);
	macaddr	   *a2 = PG_GETARG_MACADDR_P(1);

	PG_RETURN_BOOL(macaddr_cmp_internal(a1, a2) >= 0);
}

Datum
macaddr_gt(PG_FUNCTION_ARGS)
{
	macaddr	   *a1 = PG_GETARG_MACADDR_P(0);
	macaddr	   *a2 = PG_GETARG_MACADDR_P(1);

	PG_RETURN_BOOL(macaddr_cmp_internal(a1, a2) > 0);
}

Datum
macaddr_ne(PG_FUNCTION_ARGS)
{
	macaddr	   *a1 = PG_GETARG_MACADDR_P(0);
	macaddr	   *a2 = PG_GETARG_MACADDR_P(1);

	PG_RETURN_BOOL(macaddr_cmp_internal(a1, a2) != 0);
}

/*
 *	Truncation function to allow comparing mac manufacturers.
 *	From suggestion by Alex Pilosov <alex@pilosoft.com>
 */

Datum
macaddr_trunc(PG_FUNCTION_ARGS)
{
	macaddr	   *result;
	macaddr	   *addr = PG_GETARG_MACADDR_P(0);

	result = (macaddr *) palloc(sizeof(macaddr));

	result->a = addr->a;
	result->b = addr->b;
	result->c = addr->c;
	result->d = 0;
	result->e = 0;
	result->f = 0;

	PG_RETURN_MACADDR_P(result);
}
