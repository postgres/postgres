/*-------------------------------------------------------------------------
 *
 * timestamp.c
 *	  Functions for the built-in SQL types "timestamp" and "interval".
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/timestamp.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>
#include <math.h>
#include <limits.h>
#include <sys/time.h>

#include "access/xact.h"
#include "catalog/pg_type.h"
#include "common/int.h"
#include "common/int128.h"
#include "funcapi.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "nodes/supportnodes.h"
#include "optimizer/optimizer.h"
#include "parser/scansup.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/float.h"
#include "utils/numeric.h"
#include "utils/skipsupport.h"
#include "utils/sortsupport.h"

/*
 * gcc's -ffast-math switch breaks routines that expect exact results from
 * expressions like timeval / SECS_PER_HOUR, where timeval is double.
 */
#ifdef __FAST_MATH__
#error -ffast-math is known to break this code
#endif

#define SAMESIGN(a,b)	(((a) < 0) == ((b) < 0))

/* Set at postmaster start */
TimestampTz PgStartTime;

/* Set at configuration reload */
TimestampTz PgReloadTime;

typedef struct
{
	Timestamp	current;
	Timestamp	finish;
	Interval	step;
	int			step_sign;
} generate_series_timestamp_fctx;

typedef struct
{
	TimestampTz current;
	TimestampTz finish;
	Interval	step;
	int			step_sign;
	pg_tz	   *attimezone;
} generate_series_timestamptz_fctx;

/*
 * The transition datatype for interval aggregates is declared as internal.
 * It's a pointer to an IntervalAggState allocated in the aggregate context.
 */
typedef struct IntervalAggState
{
	int64		N;				/* count of finite intervals processed */
	Interval	sumX;			/* sum of finite intervals processed */
	/* These counts are *not* included in N!  Use IA_TOTAL_COUNT() as needed */
	int64		pInfcount;		/* count of +infinity intervals */
	int64		nInfcount;		/* count of -infinity intervals */
} IntervalAggState;

#define IA_TOTAL_COUNT(ia) \
	((ia)->N + (ia)->pInfcount + (ia)->nInfcount)

static TimeOffset time2t(const int hour, const int min, const int sec, const fsec_t fsec);
static Timestamp dt2local(Timestamp dt, int timezone);
static bool AdjustIntervalForTypmod(Interval *interval, int32 typmod,
									Node *escontext);
static TimestampTz timestamp2timestamptz(Timestamp timestamp);
static Timestamp timestamptz2timestamp(TimestampTz timestamp);

static void EncodeSpecialInterval(const Interval *interval, char *str);
static void interval_um_internal(const Interval *interval, Interval *result);

/* common code for timestamptypmodin and timestamptztypmodin */
static int32
anytimestamp_typmodin(bool istz, ArrayType *ta)
{
	int32	   *tl;
	int			n;

	tl = ArrayGetIntegerTypmods(ta, &n);

	/*
	 * we're not too tense about good error message here because grammar
	 * shouldn't allow wrong number of modifiers for TIMESTAMP
	 */
	if (n != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid type modifier")));

	return anytimestamp_typmod_check(istz, tl[0]);
}

/* exported so parse_expr.c can use it */
int32
anytimestamp_typmod_check(bool istz, int32 typmod)
{
	if (typmod < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("TIMESTAMP(%d)%s precision must not be negative",
						typmod, (istz ? " WITH TIME ZONE" : ""))));
	if (typmod > MAX_TIMESTAMP_PRECISION)
	{
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("TIMESTAMP(%d)%s precision reduced to maximum allowed, %d",
						typmod, (istz ? " WITH TIME ZONE" : ""),
						MAX_TIMESTAMP_PRECISION)));
		typmod = MAX_TIMESTAMP_PRECISION;
	}

	return typmod;
}

/* common code for timestamptypmodout and timestamptztypmodout */
static char *
anytimestamp_typmodout(bool istz, int32 typmod)
{
	const char *tz = istz ? " with time zone" : " without time zone";

	if (typmod >= 0)
		return psprintf("(%d)%s", (int) typmod, tz);
	else
		return pstrdup(tz);
}


/*****************************************************************************
 *	 USER I/O ROUTINES														 *
 *****************************************************************************/

/* timestamp_in()
 * Convert a string to internal form.
 */
Datum
timestamp_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		typmod = PG_GETARG_INT32(2);
	Node	   *escontext = fcinfo->context;
	Timestamp	result;
	fsec_t		fsec;
	struct pg_tm tt,
			   *tm = &tt;
	int			tz;
	int			dtype;
	int			nf;
	int			dterr;
	char	   *field[MAXDATEFIELDS];
	int			ftype[MAXDATEFIELDS];
	char		workbuf[MAXDATELEN + MAXDATEFIELDS];
	DateTimeErrorExtra extra;

	dterr = ParseDateTime(str, workbuf, sizeof(workbuf),
						  field, ftype, MAXDATEFIELDS, &nf);
	if (dterr == 0)
		dterr = DecodeDateTime(field, ftype, nf,
							   &dtype, tm, &fsec, &tz, &extra);
	if (dterr != 0)
	{
		DateTimeParseError(dterr, &extra, str, "timestamp", escontext);
		PG_RETURN_NULL();
	}

	switch (dtype)
	{
		case DTK_DATE:
			if (tm2timestamp(tm, fsec, NULL, &result) != 0)
				ereturn(escontext, (Datum) 0,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range: \"%s\"", str)));
			break;

		case DTK_EPOCH:
			result = SetEpochTimestamp();
			break;

		case DTK_LATE:
			TIMESTAMP_NOEND(result);
			break;

		case DTK_EARLY:
			TIMESTAMP_NOBEGIN(result);
			break;

		default:
			elog(ERROR, "unexpected dtype %d while parsing timestamp \"%s\"",
				 dtype, str);
			TIMESTAMP_NOEND(result);
	}

	AdjustTimestampForTypmod(&result, typmod, escontext);

	PG_RETURN_TIMESTAMP(result);
}

/* timestamp_out()
 * Convert a timestamp to external form.
 */
Datum
timestamp_out(PG_FUNCTION_ARGS)
{
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(0);
	char	   *result;
	struct pg_tm tt,
			   *tm = &tt;
	fsec_t		fsec;
	char		buf[MAXDATELEN + 1];

	if (TIMESTAMP_NOT_FINITE(timestamp))
		EncodeSpecialTimestamp(timestamp, buf);
	else if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL, NULL) == 0)
		EncodeDateTime(tm, fsec, false, 0, NULL, DateStyle, buf);
	else
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	result = pstrdup(buf);
	PG_RETURN_CSTRING(result);
}

/*
 *		timestamp_recv			- converts external binary format to timestamp
 */
Datum
timestamp_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		typmod = PG_GETARG_INT32(2);
	Timestamp	timestamp;
	struct pg_tm tt,
			   *tm = &tt;
	fsec_t		fsec;

	timestamp = (Timestamp) pq_getmsgint64(buf);

	/* range check: see if timestamp_out would like it */
	if (TIMESTAMP_NOT_FINITE(timestamp))
		 /* ok */ ;
	else if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL, NULL) != 0 ||
			 !IS_VALID_TIMESTAMP(timestamp))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	AdjustTimestampForTypmod(&timestamp, typmod, NULL);

	PG_RETURN_TIMESTAMP(timestamp);
}

/*
 *		timestamp_send			- converts timestamp to binary format
 */
Datum
timestamp_send(PG_FUNCTION_ARGS)
{
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint64(&buf, timestamp);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

Datum
timestamptypmodin(PG_FUNCTION_ARGS)
{
	ArrayType  *ta = PG_GETARG_ARRAYTYPE_P(0);

	PG_RETURN_INT32(anytimestamp_typmodin(false, ta));
}

Datum
timestamptypmodout(PG_FUNCTION_ARGS)
{
	int32		typmod = PG_GETARG_INT32(0);

	PG_RETURN_CSTRING(anytimestamp_typmodout(false, typmod));
}


/*
 * timestamp_support()
 *
 * Planner support function for the timestamp_scale() and timestamptz_scale()
 * length coercion functions (we need not distinguish them here).
 */
Datum
timestamp_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);
	Node	   *ret = NULL;

	if (IsA(rawreq, SupportRequestSimplify))
	{
		SupportRequestSimplify *req = (SupportRequestSimplify *) rawreq;

		ret = TemporalSimplify(MAX_TIMESTAMP_PRECISION, (Node *) req->fcall);
	}

	PG_RETURN_POINTER(ret);
}

/* timestamp_scale()
 * Adjust time type for specified scale factor.
 * Used by PostgreSQL type system to stuff columns.
 */
Datum
timestamp_scale(PG_FUNCTION_ARGS)
{
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(0);
	int32		typmod = PG_GETARG_INT32(1);
	Timestamp	result;

	result = timestamp;

	AdjustTimestampForTypmod(&result, typmod, NULL);

	PG_RETURN_TIMESTAMP(result);
}

/*
 * AdjustTimestampForTypmod --- round off a timestamp to suit given typmod
 * Works for either timestamp or timestamptz.
 *
 * Returns true on success, false on failure (if escontext points to an
 * ErrorSaveContext; otherwise errors are thrown).
 */
bool
AdjustTimestampForTypmod(Timestamp *time, int32 typmod, Node *escontext)
{
	static const int64 TimestampScales[MAX_TIMESTAMP_PRECISION + 1] = {
		INT64CONST(1000000),
		INT64CONST(100000),
		INT64CONST(10000),
		INT64CONST(1000),
		INT64CONST(100),
		INT64CONST(10),
		INT64CONST(1)
	};

	static const int64 TimestampOffsets[MAX_TIMESTAMP_PRECISION + 1] = {
		INT64CONST(500000),
		INT64CONST(50000),
		INT64CONST(5000),
		INT64CONST(500),
		INT64CONST(50),
		INT64CONST(5),
		INT64CONST(0)
	};

	if (!TIMESTAMP_NOT_FINITE(*time)
		&& (typmod != -1) && (typmod != MAX_TIMESTAMP_PRECISION))
	{
		if (typmod < 0 || typmod > MAX_TIMESTAMP_PRECISION)
			ereturn(escontext, false,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("timestamp(%d) precision must be between %d and %d",
							typmod, 0, MAX_TIMESTAMP_PRECISION)));

		if (*time >= INT64CONST(0))
		{
			*time = ((*time + TimestampOffsets[typmod]) / TimestampScales[typmod]) *
				TimestampScales[typmod];
		}
		else
		{
			*time = -((((-*time) + TimestampOffsets[typmod]) / TimestampScales[typmod])
					  * TimestampScales[typmod]);
		}
	}

	return true;
}

/* timestamptz_in()
 * Convert a string to internal form.
 */
Datum
timestamptz_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		typmod = PG_GETARG_INT32(2);
	Node	   *escontext = fcinfo->context;
	TimestampTz result;
	fsec_t		fsec;
	struct pg_tm tt,
			   *tm = &tt;
	int			tz;
	int			dtype;
	int			nf;
	int			dterr;
	char	   *field[MAXDATEFIELDS];
	int			ftype[MAXDATEFIELDS];
	char		workbuf[MAXDATELEN + MAXDATEFIELDS];
	DateTimeErrorExtra extra;

	dterr = ParseDateTime(str, workbuf, sizeof(workbuf),
						  field, ftype, MAXDATEFIELDS, &nf);
	if (dterr == 0)
		dterr = DecodeDateTime(field, ftype, nf,
							   &dtype, tm, &fsec, &tz, &extra);
	if (dterr != 0)
	{
		DateTimeParseError(dterr, &extra, str, "timestamp with time zone",
						   escontext);
		PG_RETURN_NULL();
	}

	switch (dtype)
	{
		case DTK_DATE:
			if (tm2timestamp(tm, fsec, &tz, &result) != 0)
				ereturn(escontext, (Datum) 0,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range: \"%s\"", str)));
			break;

		case DTK_EPOCH:
			result = SetEpochTimestamp();
			break;

		case DTK_LATE:
			TIMESTAMP_NOEND(result);
			break;

		case DTK_EARLY:
			TIMESTAMP_NOBEGIN(result);
			break;

		default:
			elog(ERROR, "unexpected dtype %d while parsing timestamptz \"%s\"",
				 dtype, str);
			TIMESTAMP_NOEND(result);
	}

	AdjustTimestampForTypmod(&result, typmod, escontext);

	PG_RETURN_TIMESTAMPTZ(result);
}

/*
 * Try to parse a timezone specification, and return its timezone offset value
 * if it's acceptable.  Otherwise, an error is thrown.
 *
 * Note: some code paths update tm->tm_isdst, and some don't; current callers
 * don't care, so we don't bother being consistent.
 */
static int
parse_sane_timezone(struct pg_tm *tm, text *zone)
{
	char		tzname[TZ_STRLEN_MAX + 1];
	int			dterr;
	int			tz;

	text_to_cstring_buffer(zone, tzname, sizeof(tzname));

	/*
	 * Look up the requested timezone.  First we try to interpret it as a
	 * numeric timezone specification; if DecodeTimezone decides it doesn't
	 * like the format, we try timezone abbreviations and names.
	 *
	 * Note pg_tzset happily parses numeric input that DecodeTimezone would
	 * reject.  To avoid having it accept input that would otherwise be seen
	 * as invalid, it's enough to disallow having a digit in the first
	 * position of our input string.
	 */
	if (isdigit((unsigned char) *tzname))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid input syntax for type %s: \"%s\"",
						"numeric time zone", tzname),
				 errhint("Numeric time zones must have \"-\" or \"+\" as first character.")));

	dterr = DecodeTimezone(tzname, &tz);
	if (dterr != 0)
	{
		int			type,
					val;
		pg_tz	   *tzp;

		if (dterr == DTERR_TZDISP_OVERFLOW)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("numeric time zone \"%s\" out of range", tzname)));
		else if (dterr != DTERR_BAD_FORMAT)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("time zone \"%s\" not recognized", tzname)));

		type = DecodeTimezoneName(tzname, &val, &tzp);

		if (type == TZNAME_FIXED_OFFSET)
		{
			/* fixed-offset abbreviation */
			tz = -val;
		}
		else if (type == TZNAME_DYNTZ)
		{
			/* dynamic-offset abbreviation, resolve using specified time */
			tz = DetermineTimeZoneAbbrevOffset(tm, tzname, tzp);
		}
		else
		{
			/* full zone name */
			tz = DetermineTimeZoneOffset(tm, tzp);
		}
	}

	return tz;
}

/*
 * Look up the requested timezone, returning a pg_tz struct.
 *
 * This is the same as DecodeTimezoneNameToTz, but starting with a text Datum.
 */
static pg_tz *
lookup_timezone(text *zone)
{
	char		tzname[TZ_STRLEN_MAX + 1];

	text_to_cstring_buffer(zone, tzname, sizeof(tzname));

	return DecodeTimezoneNameToTz(tzname);
}

/*
 * make_timestamp_internal
 *		workhorse for make_timestamp and make_timestamptz
 */
static Timestamp
make_timestamp_internal(int year, int month, int day,
						int hour, int min, double sec)
{
	struct pg_tm tm;
	TimeOffset	date;
	TimeOffset	time;
	int			dterr;
	bool		bc = false;
	Timestamp	result;

	tm.tm_year = year;
	tm.tm_mon = month;
	tm.tm_mday = day;

	/* Handle negative years as BC */
	if (tm.tm_year < 0)
	{
		bc = true;
		tm.tm_year = -tm.tm_year;
	}

	dterr = ValidateDate(DTK_DATE_M, false, false, bc, &tm);

	if (dterr != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_FIELD_OVERFLOW),
				 errmsg("date field value out of range: %d-%02d-%02d",
						year, month, day)));

	if (!IS_VALID_JULIAN(tm.tm_year, tm.tm_mon, tm.tm_mday))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("date out of range: %d-%02d-%02d",
						year, month, day)));

	date = date2j(tm.tm_year, tm.tm_mon, tm.tm_mday) - POSTGRES_EPOCH_JDATE;

	/* Check for time overflow */
	if (float_time_overflows(hour, min, sec))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_FIELD_OVERFLOW),
				 errmsg("time field value out of range: %d:%02d:%02g",
						hour, min, sec)));

	/* This should match tm2time */
	time = (((hour * MINS_PER_HOUR + min) * SECS_PER_MINUTE)
			* USECS_PER_SEC) + (int64) rint(sec * USECS_PER_SEC);

	if (unlikely(pg_mul_s64_overflow(date, USECS_PER_DAY, &result) ||
				 pg_add_s64_overflow(result, time, &result)))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range: %d-%02d-%02d %d:%02d:%02g",
						year, month, day,
						hour, min, sec)));

	/* final range check catches just-out-of-range timestamps */
	if (!IS_VALID_TIMESTAMP(result))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range: %d-%02d-%02d %d:%02d:%02g",
						year, month, day,
						hour, min, sec)));

	return result;
}

/*
 * make_timestamp() - timestamp constructor
 */
Datum
make_timestamp(PG_FUNCTION_ARGS)
{
	int32		year = PG_GETARG_INT32(0);
	int32		month = PG_GETARG_INT32(1);
	int32		mday = PG_GETARG_INT32(2);
	int32		hour = PG_GETARG_INT32(3);
	int32		min = PG_GETARG_INT32(4);
	float8		sec = PG_GETARG_FLOAT8(5);
	Timestamp	result;

	result = make_timestamp_internal(year, month, mday,
									 hour, min, sec);

	PG_RETURN_TIMESTAMP(result);
}

/*
 * make_timestamptz() - timestamp with time zone constructor
 */
Datum
make_timestamptz(PG_FUNCTION_ARGS)
{
	int32		year = PG_GETARG_INT32(0);
	int32		month = PG_GETARG_INT32(1);
	int32		mday = PG_GETARG_INT32(2);
	int32		hour = PG_GETARG_INT32(3);
	int32		min = PG_GETARG_INT32(4);
	float8		sec = PG_GETARG_FLOAT8(5);
	Timestamp	result;

	result = make_timestamp_internal(year, month, mday,
									 hour, min, sec);

	PG_RETURN_TIMESTAMPTZ(timestamp2timestamptz(result));
}

/*
 * Construct a timestamp with time zone.
 *		As above, but the time zone is specified as seventh argument.
 */
Datum
make_timestamptz_at_timezone(PG_FUNCTION_ARGS)
{
	int32		year = PG_GETARG_INT32(0);
	int32		month = PG_GETARG_INT32(1);
	int32		mday = PG_GETARG_INT32(2);
	int32		hour = PG_GETARG_INT32(3);
	int32		min = PG_GETARG_INT32(4);
	float8		sec = PG_GETARG_FLOAT8(5);
	text	   *zone = PG_GETARG_TEXT_PP(6);
	TimestampTz result;
	Timestamp	timestamp;
	struct pg_tm tt;
	int			tz;
	fsec_t		fsec;

	timestamp = make_timestamp_internal(year, month, mday,
										hour, min, sec);

	if (timestamp2tm(timestamp, NULL, &tt, &fsec, NULL, NULL) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	tz = parse_sane_timezone(&tt, zone);

	result = dt2local(timestamp, -tz);

	if (!IS_VALID_TIMESTAMP(result))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	PG_RETURN_TIMESTAMPTZ(result);
}

/*
 * to_timestamp(double precision)
 * Convert UNIX epoch to timestamptz.
 */
Datum
float8_timestamptz(PG_FUNCTION_ARGS)
{
	float8		seconds = PG_GETARG_FLOAT8(0);
	TimestampTz result;

	/* Deal with NaN and infinite inputs ... */
	if (isnan(seconds))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp cannot be NaN")));

	if (isinf(seconds))
	{
		if (seconds < 0)
			TIMESTAMP_NOBEGIN(result);
		else
			TIMESTAMP_NOEND(result);
	}
	else
	{
		/* Out of range? */
		if (seconds <
			(float8) SECS_PER_DAY * (DATETIME_MIN_JULIAN - UNIX_EPOCH_JDATE)
			|| seconds >=
			(float8) SECS_PER_DAY * (TIMESTAMP_END_JULIAN - UNIX_EPOCH_JDATE))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range: \"%g\"", seconds)));

		/* Convert UNIX epoch to Postgres epoch */
		seconds -= ((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY);

		seconds = rint(seconds * USECS_PER_SEC);
		result = (int64) seconds;

		/* Recheck in case roundoff produces something just out of range */
		if (!IS_VALID_TIMESTAMP(result))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range: \"%g\"",
							PG_GETARG_FLOAT8(0))));
	}

	PG_RETURN_TIMESTAMP(result);
}

/* timestamptz_out()
 * Convert a timestamp to external form.
 */
Datum
timestamptz_out(PG_FUNCTION_ARGS)
{
	TimestampTz dt = PG_GETARG_TIMESTAMPTZ(0);
	char	   *result;
	int			tz;
	struct pg_tm tt,
			   *tm = &tt;
	fsec_t		fsec;
	const char *tzn;
	char		buf[MAXDATELEN + 1];

	if (TIMESTAMP_NOT_FINITE(dt))
		EncodeSpecialTimestamp(dt, buf);
	else if (timestamp2tm(dt, &tz, tm, &fsec, &tzn, NULL) == 0)
		EncodeDateTime(tm, fsec, true, tz, tzn, DateStyle, buf);
	else
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	result = pstrdup(buf);
	PG_RETURN_CSTRING(result);
}

/*
 *		timestamptz_recv			- converts external binary format to timestamptz
 */
Datum
timestamptz_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		typmod = PG_GETARG_INT32(2);
	TimestampTz timestamp;
	int			tz;
	struct pg_tm tt,
			   *tm = &tt;
	fsec_t		fsec;

	timestamp = (TimestampTz) pq_getmsgint64(buf);

	/* range check: see if timestamptz_out would like it */
	if (TIMESTAMP_NOT_FINITE(timestamp))
		 /* ok */ ;
	else if (timestamp2tm(timestamp, &tz, tm, &fsec, NULL, NULL) != 0 ||
			 !IS_VALID_TIMESTAMP(timestamp))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	AdjustTimestampForTypmod(&timestamp, typmod, NULL);

	PG_RETURN_TIMESTAMPTZ(timestamp);
}

/*
 *		timestamptz_send			- converts timestamptz to binary format
 */
Datum
timestamptz_send(PG_FUNCTION_ARGS)
{
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint64(&buf, timestamp);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

Datum
timestamptztypmodin(PG_FUNCTION_ARGS)
{
	ArrayType  *ta = PG_GETARG_ARRAYTYPE_P(0);

	PG_RETURN_INT32(anytimestamp_typmodin(true, ta));
}

Datum
timestamptztypmodout(PG_FUNCTION_ARGS)
{
	int32		typmod = PG_GETARG_INT32(0);

	PG_RETURN_CSTRING(anytimestamp_typmodout(true, typmod));
}


/* timestamptz_scale()
 * Adjust time type for specified scale factor.
 * Used by PostgreSQL type system to stuff columns.
 */
Datum
timestamptz_scale(PG_FUNCTION_ARGS)
{
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(0);
	int32		typmod = PG_GETARG_INT32(1);
	TimestampTz result;

	result = timestamp;

	AdjustTimestampForTypmod(&result, typmod, NULL);

	PG_RETURN_TIMESTAMPTZ(result);
}


/* interval_in()
 * Convert a string to internal form.
 *
 * External format(s):
 *	Uses the generic date/time parsing and decoding routines.
 */
Datum
interval_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		typmod = PG_GETARG_INT32(2);
	Node	   *escontext = fcinfo->context;
	Interval   *result;
	struct pg_itm_in tt,
			   *itm_in = &tt;
	int			dtype;
	int			nf;
	int			range;
	int			dterr;
	char	   *field[MAXDATEFIELDS];
	int			ftype[MAXDATEFIELDS];
	char		workbuf[256];
	DateTimeErrorExtra extra;

	itm_in->tm_year = 0;
	itm_in->tm_mon = 0;
	itm_in->tm_mday = 0;
	itm_in->tm_usec = 0;

	if (typmod >= 0)
		range = INTERVAL_RANGE(typmod);
	else
		range = INTERVAL_FULL_RANGE;

	dterr = ParseDateTime(str, workbuf, sizeof(workbuf), field,
						  ftype, MAXDATEFIELDS, &nf);
	if (dterr == 0)
		dterr = DecodeInterval(field, ftype, nf, range,
							   &dtype, itm_in);

	/* if those functions think it's a bad format, try ISO8601 style */
	if (dterr == DTERR_BAD_FORMAT)
		dterr = DecodeISO8601Interval(str,
									  &dtype, itm_in);

	if (dterr != 0)
	{
		if (dterr == DTERR_FIELD_OVERFLOW)
			dterr = DTERR_INTERVAL_OVERFLOW;
		DateTimeParseError(dterr, &extra, str, "interval", escontext);
		PG_RETURN_NULL();
	}

	result = (Interval *) palloc(sizeof(Interval));

	switch (dtype)
	{
		case DTK_DELTA:
			if (itmin2interval(itm_in, result) != 0)
				ereturn(escontext, (Datum) 0,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("interval out of range")));
			break;

		case DTK_LATE:
			INTERVAL_NOEND(result);
			break;

		case DTK_EARLY:
			INTERVAL_NOBEGIN(result);
			break;

		default:
			elog(ERROR, "unexpected dtype %d while parsing interval \"%s\"",
				 dtype, str);
	}

	AdjustIntervalForTypmod(result, typmod, escontext);

	PG_RETURN_INTERVAL_P(result);
}

