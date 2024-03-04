/*-------------------------------------------------------------------------
 *
 * mac8.c
 *	  PostgreSQL type definitions for 8 byte (EUI-64) MAC addresses.
 *
 * EUI-48 (6 byte) MAC addresses are accepted as input and are stored in
 * EUI-64 format, with the 4th and 5th bytes set to FF and FE, respectively.
 *
 * Output is always in 8 byte (EUI-64) format.
 *
 * The following code is written with the assumption that the OUI field
 * size is 24 bits.
 *
 * Portions Copyright (c) 1998-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/backend/utils/adt/mac8.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "common/hashfn.h"
#include "libpq/pqformat.h"
#include "nodes/nodes.h"
#include "utils/fmgrprotos.h"
#include "utils/inet.h"

/*
 *	Utility macros used for sorting and comparing:
 */
#define hibits(addr) \
  ((unsigned long)(((addr)->a<<24) | ((addr)->b<<16) | ((addr)->c<<8) | ((addr)->d)))

#define lobits(addr) \
  ((unsigned long)(((addr)->e<<24) | ((addr)->f<<16) | ((addr)->g<<8) | ((addr)->h)))

static unsigned char hex2_to_uchar(const unsigned char *ptr, bool *badhex);

static const signed char hexlookup[128] = {
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, -1, -1, -1, -1, -1, -1,
	-1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};

/*
 * hex2_to_uchar - convert 2 hex digits to a byte (unsigned char)
 *
 * Sets *badhex to true if the end of the string is reached ('\0' found), or if
 * either character is not a valid hex digit.
 */
static inline unsigned char
hex2_to_uchar(const unsigned char *ptr, bool *badhex)
{
	unsigned char ret;
	signed char lookup;

	/* Handle the first character */
	if (*ptr > 127)
		goto invalid_input;

	lookup = hexlookup[*ptr];
	if (lookup < 0)
		goto invalid_input;

	ret = lookup << 4;

	/* Move to the second character */
	ptr++;

	if (*ptr > 127)
		goto invalid_input;

	lookup = hexlookup[*ptr];
	if (lookup < 0)
		goto invalid_input;

	ret += lookup;

	return ret;

invalid_input:
	*badhex = true;
	return 0;
}

/*
 * MAC address (EUI-48 and EUI-64) reader. Accepts several common notations.
 */
