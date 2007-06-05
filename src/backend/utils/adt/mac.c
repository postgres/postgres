/*
 *	PostgreSQL type definitions for MAC addresses.
 *
 *	$PostgreSQL: pgsql/src/backend/utils/adt/mac.c,v 1.38 2007/06/05 21:31:06 tgl Exp $
 */

#include "postgres.h"

#include "access/hash.h"
#include "libpq/pqformat.h"
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
	macaddr    *result;
	int			a,
				b,
				c,
				d,
				e,
				f;
	char		junk[2];
	int			count;

	/* %1s matches iff there is trailing non-whitespace garbage */

	count = sscanf(str, "%x:%x:%x:%x:%x:%x%1s",
				   &a, &b, &c, &d, &e, &f, junk);
	if (count != 6)
		count = sscanf(str, "%x-%x-%x-%x-%x-%x%1s",
					   &a, &b, &c, &d, &e, &f, junk);
	if (count != 6)
		count = sscanf(str, "%2x%2x%2x:%2x%2x%2x%1s",
					   &a, &b, &c, &d, &e, &f, junk);
	if (count != 6)
		count = sscanf(str, "%2x%2x%2x-%2x%2x%2x%1s",
					   &a, &b, &c, &d, &e, &f, junk);
	if (count != 6)
		count = sscanf(str, "%2x%2x.%2x%2x.%2x%2x%1s",
					   &a, &b, &c, &d, &e, &f, junk);
	if (count != 6)
		count = sscanf(str, "%2x%2x%2x%2x%2x%2x%1s",
					   &a, &b, &c, &d, &e, &f, junk);
	if (count != 6)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			  errmsg("invalid input syntax for type macaddr: \"%s\"", str)));

	if ((a < 0) || (a > 255) || (b < 0) || (b > 255) ||
		(c < 0) || (c > 255) || (d < 0) || (d > 255) ||
		(e < 0) || (e > 255) || (f < 0) || (f > 255))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
		   errmsg("invalid octet value in \"macaddr\" value: \"%s\"", str)));

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
	macaddr    *addr = PG_GETARG_MACADDR_P(0);
	char	   *result;

	result = (char *) palloc(32);

	snprintf(result, 32, "%02x:%02x:%02x:%02x:%02x:%02x",
			 addr->a, addr->b, addr->c, addr->d, addr->e, addr->f);

	PG_RETURN_CSTRING(result);
}

/*
 *		macaddr_recv			- converts external binary format to macaddr
 *
 * The external representation is just the six bytes, MSB first.
 */
Datum
macaddr_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	macaddr    *addr;

	addr = (macaddr *) palloc(sizeof(macaddr));

	addr->a = pq_getmsgbyte(buf);
	addr->b = pq_getmsgbyte(buf);
	addr->c = pq_getmsgbyte(buf);
	addr->d = pq_getmsgbyte(buf);
	addr->e = pq_getmsgbyte(buf);
	addr->f = pq_getmsgbyte(buf);

	PG_RETURN_MACADDR_P(addr);
}

/*
 *		macaddr_send			- converts macaddr to binary format
 */
Datum
macaddr_send(PG_FUNCTION_ARGS)
{
	macaddr    *addr = PG_GETARG_MACADDR_P(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendbyte(&buf, addr->a);
	pq_sendbyte(&buf, addr->b);
	pq_sendbyte(&buf, addr->c);
	pq_sendbyte(&buf, addr->d);
	pq_sendbyte(&buf, addr->e);
	pq_sendbyte(&buf, addr->f);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
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
	macaddr    *a1 = PG_GETARG_MACADDR_P(0);
	macaddr    *a2 = PG_GETARG_MACADDR_P(1);

	PG_RETURN_INT32(macaddr_cmp_internal(a1, a2));
}

/*
 *	Boolean comparisons.
 */

Datum
macaddr_lt(PG_FUNCTION_ARGS)
{
	macaddr    *a1 = PG_GETARG_MACADDR_P(0);
	macaddr    *a2 = PG_GETARG_MACADDR_P(1);

	PG_RETURN_BOOL(macaddr_cmp_internal(a1, a2) < 0);
}

Datum
macaddr_le(PG_FUNCTION_ARGS)
{
	macaddr    *a1 = PG_GETARG_MACADDR_P(0);
	macaddr    *a2 = PG_GETARG_MACADDR_P(1);

	PG_RETURN_BOOL(macaddr_cmp_internal(a1, a2) <= 0);
}

Datum
macaddr_eq(PG_FUNCTION_ARGS)
{
	macaddr    *a1 = PG_GETARG_MACADDR_P(0);
	macaddr    *a2 = PG_GETARG_MACADDR_P(1);

	PG_RETURN_BOOL(macaddr_cmp_internal(a1, a2) == 0);
}

Datum
macaddr_ge(PG_FUNCTION_ARGS)
{
	macaddr    *a1 = PG_GETARG_MACADDR_P(0);
	macaddr    *a2 = PG_GETARG_MACADDR_P(1);

	PG_RETURN_BOOL(macaddr_cmp_internal(a1, a2) >= 0);
}

Datum
macaddr_gt(PG_FUNCTION_ARGS)
{
	macaddr    *a1 = PG_GETARG_MACADDR_P(0);
	macaddr    *a2 = PG_GETARG_MACADDR_P(1);

	PG_RETURN_BOOL(macaddr_cmp_internal(a1, a2) > 0);
}

Datum
macaddr_ne(PG_FUNCTION_ARGS)
{
	macaddr    *a1 = PG_GETARG_MACADDR_P(0);
	macaddr    *a2 = PG_GETARG_MACADDR_P(1);

	PG_RETURN_BOOL(macaddr_cmp_internal(a1, a2) != 0);
}

/*
 * Support function for hash indexes on macaddr.
 */
Datum
hashmacaddr(PG_FUNCTION_ARGS)
{
	macaddr    *key = PG_GETARG_MACADDR_P(0);

	return hash_any((unsigned char *) key, sizeof(macaddr));
}

/*
 *	Truncation function to allow comparing mac manufacturers.
 *	From suggestion by Alex Pilosov <alex@pilosoft.com>
 */
Datum
macaddr_trunc(PG_FUNCTION_ARGS)
{
	macaddr    *addr = PG_GETARG_MACADDR_P(0);
	macaddr    *result;

	result = (macaddr *) palloc(sizeof(macaddr));

	result->a = addr->a;
	result->b = addr->b;
	result->c = addr->c;
	result->d = 0;
	result->e = 0;
	result->f = 0;

	PG_RETURN_MACADDR_P(result);
}