/* interval_out()
 * Convert a time span to external form.
 */
Datum
interval_out(PG_FUNCTION_ARGS)
{
	Interval   *span = PG_GETARG_INTERVAL_P(0);
	char	   *result;
	struct pg_itm tt,
			   *itm = &tt;
	char		buf[MAXDATELEN + 1];

	if (INTERVAL_NOT_FINITE(span))
		EncodeSpecialInterval(span, buf);
	else
	{
		interval2itm(*span, itm);
		EncodeInterval(itm, IntervalStyle, buf);
	}

	result = pstrdup(buf);
	PG_RETURN_CSTRING(result);
}

/*
 *		interval_recv			- converts external binary format to interval
 */
Datum
interval_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		typmod = PG_GETARG_INT32(2);
	Interval   *interval;

	interval = (Interval *) palloc(sizeof(Interval));

	interval->time = pq_getmsgint64(buf);
	interval->day = pq_getmsgint(buf, sizeof(interval->day));
	interval->month = pq_getmsgint(buf, sizeof(interval->month));

	AdjustIntervalForTypmod(interval, typmod, NULL);

	PG_RETURN_INTERVAL_P(interval);
}

/*
 *		interval_send			- converts interval to binary format
 */
Datum
interval_send(PG_FUNCTION_ARGS)
{
	Interval   *interval = PG_GETARG_INTERVAL_P(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint64(&buf, interval->time);
	pq_sendint32(&buf, interval->day);
	pq_sendint32(&buf, interval->month);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * The interval typmod stores a "range" in its high 16 bits and a "precision"
 * in its low 16 bits.  Both contribute to defining the resolution of the
 * type.  Range addresses resolution granules larger than one second, and
 * precision specifies resolution below one second.  This representation can
 * express all SQL standard resolutions, but we implement them all in terms of
 * truncating rightward from some position.  Range is a bitmap of permitted
 * fields, but only the temporally-smallest such field is significant to our
 * calculations.  Precision is a count of sub-second decimal places to retain.
 * Setting all bits (INTERVAL_FULL_PRECISION) gives the same truncation
 * semantics as choosing MAX_INTERVAL_PRECISION.
 */
Datum
intervaltypmodin(PG_FUNCTION_ARGS)
{
	ArrayType  *ta = PG_GETARG_ARRAYTYPE_P(0);
	int32	   *tl;
	int			n;
	int32		typmod;

	tl = ArrayGetIntegerTypmods(ta, &n);

	/*
	 * tl[0] - interval range (fields bitmask)	tl[1] - precision (optional)
	 *
	 * Note we must validate tl[0] even though it's normally guaranteed
	 * correct by the grammar --- consider SELECT 'foo'::"interval"(1000).
	 */
	if (n > 0)
	{
		switch (tl[0])
		{
			case INTERVAL_MASK(YEAR):
			case INTERVAL_MASK(MONTH):
			case INTERVAL_MASK(DAY):
			case INTERVAL_MASK(HOUR):
			case INTERVAL_MASK(MINUTE):
			case INTERVAL_MASK(SECOND):
			case INTERVAL_MASK(YEAR) | INTERVAL_MASK(MONTH):
			case INTERVAL_MASK(DAY) | INTERVAL_MASK(HOUR):
			case INTERVAL_MASK(DAY) | INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE):
			case INTERVAL_MASK(DAY) | INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE) | INTERVAL_MASK(SECOND):
			case INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE):
			case INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE) | INTERVAL_MASK(SECOND):
			case INTERVAL_MASK(MINUTE) | INTERVAL_MASK(SECOND):
			case INTERVAL_FULL_RANGE:
				/* all OK */
				break;
			default:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid INTERVAL type modifier")));
		}
	}

	if (n == 1)
	{
		if (tl[0] != INTERVAL_FULL_RANGE)
			typmod = INTERVAL_TYPMOD(INTERVAL_FULL_PRECISION, tl[0]);
		else
			typmod = -1;
	}
	else if (n == 2)
	{
		if (tl[1] < 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("INTERVAL(%d) precision must not be negative",
							tl[1])));
		if (tl[1] > MAX_INTERVAL_PRECISION)
		{
			ereport(WARNING,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("INTERVAL(%d) precision reduced to maximum allowed, %d",
							tl[1], MAX_INTERVAL_PRECISION)));
			typmod = INTERVAL_TYPMOD(MAX_INTERVAL_PRECISION, tl[0]);
		}
		else
			typmod = INTERVAL_TYPMOD(tl[1], tl[0]);
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid INTERVAL type modifier")));
		typmod = 0;				/* keep compiler quiet */
	}

	PG_RETURN_INT32(typmod);
}

Datum
intervaltypmodout(PG_FUNCTION_ARGS)
{
	int32		typmod = PG_GETARG_INT32(0);
	char	   *res = (char *) palloc(64);
	int			fields;
	int			precision;
	const char *fieldstr;

	if (typmod < 0)
	{
		*res = '\0';
		PG_RETURN_CSTRING(res);
	}

	fields = INTERVAL_RANGE(typmod);
	precision = INTERVAL_PRECISION(typmod);

	switch (fields)
	{
		case INTERVAL_MASK(YEAR):
			fieldstr = " year";
			break;
		case INTERVAL_MASK(MONTH):
			fieldstr = " month";
			break;
		case INTERVAL_MASK(DAY):
			fieldstr = " day";
			break;
		case INTERVAL_MASK(HOUR):
			fieldstr = " hour";
			break;
		case INTERVAL_MASK(MINUTE):
			fieldstr = " minute";
			break;
		case INTERVAL_MASK(SECOND):
			fieldstr = " second";
			break;
		case INTERVAL_MASK(YEAR) | INTERVAL_MASK(MONTH):
			fieldstr = " year to month";
			break;
		case INTERVAL_MASK(DAY) | INTERVAL_MASK(HOUR):
			fieldstr = " day to hour";
			break;
		case INTERVAL_MASK(DAY) | INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE):
			fieldstr = " day to minute";
			break;
		case INTERVAL_MASK(DAY) | INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE) | INTERVAL_MASK(SECOND):
			fieldstr = " day to second";
			break;
		case INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE):
			fieldstr = " hour to minute";
			break;
		case INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE) | INTERVAL_MASK(SECOND):
			fieldstr = " hour to second";
			break;
		case INTERVAL_MASK(MINUTE) | INTERVAL_MASK(SECOND):
			fieldstr = " minute to second";
			break;
		case INTERVAL_FULL_RANGE:
			fieldstr = "";
			break;
		default:
			elog(ERROR, "invalid INTERVAL typmod: 0x%x", typmod);
			fieldstr = "";
			break;
	}

	if (precision != INTERVAL_FULL_PRECISION)
		snprintf(res, 64, "%s(%d)", fieldstr, precision);
	else
		snprintf(res, 64, "%s", fieldstr);

	PG_RETURN_CSTRING(res);
}

/*
 * Given an interval typmod value, return a code for the least-significant
 * field that the typmod allows to be nonzero, for instance given
 * INTERVAL DAY TO HOUR we want to identify "hour".
 *
 * The results should be ordered by field significance, which means
 * we can't use the dt.h macros YEAR etc, because for some odd reason
 * they aren't ordered that way.  Instead, arbitrarily represent
 * SECOND = 0, MINUTE = 1, HOUR = 2, DAY = 3, MONTH = 4, YEAR = 5.
 */
static int
intervaltypmodleastfield(int32 typmod)
{
	if (typmod < 0)
		return 0;				/* SECOND */

	switch (INTERVAL_RANGE(typmod))
	{
		case INTERVAL_MASK(YEAR):
			return 5;			/* YEAR */
		case INTERVAL_MASK(MONTH):
			return 4;			/* MONTH */
		case INTERVAL_MASK(DAY):
			return 3;			/* DAY */
		case INTERVAL_MASK(HOUR):
			return 2;			/* HOUR */
		case INTERVAL_MASK(MINUTE):
			return 1;			/* MINUTE */
		case INTERVAL_MASK(SECOND):
			return 0;			/* SECOND */
		case INTERVAL_MASK(YEAR) | INTERVAL_MASK(MONTH):
			return 4;			/* MONTH */
		case INTERVAL_MASK(DAY) | INTERVAL_MASK(HOUR):
			return 2;			/* HOUR */
		case INTERVAL_MASK(DAY) | INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE):
			return 1;			/* MINUTE */
		case INTERVAL_MASK(DAY) | INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE) | INTERVAL_MASK(SECOND):
			return 0;			/* SECOND */
		case INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE):
			return 1;			/* MINUTE */
		case INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE) | INTERVAL_MASK(SECOND):
			return 0;			/* SECOND */
		case INTERVAL_MASK(MINUTE) | INTERVAL_MASK(SECOND):
			return 0;			/* SECOND */
		case INTERVAL_FULL_RANGE:
			return 0;			/* SECOND */
		default:
			elog(ERROR, "invalid INTERVAL typmod: 0x%x", typmod);
			break;
	}
	return 0;					/* can't get here, but keep compiler quiet */
}


/*
 * interval_support()
 *
 * Planner support function for interval_scale().
 *
 * Flatten superfluous calls to interval_scale().  The interval typmod is
 * complex to permit accepting and regurgitating all SQL standard variations.
 * For truncation purposes, it boils down to a single, simple granularity.
 */
Datum
interval_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);
	Node	   *ret = NULL;

	if (IsA(rawreq, SupportRequestSimplify))
	{
		SupportRequestSimplify *req = (SupportRequestSimplify *) rawreq;
		FuncExpr   *expr = req->fcall;
		Node	   *typmod;

		Assert(list_length(expr->args) >= 2);

		typmod = (Node *) lsecond(expr->args);

		if (IsA(typmod, Const) && !((Const *) typmod)->constisnull)
		{
			Node	   *source = (Node *) linitial(expr->args);
			int32		new_typmod = DatumGetInt32(((Const *) typmod)->constvalue);
			bool		noop;

			if (new_typmod < 0)
				noop = true;
			else
			{
				int32		old_typmod = exprTypmod(source);
				int			old_least_field;
				int			new_least_field;
				int			old_precis;
				int			new_precis;

				old_least_field = intervaltypmodleastfield(old_typmod);
				new_least_field = intervaltypmodleastfield(new_typmod);
				if (old_typmod < 0)
					old_precis = INTERVAL_FULL_PRECISION;
				else
					old_precis = INTERVAL_PRECISION(old_typmod);
				new_precis = INTERVAL_PRECISION(new_typmod);

				/*
				 * Cast is a no-op if least field stays the same or decreases
				 * while precision stays the same or increases.  But
				 * precision, which is to say, sub-second precision, only
				 * affects ranges that include SECOND.
				 */
				noop = (new_least_field <= old_least_field) &&
					(old_least_field > 0 /* SECOND */ ||
					 new_precis >= MAX_INTERVAL_PRECISION ||
					 new_precis >= old_precis);
			}
			if (noop)
				ret = relabel_to_typmod(source, new_typmod);
		}
	}

	PG_RETURN_POINTER(ret);
}

/* interval_scale()
 * Adjust interval type for specified fields.
 * Used by PostgreSQL type system to stuff columns.
 */
Datum
interval_scale(PG_FUNCTION_ARGS)
{
	Interval   *interval = PG_GETARG_INTERVAL_P(0);
	int32		typmod = PG_GETARG_INT32(1);
	Interval   *result;

	result = palloc(sizeof(Interval));
	*result = *interval;

	AdjustIntervalForTypmod(result, typmod, NULL);

	PG_RETURN_INTERVAL_P(result);
}

/*
 *	Adjust interval for specified precision, in both YEAR to SECOND
 *	range and sub-second precision.
 *
 * Returns true on success, false on failure (if escontext points to an
 * ErrorSaveContext; otherwise errors are thrown).
 */
static bool
AdjustIntervalForTypmod(Interval *interval, int32 typmod,
						Node *escontext)
{
	static const int64 IntervalScales[MAX_INTERVAL_PRECISION + 1] = {
		INT64CONST(1000000),
		INT64CONST(100000),
		INT64CONST(10000),
		INT64CONST(1000),
		INT64CONST(100),
		INT64CONST(10),
		INT64CONST(1)
	};

	static const int64 IntervalOffsets[MAX_INTERVAL_PRECISION + 1] = {
		INT64CONST(500000),
		INT64CONST(50000),
		INT64CONST(5000),
		INT64CONST(500),
		INT64CONST(50),
		INT64CONST(5),
		INT64CONST(0)
	};

	/* Typmod has no effect on infinite intervals */
	if (INTERVAL_NOT_FINITE(interval))
		return true;

	/*
	 * Unspecified range and precision? Then not necessary to adjust. Setting
	 * typmod to -1 is the convention for all data types.
	 */
	if (typmod >= 0)
	{
		int			range = INTERVAL_RANGE(typmod);
		int			precision = INTERVAL_PRECISION(typmod);

		/*
		 * Our interpretation of intervals with a limited set of fields is
		 * that fields to the right of the last one specified are zeroed out,
		 * but those to the left of it remain valid.  Thus for example there
		 * is no operational difference between INTERVAL YEAR TO MONTH and
		 * INTERVAL MONTH.  In some cases we could meaningfully enforce that
		 * higher-order fields are zero; for example INTERVAL DAY could reject
		 * nonzero "month" field.  However that seems a bit pointless when we
		 * can't do it consistently.  (We cannot enforce a range limit on the
		 * highest expected field, since we do not have any equivalent of
		 * SQL's <interval leading field precision>.)  If we ever decide to
		 * revisit this, interval_support will likely require adjusting.
		 *
		 * Note: before PG 8.4 we interpreted a limited set of fields as
		 * actually causing a "modulo" operation on a given value, potentially
		 * losing high-order as well as low-order information.  But there is
		 * no support for such behavior in the standard, and it seems fairly
		 * undesirable on data consistency grounds anyway.  Now we only
		 * perform truncation or rounding of low-order fields.
		 */
		if (range == INTERVAL_FULL_RANGE)
		{
			/* Do nothing... */
		}
		else if (range == INTERVAL_MASK(YEAR))
		{
			interval->month = (interval->month / MONTHS_PER_YEAR) * MONTHS_PER_YEAR;
			interval->day = 0;
			interval->time = 0;
		}
		else if (range == INTERVAL_MASK(MONTH))
		{
			interval->day = 0;
			interval->time = 0;
		}
		/* YEAR TO MONTH */
		else if (range == (INTERVAL_MASK(YEAR) | INTERVAL_MASK(MONTH)))
		{
			interval->day = 0;
			interval->time = 0;
		}
		else if (range == INTERVAL_MASK(DAY))
		{
			interval->time = 0;
		}
		else if (range == INTERVAL_MASK(HOUR))
		{
			interval->time = (interval->time / USECS_PER_HOUR) *
				USECS_PER_HOUR;
		}
		else if (range == INTERVAL_MASK(MINUTE))
		{
			interval->time = (interval->time / USECS_PER_MINUTE) *
				USECS_PER_MINUTE;
		}
		else if (range == INTERVAL_MASK(SECOND))
		{
			/* fractional-second rounding will be dealt with below */
		}
		/* DAY TO HOUR */
		else if (range == (INTERVAL_MASK(DAY) |
						   INTERVAL_MASK(HOUR)))
		{
			interval->time = (interval->time / USECS_PER_HOUR) *
				USECS_PER_HOUR;
		}
		/* DAY TO MINUTE */
		else if (range == (INTERVAL_MASK(DAY) |
						   INTERVAL_MASK(HOUR) |
						   INTERVAL_MASK(MINUTE)))
		{
			interval->time = (interval->time / USECS_PER_MINUTE) *
				USECS_PER_MINUTE;
		}
		/* DAY TO SECOND */
		else if (range == (INTERVAL_MASK(DAY) |
						   INTERVAL_MASK(HOUR) |
						   INTERVAL_MASK(MINUTE) |
						   INTERVAL_MASK(SECOND)))
		{
			/* fractional-second rounding will be dealt with below */
		}
		/* HOUR TO MINUTE */
		else if (range == (INTERVAL_MASK(HOUR) |
						   INTERVAL_MASK(MINUTE)))
		{
			interval->time = (interval->time / USECS_PER_MINUTE) *
				USECS_PER_MINUTE;
		}
		/* HOUR TO SECOND */
		else if (range == (INTERVAL_MASK(HOUR) |
						   INTERVAL_MASK(MINUTE) |
						   INTERVAL_MASK(SECOND)))
		{
			/* fractional-second rounding will be dealt with below */
		}
		/* MINUTE TO SECOND */
		else if (range == (INTERVAL_MASK(MINUTE) |
						   INTERVAL_MASK(SECOND)))
		{
			/* fractional-second rounding will be dealt with below */
		}
		else
			elog(ERROR, "unrecognized interval typmod: %d", typmod);

		/* Need to adjust sub-second precision? */
		if (precision != INTERVAL_FULL_PRECISION)
		{
			if (precision < 0 || precision > MAX_INTERVAL_PRECISION)
				ereturn(escontext, false,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("interval(%d) precision must be between %d and %d",
								precision, 0, MAX_INTERVAL_PRECISION)));

			if (interval->time >= INT64CONST(0))
			{
				if (pg_add_s64_overflow(interval->time,
										IntervalOffsets[precision],
										&interval->time))
					ereturn(escontext, false,
							(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
							 errmsg("interval out of range")));
				interval->time -= interval->time % IntervalScales[precision];
			}
			else
			{
				if (pg_sub_s64_overflow(interval->time,
										IntervalOffsets[precision],
										&interval->time))
					ereturn(escontext, false,
							(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
							 errmsg("interval out of range")));
				interval->time -= interval->time % IntervalScales[precision];
			}
		}
	}

	return true;
}

/*
 * make_interval - numeric Interval constructor
 */
Datum
make_interval(PG_FUNCTION_ARGS)
{
	int32		years = PG_GETARG_INT32(0);
	int32		months = PG_GETARG_INT32(1);
	int32		weeks = PG_GETARG_INT32(2);
	int32		days = PG_GETARG_INT32(3);
	int32		hours = PG_GETARG_INT32(4);
	int32		mins = PG_GETARG_INT32(5);
	double		secs = PG_GETARG_FLOAT8(6);
	Interval   *result;

	/*
	 * Reject out-of-range inputs.  We reject any input values that cause
	 * integer overflow of the corresponding interval fields.
	 */
	if (isinf(secs) || isnan(secs))
		goto out_of_range;

	result = (Interval *) palloc(sizeof(Interval));

	/* years and months -> months */
	if (pg_mul_s32_overflow(years, MONTHS_PER_YEAR, &result->month) ||
		pg_add_s32_overflow(result->month, months, &result->month))
		goto out_of_range;

	/* weeks and days -> days */
	if (pg_mul_s32_overflow(weeks, DAYS_PER_WEEK, &result->day) ||
		pg_add_s32_overflow(result->day, days, &result->day))
		goto out_of_range;

	/* hours and mins -> usecs (cannot overflow 64-bit) */
	result->time = hours * USECS_PER_HOUR + mins * USECS_PER_MINUTE;

	/* secs -> usecs */
	secs = rint(float8_mul(secs, USECS_PER_SEC));
	if (!FLOAT8_FITS_IN_INT64(secs) ||
		pg_add_s64_overflow(result->time, (int64) secs, &result->time))
		goto out_of_range;

	/* make sure that the result is finite */
	if (INTERVAL_NOT_FINITE(result))
		goto out_of_range;

	PG_RETURN_INTERVAL_P(result);

out_of_range:
	ereport(ERROR,
			errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
			errmsg("interval out of range"));

	PG_RETURN_NULL();			/* keep compiler quiet */
}

/* EncodeSpecialTimestamp()
 * Convert reserved timestamp data type to string.
 */
void
EncodeSpecialTimestamp(Timestamp dt, char *str)
{
	if (TIMESTAMP_IS_NOBEGIN(dt))
		strcpy(str, EARLY);
	else if (TIMESTAMP_IS_NOEND(dt))
		strcpy(str, LATE);
	else						/* shouldn't happen */
		elog(ERROR, "invalid argument for EncodeSpecialTimestamp");
}

static void
EncodeSpecialInterval(const Interval *interval, char *str)
{
	if (INTERVAL_IS_NOBEGIN(interval))
		strcpy(str, EARLY);
	else if (INTERVAL_IS_NOEND(interval))
		strcpy(str, LATE);
	else						/* shouldn't happen */
		elog(ERROR, "invalid argument for EncodeSpecialInterval");
}

Datum
now(PG_FUNCTION_ARGS)
{
	PG_RETURN_TIMESTAMPTZ(GetCurrentTransactionStartTimestamp());
}

Datum
statement_timestamp(PG_FUNCTION_ARGS)
{
	PG_RETURN_TIMESTAMPTZ(GetCurrentStatementStartTimestamp());
}

Datum
clock_timestamp(PG_FUNCTION_ARGS)
{
	PG_RETURN_TIMESTAMPTZ(GetCurrentTimestamp());
}

Datum
pg_postmaster_start_time(PG_FUNCTION_ARGS)
{
	PG_RETURN_TIMESTAMPTZ(PgStartTime);
}

Datum
pg_conf_load_time(PG_FUNCTION_ARGS)
{
	PG_RETURN_TIMESTAMPTZ(PgReloadTime);
}

/*
 * GetCurrentTimestamp -- get the current operating system time
 *
 * Result is in the form of a TimestampTz value, and is expressed to the
 * full precision of the gettimeofday() syscall
 */