Datum
macaddr8_in(PG_FUNCTION_ARGS)
{
	const unsigned char *str = (unsigned char *) PG_GETARG_CSTRING(0);
	Node	   *escontext = fcinfo->context;
	const unsigned char *ptr = str;
	bool		badhex = false;
	macaddr8   *result;
	unsigned char a = 0,
				b = 0,
				c = 0,
				d = 0,
				e = 0,
				f = 0,
				g = 0,
				h = 0;
	int			count = 0;
	unsigned char spacer = '\0';

	/* skip leading spaces */
	while (*ptr && isspace(*ptr))
		ptr++;

	/* digits must always come in pairs */
	while (*ptr && *(ptr + 1))
	{
		/*
		 * Attempt to decode each byte, which must be 2 hex digits in a row.
		 * If either digit is not hex, hex2_to_uchar will throw ereport() for
		 * us.  Either 6 or 8 byte MAC addresses are supported.
		 */

		/* Attempt to collect a byte */
		count++;

		switch (count)
		{
			case 1:
				a = hex2_to_uchar(ptr, &badhex);
				break;
			case 2:
				b = hex2_to_uchar(ptr, &badhex);
				break;
			case 3:
				c = hex2_to_uchar(ptr, &badhex);
				break;
			case 4:
				d = hex2_to_uchar(ptr, &badhex);
				break;
			case 5:
				e = hex2_to_uchar(ptr, &badhex);
				break;
			case 6:
				f = hex2_to_uchar(ptr, &badhex);
				break;
			case 7:
				g = hex2_to_uchar(ptr, &badhex);
				break;
			case 8:
				h = hex2_to_uchar(ptr, &badhex);
				break;
			default:
				/* must be trailing garbage... */
				goto fail;
		}

		if (badhex)
			goto fail;

		/* Move forward to where the next byte should be */
		ptr += 2;

		/* Check for a spacer, these are valid, anything else is not */
		if (*ptr == ':' || *ptr == '-' || *ptr == '.')
		{
			/* remember the spacer used, if it changes then it isn't valid */
			if (spacer == '\0')
				spacer = *ptr;

			/* Have to use the same spacer throughout */
			else if (spacer != *ptr)
				goto fail;

			/* move past the spacer */
			ptr++;
		}

		/* allow trailing whitespace after if we have 6 or 8 bytes */
		if (count == 6 || count == 8)
		{
			if (isspace(*ptr))
			{
				while (*++ptr && isspace(*ptr));

				/* If we found a space and then non-space, it's invalid */
				if (*ptr)
					goto fail;
			}
		}
	}

	/* Convert a 6 byte MAC address to macaddr8 */
	if (count == 6)
	{
		h = f;
		g = e;
		f = d;

		d = 0xFF;
		e = 0xFE;
	}
	else if (count != 8)
		goto fail;

	result = (macaddr8 *) palloc0(sizeof(macaddr8));

	result->a = a;
	result->b = b;
	result->c = c;
	result->d = d;
	result->e = e;
	result->f = f;
	result->g = g;
	result->h = h;

	PG_RETURN_MACADDR8_P(result);

fail:
	ereturn(escontext, (Datum) 0,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("invalid input syntax for type %s: \"%s\"", "macaddr8",
					str)));
}

/*
 * MAC8 address (EUI-64) output function. Fixed format.
 */
Datum
macaddr8_out(PG_FUNCTION_ARGS)
{
	macaddr8   *addr = PG_GETARG_MACADDR8_P(0);
	char	   *result;

	result = (char *) palloc(32);

	snprintf(result, 32, "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
			 addr->a, addr->b, addr->c, addr->d,
			 addr->e, addr->f, addr->g, addr->h);

	PG_RETURN_CSTRING(result);
}

/*
 * macaddr8_recv - converts external binary format(EUI-48 and EUI-64) to macaddr8
 *
 * The external representation is just the eight bytes, MSB first.
 */
Datum
macaddr8_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	macaddr8   *addr;

	addr = (macaddr8 *) palloc0(sizeof(macaddr8));

	addr->a = pq_getmsgbyte(buf);
	addr->b = pq_getmsgbyte(buf);
	addr->c = pq_getmsgbyte(buf);

	if (buf->len == 6)
	{
		addr->d = 0xFF;
		addr->e = 0xFE;
	}
	else
	{
		addr->d = pq_getmsgbyte(buf);
		addr->e = pq_getmsgbyte(buf);
	}

	addr->f = pq_getmsgbyte(buf);
	addr->g = pq_getmsgbyte(buf);
	addr->h = pq_getmsgbyte(buf);

	PG_RETURN_MACADDR8_P(addr);
}

/*
 * macaddr8_send - converts macaddr8(EUI-64) to binary format
 */
