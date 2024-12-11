/*-------------------------------------------------------------------------
 *
 * uuid.c
 *	  Functions for the built-in type "uuid".
 *
 * Copyright (c) 2007-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/uuid.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <time.h>				/* for clock_gettime() */

#include "common/hashfn.h"
#include "lib/hyperloglog.h"
#include "libpq/pqformat.h"
#include "port/pg_bswap.h"
#include "utils/fmgrprotos.h"
#include "utils/guc.h"
#include "utils/sortsupport.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"

/* helper macros */
#define NS_PER_S	INT64CONST(1000000000)
#define NS_PER_MS	INT64CONST(1000000)
#define NS_PER_US	INT64CONST(1000)

/*
 * UUID version 7 uses 12 bits in "rand_a" to store  1/4096 (or 2^12) fractions of
 * sub-millisecond. While most Unix-like platforms provide nanosecond-precision
 * timestamps, some systems only offer microsecond precision, limiting us to 10
 * bits of sub-millisecond information. For example, on macOS, real time is
 * truncated to microseconds. Additionally, MSVC uses the ported version of
 * gettimeofday() that returns microsecond precision.
 *
 * On systems with only 10 bits of sub-millisecond precision, we still use
 * 1/4096 parts of a millisecond, but fill lower 2 bits with random numbers
 * (see generate_uuidv7() for details).
 *
 * SUBMS_MINIMAL_STEP_NS defines the minimum number of nanoseconds that guarantees
 * an increase in the UUID's clock precision.
 */
#if defined(__darwin__) || defined(_MSC_VER)
#define SUBMS_MINIMAL_STEP_BITS 10
#else
#define SUBMS_MINIMAL_STEP_BITS 12
#endif
#define SUBMS_BITS	12
#define SUBMS_MINIMAL_STEP_NS ((NS_PER_MS / (1 << SUBMS_MINIMAL_STEP_BITS)) + 1)

/* sortsupport for uuid */
typedef struct
{
	int64		input_count;	/* number of non-null values seen */
	bool		estimating;		/* true if estimating cardinality */

	hyperLogLogState abbr_card; /* cardinality estimator */
} uuid_sortsupport_state;

static void string_to_uuid(const char *source, pg_uuid_t *uuid, Node *escontext);
static int	uuid_internal_cmp(const pg_uuid_t *arg1, const pg_uuid_t *arg2);
static int	uuid_fast_cmp(Datum x, Datum y, SortSupport ssup);
static bool uuid_abbrev_abort(int memtupcount, SortSupport ssup);
static Datum uuid_abbrev_convert(Datum original, SortSupport ssup);
static inline void uuid_set_version(pg_uuid_t *uuid, unsigned char version);
static inline int64 get_real_time_ns_ascending();

Datum
uuid_in(PG_FUNCTION_ARGS)
{
	char	   *uuid_str = PG_GETARG_CSTRING(0);
	pg_uuid_t  *uuid;

	uuid = (pg_uuid_t *) palloc(sizeof(*uuid));
	string_to_uuid(uuid_str, uuid, fcinfo->context);
	PG_RETURN_UUID_P(uuid);
}

Datum
uuid_out(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *uuid = PG_GETARG_UUID_P(0);
	static const char hex_chars[] = "0123456789abcdef";
	char	   *buf,
			   *p;
	int			i;

	/* counts for the four hyphens and the zero-terminator */
	buf = palloc(2 * UUID_LEN + 5);
	p = buf;
	for (i = 0; i < UUID_LEN; i++)
	{
		int			hi;
		int			lo;

		/*
		 * We print uuid values as a string of 8, 4, 4, 4, and then 12
		 * hexadecimal characters, with each group is separated by a hyphen
		 * ("-"). Therefore, add the hyphens at the appropriate places here.
		 */
		if (i == 4 || i == 6 || i == 8 || i == 10)
			*p++ = '-';

		hi = uuid->data[i] >> 4;
		lo = uuid->data[i] & 0x0F;

		*p++ = hex_chars[hi];
		*p++ = hex_chars[lo];
	}
	*p = '\0';

	PG_RETURN_CSTRING(buf);
}

/*
 * We allow UUIDs as a series of 32 hexadecimal digits with an optional dash
 * after each group of 4 hexadecimal digits, and optionally surrounded by {}.
 * (The canonical format 8x-4x-4x-4x-12x, where "nx" means n hexadecimal
 * digits, is the only one used for output.)
 */