TimestampTz
GetCurrentTimestamp(void)
{
	TimestampTz result;
	struct timeval tp;

	gettimeofday(&tp, NULL);

	result = (TimestampTz) tp.tv_sec -
		((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY);
	result = (result * USECS_PER_SEC) + tp.tv_usec;

	return result;
}

/*
 * GetSQLCurrentTimestamp -- implements CURRENT_TIMESTAMP, CURRENT_TIMESTAMP(n)
 */
TimestampTz
GetSQLCurrentTimestamp(int32 typmod)
{
	TimestampTz ts;

	ts = GetCurrentTransactionStartTimestamp();
	if (typmod >= 0)
		AdjustTimestampForTypmod(&ts, typmod, NULL);
	return ts;
}

/*
 * GetSQLLocalTimestamp -- implements LOCALTIMESTAMP, LOCALTIMESTAMP(n)
 */
Timestamp
GetSQLLocalTimestamp(int32 typmod)
{
	Timestamp	ts;

	ts = timestamptz2timestamp(GetCurrentTransactionStartTimestamp());
	if (typmod >= 0)
		AdjustTimestampForTypmod(&ts, typmod, NULL);
	return ts;
}

/*
 * timeofday(*) -- returns the current time as a text.
 */
Datum
timeofday(PG_FUNCTION_ARGS)
{
	struct timeval tp;
	char		templ[128];
	char		buf[128];
	pg_time_t	tt;

	gettimeofday(&tp, NULL);
	tt = (pg_time_t) tp.tv_sec;
	pg_strftime(templ, sizeof(templ), "%a %b %d %H:%M:%S.%%06d %Y %Z",
				pg_localtime(&tt, session_timezone));
	snprintf(buf, sizeof(buf), templ, tp.tv_usec);

	PG_RETURN_TEXT_P(cstring_to_text(buf));
}

/*
 * TimestampDifference -- convert the difference between two timestamps
 *		into integer seconds and microseconds
 *
 * This is typically used to calculate a wait timeout for select(2),
 * which explains the otherwise-odd choice of output format.
 *
 * Both inputs must be ordinary finite timestamps (in current usage,
 * they'll be results from GetCurrentTimestamp()).
 *
 * We expect start_time <= stop_time.  If not, we return zeros,
 * since then we're already past the previously determined stop_time.
 */
void
TimestampDifference(TimestampTz start_time, TimestampTz stop_time,
					long *secs, int *microsecs)
{
	TimestampTz diff = stop_time - start_time;

	if (diff <= 0)
	{
		*secs = 0;
		*microsecs = 0;
	}
	else
	{
		*secs = (long) (diff / USECS_PER_SEC);
		*microsecs = (int) (diff % USECS_PER_SEC);
	}
}

/*
 * TimestampDifferenceMilliseconds -- convert the difference between two
 * 		timestamps into integer milliseconds
 *
 * This is typically used to calculate a wait timeout for WaitLatch()
 * or a related function.  The choice of "long" as the result type
 * is to harmonize with that; furthermore, we clamp the result to at most
 * INT_MAX milliseconds, because that's all that WaitLatch() allows.
 *
 * We expect start_time <= stop_time.  If not, we return zero,
 * since then we're already past the previously determined stop_time.
 *
 * Subtracting finite and infinite timestamps works correctly, returning
 * zero or INT_MAX as appropriate.
 *
 * Note we round up any fractional millisecond, since waiting for just
 * less than the intended timeout is undesirable.
 */
long
TimestampDifferenceMilliseconds(TimestampTz start_time, TimestampTz stop_time)
{
	TimestampTz diff;

	/* Deal with zero or negative elapsed time quickly. */
	if (start_time >= stop_time)
		return 0;
	/* To not fail with timestamp infinities, we must detect overflow. */
	if (pg_sub_s64_overflow(stop_time, start_time, &diff))
		return (long) INT_MAX;
	if (diff >= (INT_MAX * INT64CONST(1000) - 999))
		return (long) INT_MAX;
	else
		return (long) ((diff + 999) / 1000);
}

/*
 * TimestampDifferenceExceeds -- report whether the difference between two
 *		timestamps is >= a threshold (expressed in milliseconds)
 *
 * Both inputs must be ordinary finite timestamps (in current usage,
 * they'll be results from GetCurrentTimestamp()).
 */
bool
TimestampDifferenceExceeds(TimestampTz start_time,
						   TimestampTz stop_time,
						   int msec)
{
	TimestampTz diff = stop_time - start_time;

	return (diff >= msec * INT64CONST(1000));
}

/*
 * Check if the difference between two timestamps is >= a given
 * threshold (expressed in seconds).
 */
bool
TimestampDifferenceExceedsSeconds(TimestampTz start_time,
								  TimestampTz stop_time,
								  int threshold_sec)
{
	long		secs;
	int			usecs;

	/* Calculate the difference in seconds */
	TimestampDifference(start_time, stop_time, &secs, &usecs);

	return (secs >= threshold_sec);
}

/*
 * Convert a time_t to TimestampTz.
 *
 * We do not use time_t internally in Postgres, but this is provided for use
 * by functions that need to interpret, say, a stat(2) result.
 *
 * To avoid having the function's ABI vary depending on the width of time_t,
 * we declare the argument as pg_time_t, which is cast-compatible with
 * time_t but always 64 bits wide (unless the platform has no 64-bit type).
 * This detail should be invisible to callers, at least at source code level.
 */
TimestampTz
time_t_to_timestamptz(pg_time_t tm)
{
	TimestampTz result;

	result = (TimestampTz) tm -
		((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY);
	result *= USECS_PER_SEC;

	return result;
}

/*
 * Convert a TimestampTz to time_t.
 *
 * This too is just marginally useful, but some places need it.
 *
 * To avoid having the function's ABI vary depending on the width of time_t,
 * we declare the result as pg_time_t, which is cast-compatible with
 * time_t but always 64 bits wide (unless the platform has no 64-bit type).
 * This detail should be invisible to callers, at least at source code level.
 */
pg_time_t
timestamptz_to_time_t(TimestampTz t)
{
	pg_time_t	result;

	result = (pg_time_t) (t / USECS_PER_SEC +
						  ((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY));

	return result;
}

/*
 * Produce a C-string representation of a TimestampTz.
 *
 * This is mostly for use in emitting messages.  The primary difference
 * from timestamptz_out is that we force the output format to ISO.  Note
 * also that the result is in a static buffer, not pstrdup'd.
 *
 * See also pg_strftime.
 */
const char *
timestamptz_to_str(TimestampTz t)
{
	static char buf[MAXDATELEN + 1];
	int			tz;
	struct pg_tm tt,
			   *tm = &tt;
	fsec_t		fsec;
	const char *tzn;

	if (TIMESTAMP_NOT_FINITE(t))
		EncodeSpecialTimestamp(t, buf);
	else if (timestamp2tm(t, &tz, tm, &fsec, &tzn, NULL) == 0)
		EncodeDateTime(tm, fsec, true, tz, tzn, USE_ISO_DATES, buf);
	else
		strlcpy(buf, "(timestamp out of range)", sizeof(buf));

	return buf;
}


void
dt2time(Timestamp jd, int *hour, int *min, int *sec, fsec_t *fsec)
{
	TimeOffset	time;

	time = jd;

	*hour = time / USECS_PER_HOUR;
	time -= (*hour) * USECS_PER_HOUR;
	*min = time / USECS_PER_MINUTE;
	time -= (*min) * USECS_PER_MINUTE;
	*sec = time / USECS_PER_SEC;
	*fsec = time - (*sec * USECS_PER_SEC);
}								/* dt2time() */


/*
 * timestamp2tm() - Convert timestamp data type to POSIX time structure.
 *
 * Note that year is _not_ 1900-based, but is an explicit full value.
 * Also, month is one-based, _not_ zero-based.
 * Returns:
 *	 0 on success
 *	-1 on out of range
 *
 * If attimezone is NULL, the global timezone setting will be used.
 */
int
timestamp2tm(Timestamp dt, int *tzp, struct pg_tm *tm, fsec_t *fsec, const char **tzn, pg_tz *attimezone)
{
	Timestamp	date;
	Timestamp	time;
	pg_time_t	utime;

	/* Use session timezone if caller asks for default */
	if (attimezone == NULL)
		attimezone = session_timezone;

	time = dt;
	TMODULO(time, date, USECS_PER_DAY);

	if (time < INT64CONST(0))
	{
		time += USECS_PER_DAY;
		date -= 1;
	}

	/* add offset to go from J2000 back to standard Julian date */
	date += POSTGRES_EPOCH_JDATE;

	/* Julian day routine does not work for negative Julian days */
	if (date < 0 || date > (Timestamp) INT_MAX)
		return -1;

	j2date((int) date, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
	dt2time(time, &tm->tm_hour, &tm->tm_min, &tm->tm_sec, fsec);

	/* Done if no TZ conversion wanted */
	if (tzp == NULL)
	{
		tm->tm_isdst = -1;
		tm->tm_gmtoff = 0;
		tm->tm_zone = NULL;
		if (tzn != NULL)
			*tzn = NULL;
		return 0;
	}

	/*
	 * If the time falls within the range of pg_time_t, use pg_localtime() to
	 * rotate to the local time zone.
	 *
	 * First, convert to an integral timestamp, avoiding possibly
	 * platform-specific roundoff-in-wrong-direction errors, and adjust to
	 * Unix epoch.  Then see if we can convert to pg_time_t without loss. This
	 * coding avoids hardwiring any assumptions about the width of pg_time_t,
	 * so it should behave sanely on machines without int64.
	 */
	dt = (dt - *fsec) / USECS_PER_SEC +
		(POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY;
	utime = (pg_time_t) dt;
	if ((Timestamp) utime == dt)
	{
		struct pg_tm *tx = pg_localtime(&utime, attimezone);

		tm->tm_year = tx->tm_year + 1900;
		tm->tm_mon = tx->tm_mon + 1;
		tm->tm_mday = tx->tm_mday;
		tm->tm_hour = tx->tm_hour;
		tm->tm_min = tx->tm_min;
		tm->tm_sec = tx->tm_sec;
		tm->tm_isdst = tx->tm_isdst;
		tm->tm_gmtoff = tx->tm_gmtoff;
		tm->tm_zone = tx->tm_zone;
		*tzp = -tm->tm_gmtoff;
		if (tzn != NULL)
			*tzn = tm->tm_zone;
	}
	else
	{
		/*
		 * When out of range of pg_time_t, treat as GMT
		 */
		*tzp = 0;
		/* Mark this as *no* time zone available */
		tm->tm_isdst = -1;
		tm->tm_gmtoff = 0;
		tm->tm_zone = NULL;
		if (tzn != NULL)
			*tzn = NULL;
	}

	return 0;
}


/* tm2timestamp()
 * Convert a tm structure to a timestamp data type.
 * Note that year is _not_ 1900-based, but is an explicit full value.
 * Also, month is one-based, _not_ zero-based.
 *
 * Returns -1 on failure (value out of range).
 */
int
tm2timestamp(struct pg_tm *tm, fsec_t fsec, int *tzp, Timestamp *result)
{
	TimeOffset	date;
	TimeOffset	time;

	/* Prevent overflow in Julian-day routines */
	if (!IS_VALID_JULIAN(tm->tm_year, tm->tm_mon, tm->tm_mday))
	{
		*result = 0;			/* keep compiler quiet */
		return -1;
	}

	date = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - POSTGRES_EPOCH_JDATE;
	time = time2t(tm->tm_hour, tm->tm_min, tm->tm_sec, fsec);

	if (unlikely(pg_mul_s64_overflow(date, USECS_PER_DAY, result) ||
				 pg_add_s64_overflow(*result, time, result)))
	{
		*result = 0;			/* keep compiler quiet */
		return -1;
	}
	if (tzp != NULL)
		*result = dt2local(*result, -(*tzp));

	/* final range check catches just-out-of-range timestamps */
	if (!IS_VALID_TIMESTAMP(*result))
	{
		*result = 0;			/* keep compiler quiet */
		return -1;
	}

	return 0;
}


/* interval2itm()
 * Convert an Interval to a pg_itm structure.
 * Note: overflow is not possible, because the pg_itm fields are
 * wide enough for all possible conversion results.
 */
void
interval2itm(Interval span, struct pg_itm *itm)
{
	TimeOffset	time;
	TimeOffset	tfrac;

	itm->tm_year = span.month / MONTHS_PER_YEAR;
	itm->tm_mon = span.month % MONTHS_PER_YEAR;
	itm->tm_mday = span.day;
	time = span.time;

	tfrac = time / USECS_PER_HOUR;
	time -= tfrac * USECS_PER_HOUR;
	itm->tm_hour = tfrac;
	tfrac = time / USECS_PER_MINUTE;
	time -= tfrac * USECS_PER_MINUTE;
	itm->tm_min = (int) tfrac;
	tfrac = time / USECS_PER_SEC;
	time -= tfrac * USECS_PER_SEC;
	itm->tm_sec = (int) tfrac;
	itm->tm_usec = (int) time;
}

/* itm2interval()
 * Convert a pg_itm structure to an Interval.
 * Returns 0 if OK, -1 on overflow.
 *
 * This is for use in computations expected to produce finite results.  Any
 * inputs that lead to infinite results are treated as overflows.
 */
int
itm2interval(struct pg_itm *itm, Interval *span)
{
	int64		total_months = (int64) itm->tm_year * MONTHS_PER_YEAR + itm->tm_mon;

	if (total_months > INT_MAX || total_months < INT_MIN)
		return -1;
	span->month = (int32) total_months;
	span->day = itm->tm_mday;
	if (pg_mul_s64_overflow(itm->tm_hour, USECS_PER_HOUR,
							&span->time))
		return -1;
	/* tm_min, tm_sec are 32 bits, so intermediate products can't overflow */
	if (pg_add_s64_overflow(span->time, itm->tm_min * USECS_PER_MINUTE,
							&span->time))
		return -1;
	if (pg_add_s64_overflow(span->time, itm->tm_sec * USECS_PER_SEC,
							&span->time))
		return -1;
	if (pg_add_s64_overflow(span->time, itm->tm_usec,
							&span->time))
		return -1;
	if (INTERVAL_NOT_FINITE(span))
		return -1;
	return 0;
}

/* itmin2interval()
 * Convert a pg_itm_in structure to an Interval.
 * Returns 0 if OK, -1 on overflow.
 *
 * Note: if the result is infinite, it is not treated as an overflow.  This
 * avoids any dump/reload hazards from pre-17 databases that do not support
 * infinite intervals, but do allow finite intervals with all fields set to
 * INT_MIN/INT_MAX (outside the documented range).  Such intervals will be
 * silently converted to +/-infinity.  This may not be ideal, but seems
 * preferable to failure, and ought to be pretty unlikely in practice.
 */
int
itmin2interval(struct pg_itm_in *itm_in, Interval *span)
{
	int64		total_months = (int64) itm_in->tm_year * MONTHS_PER_YEAR + itm_in->tm_mon;

	if (total_months > INT_MAX || total_months < INT_MIN)
		return -1;
	span->month = (int32) total_months;
	span->day = itm_in->tm_mday;
	span->time = itm_in->tm_usec;
	return 0;
}

static TimeOffset
time2t(const int hour, const int min, const int sec, const fsec_t fsec)
{
	return (((((hour * MINS_PER_HOUR) + min) * SECS_PER_MINUTE) + sec) * USECS_PER_SEC) + fsec;
}

static Timestamp
dt2local(Timestamp dt, int timezone)
{
	dt -= (timezone * USECS_PER_SEC);
	return dt;
}


/*****************************************************************************
 *	 PUBLIC ROUTINES														 *
 *****************************************************************************/


Datum
timestamp_finite(PG_FUNCTION_ARGS)
{
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(0);

	PG_RETURN_BOOL(!TIMESTAMP_NOT_FINITE(timestamp));
}

Datum
interval_finite(PG_FUNCTION_ARGS)
{
	Interval   *interval = PG_GETARG_INTERVAL_P(0);

	PG_RETURN_BOOL(!INTERVAL_NOT_FINITE(interval));
}


/*----------------------------------------------------------
 *	Relational operators for timestamp.
 *---------------------------------------------------------*/

void
GetEpochTime(struct pg_tm *tm)
{
	struct pg_tm *t0;
	pg_time_t	epoch = 0;

	t0 = pg_gmtime(&epoch);

	if (t0 == NULL)
		elog(ERROR, "could not convert epoch to timestamp: %m");

	tm->tm_year = t0->tm_year;
	tm->tm_mon = t0->tm_mon;
	tm->tm_mday = t0->tm_mday;
	tm->tm_hour = t0->tm_hour;
	tm->tm_min = t0->tm_min;
	tm->tm_sec = t0->tm_sec;

	tm->tm_year += 1900;
	tm->tm_mon++;
}

Timestamp
SetEpochTimestamp(void)
{
	Timestamp	dt;
	struct pg_tm tt,
			   *tm = &tt;

	GetEpochTime(tm);
	/* we don't bother to test for failure ... */
	tm2timestamp(tm, 0, NULL, &dt);

	return dt;
}								/* SetEpochTimestamp() */

/*
 * We are currently sharing some code between timestamp and timestamptz.
 * The comparison functions are among them. - thomas 2001-09-25
 *
 *		timestamp_relop - is timestamp1 relop timestamp2
 */
int
timestamp_cmp_internal(Timestamp dt1, Timestamp dt2)
{
	return (dt1 < dt2) ? -1 : ((dt1 > dt2) ? 1 : 0);
}

Datum
timestamp_eq(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(timestamp_cmp_internal(dt1, dt2) == 0);
}

Datum
timestamp_ne(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(timestamp_cmp_internal(dt1, dt2) != 0);
}

Datum
timestamp_lt(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(timestamp_cmp_internal(dt1, dt2) < 0);
}

Datum
timestamp_gt(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(timestamp_cmp_internal(dt1, dt2) > 0);
}

Datum
timestamp_le(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(timestamp_cmp_internal(dt1, dt2) <= 0);
}

Datum
timestamp_ge(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(timestamp_cmp_internal(dt1, dt2) >= 0);
}

Datum
timestamp_cmp(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_INT32(timestamp_cmp_internal(dt1, dt2));
}

#if SIZEOF_DATUM < 8
/* note: this is used for timestamptz also */
static int
timestamp_fastcmp(Datum x, Datum y, SortSupport ssup)
{
	Timestamp	a = DatumGetTimestamp(x);
	Timestamp	b = DatumGetTimestamp(y);

	return timestamp_cmp_internal(a, b);
}
#endif

Datum
timestamp_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

#if SIZEOF_DATUM >= 8

	/*
	 * If this build has pass-by-value timestamps, then we can use a standard
	 * comparator function.
	 */
	ssup->comparator = ssup_datum_signed_cmp;
#else
	ssup->comparator = timestamp_fastcmp;
#endif
	PG_RETURN_VOID();
}

/* note: this is used for timestamptz also */
static Datum
timestamp_decrement(Relation rel, Datum existing, bool *underflow)
{
	Timestamp	texisting = DatumGetTimestamp(existing);

	if (texisting == PG_INT64_MIN)
	{
		/* return value is undefined */
		*underflow = true;
		return (Datum) 0;
	}

	*underflow = false;
	return TimestampGetDatum(texisting - 1);
}

/* note: this is used for timestamptz also */
static Datum
timestamp_increment(Relation rel, Datum existing, bool *overflow)
{
	Timestamp	texisting = DatumGetTimestamp(existing);

	if (texisting == PG_INT64_MAX)
	{
		/* return value is undefined */
		*overflow = true;
		return (Datum) 0;
	}

	*overflow = false;
	return TimestampGetDatum(texisting + 1);
}

Datum
timestamp_skipsupport(PG_FUNCTION_ARGS)
{
	SkipSupport sksup = (SkipSupport) PG_GETARG_POINTER(0);

	sksup->decrement = timestamp_decrement;
	sksup->increment = timestamp_increment;
	sksup->low_elem = TimestampGetDatum(PG_INT64_MIN);
	sksup->high_elem = TimestampGetDatum(PG_INT64_MAX);

	PG_RETURN_VOID();
}

Datum
timestamp_hash(PG_FUNCTION_ARGS)
{
	return hashint8(fcinfo);
}

Datum
timestamp_hash_extended(PG_FUNCTION_ARGS)
{
	return hashint8extended(fcinfo);
}

Datum
timestamptz_hash(PG_FUNCTION_ARGS)
{
	return hashint8(fcinfo);
}

Datum
timestamptz_hash_extended(PG_FUNCTION_ARGS)
{
	return hashint8extended(fcinfo);
}

/*
 * Cross-type comparison functions for timestamp vs timestamptz
 */

int32
timestamp_cmp_timestamptz_internal(Timestamp timestampVal, TimestampTz dt2)
{
	TimestampTz dt1;
	int			overflow;

	dt1 = timestamp2timestamptz_opt_overflow(timestampVal, &overflow);
	if (overflow > 0)
	{
		/* dt1 is larger than any finite timestamp, but less than infinity */
		return TIMESTAMP_IS_NOEND(dt2) ? -1 : +1;
	}
	if (overflow < 0)
	{
		/* dt1 is less than any finite timestamp, but more than -infinity */
		return TIMESTAMP_IS_NOBEGIN(dt2) ? +1 : -1;
	}

	return timestamptz_cmp_internal(dt1, dt2);
}

Datum
timestamp_eq_timestamptz(PG_FUNCTION_ARGS)
{
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(0);
	TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

	PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt2) == 0);
}

Datum
timestamp_ne_timestamptz(PG_FUNCTION_ARGS)
{
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(0);
	TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

	PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt2) != 0);
}

Datum
timestamp_lt_timestamptz(PG_FUNCTION_ARGS)
{
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(0);
	TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

	PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt2) < 0);
}

Datum
timestamp_gt_timestamptz(PG_FUNCTION_ARGS)
{
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(0);
	TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

	PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt2) > 0);
}

Datum
timestamp_le_timestamptz(PG_FUNCTION_ARGS)
{
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(0);
	TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

	PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt2) <= 0);
}

Datum
timestamp_ge_timestamptz(PG_FUNCTION_ARGS)
{
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(0);
	TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

	PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt2) >= 0);
}

Datum
timestamp_cmp_timestamptz(PG_FUNCTION_ARGS)
{
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(0);
	TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

	PG_RETURN_INT32(timestamp_cmp_timestamptz_internal(timestampVal, dt2));
}

Datum
timestamptz_eq_timestamp(PG_FUNCTION_ARGS)
{
	TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt1) == 0);
}

Datum
timestamptz_ne_timestamp(PG_FUNCTION_ARGS)
{
	TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt1) != 0);
}

Datum
timestamptz_lt_timestamp(PG_FUNCTION_ARGS)
{
	TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt1) > 0);
}

Datum
timestamptz_gt_timestamp(PG_FUNCTION_ARGS)
{
	TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt1) < 0);
}

Datum
timestamptz_le_timestamp(PG_FUNCTION_ARGS)
{
	TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt1) >= 0);
}

Datum
timestamptz_ge_timestamp(PG_FUNCTION_ARGS)
{
	TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt1) <= 0);
}

Datum
timestamptz_cmp_timestamp(PG_FUNCTION_ARGS)
{
	TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(1);

	PG_RETURN_INT32(-timestamp_cmp_timestamptz_internal(timestampVal, dt1));
}


/*
 *		interval_relop	- is interval1 relop interval2
 *
 * Interval comparison is based on converting interval values to a linear
 * representation expressed in the units of the time field (microseconds,
 * in the case of integer timestamps) with days assumed to be always 24 hours
 * and months assumed to be always 30 days.  To avoid overflow, we need a
 * wider-than-int64 datatype for the linear representation, so use INT128.
 */

static inline INT128
interval_cmp_value(const Interval *interval)
{
	INT128		span;
	int64		days;

	/*
	 * Combine the month and day fields into an integral number of days.
	 * Because the inputs are int32, int64 arithmetic suffices here.
	 */
	days = interval->month * INT64CONST(30);
	days += interval->day;

	/* Widen time field to 128 bits */
	span = int64_to_int128(interval->time);

	/* Scale up days to microseconds, forming a 128-bit product */
	int128_add_int64_mul_int64(&span, days, USECS_PER_DAY);

	return span;
}

static int
interval_cmp_internal(const Interval *interval1, const Interval *interval2)
{
	INT128		span1 = interval_cmp_value(interval1);
	INT128		span2 = interval_cmp_value(interval2);

	return int128_compare(span1, span2);
}

static int
interval_sign(const Interval *interval)
{
	INT128		span = interval_cmp_value(interval);
	INT128		zero = int64_to_int128(0);

	return int128_compare(span, zero);
}

Datum
interval_eq(PG_FUNCTION_ARGS)
{
	Interval   *interval1 = PG_GETARG_INTERVAL_P(0);
	Interval   *interval2 = PG_GETARG_INTERVAL_P(1);

	PG_RETURN_BOOL(interval_cmp_internal(interval1, interval2) == 0);
}

Datum
interval_ne(PG_FUNCTION_ARGS)
{
	Interval   *interval1 = PG_GETARG_INTERVAL_P(0);
	Interval   *interval2 = PG_GETARG_INTERVAL_P(1);

	PG_RETURN_BOOL(interval_cmp_internal(interval1, interval2) != 0);
}

Datum
interval_lt(PG_FUNCTION_ARGS)
{
	Interval   *interval1 = PG_GETARG_INTERVAL_P(0);
	Interval   *interval2 = PG_GETARG_INTERVAL_P(1);

	PG_RETURN_BOOL(interval_cmp_internal(interval1, interval2) < 0);
}

Datum
interval_gt(PG_FUNCTION_ARGS)
{
	Interval   *interval1 = PG_GETARG_INTERVAL_P(0);
	Interval   *interval2 = PG_GETARG_INTERVAL_P(1);

	PG_RETURN_BOOL(interval_cmp_internal(interval1, interval2) > 0);
}

Datum
interval_le(PG_FUNCTION_ARGS)
{
	Interval   *interval1 = PG_GETARG_INTERVAL_P(0);
	Interval   *interval2 = PG_GETARG_INTERVAL_P(1);

	PG_RETURN_BOOL(interval_cmp_internal(interval1, interval2) <= 0);
}

Datum
interval_ge(PG_FUNCTION_ARGS)
{
	Interval   *interval1 = PG_GETARG_INTERVAL_P(0);
	Interval   *interval2 = PG_GETARG_INTERVAL_P(1);

	PG_RETURN_BOOL(interval_cmp_internal(interval1, interval2) >= 0);
}

Datum
interval_cmp(PG_FUNCTION_ARGS)
{
	Interval   *interval1 = PG_GETARG_INTERVAL_P(0);
	Interval   *interval2 = PG_GETARG_INTERVAL_P(1);

	PG_RETURN_INT32(interval_cmp_internal(interval1, interval2));
}

/*
 * Hashing for intervals
 *
 * We must produce equal hashvals for values that interval_cmp_internal()
 * considers equal.  So, compute the net span the same way it does,
 * and then hash that.
 */
