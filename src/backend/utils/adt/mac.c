/*-------------------------------------------------------------------------
 *
 * mac.c
 *	  PostgreSQL type definitions for 6 byte, EUI-48, MAC addresses.
 *
 * Portions Copyright (c) 1998-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/backend/utils/adt/mac.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "common/hashfn.h"
#include "lib/hyperloglog.h"
#include "libpq/pqformat.h"
#include "port/pg_bswap.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/inet.h"
#include "utils/sortsupport.h"


/*
 *	Utility macros used for sorting and comparing:
 */

#define hibits(addr) \
  ((unsigned long)(((addr)->a<<16)|((addr)->b<<8)|((addr)->c)))

#define lobits(addr) \
  ((unsigned long)(((addr)->d<<16)|((addr)->e<<8)|((addr)->f)))

/* sortsupport for macaddr */
typedef struct
{
	int64		input_count;	/* number of non-null values seen */
	bool		estimating;		/* true if estimating cardinality */

	hyperLogLogState abbr_card; /* cardinality estimator */
} macaddr_sortsupport_state;

static int	macaddr_cmp_internal(macaddr *a1, macaddr *a2);
static int	macaddr_fast_cmp(Datum x, Datum y, SortSupport ssup);
static bool macaddr_abbrev_abort(int memtupcount, SortSupport ssup);
static Datum macaddr_abbrev_convert(Datum original, SortSupport ssup);

/*
 *	MAC address reader.  Accepts several common notations.
 */

Datum
macaddr_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	Node	   *escontext = fcinfo->context;
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
		count = sscanf(str, "%2x%2x-%2x%2x-%2x%2x%1s",
					   &a, &b, &c, &d, &e, &f, junk);
	if (count != 6)
		count = sscanf(str, "%2x%2x%2x%2x%2x%2x%1s",
					   &a, &b, &c, &d, &e, &f, junk);
	if (count != 6)
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s: \"%s\"", "macaddr",
						str)));

	if ((a < 0) || (a > 255) || (b < 0) || (b > 255) ||
		(c < 0) || (c > 255) || (d < 0) || (d > 255) ||
		(e < 0) || (e > 255) || (f < 0) || (f > 255))
		ereturn(escontext, (Datum) 0,
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

static int
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

Datum
hashmacaddrextended(PG_FUNCTION_ARGS)
{
	macaddr    *key = PG_GETARG_MACADDR_P(0);

	return hash_any_extended((unsigned char *) key, sizeof(macaddr),
							 PG_GETARG_INT64(1));
}

/*
 * Arithmetic functions: bitwise NOT, AND, OR.
 */
Datum
macaddr_not(PG_FUNCTION_ARGS)
{
	macaddr    *addr = PG_GETARG_MACADDR_P(0);
	macaddr    *result;

	result = (macaddr *) palloc(sizeof(macaddr));
	result->a = ~addr->a;
	result->b = ~addr->b;
	result->c = ~addr->c;
	result->d = ~addr->d;
	result->e = ~addr->e;
	result->f = ~addr->f;
	PG_RETURN_MACADDR_P(result);
}

Datum
macaddr_and(PG_FUNCTION_ARGS)
{
	macaddr    *addr1 = PG_GETARG_MACADDR_P(0);
	macaddr    *addr2 = PG_GETARG_MACADDR_P(1);
	macaddr    *result;

	result = (macaddr *) palloc(sizeof(macaddr));
	result->a = addr1->a & addr2->a;
	result->b = addr1->b & addr2->b;
	result->c = addr1->c & addr2->c;
	result->d = addr1->d & addr2->d;
	result->e = addr1->e & addr2->e;
	result->f = addr1->f & addr2->f;
	PG_RETURN_MACADDR_P(result);
}

Datum
macaddr_or(PG_FUNCTION_ARGS)
{
	macaddr    *addr1 = PG_GETARG_MACADDR_P(0);
	macaddr    *addr2 = PG_GETARG_MACADDR_P(1);
	macaddr    *result;

	result = (macaddr *) palloc(sizeof(macaddr));
	result->a = addr1->a | addr2->a;
	result->b = addr1->b | addr2->b;
	result->c = addr1->c | addr2->c;
	result->d = addr1->d | addr2->d;
	result->e = addr1->e | addr2->e;
	result->f = addr1->f | addr2->f;
	PG_RETURN_MACADDR_P(result);
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

/*
 * SortSupport strategy function. Populates a SortSupport struct with the
 * information necessary to use comparison by abbreviated keys.
 */
Datum
macaddr_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	ssup->comparator = macaddr_fast_cmp;
	ssup->ssup_extra = NULL;

	if (ssup->abbreviate)
	{
		macaddr_sortsupport_state *uss;
		MemoryContext oldcontext;

		oldcontext = MemoryContextSwitchTo(ssup->ssup_cxt);

		uss = palloc(sizeof(macaddr_sortsupport_state));
		uss->input_count = 0;
		uss->estimating = true;
		initHyperLogLog(&uss->abbr_card, 10);

		ssup->ssup_extra = uss;

		ssup->comparator = ssup_datum_unsigned_cmp;
		ssup->abbrev_converter = macaddr_abbrev_convert;
		ssup->abbrev_abort = macaddr_abbrev_abort;
		ssup->abbrev_full_comparator = macaddr_fast_cmp;

		MemoryContextSwitchTo(oldcontext);
	}

	PG_RETURN_VOID();
}

/*
 * SortSupport "traditional" comparison function. Pulls two MAC addresses from
 * the heap and runs a standard comparison on them.
 */
static int
macaddr_fast_cmp(Datum x, Datum y, SortSupport ssup)
{
	macaddr    *arg1 = DatumGetMacaddrP(x);
	macaddr    *arg2 = DatumGetMacaddrP(y);

	return macaddr_cmp_internal(arg1, arg2);
}

/*
 * Callback for estimating effectiveness of abbreviated key optimization.
 *
 * We pay no attention to the cardinality of the non-abbreviated data, because
 * there is no equality fast-path within authoritative macaddr comparator.
 */
static bool
macaddr_abbrev_abort(int memtupcount, SortSupport ssup)
{
	macaddr_sortsupport_state *uss = ssup->ssup_extra;
	double		abbr_card;

	if (memtupcount < 10000 || uss->input_count < 10000 || !uss->estimating)
		return false;

	abbr_card = estimateHyperLogLog(&uss->abbr_card);

	/*
	 * If we have >100k distinct values, then even if we were sorting many
	 * billion rows we'd likely still break even, and the penalty of undoing
	 * that many rows of abbrevs would probably not be worth it. At this point
	 * we stop counting because we know that we're now fully committed.
	 */
	if (abbr_card > 100000.0)
	{
#ifdef TRACE_SORT
		if (trace_sort)
			elog(LOG,
				 "macaddr_abbrev: estimation ends at cardinality %f"
				 " after " INT64_FORMAT " values (%d rows)",
				 abbr_card, uss->input_count, memtupcount);
#endif
		uss->estimating = false;
		return false;
	}

	/*
	 * Target minimum cardinality is 1 per ~2k of non-null inputs. 0.5 row
	 * fudge factor allows us to abort earlier on genuinely pathological data
	 * where we've had exactly one abbreviated value in the first 2k
	 * (non-null) rows.
	 */
	if (abbr_card < uss->input_count / 2000.0 + 0.5)
	{
#ifdef TRACE_SORT
		if (trace_sort)
			elog(LOG,
				 "macaddr_abbrev: aborting abbreviation at cardinality %f"
				 " below threshold %f after " INT64_FORMAT " values (%d rows)",
				 abbr_card, uss->input_count / 2000.0 + 0.5, uss->input_count,
				 memtupcount);
#endif
		return true;
	}

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG,
			 "macaddr_abbrev: cardinality %f after " INT64_FORMAT
			 " values (%d rows)", abbr_card, uss->input_count, memtupcount);
#endif

	return false;
}