static void
string_to_uuid(const char *source, pg_uuid_t *uuid, Node *escontext)
{
	const char *src = source;
	bool		braces = false;
	int			i;

	if (src[0] == '{')
	{
		src++;
		braces = true;
	}

	for (i = 0; i < UUID_LEN; i++)
	{
		char		str_buf[3];

		if (src[0] == '\0' || src[1] == '\0')
			goto syntax_error;
		memcpy(str_buf, src, 2);
		if (!isxdigit((unsigned char) str_buf[0]) ||
			!isxdigit((unsigned char) str_buf[1]))
			goto syntax_error;

		str_buf[2] = '\0';
		uuid->data[i] = (unsigned char) strtoul(str_buf, NULL, 16);
		src += 2;
		if (src[0] == '-' && (i % 2) == 1 && i < UUID_LEN - 1)
			src++;
	}

	if (braces)
	{
		if (*src != '}')
			goto syntax_error;
		src++;
	}

	if (*src != '\0')
		goto syntax_error;

	return;

syntax_error:
	ereturn(escontext,,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("invalid input syntax for type %s: \"%s\"",
					"uuid", source)));
}

Datum
uuid_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buffer = (StringInfo) PG_GETARG_POINTER(0);
	pg_uuid_t  *uuid;

	uuid = (pg_uuid_t *) palloc(UUID_LEN);
	memcpy(uuid->data, pq_getmsgbytes(buffer, UUID_LEN), UUID_LEN);
	PG_RETURN_POINTER(uuid);
}

Datum
uuid_send(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *uuid = PG_GETARG_UUID_P(0);
	StringInfoData buffer;

	pq_begintypsend(&buffer);
	pq_sendbytes(&buffer, uuid->data, UUID_LEN);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buffer));
}

/* internal uuid compare function */
static int
uuid_internal_cmp(const pg_uuid_t *arg1, const pg_uuid_t *arg2)
{
	return memcmp(arg1->data, arg2->data, UUID_LEN);
}

Datum
uuid_lt(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *arg1 = PG_GETARG_UUID_P(0);
	pg_uuid_t  *arg2 = PG_GETARG_UUID_P(1);

	PG_RETURN_BOOL(uuid_internal_cmp(arg1, arg2) < 0);
}

Datum
uuid_le(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *arg1 = PG_GETARG_UUID_P(0);
	pg_uuid_t  *arg2 = PG_GETARG_UUID_P(1);

	PG_RETURN_BOOL(uuid_internal_cmp(arg1, arg2) <= 0);
}

Datum
uuid_eq(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *arg1 = PG_GETARG_UUID_P(0);
	pg_uuid_t  *arg2 = PG_GETARG_UUID_P(1);

	PG_RETURN_BOOL(uuid_internal_cmp(arg1, arg2) == 0);
}

Datum
uuid_ge(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *arg1 = PG_GETARG_UUID_P(0);
	pg_uuid_t  *arg2 = PG_GETARG_UUID_P(1);

	PG_RETURN_BOOL(uuid_internal_cmp(arg1, arg2) >= 0);
}

Datum
uuid_gt(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *arg1 = PG_GETARG_UUID_P(0);
	pg_uuid_t  *arg2 = PG_GETARG_UUID_P(1);

	PG_RETURN_BOOL(uuid_internal_cmp(arg1, arg2) > 0);
}

Datum
uuid_ne(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *arg1 = PG_GETARG_UUID_P(0);
	pg_uuid_t  *arg2 = PG_GETARG_UUID_P(1);

	PG_RETURN_BOOL(uuid_internal_cmp(arg1, arg2) != 0);
}

/* handler for btree index operator */
Datum
uuid_cmp(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *arg1 = PG_GETARG_UUID_P(0);
	pg_uuid_t  *arg2 = PG_GETARG_UUID_P(1);

	PG_RETURN_INT32(uuid_internal_cmp(arg1, arg2));
}

/*
 * Sort support strategy routine
 */
Datum
uuid_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	ssup->comparator = uuid_fast_cmp;
	ssup->ssup_extra = NULL;

	if (ssup->abbreviate)
	{
		uuid_sortsupport_state *uss;
		MemoryContext oldcontext;

		oldcontext = MemoryContextSwitchTo(ssup->ssup_cxt);

		uss = palloc(sizeof(uuid_sortsupport_state));
		uss->input_count = 0;
		uss->estimating = true;
		initHyperLogLog(&uss->abbr_card, 10);

		ssup->ssup_extra = uss;

		ssup->comparator = ssup_datum_unsigned_cmp;
		ssup->abbrev_converter = uuid_abbrev_convert;
		ssup->abbrev_abort = uuid_abbrev_abort;
		ssup->abbrev_full_comparator = uuid_fast_cmp;

		MemoryContextSwitchTo(oldcontext);
	}

	PG_RETURN_VOID();
}