Datum
interval_hash(PG_FUNCTION_ARGS)
{
	Interval   *interval = PG_GETARG_INTERVAL_P(0);
	INT128		span = interval_cmp_value(interval);
	int64		span64;

	/*
	 * Use only the least significant 64 bits for hashing.  The upper 64 bits
	 * seldom add any useful information, and besides we must do it like this
	 * for compatibility with hashes calculated before use of INT128 was
	 * introduced.
	 */
	span64 = int128_to_int64(span);

	return DirectFunctionCall1(hashint8, Int64GetDatumFast(span64));
}

Datum
interval_hash_extended(PG_FUNCTION_ARGS)
{
	Interval   *interval = PG_GETARG_INTERVAL_P(0);
	INT128		span = interval_cmp_value(interval);
	int64		span64;

	/* Same approach as interval_hash */
	span64 = int128_to_int64(span);

	return DirectFunctionCall2(hashint8extended, Int64GetDatumFast(span64),
							   PG_GETARG_DATUM(1));
}

/* overlaps_timestamp() --- implements the SQL OVERLAPS operator.
 *
 * Algorithm is per SQL spec.  This is much harder than you'd think
 * because the spec requires us to deliver a non-null answer in some cases
 * where some of the inputs are null.
 */
Datum
overlaps_timestamp(PG_FUNCTION_ARGS)
{
	/*
	 * The arguments are Timestamps, but we leave them as generic Datums to
	 * avoid unnecessary conversions between value and reference forms --- not
	 * to mention possible dereferences of null pointers.
	 */
	Datum		ts1 = PG_GETARG_DATUM(0);
	Datum		te1 = PG_GETARG_DATUM(1);
	Datum		ts2 = PG_GETARG_DATUM(2);
	Datum		te2 = PG_GETARG_DATUM(3);
	bool		ts1IsNull = PG_ARGISNULL(0);
	bool		te1IsNull = PG_ARGISNULL(1);
	bool		ts2IsNull = PG_ARGISNULL(2);
	bool		te2IsNull = PG_ARGISNULL(3);

#define TIMESTAMP_GT(t1,t2) \
	DatumGetBool(DirectFunctionCall2(timestamp_gt,t1,t2))
#define TIMESTAMP_LT(t1,t2) \
	DatumGetBool(DirectFunctionCall2(timestamp_lt,t1,t2))

	/*
	 * If both endpoints of interval 1 are null, the result is null (unknown).
	 * If just one endpoint is null, take ts1 as the non-null one. Otherwise,
	 * take ts1 as the lesser endpoint.
	 */
	if (ts1IsNull)
	{
		if (te1IsNull)
			PG_RETURN_NULL();
		/* swap null for non-null */
		ts1 = te1;
		te1IsNull = true;
	}
	else if (!te1IsNull)
	{
		if (TIMESTAMP_GT(ts1, te1))
		{
			Datum		tt = ts1;

			ts1 = te1;
			te1 = tt;
		}
	}

	/* Likewise for interval 2. */
	if (ts2IsNull)
	{
		if (te2IsNull)
			PG_RETURN_NULL();
		/* swap null for non-null */
		ts2 = te2;
		te2IsNull = true;
	}
	else if (!te2IsNull)
	{
		if (TIMESTAMP_GT(ts2, te2))
		{
			Datum		tt = ts2;

			ts2 = te2;
			te2 = tt;
		}
	}

	/*
	 * At this point neither ts1 nor ts2 is null, so we can consider three
	 * cases: ts1 > ts2, ts1 < ts2, ts1 = ts2
	 */
	if (TIMESTAMP_GT(ts1, ts2))
	{
		/*
		 * This case is ts1 < te2 OR te1 < te2, which may look redundant but
		 * in the presence of nulls it's not quite completely so.
		 */
		if (te2IsNull)
			PG_RETURN_NULL();
		if (TIMESTAMP_LT(ts1, te2))
			PG_RETURN_BOOL(true);
		if (te1IsNull)
			PG_RETURN_NULL();

		/*
		 * If te1 is not null then we had ts1 <= te1 above, and we just found
		 * ts1 >= te2, hence te1 >= te2.
		 */
		PG_RETURN_BOOL(false);
	}
	else if (TIMESTAMP_LT(ts1, ts2))
	{
		/* This case is ts2 < te1 OR te2 < te1 */
		if (te1IsNull)
			PG_RETURN_NULL();
		if (TIMESTAMP_LT(ts2, te1))
			PG_RETURN_BOOL(true);
		if (te2IsNull)
			PG_RETURN_NULL();

		/*
		 * If te2 is not null then we had ts2 <= te2 above, and we just found
		 * ts2 >= te1, hence te2 >= te1.
		 */
		PG_RETURN_BOOL(false);
	}
	else
	{
		/*
		 * For ts1 = ts2 the spec says te1 <> te2 OR te1 = te2, which is a
		 * rather silly way of saying "true if both are non-null, else null".
		 */
		if (te1IsNull || te2IsNull)
			PG_RETURN_NULL();
		PG_RETURN_BOOL(true);
	}

#undef TIMESTAMP_GT
#undef TIMESTAMP_LT
}


/*----------------------------------------------------------
 *	"Arithmetic" operators on date/times.
 *---------------------------------------------------------*/

Datum
timestamp_smaller(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);
	Timestamp	result;

	/* use timestamp_cmp_internal to be sure this agrees with comparisons */
	if (timestamp_cmp_internal(dt1, dt2) < 0)
		result = dt1;
	else
		result = dt2;
	PG_RETURN_TIMESTAMP(result);
}

Datum
timestamp_larger(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);
	Timestamp	result;

	if (timestamp_cmp_internal(dt1, dt2) > 0)
		result = dt1;
	else
		result = dt2;
	PG_RETURN_TIMESTAMP(result);
}


Datum
timestamp_mi(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);
	Interval   *result;

	result = (Interval *) palloc(sizeof(Interval));

	/*
	 * Handle infinities.
	 *
	 * We treat anything that amounts to "infinity - infinity" as an error,
	 * since the interval type has nothing equivalent to NaN.
	 */
	if (TIMESTAMP_NOT_FINITE(dt1) || TIMESTAMP_NOT_FINITE(dt2))
	{
		if (TIMESTAMP_IS_NOBEGIN(dt1))
		{
			if (TIMESTAMP_IS_NOBEGIN(dt2))
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("interval out of range")));
			else
				INTERVAL_NOBEGIN(result);
		}
		else if (TIMESTAMP_IS_NOEND(dt1))
		{
			if (TIMESTAMP_IS_NOEND(dt2))
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("interval out of range")));
			else
				INTERVAL_NOEND(result);
		}
		else if (TIMESTAMP_IS_NOBEGIN(dt2))
			INTERVAL_NOEND(result);
		else					/* TIMESTAMP_IS_NOEND(dt2) */
			INTERVAL_NOBEGIN(result);

		PG_RETURN_INTERVAL_P(result);
	}

	if (unlikely(pg_sub_s64_overflow(dt1, dt2, &result->time)))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));

	result->month = 0;
	result->day = 0;

	/*----------
	 *	This is wrong, but removing it breaks a lot of regression tests.
	 *	For example:
	 *
	 *	test=> SET timezone = 'EST5EDT';
	 *	test=> SELECT
	 *	test-> ('2005-10-30 13:22:00-05'::timestamptz -
	 *	test(>	'2005-10-29 13:22:00-04'::timestamptz);
	 *	?column?
	 *	----------------
	 *	 1 day 01:00:00
	 *	 (1 row)
	 *
	 *	so adding that to the first timestamp gets:
	 *
	 *	 test=> SELECT
	 *	 test-> ('2005-10-29 13:22:00-04'::timestamptz +
	 *	 test(> ('2005-10-30 13:22:00-05'::timestamptz -
	 *	 test(>  '2005-10-29 13:22:00-04'::timestamptz)) at time zone 'EST';
	 *		timezone
	 *	--------------------
	 *	2005-10-30 14:22:00
	 *	(1 row)
	 *----------
	 */
	result = DatumGetIntervalP(DirectFunctionCall1(interval_justify_hours,
												   IntervalPGetDatum(result)));

	PG_RETURN_INTERVAL_P(result);
}

/*
 *	interval_justify_interval()
 *
 *	Adjust interval so 'month', 'day', and 'time' portions are within
 *	customary bounds.  Specifically:
 *
 *		0 <= abs(time) < 24 hours
 *		0 <= abs(day)  < 30 days
 *
 *	Also, the sign bit on all three fields is made equal, so either
 *	all three fields are negative or all are positive.
 */
Datum
interval_justify_interval(PG_FUNCTION_ARGS)
{
	Interval   *span = PG_GETARG_INTERVAL_P(0);
	Interval   *result;
	TimeOffset	wholeday;
	int32		wholemonth;

	result = (Interval *) palloc(sizeof(Interval));
	result->month = span->month;
	result->day = span->day;
	result->time = span->time;

	/* do nothing for infinite intervals */
	if (INTERVAL_NOT_FINITE(result))
		PG_RETURN_INTERVAL_P(result);

	/* pre-justify days if it might prevent overflow */
	if ((result->day > 0 && result->time > 0) ||
		(result->day < 0 && result->time < 0))
	{
		wholemonth = result->day / DAYS_PER_MONTH;
		result->day -= wholemonth * DAYS_PER_MONTH;
		if (pg_add_s32_overflow(result->month, wholemonth, &result->month))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("interval out of range")));
	}

	/*
	 * Since TimeOffset is int64, abs(wholeday) can't exceed about 1.07e8.  If
	 * we pre-justified then abs(result->day) is less than DAYS_PER_MONTH, so
	 * this addition can't overflow.  If we didn't pre-justify, then day and
	 * time are of different signs, so it still can't overflow.
	 */
	TMODULO(result->time, wholeday, USECS_PER_DAY);
	result->day += wholeday;

	wholemonth = result->day / DAYS_PER_MONTH;
	result->day -= wholemonth * DAYS_PER_MONTH;
	if (pg_add_s32_overflow(result->month, wholemonth, &result->month))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));

	if (result->month > 0 &&
		(result->day < 0 || (result->day == 0 && result->time < 0)))
	{
		result->day += DAYS_PER_MONTH;
		result->month--;
	}
	else if (result->month < 0 &&
			 (result->day > 0 || (result->day == 0 && result->time > 0)))
	{
		result->day -= DAYS_PER_MONTH;
		result->month++;
	}

	if (result->day > 0 && result->time < 0)
	{
		result->time += USECS_PER_DAY;
		result->day--;
	}
	else if (result->day < 0 && result->time > 0)
	{
		result->time -= USECS_PER_DAY;
		result->day++;
	}

	PG_RETURN_INTERVAL_P(result);
}

/*
 *	interval_justify_hours()
 *
 *	Adjust interval so 'time' contains less than a whole day, adding
 *	the excess to 'day'.  This is useful for
 *	situations (such as non-TZ) where '1 day' = '24 hours' is valid,
 *	e.g. interval subtraction and division.
 */
Datum
interval_justify_hours(PG_FUNCTION_ARGS)
{
	Interval   *span = PG_GETARG_INTERVAL_P(0);
	Interval   *result;
	TimeOffset	wholeday;

	result = (Interval *) palloc(sizeof(Interval));
	result->month = span->month;
	result->day = span->day;
	result->time = span->time;

	/* do nothing for infinite intervals */
	if (INTERVAL_NOT_FINITE(result))
		PG_RETURN_INTERVAL_P(result);

	TMODULO(result->time, wholeday, USECS_PER_DAY);
	if (pg_add_s32_overflow(result->day, wholeday, &result->day))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));

	if (result->day > 0 && result->time < 0)
	{
		result->time += USECS_PER_DAY;
		result->day--;
	}
	else if (result->day < 0 && result->time > 0)
	{
		result->time -= USECS_PER_DAY;
		result->day++;
	}

	PG_RETURN_INTERVAL_P(result);
}

/*
 *	interval_justify_days()
 *
 *	Adjust interval so 'day' contains less than 30 days, adding
 *	the excess to 'month'.
 */
Datum
interval_justify_days(PG_FUNCTION_ARGS)
{
	Interval   *span = PG_GETARG_INTERVAL_P(0);
	Interval   *result;
	int32		wholemonth;

	result = (Interval *) palloc(sizeof(Interval));
	result->month = span->month;
	result->day = span->day;
	result->time = span->time;

	/* do nothing for infinite intervals */
	if (INTERVAL_NOT_FINITE(result))
		PG_RETURN_INTERVAL_P(result);

	wholemonth = result->day / DAYS_PER_MONTH;
	result->day -= wholemonth * DAYS_PER_MONTH;
	if (pg_add_s32_overflow(result->month, wholemonth, &result->month))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));

	if (result->month > 0 && result->day < 0)
	{
		result->day += DAYS_PER_MONTH;
		result->month--;
	}
	else if (result->month < 0 && result->day > 0)
	{
		result->day -= DAYS_PER_MONTH;
		result->month++;
	}

	PG_RETURN_INTERVAL_P(result);
}

/* timestamp_pl_interval()
 * Add an interval to a timestamp data type.
 * Note that interval has provisions for qualitative year/month and day
 *	units, so try to do the right thing with them.
 * To add a month, increment the month, and use the same day of month.
 * Then, if the next month has fewer days, set the day of month
 *	to the last day of month.
 * To add a day, increment the mday, and use the same time of day.
 * Lastly, add in the "quantitative time".
 */
Datum
timestamp_pl_interval(PG_FUNCTION_ARGS)
{
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(0);
	Interval   *span = PG_GETARG_INTERVAL_P(1);
	Timestamp	result;

	/*
	 * Handle infinities.
	 *
	 * We treat anything that amounts to "infinity - infinity" as an error,
	 * since the timestamp type has nothing equivalent to NaN.
	 */
	if (INTERVAL_IS_NOBEGIN(span))
	{
		if (TIMESTAMP_IS_NOEND(timestamp))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
		else
			TIMESTAMP_NOBEGIN(result);
	}
	else if (INTERVAL_IS_NOEND(span))
	{
		if (TIMESTAMP_IS_NOBEGIN(timestamp))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
		else
			TIMESTAMP_NOEND(result);
	}
	else if (TIMESTAMP_NOT_FINITE(timestamp))
		result = timestamp;
	else
	{
		if (span->month != 0)
		{
			struct pg_tm tt,
					   *tm = &tt;
			fsec_t		fsec;

			if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL, NULL) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));

			if (pg_add_s32_overflow(tm->tm_mon, span->month, &tm->tm_mon))
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));
			if (tm->tm_mon > MONTHS_PER_YEAR)
			{
				tm->tm_year += (tm->tm_mon - 1) / MONTHS_PER_YEAR;
				tm->tm_mon = ((tm->tm_mon - 1) % MONTHS_PER_YEAR) + 1;
			}
			else if (tm->tm_mon < 1)
			{
				tm->tm_year += tm->tm_mon / MONTHS_PER_YEAR - 1;
				tm->tm_mon = tm->tm_mon % MONTHS_PER_YEAR + MONTHS_PER_YEAR;
			}

			/* adjust for end of month boundary problems... */
			if (tm->tm_mday > day_tab[isleap(tm->tm_year)][tm->tm_mon - 1])
				tm->tm_mday = (day_tab[isleap(tm->tm_year)][tm->tm_mon - 1]);

			if (tm2timestamp(tm, fsec, NULL, &timestamp) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));
		}

		if (span->day != 0)
		{
			struct pg_tm tt,
					   *tm = &tt;
			fsec_t		fsec;
			int			julian;

			if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL, NULL) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));

			/*
			 * Add days by converting to and from Julian.  We need an overflow
			 * check here since j2date expects a non-negative integer input.
			 */
			julian = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday);
			if (pg_add_s32_overflow(julian, span->day, &julian) ||
				julian < 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));
			j2date(julian, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);

			if (tm2timestamp(tm, fsec, NULL, &timestamp) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));
		}

		if (pg_add_s64_overflow(timestamp, span->time, &timestamp))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));

		if (!IS_VALID_TIMESTAMP(timestamp))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));

		result = timestamp;
	}

	PG_RETURN_TIMESTAMP(result);
}

Datum
timestamp_mi_interval(PG_FUNCTION_ARGS)
{
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(0);
	Interval   *span = PG_GETARG_INTERVAL_P(1);
	Interval	tspan;

	interval_um_internal(span, &tspan);

	return DirectFunctionCall2(timestamp_pl_interval,
							   TimestampGetDatum(timestamp),
							   PointerGetDatum(&tspan));
}


/* timestamptz_pl_interval_internal()
 * Add an interval to a timestamptz, in the given (or session) timezone.
 *
 * Note that interval has provisions for qualitative year/month and day
 *	units, so try to do the right thing with them.
 * To add a month, increment the month, and use the same day of month.
 * Then, if the next month has fewer days, set the day of month
 *	to the last day of month.
 * To add a day, increment the mday, and use the same time of day.
 * Lastly, add in the "quantitative time".
 */
static TimestampTz
timestamptz_pl_interval_internal(TimestampTz timestamp,
								 Interval *span,
								 pg_tz *attimezone)
{
	TimestampTz result;
	int			tz;

	/*
	 * Handle infinities.
	 *
	 * We treat anything that amounts to "infinity - infinity" as an error,
	 * since the timestamptz type has nothing equivalent to NaN.
	 */
	if (INTERVAL_IS_NOBEGIN(span))
	{
		if (TIMESTAMP_IS_NOEND(timestamp))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
		else
			TIMESTAMP_NOBEGIN(result);
	}
	else if (INTERVAL_IS_NOEND(span))
	{
		if (TIMESTAMP_IS_NOBEGIN(timestamp))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
		else
			TIMESTAMP_NOEND(result);
	}
	else if (TIMESTAMP_NOT_FINITE(timestamp))
		result = timestamp;
	else
	{
		/* Use session timezone if caller asks for default */
		if (attimezone == NULL)
			attimezone = session_timezone;

		if (span->month != 0)
		{
			struct pg_tm tt,
					   *tm = &tt;
			fsec_t		fsec;

			if (timestamp2tm(timestamp, &tz, tm, &fsec, NULL, attimezone) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));

			if (pg_add_s32_overflow(tm->tm_mon, span->month, &tm->tm_mon))
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));
			if (tm->tm_mon > MONTHS_PER_YEAR)
			{
				tm->tm_year += (tm->tm_mon - 1) / MONTHS_PER_YEAR;
				tm->tm_mon = ((tm->tm_mon - 1) % MONTHS_PER_YEAR) + 1;
			}
			else if (tm->tm_mon < 1)
			{
				tm->tm_year += tm->tm_mon / MONTHS_PER_YEAR - 1;
				tm->tm_mon = tm->tm_mon % MONTHS_PER_YEAR + MONTHS_PER_YEAR;
			}

			/* adjust for end of month boundary problems... */
			if (tm->tm_mday > day_tab[isleap(tm->tm_year)][tm->tm_mon - 1])
				tm->tm_mday = (day_tab[isleap(tm->tm_year)][tm->tm_mon - 1]);

			tz = DetermineTimeZoneOffset(tm, attimezone);

			if (tm2timestamp(tm, fsec, &tz, &timestamp) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));
		}

		if (span->day != 0)
		{
			struct pg_tm tt,
					   *tm = &tt;
			fsec_t		fsec;
			int			julian;

			if (timestamp2tm(timestamp, &tz, tm, &fsec, NULL, attimezone) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));

			/*
			 * Add days by converting to and from Julian.  We need an overflow
			 * check here since j2date expects a non-negative integer input.
			 * In practice though, it will give correct answers for small
			 * negative Julian dates; we should allow -1 to avoid
			 * timezone-dependent failures, as discussed in timestamp.h.
			 */
			julian = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday);
			if (pg_add_s32_overflow(julian, span->day, &julian) ||
				julian < -1)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));
			j2date(julian, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);

			tz = DetermineTimeZoneOffset(tm, attimezone);

			if (tm2timestamp(tm, fsec, &tz, &timestamp) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));
		}

		if (pg_add_s64_overflow(timestamp, span->time, &timestamp))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));

		if (!IS_VALID_TIMESTAMP(timestamp))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));

		result = timestamp;
	}

	return result;
}

/* timestamptz_mi_interval_internal()
 * As above, but subtract the interval.
 */
static TimestampTz
timestamptz_mi_interval_internal(TimestampTz timestamp,
								 Interval *span,
								 pg_tz *attimezone)
{
	Interval	tspan;

	interval_um_internal(span, &tspan);

	return timestamptz_pl_interval_internal(timestamp, &tspan, attimezone);
}

/* timestamptz_pl_interval()
 * Add an interval to a timestamptz, in the session timezone.
 */
Datum
timestamptz_pl_interval(PG_FUNCTION_ARGS)
{
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(0);
	Interval   *span = PG_GETARG_INTERVAL_P(1);

	PG_RETURN_TIMESTAMP(timestamptz_pl_interval_internal(timestamp, span, NULL));
}

Datum
timestamptz_mi_interval(PG_FUNCTION_ARGS)
{
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(0);
	Interval   *span = PG_GETARG_INTERVAL_P(1);

	PG_RETURN_TIMESTAMP(timestamptz_mi_interval_internal(timestamp, span, NULL));
}

/* timestamptz_pl_interval_at_zone()
 * Add an interval to a timestamptz, in the specified timezone.
 */
Datum
timestamptz_pl_interval_at_zone(PG_FUNCTION_ARGS)
{
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(0);
	Interval   *span = PG_GETARG_INTERVAL_P(1);
	text	   *zone = PG_GETARG_TEXT_PP(2);
	pg_tz	   *attimezone = lookup_timezone(zone);

	PG_RETURN_TIMESTAMP(timestamptz_pl_interval_internal(timestamp, span, attimezone));
}

Datum
timestamptz_mi_interval_at_zone(PG_FUNCTION_ARGS)
{
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(0);
	Interval   *span = PG_GETARG_INTERVAL_P(1);
	text	   *zone = PG_GETARG_TEXT_PP(2);
	pg_tz	   *attimezone = lookup_timezone(zone);

	PG_RETURN_TIMESTAMP(timestamptz_mi_interval_internal(timestamp, span, attimezone));
}

/* interval_um_internal()
 * Negate an interval.
 */
static void
interval_um_internal(const Interval *interval, Interval *result)
{
	if (INTERVAL_IS_NOBEGIN(interval))
		INTERVAL_NOEND(result);
	else if (INTERVAL_IS_NOEND(interval))
		INTERVAL_NOBEGIN(result);
	else
	{
		/* Negate each field, guarding against overflow */
		if (pg_sub_s64_overflow(INT64CONST(0), interval->time, &result->time) ||
			pg_sub_s32_overflow(0, interval->day, &result->day) ||
			pg_sub_s32_overflow(0, interval->month, &result->month) ||
			INTERVAL_NOT_FINITE(result))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("interval out of range")));
	}
}

Datum
interval_um(PG_FUNCTION_ARGS)
{
	Interval   *interval = PG_GETARG_INTERVAL_P(0);
	Interval   *result;

	result = (Interval *) palloc(sizeof(Interval));
	interval_um_internal(interval, result);

	PG_RETURN_INTERVAL_P(result);
}


Datum
interval_smaller(PG_FUNCTION_ARGS)
{
	Interval   *interval1 = PG_GETARG_INTERVAL_P(0);
	Interval   *interval2 = PG_GETARG_INTERVAL_P(1);
	Interval   *result;

	/* use interval_cmp_internal to be sure this agrees with comparisons */
	if (interval_cmp_internal(interval1, interval2) < 0)
		result = interval1;
	else
		result = interval2;
	PG_RETURN_INTERVAL_P(result);
}

Datum
interval_larger(PG_FUNCTION_ARGS)
{
	Interval   *interval1 = PG_GETARG_INTERVAL_P(0);
	Interval   *interval2 = PG_GETARG_INTERVAL_P(1);
	Interval   *result;

	if (interval_cmp_internal(interval1, interval2) > 0)
		result = interval1;
	else
		result = interval2;
	PG_RETURN_INTERVAL_P(result);
}

static void
finite_interval_pl(const Interval *span1, const Interval *span2, Interval *result)
{
	Assert(!INTERVAL_NOT_FINITE(span1));
	Assert(!INTERVAL_NOT_FINITE(span2));

	if (pg_add_s32_overflow(span1->month, span2->month, &result->month) ||
		pg_add_s32_overflow(span1->day, span2->day, &result->day) ||
		pg_add_s64_overflow(span1->time, span2->time, &result->time) ||
		INTERVAL_NOT_FINITE(result))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));
}