Datum
macaddr8_send(PG_FUNCTION_ARGS)
{
	macaddr8   *addr = PG_GETARG_MACADDR8_P(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendbyte(&buf, addr->a);
	pq_sendbyte(&buf, addr->b);
	pq_sendbyte(&buf, addr->c);
	pq_sendbyte(&buf, addr->d);
	pq_sendbyte(&buf, addr->e);
	pq_sendbyte(&buf, addr->f);
	pq_sendbyte(&buf, addr->g);
	pq_sendbyte(&buf, addr->h);

	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


/*
 * macaddr8_cmp_internal - comparison function for sorting:
 */
static int32
macaddr8_cmp_internal(macaddr8 *a1, macaddr8 *a2)
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
macaddr8_cmp(PG_FUNCTION_ARGS)
{
	macaddr8   *a1 = PG_GETARG_MACADDR8_P(0);
	macaddr8   *a2 = PG_GETARG_MACADDR8_P(1);

	PG_RETURN_INT32(macaddr8_cmp_internal(a1, a2));
}

/*
 * Boolean comparison functions.
 */

Datum
macaddr8_lt(PG_FUNCTION_ARGS)
{
	macaddr8   *a1 = PG_GETARG_MACADDR8_P(0);
	macaddr8   *a2 = PG_GETARG_MACADDR8_P(1);

	PG_RETURN_BOOL(macaddr8_cmp_internal(a1, a2) < 0);
}

Datum
macaddr8_le(PG_FUNCTION_ARGS)
{
	macaddr8   *a1 = PG_GETARG_MACADDR8_P(0);
	macaddr8   *a2 = PG_GETARG_MACADDR8_P(1);

	PG_RETURN_BOOL(macaddr8_cmp_internal(a1, a2) <= 0);
}

Datum
macaddr8_eq(PG_FUNCTION_ARGS)
{
	macaddr8   *a1 = PG_GETARG_MACADDR8_P(0);
	macaddr8   *a2 = PG_GETARG_MACADDR8_P(1);

	PG_RETURN_BOOL(macaddr8_cmp_internal(a1, a2) == 0);
}

Datum
macaddr8_ge(PG_FUNCTION_ARGS)
{
	macaddr8   *a1 = PG_GETARG_MACADDR8_P(0);
	macaddr8   *a2 = PG_GETARG_MACADDR8_P(1);

	PG_RETURN_BOOL(macaddr8_cmp_internal(a1, a2) >= 0);
}

Datum
macaddr8_gt(PG_FUNCTION_ARGS)
{
	macaddr8   *a1 = PG_GETARG_MACADDR8_P(0);
	macaddr8   *a2 = PG_GETARG_MACADDR8_P(1);

	PG_RETURN_BOOL(macaddr8_cmp_internal(a1, a2) > 0);
}

Datum
macaddr8_ne(PG_FUNCTION_ARGS)
{
	macaddr8   *a1 = PG_GETARG_MACADDR8_P(0);
	macaddr8   *a2 = PG_GETARG_MACADDR8_P(1);

	PG_RETURN_BOOL(macaddr8_cmp_internal(a1, a2) != 0);
}

/*
 * Support function for hash indexes on macaddr8.
 */
Datum
hashmacaddr8(PG_FUNCTION_ARGS)
{
	macaddr8   *key = PG_GETARG_MACADDR8_P(0);

	return hash_any((unsigned char *) key, sizeof(macaddr8));
}

Datum
hashmacaddr8extended(PG_FUNCTION_ARGS)
{
	macaddr8   *key = PG_GETARG_MACADDR8_P(0);

	return hash_any_extended((unsigned char *) key, sizeof(macaddr8),
							 PG_GETARG_INT64(1));
}

/*
 * Arithmetic functions: bitwise NOT, AND, OR.
 */
Datum
macaddr8_not(PG_FUNCTION_ARGS)
{
	macaddr8   *addr = PG_GETARG_MACADDR8_P(0);
	macaddr8   *result;

	result = (macaddr8 *) palloc0(sizeof(macaddr8));
	result->a = ~addr->a;
	result->b = ~addr->b;
	result->c = ~addr->c;
	result->d = ~addr->d;
	result->e = ~addr->e;
	result->f = ~addr->f;
	result->g = ~addr->g;
	result->h = ~addr->h;

	PG_RETURN_MACADDR8_P(result);
}

Datum
macaddr8_and(PG_FUNCTION_ARGS)
{
	macaddr8   *addr1 = PG_GETARG_MACADDR8_P(0);
	macaddr8   *addr2 = PG_GETARG_MACADDR8_P(1);
	macaddr8   *result;

	result = (macaddr8 *) palloc0(sizeof(macaddr8));
	result->a = addr1->a & addr2->a;
	result->b = addr1->b & addr2->b;
	result->c = addr1->c & addr2->c;
	result->d = addr1->d & addr2->d;
	result->e = addr1->e & addr2->e;
	result->f = addr1->f & addr2->f;
	result->g = addr1->g & addr2->g;
	result->h = addr1->h & addr2->h;

	PG_RETURN_MACADDR8_P(result);
}

Datum
macaddr8_or(PG_FUNCTION_ARGS)
{
	macaddr8   *addr1 = PG_GETARG_MACADDR8_P(0);
	macaddr8   *addr2 = PG_GETARG_MACADDR8_P(1);
	macaddr8   *result;

	result = (macaddr8 *) palloc0(sizeof(macaddr8));
	result->a = addr1->a | addr2->a;
	result->b = addr1->b | addr2->b;
	result->c = addr1->c | addr2->c;
	result->d = addr1->d | addr2->d;
	result->e = addr1->e | addr2->e;
	result->f = addr1->f | addr2->f;
	result->g = addr1->g | addr2->g;
	result->h = addr1->h | addr2->h;

	PG_RETURN_MACADDR8_P(result);
}

/*
 * Truncation function to allow comparing macaddr8 manufacturers.
 */
Datum
macaddr8_trunc(PG_FUNCTION_ARGS)
{
	macaddr8   *addr = PG_GETARG_MACADDR8_P(0);
	macaddr8   *result;

	result = (macaddr8 *) palloc0(sizeof(macaddr8));

	result->a = addr->a;
	result->b = addr->b;
	result->c = addr->c;
	result->d = 0;
	result->e = 0;
	result->f = 0;
	result->g = 0;
	result->h = 0;

	PG_RETURN_MACADDR8_P(result);
}

/*
 * Set 7th bit for modified EUI-64 as used in IPv6.
 */
Datum
macaddr8_set7bit(PG_FUNCTION_ARGS)
{
	macaddr8   *addr = PG_GETARG_MACADDR8_P(0);
	macaddr8   *result;

	result = (macaddr8 *) palloc0(sizeof(macaddr8));

	result->a = addr->a | 0x02;
	result->b = addr->b;
	result->c = addr->c;
	result->d = addr->d;
	result->e = addr->e;
	result->f = addr->f;
	result->g = addr->g;
	result->h = addr->h;

	PG_RETURN_MACADDR8_P(result);
}

/*----------------------------------------------------------
 *	Conversion operators.
 *---------------------------------------------------------*/

Datum
macaddrtomacaddr8(PG_FUNCTION_ARGS)
{
	macaddr    *addr6 = PG_GETARG_MACADDR_P(0);
	macaddr8   *result;

	result = (macaddr8 *) palloc0(sizeof(macaddr8));

	result->a = addr6->a;
	result->b = addr6->b;
	result->c = addr6->c;
	result->d = 0xFF;
	result->e = 0xFE;
	result->f = addr6->d;
	result->g = addr6->e;
	result->h = addr6->f;


	PG_RETURN_MACADDR8_P(result);
}

Datum
macaddr8tomacaddr(PG_FUNCTION_ARGS)
{
	macaddr8   *addr = PG_GETARG_MACADDR8_P(0);
	macaddr    *result;

	result = (macaddr *) palloc0(sizeof(macaddr));

	if ((addr->d != 0xFF) || (addr->e != 0xFE))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("macaddr8 data out of range to convert to macaddr"),
				 errhint("Only addresses that have FF and FE as values in the "
						 "4th and 5th bytes from the left, for example "
						 "xx:xx:xx:ff:fe:xx:xx:xx, are eligible to be converted "
						 "from macaddr8 to macaddr.")));

	result->a = addr->a;
	result->b = addr->b;
	result->c = addr->c;
	result->d = addr->f;
	result->e = addr->g;
	result->f = addr->h;

	PG_RETURN_MACADDR_P(result);
}