/*
 * SortSupport comparison func
 */
static int
uuid_fast_cmp(Datum x, Datum y, SortSupport ssup)
{
	pg_uuid_t  *arg1 = DatumGetUUIDP(x);
	pg_uuid_t  *arg2 = DatumGetUUIDP(y);

	return uuid_internal_cmp(arg1, arg2);
}

/*
 * Callback for estimating effectiveness of abbreviated key optimization.
 *
 * We pay no attention to the cardinality of the non-abbreviated data, because
 * there is no equality fast-path within authoritative uuid comparator.
 */
static bool
uuid_abbrev_abort(int memtupcount, SortSupport ssup)
{
	uuid_sortsupport_state *uss = ssup->ssup_extra;
	double		abbr_card;

	if (memtupcount < 10000 || uss->input_count < 10000 || !uss->estimating)
		return false;

	abbr_card = estimateHyperLogLog(&uss->abbr_card);

	/*
	 * If we have >100k distinct values, then even if we were sorting many
	 * billion rows we'd likely still break even, and the penalty of undoing
	 * that many rows of abbrevs would probably not be worth it.  Stop even
	 * counting at that point.
	 */
	if (abbr_card > 100000.0)
	{
		if (trace_sort)
			elog(LOG,
				 "uuid_abbrev: estimation ends at cardinality %f"
				 " after " INT64_FORMAT " values (%d rows)",
				 abbr_card, uss->input_count, memtupcount);
		uss->estimating = false;
		return false;
	}

	/*
	 * Target minimum cardinality is 1 per ~2k of non-null inputs.  0.5 row
	 * fudge factor allows us to abort earlier on genuinely pathological data
	 * where we've had exactly one abbreviated value in the first 2k
	 * (non-null) rows.
	 */
	if (abbr_card < uss->input_count / 2000.0 + 0.5)
	{
		if (trace_sort)
			elog(LOG,
				 "uuid_abbrev: aborting abbreviation at cardinality %f"
				 " below threshold %f after " INT64_FORMAT " values (%d rows)",
				 abbr_card, uss->input_count / 2000.0 + 0.5, uss->input_count,
				 memtupcount);
		return true;
	}

	if (trace_sort)
		elog(LOG,
			 "uuid_abbrev: cardinality %f after " INT64_FORMAT
			 " values (%d rows)", abbr_card, uss->input_count, memtupcount);

	return false;
}

/*
 * Conversion routine for sortsupport.  Converts original uuid representation
 * to abbreviated key representation.  Our encoding strategy is simple -- pack
 * the first `sizeof(Datum)` bytes of uuid data into a Datum (on little-endian
 * machines, the bytes are stored in reverse order), and treat it as an
 * unsigned integer.
 */
static Datum
uuid_abbrev_convert(Datum original, SortSupport ssup)
{
	uuid_sortsupport_state *uss = ssup->ssup_extra;
	pg_uuid_t  *authoritative = DatumGetUUIDP(original);
	Datum		res;

	memcpy(&res, authoritative->data, sizeof(Datum));
	uss->input_count += 1;

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
	 * 3-way comparator) works correctly on all platforms.  If we didn't do
	 * this, the comparator would have to call memcmp() with a pair of
	 * pointers to the first byte of each abbreviated key, which is slower.
	 */
	res = DatumBigEndianToNative(res);

	return res;
}

/* hash index support */
Datum
uuid_hash(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *key = PG_GETARG_UUID_P(0);

	return hash_any(key->data, UUID_LEN);
}

Datum
uuid_hash_extended(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *key = PG_GETARG_UUID_P(0);

	return hash_any_extended(key->data, UUID_LEN, PG_GETARG_INT64(1));
}

/*
 * Set the given UUID version and the variant bits
 */
static inline void
uuid_set_version(pg_uuid_t *uuid, unsigned char version)
{
	/* set version field, top four bits */
	uuid->data[6] = (uuid->data[6] & 0x0f) | (version << 4);

	/* set variant field, top two bits are 1, 0 */
	uuid->data[8] = (uuid->data[8] & 0x3f) | 0x80;
}

/*
 * Generate UUID version 4.
 *
 * All UUID bytes are filled with strong random numbers except version and
 * variant bits.
 */