Datum
interval_pl(PG_FUNCTION_ARGS)
{
	Interval   *span1 = PG_GETARG_INTERVAL_P(0);
	Interval   *span2 = PG_GETARG_INTERVAL_P(1);
	Interval   *result;

	result = (Interval *) palloc(sizeof(Interval));

	/*
	 * Handle infinities.
	 *
	 * We treat anything that amounts to "infinity - infinity" as an error,
	 * since the interval type has nothing equivalent to NaN.
	 */
	if (INTERVAL_IS_NOBEGIN(span1))
	{
		if (INTERVAL_IS_NOEND(span2))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("interval out of range")));
		else
			INTERVAL_NOBEGIN(result);
	}
	else if (INTERVAL_IS_NOEND(span1))
	{
		if (INTERVAL_IS_NOBEGIN(span2))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("interval out of range")));
		else
			INTERVAL_NOEND(result);
	}
	else if (INTERVAL_NOT_FINITE(span2))
		memcpy(result, span2, sizeof(Interval));
	else
		finite_interval_pl(span1, span2, result);

	PG_RETURN_INTERVAL_P(result);
}

static void
finite_interval_mi(const Interval *span1, const Interval *span2, Interval *result)
{
	Assert(!INTERVAL_NOT_FINITE(span1));
	Assert(!INTERVAL_NOT_FINITE(span2));

	if (pg_sub_s32_overflow(span1->month, span2->month, &result->month) ||
		pg_sub_s32_overflow(span1->day, span2->day, &result->day) ||
		pg_sub_s64_overflow(span1->time, span2->time, &result->time) ||
		INTERVAL_NOT_FINITE(result))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));
}

Datum
interval_mi(PG_FUNCTION_ARGS)
{
	Interval   *span1 = PG_GETARG_INTERVAL_P(0);
	Interval   *span2 = PG_GETARG_INTERVAL_P(1);
	Interval   *result;

	result = (Interval *) palloc(sizeof(Interval));

	/*
	 * Handle infinities.
	 *
	 * We treat anything that amounts to "infinity - infinity" as an error,
	 * since the interval type has nothing equivalent to NaN.
	 */
	if (INTERVAL_IS_NOBEGIN(span1))
	{
		if (INTERVAL_IS_NOBEGIN(span2))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("interval out of range")));
		else
			INTERVAL_NOBEGIN(result);
	}
	else if (INTERVAL_IS_NOEND(span1))
	{
		if (INTERVAL_IS_NOEND(span2))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("interval out of range")));
		else
			INTERVAL_NOEND(result);
	}
	else if (INTERVAL_IS_NOBEGIN(span2))
		INTERVAL_NOEND(result);
	else if (INTERVAL_IS_NOEND(span2))
		INTERVAL_NOBEGIN(result);
	else
		finite_interval_mi(span1, span2, result);

	PG_RETURN_INTERVAL_P(result);
}

/*
 *	There is no interval_abs():  it is unclear what value to return:
 *	  http://archives.postgresql.org/pgsql-general/2009-10/msg01031.php
 *	  http://archives.postgresql.org/pgsql-general/2009-11/msg00041.php
 */

Datum
interval_mul(PG_FUNCTION_ARGS)
{
	Interval   *span = PG_GETARG_INTERVAL_P(0);
	float8		factor = PG_GETARG_FLOAT8(1);
	double		month_remainder_days,
				sec_remainder,
				result_double;
	int32		orig_month = span->month,
				orig_day = span->day;
	Interval   *result;

	result = (Interval *) palloc(sizeof(Interval));

	/*
	 * Handle NaN and infinities.
	 *
	 * We treat "0 * infinity" and "infinity * 0" as errors, since the
	 * interval type has nothing equivalent to NaN.
	 */
	if (isnan(factor))
		goto out_of_range;

	if (INTERVAL_NOT_FINITE(span))
	{
		if (factor == 0.0)
			goto out_of_range;

		if (factor < 0.0)
			interval_um_internal(span, result);
		else
			memcpy(result, span, sizeof(Interval));

		PG_RETURN_INTERVAL_P(result);
	}
	if (isinf(factor))
	{
		int			isign = interval_sign(span);

		if (isign == 0)
			goto out_of_range;

		if (factor * isign < 0)
			INTERVAL_NOBEGIN(result);
		else
			INTERVAL_NOEND(result);

		PG_RETURN_INTERVAL_P(result);
	}

	result_double = span->month * factor;
	if (isnan(result_double) || !FLOAT8_FITS_IN_INT32(result_double))
		goto out_of_range;
	result->month = (int32) result_double;

	result_double = span->day * factor;
	if (isnan(result_double) || !FLOAT8_FITS_IN_INT32(result_double))
		goto out_of_range;
	result->day = (int32) result_double;

	/*
	 * The above correctly handles the whole-number part of the month and day
	 * products, but we have to do something with any fractional part
	 * resulting when the factor is non-integral.  We cascade the fractions
	 * down to lower units using the conversion factors DAYS_PER_MONTH and
	 * SECS_PER_DAY.  Note we do NOT cascade up, since we are not forced to do
	 * so by the representation.  The user can choose to cascade up later,
	 * using justify_hours and/or justify_days.
	 */

	/*
	 * Fractional months full days into days.
	 *
	 * Floating point calculation are inherently imprecise, so these
	 * calculations are crafted to produce the most reliable result possible.
	 * TSROUND() is needed to more accurately produce whole numbers where
	 * appropriate.
	 */
	month_remainder_days = (orig_month * factor - result->month) * DAYS_PER_MONTH;
	month_remainder_days = TSROUND(month_remainder_days);
	sec_remainder = (orig_day * factor - result->day +
					 month_remainder_days - (int) month_remainder_days) * SECS_PER_DAY;
	sec_remainder = TSROUND(sec_remainder);

	/*
	 * Might have 24:00:00 hours due to rounding, or >24 hours because of time
	 * cascade from months and days.  It might still be >24 if the combination
	 * of cascade and the seconds factor operation itself.
	 */
	if (fabs(sec_remainder) >= SECS_PER_DAY)
	{
		if (pg_add_s32_overflow(result->day,
								(int) (sec_remainder / SECS_PER_DAY),
								&result->day))
			goto out_of_range;
		sec_remainder -= (int) (sec_remainder / SECS_PER_DAY) * SECS_PER_DAY;
	}

	/* cascade units down */
	if (pg_add_s32_overflow(result->day, (int32) month_remainder_days,
							&result->day))
		goto out_of_range;
	result_double = rint(span->time * factor + sec_remainder * USECS_PER_SEC);
	if (isnan(result_double) || !FLOAT8_FITS_IN_INT64(result_double))
		goto out_of_range;
	result->time = (int64) result_double;

	if (INTERVAL_NOT_FINITE(result))
		goto out_of_range;

	PG_RETURN_INTERVAL_P(result);

out_of_range:
	ereport(ERROR,
			errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
			errmsg("interval out of range"));

	PG_RETURN_NULL();			/* keep compiler quiet */
}

Datum
mul_d_interval(PG_FUNCTION_ARGS)
{
	/* Args are float8 and Interval *, but leave them as generic Datum */
	Datum		factor = PG_GETARG_DATUM(0);
	Datum		span = PG_GETARG_DATUM(1);

	return DirectFunctionCall2(interval_mul, span, factor);
}

Datum
interval_div(PG_FUNCTION_ARGS)
{
	Interval   *span = PG_GETARG_INTERVAL_P(0);
	float8		factor = PG_GETARG_FLOAT8(1);
	double		month_remainder_days,
				sec_remainder,
				result_double;
	int32		orig_month = span->month,
				orig_day = span->day;
	Interval   *result;

	result = (Interval *) palloc(sizeof(Interval));

	if (factor == 0.0)
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));

	/*
	 * Handle NaN and infinities.
	 *
	 * We treat "infinity / infinity" as an error, since the interval type has
	 * nothing equivalent to NaN.  Otherwise, dividing by infinity is handled
	 * by the regular division code, causing all fields to be set to zero.
	 */
	if (isnan(factor))
		goto out_of_range;

	if (INTERVAL_NOT_FINITE(span))
	{
		if (isinf(factor))
			goto out_of_range;

		if (factor < 0.0)
			interval_um_internal(span, result);
		else
			memcpy(result, span, sizeof(Interval));

		PG_RETURN_INTERVAL_P(result);
	}

	result_double = span->month / factor;
	if (isnan(result_double) || !FLOAT8_FITS_IN_INT32(result_double))
		goto out_of_range;
	result->month = (int32) result_double;

	result_double = span->day / factor;
	if (isnan(result_double) || !FLOAT8_FITS_IN_INT32(result_double))
		goto out_of_range;
	result->day = (int32) result_double;

	/*
	 * Fractional months full days into days.  See comment in interval_mul().
	 */
	month_remainder_days = (orig_month / factor - result->month) * DAYS_PER_MONTH;
	month_remainder_days = TSROUND(month_remainder_days);
	sec_remainder = (orig_day / factor - result->day +
					 month_remainder_days - (int) month_remainder_days) * SECS_PER_DAY;
	sec_remainder = TSROUND(sec_remainder);
	if (fabs(sec_remainder) >= SECS_PER_DAY)
	{
		if (pg_add_s32_overflow(result->day,
								(int) (sec_remainder / SECS_PER_DAY),
								&result->day))
			goto out_of_range;
		sec_remainder -= (int) (sec_remainder / SECS_PER_DAY) * SECS_PER_DAY;
	}

	/* cascade units down */
	if (pg_add_s32_overflow(result->day, (int32) month_remainder_days,
							&result->day))
		goto out_of_range;
	result_double = rint(span->time / factor + sec_remainder * USECS_PER_SEC);
	if (isnan(result_double) || !FLOAT8_FITS_IN_INT64(result_double))
		goto out_of_range;
	result->time = (int64) result_double;

	if (INTERVAL_NOT_FINITE(result))
		goto out_of_range;

	PG_RETURN_INTERVAL_P(result);

out_of_range:
	ereport(ERROR,
			errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
			errmsg("interval out of range"));

	PG_RETURN_NULL();			/* keep compiler quiet */
}


/*
 * in_range support functions for timestamps and intervals.
 *
 * Per SQL spec, we support these with interval as the offset type.
 * The spec's restriction that the offset not be negative is a bit hard to
 * decipher for intervals, but we choose to interpret it the same as our
 * interval comparison operators would.
 */

Datum
in_range_timestamptz_interval(PG_FUNCTION_ARGS)
{
	TimestampTz val = PG_GETARG_TIMESTAMPTZ(0);
	TimestampTz base = PG_GETARG_TIMESTAMPTZ(1);
	Interval   *offset = PG_GETARG_INTERVAL_P(2);
	bool		sub = PG_GETARG_BOOL(3);
	bool		less = PG_GETARG_BOOL(4);
	TimestampTz sum;

	if (interval_sign(offset) < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PRECEDING_OR_FOLLOWING_SIZE),
				 errmsg("invalid preceding or following size in window function")));

	/*
	 * Deal with cases where both base and offset are infinite, and computing
	 * base +/- offset would cause an error.  As for float and numeric types,
	 * we assume that all values infinitely precede +infinity and infinitely
	 * follow -infinity.  See in_range_float8_float8() for reasoning.
	 */
	if (INTERVAL_IS_NOEND(offset) &&
		(sub ? TIMESTAMP_IS_NOEND(base) : TIMESTAMP_IS_NOBEGIN(base)))
		PG_RETURN_BOOL(true);

	/* We don't currently bother to avoid overflow hazards here */
	if (sub)
		sum = timestamptz_mi_interval_internal(base, offset, NULL);
	else
		sum = timestamptz_pl_interval_internal(base, offset, NULL);

	if (less)
		PG_RETURN_BOOL(val <= sum);
	else
		PG_RETURN_BOOL(val >= sum);
}

Datum
in_range_timestamp_interval(PG_FUNCTION_ARGS)
{
	Timestamp	val = PG_GETARG_TIMESTAMP(0);
	Timestamp	base = PG_GETARG_TIMESTAMP(1);
	Interval   *offset = PG_GETARG_INTERVAL_P(2);
	bool		sub = PG_GETARG_BOOL(3);
	bool		less = PG_GETARG_BOOL(4);
	Timestamp	sum;

	if (interval_sign(offset) < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PRECEDING_OR_FOLLOWING_SIZE),
				 errmsg("invalid preceding or following size in window function")));

	/*
	 * Deal with cases where both base and offset are infinite, and computing
	 * base +/- offset would cause an error.  As for float and numeric types,
	 * we assume that all values infinitely precede +infinity and infinitely
	 * follow -infinity.  See in_range_float8_float8() for reasoning.
	 */
	if (INTERVAL_IS_NOEND(offset) &&
		(sub ? TIMESTAMP_IS_NOEND(base) : TIMESTAMP_IS_NOBEGIN(base)))
		PG_RETURN_BOOL(true);

	/* We don't currently bother to avoid overflow hazards here */
	if (sub)
		sum = DatumGetTimestamp(DirectFunctionCall2(timestamp_mi_interval,
													TimestampGetDatum(base),
													IntervalPGetDatum(offset)));
	else
		sum = DatumGetTimestamp(DirectFunctionCall2(timestamp_pl_interval,
													TimestampGetDatum(base),
													IntervalPGetDatum(offset)));

	if (less)
		PG_RETURN_BOOL(val <= sum);
	else
		PG_RETURN_BOOL(val >= sum);
}

Datum
in_range_interval_interval(PG_FUNCTION_ARGS)
{
	Interval   *val = PG_GETARG_INTERVAL_P(0);
	Interval   *base = PG_GETARG_INTERVAL_P(1);
	Interval   *offset = PG_GETARG_INTERVAL_P(2);
	bool		sub = PG_GETARG_BOOL(3);
	bool		less = PG_GETARG_BOOL(4);
	Interval   *sum;

	if (interval_sign(offset) < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PRECEDING_OR_FOLLOWING_SIZE),
				 errmsg("invalid preceding or following size in window function")));

	/*
	 * Deal with cases where both base and offset are infinite, and computing
	 * base +/- offset would cause an error.  As for float and numeric types,
	 * we assume that all values infinitely precede +infinity and infinitely
	 * follow -infinity.  See in_range_float8_float8() for reasoning.
	 */
	if (INTERVAL_IS_NOEND(offset) &&
		(sub ? INTERVAL_IS_NOEND(base) : INTERVAL_IS_NOBEGIN(base)))
		PG_RETURN_BOOL(true);

	/* We don't currently bother to avoid overflow hazards here */
	if (sub)
		sum = DatumGetIntervalP(DirectFunctionCall2(interval_mi,
													IntervalPGetDatum(base),
													IntervalPGetDatum(offset)));
	else
		sum = DatumGetIntervalP(DirectFunctionCall2(interval_pl,
													IntervalPGetDatum(base),
													IntervalPGetDatum(offset)));

	if (less)
		PG_RETURN_BOOL(interval_cmp_internal(val, sum) <= 0);
	else
		PG_RETURN_BOOL(interval_cmp_internal(val, sum) >= 0);
}


/*
 * Prepare state data for an interval aggregate function, that needs to compute
 * sum and count, in the aggregate's memory context.
 *
 * The function is used when the state data needs to be allocated in aggregate's
 * context. When the state data needs to be allocated in the current memory
 * context, we use palloc0 directly e.g. interval_avg_deserialize().
 */
static IntervalAggState *
makeIntervalAggState(FunctionCallInfo fcinfo)
{
	IntervalAggState *state;
	MemoryContext agg_context;
	MemoryContext old_context;

	if (!AggCheckCallContext(fcinfo, &agg_context))
		elog(ERROR, "aggregate function called in non-aggregate context");

	old_context = MemoryContextSwitchTo(agg_context);

	state = (IntervalAggState *) palloc0(sizeof(IntervalAggState));

	MemoryContextSwitchTo(old_context);

	return state;
}

/*
 * Accumulate a new input value for interval aggregate functions.
 */
static void
do_interval_accum(IntervalAggState *state, Interval *newval)
{
	/* Infinite inputs are counted separately, and do not affect "N" */
	if (INTERVAL_IS_NOBEGIN(newval))
	{
		state->nInfcount++;
		return;
	}

	if (INTERVAL_IS_NOEND(newval))
	{
		state->pInfcount++;
		return;
	}

	finite_interval_pl(&state->sumX, newval, &state->sumX);
	state->N++;
}

/*
 * Remove the given interval value from the aggregated state.
 */
static void
do_interval_discard(IntervalAggState *state, Interval *newval)
{
	/* Infinite inputs are counted separately, and do not affect "N" */
	if (INTERVAL_IS_NOBEGIN(newval))
	{
		state->nInfcount--;
		return;
	}

	if (INTERVAL_IS_NOEND(newval))
	{
		state->pInfcount--;
		return;
	}

	/* Handle the to-be-discarded finite value. */
	state->N--;
	if (state->N > 0)
		finite_interval_mi(&state->sumX, newval, &state->sumX);
	else
	{
		/* All values discarded, reset the state */
		Assert(state->N == 0);
		memset(&state->sumX, 0, sizeof(state->sumX));
	}
}

/*
 * Transition function for sum() and avg() interval aggregates.
 */
Datum
interval_avg_accum(PG_FUNCTION_ARGS)
{
	IntervalAggState *state;

	state = PG_ARGISNULL(0) ? NULL : (IntervalAggState *) PG_GETARG_POINTER(0);

	/* Create the state data on the first call */
	if (state == NULL)
		state = makeIntervalAggState(fcinfo);

	if (!PG_ARGISNULL(1))
		do_interval_accum(state, PG_GETARG_INTERVAL_P(1));

	PG_RETURN_POINTER(state);
}

/*
 * Combine function for sum() and avg() interval aggregates.
 *
 * Combine the given internal aggregate states and place the combination in
 * the first argument.
 */
Datum
interval_avg_combine(PG_FUNCTION_ARGS)
{
	IntervalAggState *state1;
	IntervalAggState *state2;

	state1 = PG_ARGISNULL(0) ? NULL : (IntervalAggState *) PG_GETARG_POINTER(0);
	state2 = PG_ARGISNULL(1) ? NULL : (IntervalAggState *) PG_GETARG_POINTER(1);

	if (state2 == NULL)
		PG_RETURN_POINTER(state1);

	if (state1 == NULL)
	{
		/* manually copy all fields from state2 to state1 */
		state1 = makeIntervalAggState(fcinfo);

		state1->N = state2->N;
		state1->pInfcount = state2->pInfcount;
		state1->nInfcount = state2->nInfcount;

		state1->sumX.day = state2->sumX.day;
		state1->sumX.month = state2->sumX.month;
		state1->sumX.time = state2->sumX.time;

		PG_RETURN_POINTER(state1);
	}

	state1->N += state2->N;
	state1->pInfcount += state2->pInfcount;
	state1->nInfcount += state2->nInfcount;

	/* Accumulate finite interval values, if any. */
	if (state2->N > 0)
		finite_interval_pl(&state1->sumX, &state2->sumX, &state1->sumX);

	PG_RETURN_POINTER(state1);
}

/*
 * interval_avg_serialize
 *		Serialize IntervalAggState for interval aggregates.
 */
Datum
interval_avg_serialize(PG_FUNCTION_ARGS)
{
	IntervalAggState *state;
	StringInfoData buf;
	bytea	   *result;

	/* Ensure we disallow calling when not in aggregate context */
	if (!AggCheckCallContext(fcinfo, NULL))
		elog(ERROR, "aggregate function called in non-aggregate context");

	state = (IntervalAggState *) PG_GETARG_POINTER(0);

	pq_begintypsend(&buf);

	/* N */
	pq_sendint64(&buf, state->N);

	/* sumX */
	pq_sendint64(&buf, state->sumX.time);
	pq_sendint32(&buf, state->sumX.day);
	pq_sendint32(&buf, state->sumX.month);

	/* pInfcount */
	pq_sendint64(&buf, state->pInfcount);

	/* nInfcount */
	pq_sendint64(&buf, state->nInfcount);

	result = pq_endtypsend(&buf);

	PG_RETURN_BYTEA_P(result);
}

/*
 * interval_avg_deserialize
 *		Deserialize bytea into IntervalAggState for interval aggregates.
 */
Datum
interval_avg_deserialize(PG_FUNCTION_ARGS)
{
	bytea	   *sstate;
	IntervalAggState *result;
	StringInfoData buf;

	if (!AggCheckCallContext(fcinfo, NULL))
		elog(ERROR, "aggregate function called in non-aggregate context");

	sstate = PG_GETARG_BYTEA_PP(0);

	/*
	 * Initialize a StringInfo so that we can "receive" it using the standard
	 * recv-function infrastructure.
	 */
	initReadOnlyStringInfo(&buf, VARDATA_ANY(sstate),
						   VARSIZE_ANY_EXHDR(sstate));

	result = (IntervalAggState *) palloc0(sizeof(IntervalAggState));

	/* N */
	result->N = pq_getmsgint64(&buf);

	/* sumX */
	result->sumX.time = pq_getmsgint64(&buf);
	result->sumX.day = pq_getmsgint(&buf, 4);
	result->sumX.month = pq_getmsgint(&buf, 4);

	/* pInfcount */
	result->pInfcount = pq_getmsgint64(&buf);

	/* nInfcount */
	result->nInfcount = pq_getmsgint64(&buf);

	pq_getmsgend(&buf);

	PG_RETURN_POINTER(result);
}

/*
 * Inverse transition function for sum() and avg() interval aggregates.
 */
Datum
interval_avg_accum_inv(PG_FUNCTION_ARGS)
{
	IntervalAggState *state;

	state = PG_ARGISNULL(0) ? NULL : (IntervalAggState *) PG_GETARG_POINTER(0);

	/* Should not get here with no state */
	if (state == NULL)
		elog(ERROR, "interval_avg_accum_inv called with NULL state");

	if (!PG_ARGISNULL(1))
		do_interval_discard(state, PG_GETARG_INTERVAL_P(1));

	PG_RETURN_POINTER(state);
}

/* avg(interval) aggregate final function */
Datum
interval_avg(PG_FUNCTION_ARGS)
{
	IntervalAggState *state;

	state = PG_ARGISNULL(0) ? NULL : (IntervalAggState *) PG_GETARG_POINTER(0);

	/* If there were no non-null inputs, return NULL */
	if (state == NULL || IA_TOTAL_COUNT(state) == 0)
		PG_RETURN_NULL();

	/*
	 * Aggregating infinities that all have the same sign produces infinity
	 * with that sign.  Aggregating infinities with different signs results in
	 * an error.
	 */
	if (state->pInfcount > 0 || state->nInfcount > 0)
	{
		Interval   *result;

		if (state->pInfcount > 0 && state->nInfcount > 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("interval out of range")));

		result = (Interval *) palloc(sizeof(Interval));
		if (state->pInfcount > 0)
			INTERVAL_NOEND(result);
		else
			INTERVAL_NOBEGIN(result);

		PG_RETURN_INTERVAL_P(result);
	}

	return DirectFunctionCall2(interval_div,
							   IntervalPGetDatum(&state->sumX),
							   Float8GetDatum((double) state->N));
}

/* sum(interval) aggregate final function */
Datum
interval_sum(PG_FUNCTION_ARGS)
{
	IntervalAggState *state;
	Interval   *result;

	state = PG_ARGISNULL(0) ? NULL : (IntervalAggState *) PG_GETARG_POINTER(0);

	/* If there were no non-null inputs, return NULL */
	if (state == NULL || IA_TOTAL_COUNT(state) == 0)
		PG_RETURN_NULL();

	/*
	 * Aggregating infinities that all have the same sign produces infinity
	 * with that sign.  Aggregating infinities with different signs results in
	 * an error.
	 */
	if (state->pInfcount > 0 && state->nInfcount > 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));

	result = (Interval *) palloc(sizeof(Interval));

	if (state->pInfcount > 0)
		INTERVAL_NOEND(result);
	else if (state->nInfcount > 0)
		INTERVAL_NOBEGIN(result);
	else
		memcpy(result, &state->sumX, sizeof(Interval));

	PG_RETURN_INTERVAL_P(result);
}

/* timestamp_age()
 * Calculate time difference while retaining year/month fields.
 * Note that this does not result in an accurate absolute time span
 *	since year and month are out of context once the arithmetic
 *	is done.
 */