/*
 * SortSupport conversion routine. Converts original macaddr representation
 * to abbreviated key representation.
 *
 * Packs the bytes of a 6-byte MAC address into a Datum and treats it as an
 * unsigned integer for purposes of comparison. On a 64-bit machine, there
 * will be two zeroed bytes of padding. The integer is converted to native
 * endianness to facilitate easy comparison.
 */
static Datum
macaddr_abbrev_convert(Datum original, SortSupport ssup)
{
	macaddr_sortsupport_state *uss = ssup->ssup_extra;
	macaddr    *authoritative = DatumGetMacaddrP(original);
	Datum		res;

	/*
	 * On a 64-bit machine, zero out the 8-byte datum and copy the 6 bytes of
	 * the MAC address in. There will be two bytes of zero padding on the end
	 * of the least significant bits.
	 */
#if SIZEOF_DATUM == 8
	memset(&res, 0, SIZEOF_DATUM);
	memcpy(&res, authoritative, sizeof(macaddr));
#else							/* SIZEOF_DATUM != 8 */
	memcpy(&res, authoritative, SIZEOF_DATUM);
#endif
	uss->input_count += 1;

	/*
	 * Cardinality estimation. The estimate uses uint32, so on a 64-bit
	 * architecture, XOR the two 32-bit halves together to produce slightly
	 * more entropy. The two zeroed bytes won't have any practical impact on
	 * this operation.
	 */
	if (uss->estimating)
	{
		uint32		tmp;

#if SIZEOF_DATUM == 8
		tmp = (uint32) res ^ (uint32) ((uint64) res >> 32);
#else							/* SIZEOF_DATUM != 8 */
		tmp = (uint32) res;
#endif

		addHyperLogLog(&uss->abbr_card, DatumGetUInt32(hash_uint32(tmp)));
	}

	/*
	 * Byteswap on little-endian machines.
	 *
	 * This is needed so that ssup_datum_unsigned_cmp() (an unsigned integer
	 * 3-way comparator) works correctly on all platforms. Without this, the
	 * comparator would have to call memcmp() with a pair of pointers to the
	 * first byte of each abbreviated key, which is slower.
	 */
	res = DatumBigEndianToNative(res);

	return res;
}