Datum
gen_random_uuid(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *uuid = palloc(UUID_LEN);

	if (!pg_strong_random(uuid, UUID_LEN))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not generate random values")));

	/*
	 * Set magic numbers for a "version 4" (pseudorandom) UUID and variant,
	 * see https://datatracker.ietf.org/doc/html/rfc9562#name-uuid-version-4
	 */
	uuid_set_version(uuid, 4);

	PG_RETURN_UUID_P(uuid);
}

/*
 * Get the current timestamp with nanosecond precision for UUID generation.
 * The returned timestamp is ensured to be at least SUBMS_MINIMAL_STEP greater
 * than the previous returned timestamp (on this backend).
 */
static inline int64
get_real_time_ns_ascending()
{
	static int64 previous_ns = 0;
	int64		ns;

	/* Get the current real timestamp */

#ifdef	_MSC_VER
	struct timeval tmp;

	gettimeofday(&tmp, NULL);
	ns = tmp.tv_sec * NS_PER_S + tmp.tv_usec * NS_PER_US;
#else
	struct timespec tmp;

	/*
	 * We don't use gettimeofday(), instead use clock_gettime() with
	 * CLOCK_REALTIME where available in order to get a high-precision
	 * (nanoseconds) real timestamp.
	 *
	 * Note while a timestamp returned by clock_gettime() with CLOCK_REALTIME
	 * is nanosecond-precision on most Unix-like platforms, on some platforms
	 * such as macOS it's restricted to microsecond-precision.
	 */
	clock_gettime(CLOCK_REALTIME, &tmp);
	ns = tmp.tv_sec * NS_PER_S + tmp.tv_nsec;
#endif

	/* Guarantee the minimal step advancement of the timestamp */
	if (previous_ns + SUBMS_MINIMAL_STEP_NS >= ns)
		ns = previous_ns + SUBMS_MINIMAL_STEP_NS;
	previous_ns = ns;

	return ns;
}

/*
 * Generate UUID version 7 per RFC 9562, with the given timestamp.
 *
 * UUID version 7 consists of a Unix timestamp in milliseconds (48 bits) and
 * 74 random bits, excluding the required version and variant bits. To ensure
 * monotonicity in scenarios of high-frequency UUID generation, we employ the
 * method "Replace Leftmost Random Bits with Increased Clock Precision (Method 3)",
 * described in the RFC. This method utilizes 12 bits from the "rand_a" bits
 * to store a 1/4096 (or 2^12) fraction of sub-millisecond precision.
 *
 * ns is a number of nanoseconds since start of the UNIX epoch. This value is
 * used for time-dependent bits of UUID.
 */
static pg_uuid_t *
generate_uuidv7(int64 ns)
{
	pg_uuid_t  *uuid = palloc(UUID_LEN);
	int64		unix_ts_ms;
	int32		increased_clock_precision;

	unix_ts_ms = ns / NS_PER_MS;

	/* Fill in time part */
	uuid->data[0] = (unsigned char) (unix_ts_ms >> 40);
	uuid->data[1] = (unsigned char) (unix_ts_ms >> 32);
	uuid->data[2] = (unsigned char) (unix_ts_ms >> 24);
	uuid->data[3] = (unsigned char) (unix_ts_ms >> 16);
	uuid->data[4] = (unsigned char) (unix_ts_ms >> 8);
	uuid->data[5] = (unsigned char) unix_ts_ms;

	/*
	 * sub-millisecond timestamp fraction (SUBMS_BITS bits, not
	 * SUBMS_MINIMAL_STEP_BITS)
	 */
	increased_clock_precision = ((ns % NS_PER_MS) * (1 << SUBMS_BITS)) / NS_PER_MS;

	/* Fill the increased clock precision to "rand_a" bits */
	uuid->data[6] = (unsigned char) (increased_clock_precision >> 8);
	uuid->data[7] = (unsigned char) (increased_clock_precision);

	/* fill everything after the increased clock precision with random bytes */
	if (!pg_strong_random(&uuid->data[8], UUID_LEN - 8))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not generate random values")));

#if SUBMS_MINIMAL_STEP_BITS == 10

	/*
	 * On systems that have only 10 bits of sub-ms precision,  2 least
	 * significant are dependent on other time-specific bits, and they do not
	 * contribute to uniqueness. To make these bit random we mix in two bits
	 * from CSPRNG. SUBMS_MINIMAL_STEP is chosen so that we still guarantee
	 * monotonicity despite altering these bits.
	 */
	uuid->data[7] = uuid->data[7] ^ (uuid->data[8] >> 6);
#endif

	/*
	 * Set magic numbers for a "version 7" (pseudorandom) UUID and variant,
	 * see https://www.rfc-editor.org/rfc/rfc9562#name-version-field
	 */
	uuid_set_version(uuid, 7);

	return uuid;
}