Datum
timestamp_age(PG_FUNCTION_ARGS)
{
	Timestamp	dt1 = PG_GETARG_TIMESTAMP(0);
	Timestamp	dt2 = PG_GETARG_TIMESTAMP(1);
	Interval   *result;
	fsec_t		fsec1,
				fsec2;
	struct pg_itm tt,
			   *tm = &tt;
	struct pg_tm tt1,
			   *tm1 = &tt1;
	struct pg_tm tt2,
			   *tm2 = &tt2;

	result = (Interval *) palloc(sizeof(Interval));

	/*
	 * Handle infinities.
	 *
	 * We treat anything that amounts to "infinity - infinity" as an error,
	 * since the interval type has nothing equivalent to NaN.
	 */
	if (TIMESTAMP_IS_NOBEGIN(dt1))
	{
		if (TIMESTAMP_IS_NOBEGIN(dt2))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("interval out of range")));
		else
			INTERVAL_NOBEGIN(result);
	}
	else if (TIMESTAMP_IS_NOEND(dt1))
	{
		if (TIMESTAMP_IS_NOEND(dt2))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("interval out of range")));
		else
			INTERVAL_NOEND(result);
	}
	else if (TIMESTAMP_IS_NOBEGIN(dt2))
		INTERVAL_NOEND(result);
	else if (TIMESTAMP_IS_NOEND(dt2))
		INTERVAL_NOBEGIN(result);
	else if (timestamp2tm(dt1, NULL, tm1, &fsec1, NULL, NULL) == 0 &&
			 timestamp2tm(dt2, NULL, tm2, &fsec2, NULL, NULL) == 0)
	{
		/* form the symbolic difference */
		tm->tm_usec = fsec1 - fsec2;
		tm->tm_sec = tm1->tm_sec - tm2->tm_sec;
		tm->tm_min = tm1->tm_min - tm2->tm_min;
		tm->tm_hour = tm1->tm_hour - tm2->tm_hour;
		tm->tm_mday = tm1->tm_mday - tm2->tm_mday;
		tm->tm_mon = tm1->tm_mon - tm2->tm_mon;
		tm->tm_year = tm1->tm_year - tm2->tm_year;

		/* flip sign if necessary... */
		if (dt1 < dt2)
		{
			tm->tm_usec = -tm->tm_usec;
			tm->tm_sec = -tm->tm_sec;
			tm->tm_min = -tm->tm_min;
			tm->tm_hour = -tm->tm_hour;
			tm->tm_mday = -tm->tm_mday;
			tm->tm_mon = -tm->tm_mon;
			tm->tm_year = -tm->tm_year;
		}

		/* propagate any negative fields into the next higher field */
		while (tm->tm_usec < 0)
		{
			tm->tm_usec += USECS_PER_SEC;
			tm->tm_sec--;
		}

		while (tm->tm_sec < 0)
		{
			tm->tm_sec += SECS_PER_MINUTE;
			tm->tm_min--;
		}

		while (tm->tm_min < 0)
		{
			tm->tm_min += MINS_PER_HOUR;
			tm->tm_hour--;
		}

		while (tm->tm_hour < 0)
		{
			tm->tm_hour += HOURS_PER_DAY;
			tm->tm_mday--;
		}

		while (tm->tm_mday < 0)
		{
			if (dt1 < dt2)
			{
				tm->tm_mday += day_tab[isleap(tm1->tm_year)][tm1->tm_mon - 1];
				tm->tm_mon--;
			}
			else
			{
				tm->tm_mday += day_tab[isleap(tm2->tm_year)][tm2->tm_mon - 1];
				tm->tm_mon--;
			}
		}

		while (tm->tm_mon < 0)
		{
			tm->tm_mon += MONTHS_PER_YEAR;
			tm->tm_year--;
		}

		/* recover sign if necessary... */
		if (dt1 < dt2)
		{
			tm->tm_usec = -tm->tm_usec;
			tm->tm_sec = -tm->tm_sec;
			tm->tm_min = -tm->tm_min;
			tm->tm_hour = -tm->tm_hour;
			tm->tm_mday = -tm->tm_mday;
			tm->tm_mon = -tm->tm_mon;
			tm->tm_year = -tm->tm_year;
		}

		if (itm2interval(tm, result) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("interval out of range")));
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	PG_RETURN_INTERVAL_P(result);
}


/* timestamptz_age()
 * Calculate time difference while retaining year/month fields.
 * Note that this does not result in an accurate absolute time span
 *	since year and month are out of context once the arithmetic
 *	is done.
 */
Datum
timestamptz_age(PG_FUNCTION_ARGS)
{
	TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
	TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);
	Interval   *result;
	fsec_t		fsec1,
				fsec2;
	struct pg_itm tt,
			   *tm = &tt;
	struct pg_tm tt1,
			   *tm1 = &tt1;
	struct pg_tm tt2,
			   *tm2 = &tt2;
	int			tz1;
	int			tz2;

	result = (Interval *) palloc(sizeof(Interval));

	/*
	 * Handle infinities.
	 *
	 * We treat anything that amounts to "infinity - infinity" as an error,
	 * since the interval type has nothing equivalent to NaN.
	 */
	if (TIMESTAMP_IS_NOBEGIN(dt1))
	{
		if (TIMESTAMP_IS_NOBEGIN(dt2))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("interval out of range")));
		else
			INTERVAL_NOBEGIN(result);
	}
	else if (TIMESTAMP_IS_NOEND(dt1))
	{
		if (TIMESTAMP_IS_NOEND(dt2))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("interval out of range")));
		else
			INTERVAL_NOEND(result);
	}
	else if (TIMESTAMP_IS_NOBEGIN(dt2))
		INTERVAL_NOEND(result);
	else if (TIMESTAMP_IS_NOEND(dt2))
		INTERVAL_NOBEGIN(result);
	else if (timestamp2tm(dt1, &tz1, tm1, &fsec1, NULL, NULL) == 0 &&
			 timestamp2tm(dt2, &tz2, tm2, &fsec2, NULL, NULL) == 0)
	{
		/* form the symbolic difference */
		tm->tm_usec = fsec1 - fsec2;
		tm->tm_sec = tm1->tm_sec - tm2->tm_sec;
		tm->tm_min = tm1->tm_min - tm2->tm_min;
		tm->tm_hour = tm1->tm_hour - tm2->tm_hour;
		tm->tm_mday = tm1->tm_mday - tm2->tm_mday;
		tm->tm_mon = tm1->tm_mon - tm2->tm_mon;
		tm->tm_year = tm1->tm_year - tm2->tm_year;

		/* flip sign if necessary... */
		if (dt1 < dt2)
		{
			tm->tm_usec = -tm->tm_usec;
			tm->tm_sec = -tm->tm_sec;
			tm->tm_min = -tm->tm_min;
			tm->tm_hour = -tm->tm_hour;
			tm->tm_mday = -tm->tm_mday;
			tm->tm_mon = -tm->tm_mon;
			tm->tm_year = -tm->tm_year;
		}

		/* propagate any negative fields into the next higher field */
		while (tm->tm_usec < 0)
		{
			tm->tm_usec += USECS_PER_SEC;
			tm->tm_sec--;
		}

		while (tm->tm_sec < 0)
		{
			tm->tm_sec += SECS_PER_MINUTE;
			tm->tm_min--;
		}

		while (tm->tm_min < 0)
		{
			tm->tm_min += MINS_PER_HOUR;
			tm->tm_hour--;
		}

		while (tm->tm_hour < 0)
		{
			tm->tm_hour += HOURS_PER_DAY;
			tm->tm_mday--;
		}

		while (tm->tm_mday < 0)
		{
			if (dt1 < dt2)
			{
				tm->tm_mday += day_tab[isleap(tm1->tm_year)][tm1->tm_mon - 1];
				tm->tm_mon--;
			}
			else
			{
				tm->tm_mday += day_tab[isleap(tm2->tm_year)][tm2->tm_mon - 1];
				tm->tm_mon--;
			}
		}

		while (tm->tm_mon < 0)
		{
			tm->tm_mon += MONTHS_PER_YEAR;
			tm->tm_year--;
		}

		/*
		 * Note: we deliberately ignore any difference between tz1 and tz2.
		 */

		/* recover sign if necessary... */
		if (dt1 < dt2)
		{
			tm->tm_usec = -tm->tm_usec;
			tm->tm_sec = -tm->tm_sec;
			tm->tm_min = -tm->tm_min;
			tm->tm_hour = -tm->tm_hour;
			tm->tm_mday = -tm->tm_mday;
			tm->tm_mon = -tm->tm_mon;
			tm->tm_year = -tm->tm_year;
		}

		if (itm2interval(tm, result) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("interval out of range")));
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	PG_RETURN_INTERVAL_P(result);
}


/*----------------------------------------------------------
 *	Conversion operators.
 *---------------------------------------------------------*/


/* timestamp_bin()
 * Bin timestamp into specified interval.
 */
Datum
timestamp_bin(PG_FUNCTION_ARGS)
{
	Interval   *stride = PG_GETARG_INTERVAL_P(0);
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(1);
	Timestamp	origin = PG_GETARG_TIMESTAMP(2);
	Timestamp	result,
				stride_usecs,
				tm_diff,
				tm_modulo,
				tm_delta;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		PG_RETURN_TIMESTAMP(timestamp);

	if (TIMESTAMP_NOT_FINITE(origin))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("origin out of range")));

	if (INTERVAL_NOT_FINITE(stride))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamps cannot be binned into infinite intervals")));

	if (stride->month != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("timestamps cannot be binned into intervals containing months or years")));

	if (unlikely(pg_mul_s64_overflow(stride->day, USECS_PER_DAY, &stride_usecs)) ||
		unlikely(pg_add_s64_overflow(stride_usecs, stride->time, &stride_usecs)))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));

	if (stride_usecs <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("stride must be greater than zero")));

	if (unlikely(pg_sub_s64_overflow(timestamp, origin, &tm_diff)))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));

	/* These calculations cannot overflow */
	tm_modulo = tm_diff % stride_usecs;
	tm_delta = tm_diff - tm_modulo;
	result = origin + tm_delta;

	/*
	 * We want to round towards -infinity, not 0, when tm_diff is negative and
	 * not a multiple of stride_usecs.  This adjustment *can* cause overflow,
	 * since the result might now be out of the range origin .. timestamp.
	 */
	if (tm_modulo < 0)
	{
		if (unlikely(pg_sub_s64_overflow(result, stride_usecs, &result)) ||
			!IS_VALID_TIMESTAMP(result))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
	}

	PG_RETURN_TIMESTAMP(result);
}

/* timestamp_trunc()
 * Truncate timestamp to specified units.
 */
Datum
timestamp_trunc(PG_FUNCTION_ARGS)
{
	text	   *units = PG_GETARG_TEXT_PP(0);
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(1);
	Timestamp	result;
	int			type,
				val;
	char	   *lowunits;
	fsec_t		fsec;
	struct pg_tm tt,
			   *tm = &tt;

	lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
											VARSIZE_ANY_EXHDR(units),
											false);

	type = DecodeUnits(0, lowunits, &val);

	if (type == UNITS)
	{
		if (TIMESTAMP_NOT_FINITE(timestamp))
		{
			/*
			 * Errors thrown here for invalid units should exactly match those
			 * below, else there will be unexpected discrepancies between
			 * finite- and infinite-input cases.
			 */
			switch (val)
			{
				case DTK_WEEK:
				case DTK_MILLENNIUM:
				case DTK_CENTURY:
				case DTK_DECADE:
				case DTK_YEAR:
				case DTK_QUARTER:
				case DTK_MONTH:
				case DTK_DAY:
				case DTK_HOUR:
				case DTK_MINUTE:
				case DTK_SECOND:
				case DTK_MILLISEC:
				case DTK_MICROSEC:
					PG_RETURN_TIMESTAMP(timestamp);
					break;
				default:
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("unit \"%s\" not supported for type %s",
									lowunits, format_type_be(TIMESTAMPOID))));
					result = 0;
			}
		}

		if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL, NULL) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));

		switch (val)
		{
			case DTK_WEEK:
				{
					int			woy;

					woy = date2isoweek(tm->tm_year, tm->tm_mon, tm->tm_mday);

					/*
					 * If it is week 52/53 and the month is January, then the
					 * week must belong to the previous year. Also, some
					 * December dates belong to the next year.
					 */
					if (woy >= 52 && tm->tm_mon == 1)
						--tm->tm_year;
					if (woy <= 1 && tm->tm_mon == MONTHS_PER_YEAR)
						++tm->tm_year;
					isoweek2date(woy, &(tm->tm_year), &(tm->tm_mon), &(tm->tm_mday));
					tm->tm_hour = 0;
					tm->tm_min = 0;
					tm->tm_sec = 0;
					fsec = 0;
					break;
				}
			case DTK_MILLENNIUM:
				/* see comments in timestamptz_trunc */
				if (tm->tm_year > 0)
					tm->tm_year = ((tm->tm_year + 999) / 1000) * 1000 - 999;
				else
					tm->tm_year = -((999 - (tm->tm_year - 1)) / 1000) * 1000 + 1;
				/* FALL THRU */
			case DTK_CENTURY:
				/* see comments in timestamptz_trunc */
				if (tm->tm_year > 0)
					tm->tm_year = ((tm->tm_year + 99) / 100) * 100 - 99;
				else
					tm->tm_year = -((99 - (tm->tm_year - 1)) / 100) * 100 + 1;
				/* FALL THRU */
			case DTK_DECADE:
				/* see comments in timestamptz_trunc */
				if (val != DTK_MILLENNIUM && val != DTK_CENTURY)
				{
					if (tm->tm_year > 0)
						tm->tm_year = (tm->tm_year / 10) * 10;
					else
						tm->tm_year = -((8 - (tm->tm_year - 1)) / 10) * 10;
				}
				/* FALL THRU */
			case DTK_YEAR:
				tm->tm_mon = 1;
				/* FALL THRU */
			case DTK_QUARTER:
				tm->tm_mon = (3 * ((tm->tm_mon - 1) / 3)) + 1;
				/* FALL THRU */
			case DTK_MONTH:
				tm->tm_mday = 1;
				/* FALL THRU */
			case DTK_DAY:
				tm->tm_hour = 0;
				/* FALL THRU */
			case DTK_HOUR:
				tm->tm_min = 0;
				/* FALL THRU */
			case DTK_MINUTE:
				tm->tm_sec = 0;
				/* FALL THRU */
			case DTK_SECOND:
				fsec = 0;
				break;

			case DTK_MILLISEC:
				fsec = (fsec / 1000) * 1000;
				break;

			case DTK_MICROSEC:
				break;

			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("unit \"%s\" not supported for type %s",
								lowunits, format_type_be(TIMESTAMPOID))));
				result = 0;
		}

		if (tm2timestamp(tm, fsec, NULL, &result) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unit \"%s\" not recognized for type %s",
						lowunits, format_type_be(TIMESTAMPOID))));
		result = 0;
	}

	PG_RETURN_TIMESTAMP(result);
}

/* timestamptz_bin()
 * Bin timestamptz into specified interval using specified origin.
 */
Datum
timestamptz_bin(PG_FUNCTION_ARGS)
{
	Interval   *stride = PG_GETARG_INTERVAL_P(0);
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(1);
	TimestampTz origin = PG_GETARG_TIMESTAMPTZ(2);
	TimestampTz result,
				stride_usecs,
				tm_diff,
				tm_modulo,
				tm_delta;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		PG_RETURN_TIMESTAMPTZ(timestamp);

	if (TIMESTAMP_NOT_FINITE(origin))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("origin out of range")));

	if (INTERVAL_NOT_FINITE(stride))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamps cannot be binned into infinite intervals")));

	if (stride->month != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("timestamps cannot be binned into intervals containing months or years")));

	if (unlikely(pg_mul_s64_overflow(stride->day, USECS_PER_DAY, &stride_usecs)) ||
		unlikely(pg_add_s64_overflow(stride_usecs, stride->time, &stride_usecs)))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));

	if (stride_usecs <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("stride must be greater than zero")));

	if (unlikely(pg_sub_s64_overflow(timestamp, origin, &tm_diff)))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));

	/* These calculations cannot overflow */
	tm_modulo = tm_diff % stride_usecs;
	tm_delta = tm_diff - tm_modulo;
	result = origin + tm_delta;

	/*
	 * We want to round towards -infinity, not 0, when tm_diff is negative and
	 * not a multiple of stride_usecs.  This adjustment *can* cause overflow,
	 * since the result might now be out of the range origin .. timestamp.
	 */
	if (tm_modulo < 0)
	{
		if (unlikely(pg_sub_s64_overflow(result, stride_usecs, &result)) ||
			!IS_VALID_TIMESTAMP(result))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
	}

	PG_RETURN_TIMESTAMPTZ(result);
}

/*
 * Common code for timestamptz_trunc() and timestamptz_trunc_zone().
 *
 * tzp identifies the zone to truncate with respect to.  We assume
 * infinite timestamps have already been rejected.
 */
static TimestampTz
timestamptz_trunc_internal(text *units, TimestampTz timestamp, pg_tz *tzp)
{
	TimestampTz result;
	int			tz;
	int			type,
				val;
	bool		redotz = false;
	char	   *lowunits;
	fsec_t		fsec;
	struct pg_tm tt,
			   *tm = &tt;

	lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
											VARSIZE_ANY_EXHDR(units),
											false);

	type = DecodeUnits(0, lowunits, &val);

	if (type == UNITS)
	{
		if (TIMESTAMP_NOT_FINITE(timestamp))
		{
			/*
			 * Errors thrown here for invalid units should exactly match those
			 * below, else there will be unexpected discrepancies between
			 * finite- and infinite-input cases.
			 */
			switch (val)
			{
				case DTK_WEEK:
				case DTK_MILLENNIUM:
				case DTK_CENTURY:
				case DTK_DECADE:
				case DTK_YEAR:
				case DTK_QUARTER:
				case DTK_MONTH:
				case DTK_DAY:
				case DTK_HOUR:
				case DTK_MINUTE:
				case DTK_SECOND:
				case DTK_MILLISEC:
				case DTK_MICROSEC:
					PG_RETURN_TIMESTAMPTZ(timestamp);
					break;

				default:
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("unit \"%s\" not supported for type %s",
									lowunits, format_type_be(TIMESTAMPTZOID))));
					result = 0;
			}
		}

		if (timestamp2tm(timestamp, &tz, tm, &fsec, NULL, tzp) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));

		switch (val)
		{
			case DTK_WEEK:
				{
					int			woy;

					woy = date2isoweek(tm->tm_year, tm->tm_mon, tm->tm_mday);

					/*
					 * If it is week 52/53 and the month is January, then the
					 * week must belong to the previous year. Also, some
					 * December dates belong to the next year.
					 */
					if (woy >= 52 && tm->tm_mon == 1)
						--tm->tm_year;
					if (woy <= 1 && tm->tm_mon == MONTHS_PER_YEAR)
						++tm->tm_year;
					isoweek2date(woy, &(tm->tm_year), &(tm->tm_mon), &(tm->tm_mday));
					tm->tm_hour = 0;
					tm->tm_min = 0;
					tm->tm_sec = 0;
					fsec = 0;
					redotz = true;
					break;
				}
				/* one may consider DTK_THOUSAND and DTK_HUNDRED... */
			case DTK_MILLENNIUM:

				/*
				 * truncating to the millennium? what is this supposed to
				 * mean? let us put the first year of the millennium... i.e.
				 * -1000, 1, 1001, 2001...
				 */
				if (tm->tm_year > 0)
					tm->tm_year = ((tm->tm_year + 999) / 1000) * 1000 - 999;
				else
					tm->tm_year = -((999 - (tm->tm_year - 1)) / 1000) * 1000 + 1;
				/* FALL THRU */
			case DTK_CENTURY:
				/* truncating to the century? as above: -100, 1, 101... */
				if (tm->tm_year > 0)
					tm->tm_year = ((tm->tm_year + 99) / 100) * 100 - 99;
				else
					tm->tm_year = -((99 - (tm->tm_year - 1)) / 100) * 100 + 1;
				/* FALL THRU */
			case DTK_DECADE:

				/*
				 * truncating to the decade? first year of the decade. must
				 * not be applied if year was truncated before!
				 */
				if (val != DTK_MILLENNIUM && val != DTK_CENTURY)
				{
					if (tm->tm_year > 0)
						tm->tm_year = (tm->tm_year / 10) * 10;
					else
						tm->tm_year = -((8 - (tm->tm_year - 1)) / 10) * 10;
				}
				/* FALL THRU */
			case DTK_YEAR:
				tm->tm_mon = 1;
				/* FALL THRU */
			case DTK_QUARTER:
				tm->tm_mon = (3 * ((tm->tm_mon - 1) / 3)) + 1;
				/* FALL THRU */
			case DTK_MONTH:
				tm->tm_mday = 1;
				/* FALL THRU */
			case DTK_DAY:
				tm->tm_hour = 0;
				redotz = true;	/* for all cases >= DAY */
				/* FALL THRU */
			case DTK_HOUR:
				tm->tm_min = 0;
				/* FALL THRU */
			case DTK_MINUTE:
				tm->tm_sec = 0;
				/* FALL THRU */
			case DTK_SECOND:
				fsec = 0;
				break;
			case DTK_MILLISEC:
				fsec = (fsec / 1000) * 1000;
				break;
			case DTK_MICROSEC:
				break;

			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("unit \"%s\" not supported for type %s",
								lowunits, format_type_be(TIMESTAMPTZOID))));
				result = 0;
		}

		if (redotz)
			tz = DetermineTimeZoneOffset(tm, tzp);

		if (tm2timestamp(tm, fsec, &tz, &result) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unit \"%s\" not recognized for type %s",
						lowunits, format_type_be(TIMESTAMPTZOID))));
		result = 0;
	}

	return result;
}

/* timestamptz_trunc()
 * Truncate timestamptz to specified units in session timezone.
 */
Datum
timestamptz_trunc(PG_FUNCTION_ARGS)
{
	text	   *units = PG_GETARG_TEXT_PP(0);
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(1);
	TimestampTz result;

	result = timestamptz_trunc_internal(units, timestamp, session_timezone);

	PG_RETURN_TIMESTAMPTZ(result);
}

/* timestamptz_trunc_zone()
 * Truncate timestamptz to specified units in specified timezone.
 */
Datum
timestamptz_trunc_zone(PG_FUNCTION_ARGS)
{
	text	   *units = PG_GETARG_TEXT_PP(0);
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(1);
	text	   *zone = PG_GETARG_TEXT_PP(2);
	TimestampTz result;
	pg_tz	   *tzp;

	/*
	 * Look up the requested timezone.
	 */
	tzp = lookup_timezone(zone);

	result = timestamptz_trunc_internal(units, timestamp, tzp);

	PG_RETURN_TIMESTAMPTZ(result);
}

/* interval_trunc()
 * Extract specified field from interval.
 */
Datum
interval_trunc(PG_FUNCTION_ARGS)
{
	text	   *units = PG_GETARG_TEXT_PP(0);
	Interval   *interval = PG_GETARG_INTERVAL_P(1);
	Interval   *result;
	int			type,
				val;
	char	   *lowunits;
	struct pg_itm tt,
			   *tm = &tt;

	result = (Interval *) palloc(sizeof(Interval));

	lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
											VARSIZE_ANY_EXHDR(units),
											false);

	type = DecodeUnits(0, lowunits, &val);

	if (type == UNITS)
	{
		if (INTERVAL_NOT_FINITE(interval))
		{
			/*
			 * Errors thrown here for invalid units should exactly match those
			 * below, else there will be unexpected discrepancies between
			 * finite- and infinite-input cases.
			 */
			switch (val)
			{
				case DTK_MILLENNIUM:
				case DTK_CENTURY:
				case DTK_DECADE:
				case DTK_YEAR:
				case DTK_QUARTER:
				case DTK_MONTH:
				case DTK_DAY:
				case DTK_HOUR:
				case DTK_MINUTE:
				case DTK_SECOND:
				case DTK_MILLISEC:
				case DTK_MICROSEC:
					memcpy(result, interval, sizeof(Interval));
					PG_RETURN_INTERVAL_P(result);
					break;

				default:
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("unit \"%s\" not supported for type %s",
									lowunits, format_type_be(INTERVALOID)),
							 (val == DTK_WEEK) ? errdetail("Months usually have fractional weeks.") : 0));
					result = 0;
			}
		}

		interval2itm(*interval, tm);
		switch (val)
		{
			case DTK_MILLENNIUM:
				/* caution: C division may have negative remainder */
				tm->tm_year = (tm->tm_year / 1000) * 1000;
				/* FALL THRU */
			case DTK_CENTURY:
				/* caution: C division may have negative remainder */
				tm->tm_year = (tm->tm_year / 100) * 100;
				/* FALL THRU */
			case DTK_DECADE:
				/* caution: C division may have negative remainder */
				tm->tm_year = (tm->tm_year / 10) * 10;
				/* FALL THRU */
			case DTK_YEAR:
				tm->tm_mon = 0;
				/* FALL THRU */
			case DTK_QUARTER:
				tm->tm_mon = 3 * (tm->tm_mon / 3);
				/* FALL THRU */
			case DTK_MONTH:
				tm->tm_mday = 0;
				/* FALL THRU */
			case DTK_DAY:
				tm->tm_hour = 0;
				/* FALL THRU */
			case DTK_HOUR:
				tm->tm_min = 0;
				/* FALL THRU */
			case DTK_MINUTE:
				tm->tm_sec = 0;
				/* FALL THRU */
			case DTK_SECOND:
				tm->tm_usec = 0;
				break;
			case DTK_MILLISEC:
				tm->tm_usec = (tm->tm_usec / 1000) * 1000;
				break;
			case DTK_MICROSEC:
				break;

			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("unit \"%s\" not supported for type %s",
								lowunits, format_type_be(INTERVALOID)),
						 (val == DTK_WEEK) ? errdetail("Months usually have fractional weeks.") : 0));
		}

		if (itm2interval(tm, result) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("interval out of range")));
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unit \"%s\" not recognized for type %s",
						lowunits, format_type_be(INTERVALOID))));
	}

	PG_RETURN_INTERVAL_P(result);
}

/* isoweek2j()
 *
 *	Return the Julian day which corresponds to the first day (Monday) of the given ISO 8601 year and week.
 *	Julian days are used to convert between ISO week dates and Gregorian dates.
 *
 *	XXX: This function has integer overflow hazards, but restructuring it to
 *	work with the soft-error handling that its callers do is likely more
 *	trouble than it's worth.
 */
int
isoweek2j(int year, int week)
{
	int			day0,
				day4;

	/* fourth day of current year */
	day4 = date2j(year, 1, 4);

	/* day0 == offset to first day of week (Monday) */
	day0 = j2day(day4 - 1);

	return ((week - 1) * 7) + (day4 - day0);
}

/* isoweek2date()
 * Convert ISO week of year number to date.
 * The year field must be specified with the ISO year!
 * karel 2000/08/07
 */
void
isoweek2date(int woy, int *year, int *mon, int *mday)
{
	j2date(isoweek2j(*year, woy), year, mon, mday);
}

/* isoweekdate2date()
 *
 *	Convert an ISO 8601 week date (ISO year, ISO week) into a Gregorian date.
 *	Gregorian day of week sent so weekday strings can be supplied.
 *	Populates year, mon, and mday with the correct Gregorian values.
 *	year must be passed in as the ISO year.
 */
void
isoweekdate2date(int isoweek, int wday, int *year, int *mon, int *mday)
{
	int			jday;

	jday = isoweek2j(*year, isoweek);
	/* convert Gregorian week start (Sunday=1) to ISO week start (Monday=1) */
	if (wday > 1)
		jday += wday - 2;
	else
		jday += 6;
	j2date(jday, year, mon, mday);
}

/* date2isoweek()
 *
 *	Returns ISO week number of year.
 */
int
date2isoweek(int year, int mon, int mday)
{
	float8		result;
	int			day0,
				day4,
				dayn;

	/* current day */
	dayn = date2j(year, mon, mday);

	/* fourth day of current year */
	day4 = date2j(year, 1, 4);

	/* day0 == offset to first day of week (Monday) */
	day0 = j2day(day4 - 1);

	/*
	 * We need the first week containing a Thursday, otherwise this day falls
	 * into the previous year for purposes of counting weeks
	 */
	if (dayn < day4 - day0)
	{
		day4 = date2j(year - 1, 1, 4);

		/* day0 == offset to first day of week (Monday) */
		day0 = j2day(day4 - 1);
	}

	result = (dayn - (day4 - day0)) / 7 + 1;

	/*
	 * Sometimes the last few days in a year will fall into the first week of
	 * the next year, so check for this.
	 */
	if (result >= 52)
	{
		day4 = date2j(year + 1, 1, 4);

		/* day0 == offset to first day of week (Monday) */
		day0 = j2day(day4 - 1);

		if (dayn >= day4 - day0)
			result = (dayn - (day4 - day0)) / 7 + 1;
	}

	return (int) result;
}


/* date2isoyear()
 *
 *	Returns ISO 8601 year number.
 *	Note: zero or negative results follow the year-zero-exists convention.
 */
int
date2isoyear(int year, int mon, int mday)
{
	float8		result;
	int			day0,
				day4,
				dayn;

	/* current day */
	dayn = date2j(year, mon, mday);

	/* fourth day of current year */
	day4 = date2j(year, 1, 4);

	/* day0 == offset to first day of week (Monday) */
	day0 = j2day(day4 - 1);

	/*
	 * We need the first week containing a Thursday, otherwise this day falls
	 * into the previous year for purposes of counting weeks
	 */
	if (dayn < day4 - day0)
	{
		day4 = date2j(year - 1, 1, 4);

		/* day0 == offset to first day of week (Monday) */
		day0 = j2day(day4 - 1);

		year--;
	}

	result = (dayn - (day4 - day0)) / 7 + 1;

	/*
	 * Sometimes the last few days in a year will fall into the first week of
	 * the next year, so check for this.
	 */
	if (result >= 52)
	{
		day4 = date2j(year + 1, 1, 4);

		/* day0 == offset to first day of week (Monday) */
		day0 = j2day(day4 - 1);

		if (dayn >= day4 - day0)
			year++;
	}

	return year;
}


/* date2isoyearday()
 *
 *	Returns the ISO 8601 day-of-year, given a Gregorian year, month and day.
 *	Possible return values are 1 through 371 (364 in non-leap years).
 */
int
date2isoyearday(int year, int mon, int mday)
{
	return date2j(year, mon, mday) - isoweek2j(date2isoyear(year, mon, mday), 1) + 1;
}

/*
 * NonFiniteTimestampTzPart
 *
 *	Used by timestamp_part and timestamptz_part when extracting from infinite
 *	timestamp[tz].  Returns +/-Infinity if that is the appropriate result,
 *	otherwise returns zero (which should be taken as meaning to return NULL).
 *
 *	Errors thrown here for invalid units should exactly match those that
 *	would be thrown in the calling functions, else there will be unexpected
 *	discrepancies between finite- and infinite-input cases.
 */
static float8
NonFiniteTimestampTzPart(int type, int unit, char *lowunits,
						 bool isNegative, bool isTz)
{
	if ((type != UNITS) && (type != RESERV))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unit \"%s\" not recognized for type %s",
						lowunits,
						format_type_be(isTz ? TIMESTAMPTZOID : TIMESTAMPOID))));

	switch (unit)
	{
			/* Oscillating units */
		case DTK_MICROSEC:
		case DTK_MILLISEC:
		case DTK_SECOND:
		case DTK_MINUTE:
		case DTK_HOUR:
		case DTK_DAY:
		case DTK_MONTH:
		case DTK_QUARTER:
		case DTK_WEEK:
		case DTK_DOW:
		case DTK_ISODOW:
		case DTK_DOY:
		case DTK_TZ:
		case DTK_TZ_MINUTE:
		case DTK_TZ_HOUR:
			return 0.0;

			/* Monotonically-increasing units */
		case DTK_YEAR:
		case DTK_DECADE:
		case DTK_CENTURY:
		case DTK_MILLENNIUM:
		case DTK_JULIAN:
		case DTK_ISOYEAR:
		case DTK_EPOCH:
			if (isNegative)
				return -get_float8_infinity();
			else
				return get_float8_infinity();

		default:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("unit \"%s\" not supported for type %s",
							lowunits,
							format_type_be(isTz ? TIMESTAMPTZOID : TIMESTAMPOID))));
			return 0.0;			/* keep compiler quiet */
	}
}

/* timestamp_part() and extract_timestamp()
 * Extract specified field from timestamp.
 */
static Datum
timestamp_part_common(PG_FUNCTION_ARGS, bool retnumeric)
{
	text	   *units = PG_GETARG_TEXT_PP(0);
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(1);
	int64		intresult;
	Timestamp	epoch;
	int			type,
				val;
	char	   *lowunits;
	fsec_t		fsec;
	struct pg_tm tt,
			   *tm = &tt;

	lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
											VARSIZE_ANY_EXHDR(units),
											false);

	type = DecodeUnits(0, lowunits, &val);
	if (type == UNKNOWN_FIELD)
		type = DecodeSpecial(0, lowunits, &val);

	if (TIMESTAMP_NOT_FINITE(timestamp))
	{
		double		r = NonFiniteTimestampTzPart(type, val, lowunits,
												 TIMESTAMP_IS_NOBEGIN(timestamp),
												 false);

		if (r != 0.0)
		{
			if (retnumeric)
			{
				if (r < 0)
					return DirectFunctionCall3(numeric_in,
											   CStringGetDatum("-Infinity"),
											   ObjectIdGetDatum(InvalidOid),
											   Int32GetDatum(-1));
				else if (r > 0)
					return DirectFunctionCall3(numeric_in,
											   CStringGetDatum("Infinity"),
											   ObjectIdGetDatum(InvalidOid),
											   Int32GetDatum(-1));
			}
			else
				PG_RETURN_FLOAT8(r);
		}
		else
			PG_RETURN_NULL();
	}

	if (type == UNITS)
	{
		if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL, NULL) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));

		switch (val)
		{
			case DTK_MICROSEC:
				intresult = tm->tm_sec * INT64CONST(1000000) + fsec;
				break;

			case DTK_MILLISEC:
				if (retnumeric)
					/*---
					 * tm->tm_sec * 1000 + fsec / 1000
					 * = (tm->tm_sec * 1'000'000 + fsec) / 1000
					 */
					PG_RETURN_NUMERIC(int64_div_fast_to_numeric(tm->tm_sec * INT64CONST(1000000) + fsec, 3));
				else
					PG_RETURN_FLOAT8(tm->tm_sec * 1000.0 + fsec / 1000.0);
				break;

			case DTK_SECOND:
				if (retnumeric)
					/*---
					 * tm->tm_sec + fsec / 1'000'000
					 * = (tm->tm_sec * 1'000'000 + fsec) / 1'000'000
					 */
					PG_RETURN_NUMERIC(int64_div_fast_to_numeric(tm->tm_sec * INT64CONST(1000000) + fsec, 6));
				else
					PG_RETURN_FLOAT8(tm->tm_sec + fsec / 1000000.0);
				break;

			case DTK_MINUTE:
				intresult = tm->tm_min;
				break;

			case DTK_HOUR:
				intresult = tm->tm_hour;
				break;

			case DTK_DAY:
				intresult = tm->tm_mday;
				break;

			case DTK_MONTH:
				intresult = tm->tm_mon;
				break;

			case DTK_QUARTER:
				intresult = (tm->tm_mon - 1) / 3 + 1;
				break;

			case DTK_WEEK:
				intresult = date2isoweek(tm->tm_year, tm->tm_mon, tm->tm_mday);
				break;

			case DTK_YEAR:
				if (tm->tm_year > 0)
					intresult = tm->tm_year;
				else
					/* there is no year 0, just 1 BC and 1 AD */
					intresult = tm->tm_year - 1;
				break;

			case DTK_DECADE:

				/*
				 * what is a decade wrt dates? let us assume that decade 199
				 * is 1990 thru 1999... decade 0 starts on year 1 BC, and -1
				 * is 11 BC thru 2 BC...
				 */
				if (tm->tm_year >= 0)
					intresult = tm->tm_year / 10;
				else
					intresult = -((8 - (tm->tm_year - 1)) / 10);
				break;

			case DTK_CENTURY:

				/* ----
				 * centuries AD, c>0: year in [ (c-1)* 100 + 1 : c*100 ]
				 * centuries BC, c<0: year in [ c*100 : (c+1) * 100 - 1]
				 * there is no number 0 century.
				 * ----
				 */
				if (tm->tm_year > 0)
					intresult = (tm->tm_year + 99) / 100;
				else
					/* caution: C division may have negative remainder */
					intresult = -((99 - (tm->tm_year - 1)) / 100);
				break;

			case DTK_MILLENNIUM:
				/* see comments above. */
				if (tm->tm_year > 0)
					intresult = (tm->tm_year + 999) / 1000;
				else
					intresult = -((999 - (tm->tm_year - 1)) / 1000);
				break;

			case DTK_JULIAN:
				if (retnumeric)
					PG_RETURN_NUMERIC(numeric_add_opt_error(int64_to_numeric(date2j(tm->tm_year, tm->tm_mon, tm->tm_mday)),
															numeric_div_opt_error(int64_to_numeric(((((tm->tm_hour * MINS_PER_HOUR) + tm->tm_min) * SECS_PER_MINUTE) + tm->tm_sec) * INT64CONST(1000000) + fsec),
																				  int64_to_numeric(SECS_PER_DAY * INT64CONST(1000000)),
																				  NULL),
															NULL));
				else
					PG_RETURN_FLOAT8(date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) +
									 ((((tm->tm_hour * MINS_PER_HOUR) + tm->tm_min) * SECS_PER_MINUTE) +
									  tm->tm_sec + (fsec / 1000000.0)) / (double) SECS_PER_DAY);
				break;

			case DTK_ISOYEAR:
				intresult = date2isoyear(tm->tm_year, tm->tm_mon, tm->tm_mday);
				/* Adjust BC years */
				if (intresult <= 0)
					intresult -= 1;
				break;

			case DTK_DOW:
			case DTK_ISODOW:
				intresult = j2day(date2j(tm->tm_year, tm->tm_mon, tm->tm_mday));
				if (val == DTK_ISODOW && intresult == 0)
					intresult = 7;
				break;

			case DTK_DOY:
				intresult = (date2j(tm->tm_year, tm->tm_mon, tm->tm_mday)
							 - date2j(tm->tm_year, 1, 1) + 1);
				break;

			case DTK_TZ:
			case DTK_TZ_MINUTE:
			case DTK_TZ_HOUR:
			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("unit \"%s\" not supported for type %s",
								lowunits, format_type_be(TIMESTAMPOID))));
				intresult = 0;
		}
	}
	else if (type == RESERV)
	{
		switch (val)
		{
			case DTK_EPOCH:
				epoch = SetEpochTimestamp();
				/* (timestamp - epoch) / 1000000 */
				if (retnumeric)
				{
					Numeric		result;

					if (timestamp < (PG_INT64_MAX + epoch))
						result = int64_div_fast_to_numeric(timestamp - epoch, 6);
					else
					{
						result = numeric_div_opt_error(numeric_sub_opt_error(int64_to_numeric(timestamp),
																			 int64_to_numeric(epoch),
																			 NULL),
													   int64_to_numeric(1000000),
													   NULL);
						result = DatumGetNumeric(DirectFunctionCall2(numeric_round,
																	 NumericGetDatum(result),
																	 Int32GetDatum(6)));
					}
					PG_RETURN_NUMERIC(result);
				}
				else
				{
					float8		result;

					/* try to avoid precision loss in subtraction */
					if (timestamp < (PG_INT64_MAX + epoch))
						result = (timestamp - epoch) / 1000000.0;
					else
						result = ((float8) timestamp - epoch) / 1000000.0;
					PG_RETURN_FLOAT8(result);
				}
				break;

			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("unit \"%s\" not supported for type %s",
								lowunits, format_type_be(TIMESTAMPOID))));
				intresult = 0;
		}
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unit \"%s\" not recognized for type %s",
						lowunits, format_type_be(TIMESTAMPOID))));
		intresult = 0;
	}

	if (retnumeric)
		PG_RETURN_NUMERIC(int64_to_numeric(intresult));
	else
		PG_RETURN_FLOAT8(intresult);
}

Datum
timestamp_part(PG_FUNCTION_ARGS)
{
	return timestamp_part_common(fcinfo, false);
}

Datum
extract_timestamp(PG_FUNCTION_ARGS)
{
	return timestamp_part_common(fcinfo, true);
}

/* timestamptz_part() and extract_timestamptz()
 * Extract specified field from timestamp with time zone.
 */
static Datum
timestamptz_part_common(PG_FUNCTION_ARGS, bool retnumeric)
{
	text	   *units = PG_GETARG_TEXT_PP(0);
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(1);
	int64		intresult;
	Timestamp	epoch;
	int			tz;
	int			type,
				val;
	char	   *lowunits;
	fsec_t		fsec;
	struct pg_tm tt,
			   *tm = &tt;

	lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
											VARSIZE_ANY_EXHDR(units),
											false);

	type = DecodeUnits(0, lowunits, &val);
	if (type == UNKNOWN_FIELD)
		type = DecodeSpecial(0, lowunits, &val);

	if (TIMESTAMP_NOT_FINITE(timestamp))
	{
		double		r = NonFiniteTimestampTzPart(type, val, lowunits,
												 TIMESTAMP_IS_NOBEGIN(timestamp),
												 true);

		if (r != 0.0)
		{
			if (retnumeric)
			{
				if (r < 0)
					return DirectFunctionCall3(numeric_in,
											   CStringGetDatum("-Infinity"),
											   ObjectIdGetDatum(InvalidOid),
											   Int32GetDatum(-1));
				else if (r > 0)
					return DirectFunctionCall3(numeric_in,
											   CStringGetDatum("Infinity"),
											   ObjectIdGetDatum(InvalidOid),
											   Int32GetDatum(-1));
			}
			else
				PG_RETURN_FLOAT8(r);
		}
		else
			PG_RETURN_NULL();
	}

	if (type == UNITS)
	{
		if (timestamp2tm(timestamp, &tz, tm, &fsec, NULL, NULL) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));

		switch (val)
		{
			case DTK_TZ:
				intresult = -tz;
				break;

			case DTK_TZ_MINUTE:
				intresult = (-tz / SECS_PER_MINUTE) % MINS_PER_HOUR;
				break;

			case DTK_TZ_HOUR:
				intresult = -tz / SECS_PER_HOUR;
				break;

			case DTK_MICROSEC:
				intresult = tm->tm_sec * INT64CONST(1000000) + fsec;
				break;

			case DTK_MILLISEC:
				if (retnumeric)
					/*---
					 * tm->tm_sec * 1000 + fsec / 1000
					 * = (tm->tm_sec * 1'000'000 + fsec) / 1000
					 */
					PG_RETURN_NUMERIC(int64_div_fast_to_numeric(tm->tm_sec * INT64CONST(1000000) + fsec, 3));
				else
					PG_RETURN_FLOAT8(tm->tm_sec * 1000.0 + fsec / 1000.0);
				break;

			case DTK_SECOND:
				if (retnumeric)
					/*---
					 * tm->tm_sec + fsec / 1'000'000
					 * = (tm->tm_sec * 1'000'000 + fsec) / 1'000'000
					 */
					PG_RETURN_NUMERIC(int64_div_fast_to_numeric(tm->tm_sec * INT64CONST(1000000) + fsec, 6));
				else
					PG_RETURN_FLOAT8(tm->tm_sec + fsec / 1000000.0);
				break;

			case DTK_MINUTE:
				intresult = tm->tm_min;
				break;

			case DTK_HOUR:
				intresult = tm->tm_hour;
				break;

			case DTK_DAY:
				intresult = tm->tm_mday;
				break;

			case DTK_MONTH:
				intresult = tm->tm_mon;
				break;

			case DTK_QUARTER:
				intresult = (tm->tm_mon - 1) / 3 + 1;
				break;

			case DTK_WEEK:
				intresult = date2isoweek(tm->tm_year, tm->tm_mon, tm->tm_mday);
				break;

			case DTK_YEAR:
				if (tm->tm_year > 0)
					intresult = tm->tm_year;
				else
					/* there is no year 0, just 1 BC and 1 AD */
					intresult = tm->tm_year - 1;
				break;

			case DTK_DECADE:
				/* see comments in timestamp_part */
				if (tm->tm_year > 0)
					intresult = tm->tm_year / 10;
				else
					intresult = -((8 - (tm->tm_year - 1)) / 10);
				break;

			case DTK_CENTURY:
				/* see comments in timestamp_part */
				if (tm->tm_year > 0)
					intresult = (tm->tm_year + 99) / 100;
				else
					intresult = -((99 - (tm->tm_year - 1)) / 100);
				break;

			case DTK_MILLENNIUM:
				/* see comments in timestamp_part */
				if (tm->tm_year > 0)
					intresult = (tm->tm_year + 999) / 1000;
				else
					intresult = -((999 - (tm->tm_year - 1)) / 1000);
				break;

			case DTK_JULIAN:
				if (retnumeric)
					PG_RETURN_NUMERIC(numeric_add_opt_error(int64_to_numeric(date2j(tm->tm_year, tm->tm_mon, tm->tm_mday)),
															numeric_div_opt_error(int64_to_numeric(((((tm->tm_hour * MINS_PER_HOUR) + tm->tm_min) * SECS_PER_MINUTE) + tm->tm_sec) * INT64CONST(1000000) + fsec),
																				  int64_to_numeric(SECS_PER_DAY * INT64CONST(1000000)),
																				  NULL),
															NULL));
				else
					PG_RETURN_FLOAT8(date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) +
									 ((((tm->tm_hour * MINS_PER_HOUR) + tm->tm_min) * SECS_PER_MINUTE) +
									  tm->tm_sec + (fsec / 1000000.0)) / (double) SECS_PER_DAY);
				break;

			case DTK_ISOYEAR:
				intresult = date2isoyear(tm->tm_year, tm->tm_mon, tm->tm_mday);
				/* Adjust BC years */
				if (intresult <= 0)
					intresult -= 1;
				break;

			case DTK_DOW:
			case DTK_ISODOW:
				intresult = j2day(date2j(tm->tm_year, tm->tm_mon, tm->tm_mday));
				if (val == DTK_ISODOW && intresult == 0)
					intresult = 7;
				break;

			case DTK_DOY:
				intresult = (date2j(tm->tm_year, tm->tm_mon, tm->tm_mday)
							 - date2j(tm->tm_year, 1, 1) + 1);
				break;

			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("unit \"%s\" not supported for type %s",
								lowunits, format_type_be(TIMESTAMPTZOID))));
				intresult = 0;
		}
	}
	else if (type == RESERV)
	{
		switch (val)
		{
			case DTK_EPOCH:
				epoch = SetEpochTimestamp();
				/* (timestamp - epoch) / 1000000 */
				if (retnumeric)
				{
					Numeric		result;

					if (timestamp < (PG_INT64_MAX + epoch))
						result = int64_div_fast_to_numeric(timestamp - epoch, 6);
					else
					{
						result = numeric_div_opt_error(numeric_sub_opt_error(int64_to_numeric(timestamp),
																			 int64_to_numeric(epoch),
																			 NULL),
													   int64_to_numeric(1000000),
													   NULL);
						result = DatumGetNumeric(DirectFunctionCall2(numeric_round,
																	 NumericGetDatum(result),
																	 Int32GetDatum(6)));
					}
					PG_RETURN_NUMERIC(result);
				}
				else
				{
					float8		result;

					/* try to avoid precision loss in subtraction */
					if (timestamp < (PG_INT64_MAX + epoch))
						result = (timestamp - epoch) / 1000000.0;
					else
						result = ((float8) timestamp - epoch) / 1000000.0;
					PG_RETURN_FLOAT8(result);
				}
				break;

			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("unit \"%s\" not supported for type %s",
								lowunits, format_type_be(TIMESTAMPTZOID))));
				intresult = 0;
		}
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unit \"%s\" not recognized for type %s",
						lowunits, format_type_be(TIMESTAMPTZOID))));

		intresult = 0;
	}

	if (retnumeric)
		PG_RETURN_NUMERIC(int64_to_numeric(intresult));
	else
		PG_RETURN_FLOAT8(intresult);
}

Datum
timestamptz_part(PG_FUNCTION_ARGS)
{
	return timestamptz_part_common(fcinfo, false);
}

Datum
extract_timestamptz(PG_FUNCTION_ARGS)
{
	return timestamptz_part_common(fcinfo, true);
}

/*
 * NonFiniteIntervalPart
 *
 *	Used by interval_part when extracting from infinite interval.  Returns
 *	+/-Infinity if that is the appropriate result, otherwise returns zero
 *	(which should be taken as meaning to return NULL).
 *
 *	Errors thrown here for invalid units should exactly match those that
 *	would be thrown in the calling functions, else there will be unexpected
 *	discrepancies between finite- and infinite-input cases.
 */
static float8
NonFiniteIntervalPart(int type, int unit, char *lowunits, bool isNegative)
{
	if ((type != UNITS) && (type != RESERV))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unit \"%s\" not recognized for type %s",
						lowunits, format_type_be(INTERVALOID))));

	switch (unit)
	{
			/* Oscillating units */
		case DTK_MICROSEC:
		case DTK_MILLISEC:
		case DTK_SECOND:
		case DTK_MINUTE:
		case DTK_WEEK:
		case DTK_MONTH:
		case DTK_QUARTER:
			return 0.0;

			/* Monotonically-increasing units */
		case DTK_HOUR:
		case DTK_DAY:
		case DTK_YEAR:
		case DTK_DECADE:
		case DTK_CENTURY:
		case DTK_MILLENNIUM:
		case DTK_EPOCH:
			if (isNegative)
				return -get_float8_infinity();
			else
				return get_float8_infinity();

		default:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("unit \"%s\" not supported for type %s",
							lowunits, format_type_be(INTERVALOID))));
			return 0.0;			/* keep compiler quiet */
	}
}