/*
 * Generate UUID version 7 with the current timestamp.
 */
Datum
uuidv7(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *uuid = generate_uuidv7(get_real_time_ns_ascending());

	PG_RETURN_UUID_P(uuid);
}

/*
 * Similar to uuidv7() but with the timestamp adjusted by the given interval.
 */
Datum
uuidv7_interval(PG_FUNCTION_ARGS)
{
	Interval   *shift = PG_GETARG_INTERVAL_P(0);
	TimestampTz ts;
	pg_uuid_t  *uuid;
	int64		ns = get_real_time_ns_ascending();

	/*
	 * Shift the current timestamp by the given interval. To calculate time
	 * shift correctly, we convert the UNIX epoch to TimestampTz and use
	 * timestamptz_pl_interval(). Since this calculation is done with
	 * microsecond precision, we carry nanoseconds from original ns value to
	 * shifted ns value.
	 */

	ts = (TimestampTz) (ns / NS_PER_US) -
		(POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY * USECS_PER_SEC;

	/* Compute time shift */
	ts = DatumGetTimestampTz(DirectFunctionCall2(timestamptz_pl_interval,
												 TimestampTzGetDatum(ts),
												 IntervalPGetDatum(shift)));

	/*
	 * Convert a TimestampTz value back to an UNIX epoch and back nanoseconds.
	 */
	ns = (ts + (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY * USECS_PER_SEC)
		* NS_PER_US + ns % NS_PER_US;

	/* Generate an UUIDv7 */
	uuid = generate_uuidv7(ns);

	PG_RETURN_UUID_P(uuid);
}

/*
 * Start of a Gregorian epoch == date2j(1582,10,15)
 * We cast it to 64-bit because it's used in overflow-prone computations
 */
#define GREGORIAN_EPOCH_JDATE  INT64CONST(2299161)

/*
 * Extract timestamp from UUID.
 *
 * Returns null if not RFC 9562 variant or not a version that has a timestamp.
 */
Datum
uuid_extract_timestamp(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *uuid = PG_GETARG_UUID_P(0);
	int			version;
	uint64		tms;
	TimestampTz ts;

	/* check if RFC 9562 variant */
	if ((uuid->data[8] & 0xc0) != 0x80)
		PG_RETURN_NULL();

	version = uuid->data[6] >> 4;

	if (version == 1)
	{
		tms = ((uint64) uuid->data[0] << 24)
			+ ((uint64) uuid->data[1] << 16)
			+ ((uint64) uuid->data[2] << 8)
			+ ((uint64) uuid->data[3])
			+ ((uint64) uuid->data[4] << 40)
			+ ((uint64) uuid->data[5] << 32)
			+ (((uint64) uuid->data[6] & 0xf) << 56)
			+ ((uint64) uuid->data[7] << 48);

		/* convert 100-ns intervals to us, then adjust */
		ts = (TimestampTz) (tms / 10) -
			((uint64) POSTGRES_EPOCH_JDATE - GREGORIAN_EPOCH_JDATE) * SECS_PER_DAY * USECS_PER_SEC;
		PG_RETURN_TIMESTAMPTZ(ts);
	}

	if (version == 7)
	{
		tms = (uuid->data[5])
			+ (((uint64) uuid->data[4]) << 8)
			+ (((uint64) uuid->data[3]) << 16)
			+ (((uint64) uuid->data[2]) << 24)
			+ (((uint64) uuid->data[1]) << 32)
			+ (((uint64) uuid->data[0]) << 40);

		/* convert ms to us, then adjust */
		ts = (TimestampTz) (tms * NS_PER_US) -
			(POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY * USECS_PER_SEC;

		PG_RETURN_TIMESTAMPTZ(ts);
	}

	/* not a timestamp-containing UUID version */
	PG_RETURN_NULL();
}

/*
 * Extract UUID version.
 *
 * Returns null if not RFC 9562 variant.
 */
Datum
uuid_extract_version(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *uuid = PG_GETARG_UUID_P(0);
	uint16		version;

	/* check if RFC 9562 variant */
	if ((uuid->data[8] & 0xc0) != 0x80)
		PG_RETURN_NULL();

	version = uuid->data[6] >> 4;

	PG_RETURN_UINT16(version);
}