/* interval_part() and extract_interval()
 * Extract specified field from interval.
 */
static Datum
interval_part_common(PG_FUNCTION_ARGS, bool retnumeric)
{
	text	   *units = PG_GETARG_TEXT_PP(0);
	Interval   *interval = PG_GETARG_INTERVAL_P(1);
	int64		intresult;
	int			type,
				val;
	char	   *lowunits;
	struct pg_itm tt,
			   *tm = &tt;

	lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
											VARSIZE_ANY_EXHDR(units),
											false);

	type = DecodeUnits(0, lowunits, &val);
	if (type == UNKNOWN_FIELD)
		type = DecodeSpecial(0, lowunits, &val);

	if (INTERVAL_NOT_FINITE(interval))
	{
		double		r = NonFiniteIntervalPart(type, val, lowunits,
											  INTERVAL_IS_NOBEGIN(interval));

		if (r != 0.0)
		{
			if (retnumeric)
			{
				if (r < 0)
					return DirectFunctionCall3(numeric_in,
											   CStringGetDatum("-Infinity"),
											   ObjectIdGetDatum(InvalidOid),
											   Int32GetDatum(-1));
				else if (r > 0)
					return DirectFunctionCall3(numeric_in,
											   CStringGetDatum("Infinity"),
											   ObjectIdGetDatum(InvalidOid),
											   Int32GetDatum(-1));
			}
			else
				PG_RETURN_FLOAT8(r);
		}
		else
			PG_RETURN_NULL();
	}

	if (type == UNITS)
	{
		interval2itm(*interval, tm);
		switch (val)
		{
			case DTK_MICROSEC:
				intresult = tm->tm_sec * INT64CONST(1000000) + tm->tm_usec;
				break;

			case DTK_MILLISEC:
				if (retnumeric)
					/*---
					 * tm->tm_sec * 1000 + fsec / 1000
					 * = (tm->tm_sec * 1'000'000 + fsec) / 1000
					 */
					PG_RETURN_NUMERIC(int64_div_fast_to_numeric(tm->tm_sec * INT64CONST(1000000) + tm->tm_usec, 3));
				else
					PG_RETURN_FLOAT8(tm->tm_sec * 1000.0 + tm->tm_usec / 1000.0);
				break;

			case DTK_SECOND:
				if (retnumeric)
					/*---
					 * tm->tm_sec + fsec / 1'000'000
					 * = (tm->tm_sec * 1'000'000 + fsec) / 1'000'000
					 */
					PG_RETURN_NUMERIC(int64_div_fast_to_numeric(tm->tm_sec * INT64CONST(1000000) + tm->tm_usec, 6));
				else
					PG_RETURN_FLOAT8(tm->tm_sec + tm->tm_usec / 1000000.0);
				break;

			case DTK_MINUTE:
				intresult = tm->tm_min;
				break;

			case DTK_HOUR:
				intresult = tm->tm_hour;
				break;

			case DTK_DAY:
				intresult = tm->tm_mday;
				break;

			case DTK_WEEK:
				intresult = tm->tm_mday / 7;
				break;

			case DTK_MONTH:
				intresult = tm->tm_mon;
				break;

			case DTK_QUARTER:

				/*
				 * We want to maintain the rule that a field extracted from a
				 * negative interval is the negative of the field's value for
				 * the sign-reversed interval.  The broken-down tm_year and
				 * tm_mon aren't very helpful for that, so work from
				 * interval->month.
				 */
				if (interval->month >= 0)
					intresult = (tm->tm_mon / 3) + 1;
				else
					intresult = -(((-interval->month % MONTHS_PER_YEAR) / 3) + 1);
				break;

			case DTK_YEAR:
				intresult = tm->tm_year;
				break;

			case DTK_DECADE:
				/* caution: C division may have negative remainder */
				intresult = tm->tm_year / 10;
				break;

			case DTK_CENTURY:
				/* caution: C division may have negative remainder */
				intresult = tm->tm_year / 100;
				break;

			case DTK_MILLENNIUM:
				/* caution: C division may have negative remainder */
				intresult = tm->tm_year / 1000;
				break;

			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("unit \"%s\" not supported for type %s",
								lowunits, format_type_be(INTERVALOID))));
				intresult = 0;
		}
	}
	else if (type == RESERV && val == DTK_EPOCH)
	{
		if (retnumeric)
		{
			Numeric		result;
			int64		secs_from_day_month;
			int64		val;

			/*
			 * To do this calculation in integer arithmetic even though
			 * DAYS_PER_YEAR is fractional, multiply everything by 4 and then
			 * divide by 4 again at the end.  This relies on DAYS_PER_YEAR
			 * being a multiple of 0.25 and on SECS_PER_DAY being a multiple
			 * of 4.
			 */
			secs_from_day_month = ((int64) (4 * DAYS_PER_YEAR) * (interval->month / MONTHS_PER_YEAR) +
								   (int64) (4 * DAYS_PER_MONTH) * (interval->month % MONTHS_PER_YEAR) +
								   (int64) 4 * interval->day) * (SECS_PER_DAY / 4);

			/*---
			 * result = secs_from_day_month + interval->time / 1'000'000
			 * = (secs_from_day_month * 1'000'000 + interval->time) / 1'000'000
			 */

			/*
			 * Try the computation inside int64; if it overflows, do it in
			 * numeric (slower).  This overflow happens around 10^9 days, so
			 * not common in practice.
			 */
			if (!pg_mul_s64_overflow(secs_from_day_month, 1000000, &val) &&
				!pg_add_s64_overflow(val, interval->time, &val))
				result = int64_div_fast_to_numeric(val, 6);
			else
				result =
					numeric_add_opt_error(int64_div_fast_to_numeric(interval->time, 6),
										  int64_to_numeric(secs_from_day_month),
										  NULL);

			PG_RETURN_NUMERIC(result);
		}
		else
		{
			float8		result;

			result = interval->time / 1000000.0;
			result += ((double) DAYS_PER_YEAR * SECS_PER_DAY) * (interval->month / MONTHS_PER_YEAR);
			result += ((double) DAYS_PER_MONTH * SECS_PER_DAY) * (interval->month % MONTHS_PER_YEAR);
			result += ((double) SECS_PER_DAY) * interval->day;

			PG_RETURN_FLOAT8(result);
		}
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unit \"%s\" not recognized for type %s",
						lowunits, format_type_be(INTERVALOID))));
		intresult = 0;
	}

	if (retnumeric)
		PG_RETURN_NUMERIC(int64_to_numeric(intresult));
	else
		PG_RETURN_FLOAT8(intresult);
}

Datum
interval_part(PG_FUNCTION_ARGS)
{
	return interval_part_common(fcinfo, false);
}

Datum
extract_interval(PG_FUNCTION_ARGS)
{
	return interval_part_common(fcinfo, true);
}


/*	timestamp_zone()
 *	Encode timestamp type with specified time zone.
 *	This function is just timestamp2timestamptz() except instead of
 *	shifting to the global timezone, we shift to the specified timezone.
 *	This is different from the other AT TIME ZONE cases because instead
 *	of shifting _to_ a new time zone, it sets the time to _be_ the
 *	specified timezone.
 */
Datum
timestamp_zone(PG_FUNCTION_ARGS)
{
	text	   *zone = PG_GETARG_TEXT_PP(0);
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(1);
	TimestampTz result;
	int			tz;
	char		tzname[TZ_STRLEN_MAX + 1];
	int			type,
				val;
	pg_tz	   *tzp;
	struct pg_tm tm;
	fsec_t		fsec;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		PG_RETURN_TIMESTAMPTZ(timestamp);

	/*
	 * Look up the requested timezone.
	 */
	text_to_cstring_buffer(zone, tzname, sizeof(tzname));

	type = DecodeTimezoneName(tzname, &val, &tzp);

	if (type == TZNAME_FIXED_OFFSET)
	{
		/* fixed-offset abbreviation */
		tz = val;
		result = dt2local(timestamp, tz);
	}
	else if (type == TZNAME_DYNTZ)
	{
		/* dynamic-offset abbreviation, resolve using specified time */
		if (timestamp2tm(timestamp, NULL, &tm, &fsec, NULL, tzp) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
		tz = -DetermineTimeZoneAbbrevOffset(&tm, tzname, tzp);
		result = dt2local(timestamp, tz);
	}
	else
	{
		/* full zone name, rotate to that zone */
		if (timestamp2tm(timestamp, NULL, &tm, &fsec, NULL, tzp) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
		tz = DetermineTimeZoneOffset(&tm, tzp);
		if (tm2timestamp(&tm, fsec, &tz, &result) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
	}

	if (!IS_VALID_TIMESTAMP(result))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	PG_RETURN_TIMESTAMPTZ(result);
}

/* timestamp_izone()
 * Encode timestamp type with specified time interval as time zone.
 */
Datum
timestamp_izone(PG_FUNCTION_ARGS)
{
	Interval   *zone = PG_GETARG_INTERVAL_P(0);
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(1);
	TimestampTz result;
	int			tz;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		PG_RETURN_TIMESTAMPTZ(timestamp);

	if (INTERVAL_NOT_FINITE(zone))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("interval time zone \"%s\" must be finite",
						DatumGetCString(DirectFunctionCall1(interval_out,
															PointerGetDatum(zone))))));

	if (zone->month != 0 || zone->day != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("interval time zone \"%s\" must not include months or days",
						DatumGetCString(DirectFunctionCall1(interval_out,
															PointerGetDatum(zone))))));

	tz = zone->time / USECS_PER_SEC;

	result = dt2local(timestamp, tz);

	if (!IS_VALID_TIMESTAMP(result))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	PG_RETURN_TIMESTAMPTZ(result);
}								/* timestamp_izone() */

/* TimestampTimestampTzRequiresRewrite()
 *
 * Returns false if the TimeZone GUC setting causes timestamp_timestamptz and
 * timestamptz_timestamp to be no-ops, where the return value has the same
 * bits as the argument.  Since project convention is to assume a GUC changes
 * no more often than STABLE functions change, the answer is valid that long.
 */
bool
TimestampTimestampTzRequiresRewrite(void)
{
	long		offset;

	if (pg_get_timezone_offset(session_timezone, &offset) && offset == 0)
		return false;
	return true;
}

/* timestamp_timestamptz()
 * Convert local timestamp to timestamp at GMT
 */
Datum
timestamp_timestamptz(PG_FUNCTION_ARGS)
{
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(0);

	PG_RETURN_TIMESTAMPTZ(timestamp2timestamptz(timestamp));
}

/*
 * Convert timestamp to timestamp with time zone.
 *
 * On successful conversion, *overflow is set to zero if it's not NULL.
 *
 * If the timestamp is finite but out of the valid range for timestamptz, then:
 * if overflow is NULL, we throw an out-of-range error.
 * if overflow is not NULL, we store +1 or -1 there to indicate the sign
 * of the overflow, and return the appropriate timestamptz infinity.
 */
TimestampTz
timestamp2timestamptz_opt_overflow(Timestamp timestamp, int *overflow)
{
	TimestampTz result;
	struct pg_tm tt,
			   *tm = &tt;
	fsec_t		fsec;
	int			tz;

	if (overflow)
		*overflow = 0;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		return timestamp;

	/* timestamp2tm should not fail on valid timestamps, but cope */
	if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL, NULL) == 0)
	{
		tz = DetermineTimeZoneOffset(tm, session_timezone);

		result = dt2local(timestamp, -tz);

		if (IS_VALID_TIMESTAMP(result))
			return result;
	}

	if (overflow)
	{
		if (timestamp < 0)
		{
			*overflow = -1;
			TIMESTAMP_NOBEGIN(result);
		}
		else
		{
			*overflow = 1;
			TIMESTAMP_NOEND(result);
		}
		return result;
	}

	ereport(ERROR,
			(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
			 errmsg("timestamp out of range")));

	return 0;
}

/*
 * Promote timestamp to timestamptz, throwing error for overflow.
 */
static TimestampTz
timestamp2timestamptz(Timestamp timestamp)
{
	return timestamp2timestamptz_opt_overflow(timestamp, NULL);
}

/* timestamptz_timestamp()
 * Convert timestamp at GMT to local timestamp
 */
Datum
timestamptz_timestamp(PG_FUNCTION_ARGS)
{
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(0);

	PG_RETURN_TIMESTAMP(timestamptz2timestamp(timestamp));
}

/*
 * Convert timestamptz to timestamp, throwing error for overflow.
 */
static Timestamp
timestamptz2timestamp(TimestampTz timestamp)
{
	return timestamptz2timestamp_opt_overflow(timestamp, NULL);
}

/*
 * Convert timestamp with time zone to timestamp.
 *
 * On successful conversion, *overflow is set to zero if it's not NULL.
 *
 * If the timestamptz is finite but out of the valid range for timestamp, then:
 * if overflow is NULL, we throw an out-of-range error.
 * if overflow is not NULL, we store +1 or -1 there to indicate the sign
 * of the overflow, and return the appropriate timestamp infinity.
 */
Timestamp
timestamptz2timestamp_opt_overflow(TimestampTz timestamp, int *overflow)
{
	Timestamp	result;
	struct pg_tm tt,
			   *tm = &tt;
	fsec_t		fsec;
	int			tz;

	if (overflow)
		*overflow = 0;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		result = timestamp;
	else
	{
		if (timestamp2tm(timestamp, &tz, tm, &fsec, NULL, NULL) != 0)
		{
			if (overflow)
			{
				if (timestamp < 0)
				{
					*overflow = -1;
					TIMESTAMP_NOBEGIN(result);
				}
				else
				{
					*overflow = 1;
					TIMESTAMP_NOEND(result);
				}
				return result;
			}
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
		}
		if (tm2timestamp(tm, fsec, NULL, &result) != 0)
		{
			if (overflow)
			{
				if (timestamp < 0)
				{
					*overflow = -1;
					TIMESTAMP_NOBEGIN(result);
				}
				else
				{
					*overflow = 1;
					TIMESTAMP_NOEND(result);
				}
				return result;
			}
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
		}
	}
	return result;
}

/* timestamptz_zone()
 * Evaluate timestamp with time zone type at the specified time zone.
 * Returns a timestamp without time zone.
 */
Datum
timestamptz_zone(PG_FUNCTION_ARGS)
{
	text	   *zone = PG_GETARG_TEXT_PP(0);
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(1);
	Timestamp	result;
	int			tz;
	char		tzname[TZ_STRLEN_MAX + 1];
	int			type,
				val;
	pg_tz	   *tzp;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		PG_RETURN_TIMESTAMP(timestamp);

	/*
	 * Look up the requested timezone.
	 */
	text_to_cstring_buffer(zone, tzname, sizeof(tzname));

	type = DecodeTimezoneName(tzname, &val, &tzp);

	if (type == TZNAME_FIXED_OFFSET)
	{
		/* fixed-offset abbreviation */
		tz = -val;
		result = dt2local(timestamp, tz);
	}
	else if (type == TZNAME_DYNTZ)
	{
		/* dynamic-offset abbreviation, resolve using specified time */
		int			isdst;

		tz = DetermineTimeZoneAbbrevOffsetTS(timestamp, tzname, tzp, &isdst);
		result = dt2local(timestamp, tz);
	}
	else
	{
		/* full zone name, rotate from that zone */
		struct pg_tm tm;
		fsec_t		fsec;

		if (timestamp2tm(timestamp, &tz, &tm, &fsec, NULL, tzp) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
		if (tm2timestamp(&tm, fsec, NULL, &result) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
	}

	if (!IS_VALID_TIMESTAMP(result))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	PG_RETURN_TIMESTAMP(result);
}

/* timestamptz_izone()
 * Encode timestamp with time zone type with specified time interval as time zone.
 * Returns a timestamp without time zone.
 */
Datum
timestamptz_izone(PG_FUNCTION_ARGS)
{
	Interval   *zone = PG_GETARG_INTERVAL_P(0);
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(1);
	Timestamp	result;
	int			tz;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		PG_RETURN_TIMESTAMP(timestamp);

	if (INTERVAL_NOT_FINITE(zone))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("interval time zone \"%s\" must be finite",
						DatumGetCString(DirectFunctionCall1(interval_out,
															PointerGetDatum(zone))))));

	if (zone->month != 0 || zone->day != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("interval time zone \"%s\" must not include months or days",
						DatumGetCString(DirectFunctionCall1(interval_out,
															PointerGetDatum(zone))))));

	tz = -(zone->time / USECS_PER_SEC);

	result = dt2local(timestamp, tz);

	if (!IS_VALID_TIMESTAMP(result))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	PG_RETURN_TIMESTAMP(result);
}

/* generate_series_timestamp()
 * Generate the set of timestamps from start to finish by step
 */
Datum
generate_series_timestamp(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	generate_series_timestamp_fctx *fctx;
	Timestamp	result;

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		Timestamp	start = PG_GETARG_TIMESTAMP(0);
		Timestamp	finish = PG_GETARG_TIMESTAMP(1);
		Interval   *step = PG_GETARG_INTERVAL_P(2);
		MemoryContext oldcontext;

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * switch to memory context appropriate for multiple function calls
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* allocate memory for user context */
		fctx = (generate_series_timestamp_fctx *)
			palloc(sizeof(generate_series_timestamp_fctx));

		/*
		 * Use fctx to keep state from call to call. Seed current with the
		 * original start value
		 */
		fctx->current = start;
		fctx->finish = finish;
		fctx->step = *step;

		/* Determine sign of the interval */
		fctx->step_sign = interval_sign(&fctx->step);

		if (fctx->step_sign == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("step size cannot equal zero")));

		if (INTERVAL_NOT_FINITE((&fctx->step)))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("step size cannot be infinite")));

		funcctx->user_fctx = fctx;
		MemoryContextSwitchTo(oldcontext);
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();

	/*
	 * get the saved state and use current as the result for this iteration
	 */
	fctx = funcctx->user_fctx;
	result = fctx->current;

	if (fctx->step_sign > 0 ?
		timestamp_cmp_internal(result, fctx->finish) <= 0 :
		timestamp_cmp_internal(result, fctx->finish) >= 0)
	{
		/* increment current in preparation for next iteration */
		fctx->current = DatumGetTimestamp(DirectFunctionCall2(timestamp_pl_interval,
															  TimestampGetDatum(fctx->current),
															  PointerGetDatum(&fctx->step)));

		/* do when there is more left to send */
		SRF_RETURN_NEXT(funcctx, TimestampGetDatum(result));
	}
	else
	{
		/* do when there is no more left */
		SRF_RETURN_DONE(funcctx);
	}
}

/* generate_series_timestamptz()
 * Generate the set of timestamps from start to finish by step,
 * doing arithmetic in the specified or session timezone.
 */
static Datum
generate_series_timestamptz_internal(FunctionCallInfo fcinfo)
{
	FuncCallContext *funcctx;
	generate_series_timestamptz_fctx *fctx;
	TimestampTz result;

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		TimestampTz start = PG_GETARG_TIMESTAMPTZ(0);
		TimestampTz finish = PG_GETARG_TIMESTAMPTZ(1);
		Interval   *step = PG_GETARG_INTERVAL_P(2);
		text	   *zone = (PG_NARGS() == 4) ? PG_GETARG_TEXT_PP(3) : NULL;
		MemoryContext oldcontext;

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * switch to memory context appropriate for multiple function calls
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* allocate memory for user context */
		fctx = (generate_series_timestamptz_fctx *)
			palloc(sizeof(generate_series_timestamptz_fctx));

		/*
		 * Use fctx to keep state from call to call. Seed current with the
		 * original start value
		 */
		fctx->current = start;
		fctx->finish = finish;
		fctx->step = *step;
		fctx->attimezone = zone ? lookup_timezone(zone) : session_timezone;

		/* Determine sign of the interval */
		fctx->step_sign = interval_sign(&fctx->step);

		if (fctx->step_sign == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("step size cannot equal zero")));

		if (INTERVAL_NOT_FINITE((&fctx->step)))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("step size cannot be infinite")));

		funcctx->user_fctx = fctx;
		MemoryContextSwitchTo(oldcontext);
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();

	/*
	 * get the saved state and use current as the result for this iteration
	 */
	fctx = funcctx->user_fctx;
	result = fctx->current;

	if (fctx->step_sign > 0 ?
		timestamp_cmp_internal(result, fctx->finish) <= 0 :
		timestamp_cmp_internal(result, fctx->finish) >= 0)
	{
		/* increment current in preparation for next iteration */
		fctx->current = timestamptz_pl_interval_internal(fctx->current,
														 &fctx->step,
														 fctx->attimezone);

		/* do when there is more left to send */
		SRF_RETURN_NEXT(funcctx, TimestampTzGetDatum(result));
	}
	else
	{
		/* do when there is no more left */
		SRF_RETURN_DONE(funcctx);
	}
}

Datum
generate_series_timestamptz(PG_FUNCTION_ARGS)
{
	return generate_series_timestamptz_internal(fcinfo);
}

Datum
generate_series_timestamptz_at_zone(PG_FUNCTION_ARGS)
{
	return generate_series_timestamptz_internal(fcinfo);
}

/*
 * Planner support function for generate_series(timestamp, timestamp, interval)
 */
Datum
generate_series_timestamp_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);
	Node	   *ret = NULL;

	if (IsA(rawreq, SupportRequestRows))
	{
		/* Try to estimate the number of rows returned */
		SupportRequestRows *req = (SupportRequestRows *) rawreq;

		if (is_funcclause(req->node))	/* be paranoid */
		{
			List	   *args = ((FuncExpr *) req->node)->args;
			Node	   *arg1,
					   *arg2,
					   *arg3;

			/* We can use estimated argument values here */
			arg1 = estimate_expression_value(req->root, linitial(args));
			arg2 = estimate_expression_value(req->root, lsecond(args));
			arg3 = estimate_expression_value(req->root, lthird(args));

			/*
			 * If any argument is constant NULL, we can safely assume that
			 * zero rows are returned.  Otherwise, if they're all non-NULL
			 * constants, we can calculate the number of rows that will be
			 * returned.
			 */
			if ((IsA(arg1, Const) && ((Const *) arg1)->constisnull) ||
				(IsA(arg2, Const) && ((Const *) arg2)->constisnull) ||
				(IsA(arg3, Const) && ((Const *) arg3)->constisnull))
			{
				req->rows = 0;
				ret = (Node *) req;
			}
			else if (IsA(arg1, Const) && IsA(arg2, Const) && IsA(arg3, Const))
			{
				Timestamp	start,
							finish;
				Interval   *step;
				Datum		diff;
				double		dstep;
				int64		dummy;

				start = DatumGetTimestamp(((Const *) arg1)->constvalue);
				finish = DatumGetTimestamp(((Const *) arg2)->constvalue);
				step = DatumGetIntervalP(((Const *) arg3)->constvalue);

				/*
				 * Perform some prechecks which could cause timestamp_mi to
				 * raise an ERROR.  It's much better to just return some
				 * default estimate than error out in a support function.
				 */
				if (!TIMESTAMP_NOT_FINITE(start) && !TIMESTAMP_NOT_FINITE(finish) &&
					!pg_sub_s64_overflow(finish, start, &dummy))
				{
					diff = DirectFunctionCall2(timestamp_mi,
											   TimestampGetDatum(finish),
											   TimestampGetDatum(start));

#define INTERVAL_TO_MICROSECONDS(i) ((((double) (i)->month * DAYS_PER_MONTH + (i)->day)) * USECS_PER_DAY + (i)->time)

					dstep = INTERVAL_TO_MICROSECONDS(step);

					/* This equation works for either sign of step */
					if (dstep != 0.0)
					{
						Interval   *idiff = DatumGetIntervalP(diff);
						double		ddiff = INTERVAL_TO_MICROSECONDS(idiff);

						req->rows = floor(ddiff / dstep + 1.0);
						ret = (Node *) req;
					}
#undef INTERVAL_TO_MICROSECONDS
				}
			}
		}
	}

	PG_RETURN_POINTER(ret);
}


/* timestamp_at_local()
 * timestamptz_at_local()
 *
 * The regression tests do not like two functions with the same proargs and
 * prosrc but different proname, but the grammar for AT LOCAL needs an
 * overloaded name to handle both types of timestamp, so we make simple
 * wrappers for it.
 */
Datum
timestamp_at_local(PG_FUNCTION_ARGS)
{
	return timestamp_timestamptz(fcinfo);
}

Datum
timestamptz_at_local(PG_FUNCTION_ARGS)
{
	return timestamptz_timestamp(fcinfo);
}
