/*-------------------------------------------------------------------------
 *
 * timestamp.c
 *	  Functions for the built-in SQL types "timestamp" and "interval".
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
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
#include <float.h>
#include <limits.h>
#include <sys/time.h>

#include "access/hash.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/scansup.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datetime.h"

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
} generate_series_timestamptz_fctx;


static TimeOffset time2t(const int hour, const int min, const int sec, const fsec_t fsec);
static Timestamp dt2local(Timestamp dt, int timezone);
static void AdjustTimestampForTypmod(Timestamp *time, int32 typmod);
static void AdjustIntervalForTypmod(Interval *interval, int32 typmod);
static TimestampTz timestamp2timestamptz(Timestamp timestamp);


/* common code for timestamptypmodin and timestamptztypmodin */
static int32
anytimestamp_typmodin(bool istz, ArrayType *ta)
{
	int32		typmod;
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

	if (*tl < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("TIMESTAMP(%d)%s precision must not be negative",
						*tl, (istz ? " WITH TIME ZONE" : ""))));
	if (*tl > MAX_TIMESTAMP_PRECISION)
	{
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		   errmsg("TIMESTAMP(%d)%s precision reduced to maximum allowed, %d",
				  *tl, (istz ? " WITH TIME ZONE" : ""),
				  MAX_TIMESTAMP_PRECISION)));
		typmod = MAX_TIMESTAMP_PRECISION;
	}
	else
		typmod = *tl;

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
		return psprintf("%s", tz);
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

	dterr = ParseDateTime(str, workbuf, sizeof(workbuf),
						  field, ftype, MAXDATEFIELDS, &nf);
	if (dterr == 0)
		dterr = DecodeDateTime(field, ftype, nf, &dtype, tm, &fsec, &tz);
	if (dterr != 0)
		DateTimeParseError(dterr, str, "timestamp");

	switch (dtype)
	{
		case DTK_DATE:
			if (tm2timestamp(tm, fsec, NULL, &result) != 0)
				ereport(ERROR,
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

		case DTK_INVALID:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			  errmsg("date/time value \"%s\" is no longer supported", str)));

			TIMESTAMP_NOEND(result);
			break;

		default:
			elog(ERROR, "unexpected dtype %d while parsing timestamp \"%s\"",
				 dtype, str);
			TIMESTAMP_NOEND(result);
	}

	AdjustTimestampForTypmod(&result, typmod);

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
 *
 * We make no attempt to provide compatibility between int and float
 * timestamp representations ...
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

#ifdef HAVE_INT64_TIMESTAMP
	timestamp = (Timestamp) pq_getmsgint64(buf);
#else
	timestamp = (Timestamp) pq_getmsgfloat8(buf);

	if (isnan(timestamp))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp cannot be NaN")));
#endif

	/* rangecheck: see if timestamp_out would like it */
	if (TIMESTAMP_NOT_FINITE(timestamp))
		 /* ok */ ;
	else if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL, NULL) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	AdjustTimestampForTypmod(&timestamp, typmod);

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
#ifdef HAVE_INT64_TIMESTAMP
	pq_sendint64(&buf, timestamp);
#else
	pq_sendfloat8(&buf, timestamp);
#endif
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


/* timestamp_transform()
 * Flatten calls to timestamp_scale() and timestamptz_scale() that solely
 * represent increases in allowed precision.
 */
Datum
timestamp_transform(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(TemporalTransform(MAX_TIMESTAMP_PRECISION,
										(Node *) PG_GETARG_POINTER(0)));
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

	AdjustTimestampForTypmod(&result, typmod);

	PG_RETURN_TIMESTAMP(result);
}

static void
AdjustTimestampForTypmod(Timestamp *time, int32 typmod)
{
#ifdef HAVE_INT64_TIMESTAMP
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
#else
	static const double TimestampScales[MAX_TIMESTAMP_PRECISION + 1] = {
		1,
		10,
		100,
		1000,
		10000,
		100000,
		1000000
	};
#endif

	if (!TIMESTAMP_NOT_FINITE(*time)
		&& (typmod != -1) && (typmod != MAX_TIMESTAMP_PRECISION))
	{
		if (typmod < 0 || typmod > MAX_TIMESTAMP_PRECISION)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				  errmsg("timestamp(%d) precision must be between %d and %d",
						 typmod, 0, MAX_TIMESTAMP_PRECISION)));

		/*
		 * Note: this round-to-nearest code is not completely consistent about
		 * rounding values that are exactly halfway between integral values.
		 * On most platforms, rint() will implement round-to-nearest-even, but
		 * the integer code always rounds up (away from zero).  Is it worth
		 * trying to be consistent?
		 */
#ifdef HAVE_INT64_TIMESTAMP
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
#else
		*time = rint((double) *time * TimestampScales[typmod]) / TimestampScales[typmod];
#endif
	}
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

	dterr = ParseDateTime(str, workbuf, sizeof(workbuf),
						  field, ftype, MAXDATEFIELDS, &nf);
	if (dterr == 0)
		dterr = DecodeDateTime(field, ftype, nf, &dtype, tm, &fsec, &tz);
	if (dterr != 0)
		DateTimeParseError(dterr, str, "timestamp with time zone");

	switch (dtype)
	{
		case DTK_DATE:
			if (tm2timestamp(tm, fsec, &tz, &result) != 0)
				ereport(ERROR,
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

		case DTK_INVALID:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			  errmsg("date/time value \"%s\" is no longer supported", str)));

			TIMESTAMP_NOEND(result);
			break;

		default:
			elog(ERROR, "unexpected dtype %d while parsing timestamptz \"%s\"",
				 dtype, str);
			TIMESTAMP_NOEND(result);
	}

	AdjustTimestampForTypmod(&result, typmod);

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
parse_sane_timezone(struct pg_tm * tm, text *zone)
{
	char		tzname[TZ_STRLEN_MAX + 1];
	int			rt;
	int			tz;

	text_to_cstring_buffer(zone, tzname, sizeof(tzname));

	/*
	 * Look up the requested timezone.  First we try to interpret it as a
	 * numeric timezone specification; if DecodeTimezone decides it doesn't
	 * like the format, we look in the timezone abbreviation table (to handle
	 * cases like "EST"), and if that also fails, we look in the timezone
	 * database (to handle cases like "America/New_York").  (This matches the
	 * order in which timestamp input checks the cases; it's important because
	 * the timezone database unwisely uses a few zone names that are identical
	 * to offset abbreviations.)
	 *
	 * Note pg_tzset happily parses numeric input that DecodeTimezone would
	 * reject.  To avoid having it accept input that would otherwise be seen
	 * as invalid, it's enough to disallow having a digit in the first
	 * position of our input string.
	 */
	if (isdigit((unsigned char) *tzname))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid input syntax for numeric time zone: \"%s\"",
						tzname),
				 errhint("Numeric time zones must have \"-\" or \"+\" as first character.")));

	rt = DecodeTimezone(tzname, &tz);
	if (rt != 0)
	{
		char	   *lowzone;
		int			type,
					val;
		pg_tz	   *tzp;

		if (rt == DTERR_TZDISP_OVERFLOW)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				   errmsg("numeric time zone \"%s\" out of range", tzname)));
		else if (rt != DTERR_BAD_FORMAT)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("time zone \"%s\" not recognized", tzname)));

		/* DecodeTimezoneAbbrev requires lowercase input */
		lowzone = downcase_truncate_identifier(tzname,
											   strlen(tzname),
											   false);
		type = DecodeTimezoneAbbrev(0, lowzone, &val, &tzp);

		if (type == TZ || type == DTZ)
		{
			/* fixed-offset abbreviation */
			tz = -val;
		}
		else if (type == DYNTZ)
		{
			/* dynamic-offset abbreviation, resolve using specified time */
			tz = DetermineTimeZoneAbbrevOffset(tm, tzname, tzp);
		}
		else
		{
			/* try it as a full zone name */
			tzp = pg_tzset(tzname);
			if (tzp)
				tz = DetermineTimeZoneOffset(tm, tzp);
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("time zone \"%s\" not recognized", tzname)));
		}
	}

	return tz;
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
	Timestamp	result;

	tm.tm_year = year;
	tm.tm_mon = month;
	tm.tm_mday = day;

	/*
	 * Note: we'll reject zero or negative year values.  Perhaps negatives
	 * should be allowed to represent BC years?
	 */
	dterr = ValidateDate(DTK_DATE_M, false, false, false, &tm);

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

	/*
	 * This should match the checks in DecodeTimeOnly, except that since we're
	 * dealing with a float "sec" value, we also explicitly reject NaN.  (An
	 * infinity input should get rejected by the range comparisons, but we
	 * can't be sure how those will treat a NaN.)
	 */
	if (hour < 0 || min < 0 || min > MINS_PER_HOUR - 1 ||
		isnan(sec) ||
		sec < 0 || sec > SECS_PER_MINUTE ||
		hour > HOURS_PER_DAY ||
	/* test for > 24:00:00 */
		(hour == HOURS_PER_DAY && (min > 0 || sec > 0)))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_FIELD_OVERFLOW),
				 errmsg("time field value out of range: %d:%02d:%02g",
						hour, min, sec)));

	/* This should match tm2time */
#ifdef HAVE_INT64_TIMESTAMP
	time = (((hour * MINS_PER_HOUR + min) * SECS_PER_MINUTE)
			* USECS_PER_SEC) + rint(sec * USECS_PER_SEC);

	result = date * USECS_PER_DAY + time;
	/* check for major overflow */
	if ((result - time) / USECS_PER_DAY != date)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range: %d-%02d-%02d %d:%02d:%02g",
						year, month, day,
						hour, min, sec)));

	/* check for just-barely overflow (okay except time-of-day wraps) */
	/* caution: we want to allow 1999-12-31 24:00:00 */
	if ((result < 0 && date > 0) ||
		(result > 0 && date < -1))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range: %d-%02d-%02d %d:%02d:%02g",
						year, month, day,
						hour, min, sec)));
#else
	time = ((hour * MINS_PER_HOUR + min) * SECS_PER_MINUTE) + sec;
	result = date * SECS_PER_DAY + time;
#endif

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

	PG_RETURN_TIMESTAMPTZ((TimestampTz) dt2local(timestamp, -tz));
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
 *
 * We make no attempt to provide compatibility between int and float
 * timestamp representations ...
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

#ifdef HAVE_INT64_TIMESTAMP
	timestamp = (TimestampTz) pq_getmsgint64(buf);
#else
	timestamp = (TimestampTz) pq_getmsgfloat8(buf);
#endif

	/* rangecheck: see if timestamptz_out would like it */
	if (TIMESTAMP_NOT_FINITE(timestamp))
		 /* ok */ ;
	else if (timestamp2tm(timestamp, &tz, tm, &fsec, NULL, NULL) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	AdjustTimestampForTypmod(&timestamp, typmod);

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
#ifdef HAVE_INT64_TIMESTAMP
	pq_sendint64(&buf, timestamp);
#else
	pq_sendfloat8(&buf, timestamp);
#endif
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

	AdjustTimestampForTypmod(&result, typmod);

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
	Interval   *result;
	fsec_t		fsec;
	struct pg_tm tt,
			   *tm = &tt;
	int			dtype;
	int			nf;
	int			range;
	int			dterr;
	char	   *field[MAXDATEFIELDS];
	int			ftype[MAXDATEFIELDS];
	char		workbuf[256];

	tm->tm_year = 0;
	tm->tm_mon = 0;
	tm->tm_mday = 0;
	tm->tm_hour = 0;
	tm->tm_min = 0;
	tm->tm_sec = 0;
	fsec = 0;

	if (typmod >= 0)
		range = INTERVAL_RANGE(typmod);
	else
		range = INTERVAL_FULL_RANGE;

	dterr = ParseDateTime(str, workbuf, sizeof(workbuf), field,
						  ftype, MAXDATEFIELDS, &nf);
	if (dterr == 0)
		dterr = DecodeInterval(field, ftype, nf, range,
							   &dtype, tm, &fsec);

	/* if those functions think it's a bad format, try ISO8601 style */
	if (dterr == DTERR_BAD_FORMAT)
		dterr = DecodeISO8601Interval(str,
									  &dtype, tm, &fsec);

	if (dterr != 0)
	{
		if (dterr == DTERR_FIELD_OVERFLOW)
			dterr = DTERR_INTERVAL_OVERFLOW;
		DateTimeParseError(dterr, str, "interval");
	}

	result = (Interval *) palloc(sizeof(Interval));

	switch (dtype)
	{
		case DTK_DELTA:
			if (tm2interval(tm, fsec, result) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("interval out of range")));
			break;

		case DTK_INVALID:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			  errmsg("date/time value \"%s\" is no longer supported", str)));
			break;

		default:
			elog(ERROR, "unexpected dtype %d while parsing interval \"%s\"",
				 dtype, str);
	}

	AdjustIntervalForTypmod(result, typmod);

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
	struct pg_tm tt,
			   *tm = &tt;
	fsec_t		fsec;
	char		buf[MAXDATELEN + 1];

	if (interval2tm(*span, tm, &fsec) != 0)
		elog(ERROR, "could not convert interval to tm");

	EncodeInterval(tm, fsec, IntervalStyle, buf);

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

#ifdef HAVE_INT64_TIMESTAMP
	interval->time = pq_getmsgint64(buf);
#else
	interval->time = pq_getmsgfloat8(buf);
#endif
	interval->day = pq_getmsgint(buf, sizeof(interval->day));
	interval->month = pq_getmsgint(buf, sizeof(interval->month));

	AdjustIntervalForTypmod(interval, typmod);

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
#ifdef HAVE_INT64_TIMESTAMP
	pq_sendint64(&buf, interval->time);
#else
	pq_sendfloat8(&buf, interval->time);
#endif
	pq_sendint(&buf, interval->day, sizeof(interval->day));
	pq_sendint(&buf, interval->month, sizeof(interval->month));
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


/* interval_transform()
 * Flatten superfluous calls to interval_scale().  The interval typmod is
 * complex to permit accepting and regurgitating all SQL standard variations.
 * For truncation purposes, it boils down to a single, simple granularity.
 */
Datum
interval_transform(PG_FUNCTION_ARGS)
{
	FuncExpr   *expr = (FuncExpr *) PG_GETARG_POINTER(0);
	Node	   *ret = NULL;
	Node	   *typmod;

	Assert(IsA(expr, FuncExpr));
	Assert(list_length(expr->args) >= 2);

	typmod = (Node *) lsecond(expr->args);

	if (IsA(typmod, Const) &&!((Const *) typmod)->constisnull)
	{
		Node	   *source = (Node *) linitial(expr->args);
		int32		old_typmod = exprTypmod(source);
		int32		new_typmod = DatumGetInt32(((Const *) typmod)->constvalue);
		int			old_range;
		int			old_precis;
		int			new_range = INTERVAL_RANGE(new_typmod);
		int			new_precis = INTERVAL_PRECISION(new_typmod);
		int			new_range_fls;
		int			old_range_fls;

		if (old_typmod < 0)
		{
			old_range = INTERVAL_FULL_RANGE;
			old_precis = INTERVAL_FULL_PRECISION;
		}
		else
		{
			old_range = INTERVAL_RANGE(old_typmod);
			old_precis = INTERVAL_PRECISION(old_typmod);
		}

		/*
		 * Temporally-smaller fields occupy higher positions in the range
		 * bitmap.  Since only the temporally-smallest bit matters for length
		 * coercion purposes, we compare the last-set bits in the ranges.
		 * Precision, which is to say, sub-second precision, only affects
		 * ranges that include SECOND.
		 */
		new_range_fls = fls(new_range);
		old_range_fls = fls(old_range);
		if (new_typmod < 0 ||
			((new_range_fls >= SECOND || new_range_fls >= old_range_fls) &&
		   (old_range_fls < SECOND || new_precis >= MAX_INTERVAL_PRECISION ||
			new_precis >= old_precis)))
			ret = relabel_to_typmod(source, new_typmod);
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

	AdjustIntervalForTypmod(result, typmod);

	PG_RETURN_INTERVAL_P(result);
}

/*
 *	Adjust interval for specified precision, in both YEAR to SECOND
 *	range and sub-second precision.
 */
static void
AdjustIntervalForTypmod(Interval *interval, int32 typmod)
{
#ifdef HAVE_INT64_TIMESTAMP
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
#else
	static const double IntervalScales[MAX_INTERVAL_PRECISION + 1] = {
		1,
		10,
		100,
		1000,
		10000,
		100000,
		1000000
	};
#endif

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
		 * revisit this, interval_transform will likely require adjusting.
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
#ifdef HAVE_INT64_TIMESTAMP
			interval->time = (interval->time / USECS_PER_HOUR) *
				USECS_PER_HOUR;
#else
			interval->time = ((int) (interval->time / SECS_PER_HOUR)) * (double) SECS_PER_HOUR;
#endif
		}
		else if (range == INTERVAL_MASK(MINUTE))
		{
#ifdef HAVE_INT64_TIMESTAMP
			interval->time = (interval->time / USECS_PER_MINUTE) *
				USECS_PER_MINUTE;
#else
			interval->time = ((int) (interval->time / SECS_PER_MINUTE)) * (double) SECS_PER_MINUTE;
#endif
		}
		else if (range == INTERVAL_MASK(SECOND))
		{
			/* fractional-second rounding will be dealt with below */
		}
		/* DAY TO HOUR */
		else if (range == (INTERVAL_MASK(DAY) |
						   INTERVAL_MASK(HOUR)))
		{
#ifdef HAVE_INT64_TIMESTAMP
			interval->time = (interval->time / USECS_PER_HOUR) *
				USECS_PER_HOUR;
#else
			interval->time = ((int) (interval->time / SECS_PER_HOUR)) * (double) SECS_PER_HOUR;
#endif
		}
		/* DAY TO MINUTE */
		else if (range == (INTERVAL_MASK(DAY) |
						   INTERVAL_MASK(HOUR) |
						   INTERVAL_MASK(MINUTE)))
		{
#ifdef HAVE_INT64_TIMESTAMP
			interval->time = (interval->time / USECS_PER_MINUTE) *
				USECS_PER_MINUTE;
#else
			interval->time = ((int) (interval->time / SECS_PER_MINUTE)) * (double) SECS_PER_MINUTE;
#endif
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
#ifdef HAVE_INT64_TIMESTAMP
			interval->time = (interval->time / USECS_PER_MINUTE) *
				USECS_PER_MINUTE;
#else
			interval->time = ((int) (interval->time / SECS_PER_MINUTE)) * (double) SECS_PER_MINUTE;
#endif
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

		/* Need to adjust subsecond precision? */
		if (precision != INTERVAL_FULL_PRECISION)
		{
			if (precision < 0 || precision > MAX_INTERVAL_PRECISION)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				   errmsg("interval(%d) precision must be between %d and %d",
						  precision, 0, MAX_INTERVAL_PRECISION)));

			/*
			 * Note: this round-to-nearest code is not completely consistent
			 * about rounding values that are exactly halfway between integral
			 * values.  On most platforms, rint() will implement
			 * round-to-nearest-even, but the integer code always rounds up
			 * (away from zero).  Is it worth trying to be consistent?
			 */
#ifdef HAVE_INT64_TIMESTAMP
			if (interval->time >= INT64CONST(0))
			{
				interval->time = ((interval->time +
								   IntervalOffsets[precision]) /
								  IntervalScales[precision]) *
					IntervalScales[precision];
			}
			else
			{
				interval->time = -(((-interval->time +
									 IntervalOffsets[precision]) /
									IntervalScales[precision]) *
								   IntervalScales[precision]);
			}
#else
			interval->time = rint(((double) interval->time) *
								  IntervalScales[precision]) /
				IntervalScales[precision];
#endif
		}
	}
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
	 * Reject out-of-range inputs.  We really ought to check the integer
	 * inputs as well, but it's not entirely clear what limits to apply.
	 */
	if (isinf(secs) || isnan(secs))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));

	result = (Interval *) palloc(sizeof(Interval));
	result->month = years * MONTHS_PER_YEAR + months;
	result->day = weeks * 7 + days;

	secs += hours * (double) SECS_PER_HOUR + mins * (double) SECS_PER_MINUTE;

#ifdef HAVE_INT64_TIMESTAMP
	result->time = (int64) (secs * USECS_PER_SEC);
#else
	result->time = secs;
#endif

	PG_RETURN_INTERVAL_P(result);
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
	else	/* shouldn't happen */
		elog(ERROR, "invalid argument for EncodeSpecialTimestamp");
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

#ifdef HAVE_INT64_TIMESTAMP
	result = (result * USECS_PER_SEC) + tp.tv_usec;
#else
	result = result + (tp.tv_usec / 1000000.0);
#endif

	return result;
}

/*
 * GetCurrentIntegerTimestamp -- get the current operating system time as int64
 *
 * Result is the number of microseconds since the Postgres epoch. If compiled
 * with --enable-integer-datetimes, this is identical to GetCurrentTimestamp(),
 * and is implemented as a macro.
 */
#ifndef HAVE_INT64_TIMESTAMP
int64
GetCurrentIntegerTimestamp(void)
{
	int64		result;
	struct timeval tp;

	gettimeofday(&tp, NULL);

	result = (int64) tp.tv_sec -
		((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY);

	result = (result * USECS_PER_SEC) + tp.tv_usec;

	return result;
}
#endif

/*
 * IntegetTimestampToTimestampTz -- convert an int64 timestamp to native format
 *
 * When compiled with --enable-integer-datetimes, this is implemented as a
 * no-op macro.
 */
#ifndef HAVE_INT64_TIMESTAMP
TimestampTz
IntegerTimestampToTimestampTz(int64 timestamp)
{
	TimestampTz result;

	result = timestamp / USECS_PER_SEC;
	result += (timestamp % USECS_PER_SEC) / 1000000.0;

	return result;
}
#endif

/*
 * TimestampDifference -- convert the difference between two timestamps
 *		into integer seconds and microseconds
 *
 * Both inputs must be ordinary finite timestamps (in current usage,
 * they'll be results from GetCurrentTimestamp()).
 *
 * We expect start_time <= stop_time.  If not, we return zeroes; for current
 * callers there is no need to be tense about which way division rounds on
 * negative inputs.
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
#ifdef HAVE_INT64_TIMESTAMP
		*secs = (long) (diff / USECS_PER_SEC);
		*microsecs = (int) (diff % USECS_PER_SEC);
#else
		*secs = (long) diff;
		*microsecs = (int) ((diff - *secs) * 1000000.0);
#endif
	}
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

#ifdef HAVE_INT64_TIMESTAMP
	return (diff >= msec * INT64CONST(1000));
#else
	return (diff * 1000.0 >= msec);
#endif
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

#ifdef HAVE_INT64_TIMESTAMP
	result *= USECS_PER_SEC;
#endif

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

#ifdef HAVE_INT64_TIMESTAMP
	result = (pg_time_t) (t / USECS_PER_SEC +
				 ((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY));
#else
	result = (pg_time_t) (t +
				 ((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY));
#endif

	return result;
}

/*
 * Produce a C-string representation of a TimestampTz.
 *
 * This is mostly for use in emitting messages.  The primary difference
 * from timestamptz_out is that we force the output format to ISO.  Note
 * also that the result is in a static buffer, not pstrdup'd.
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

#ifdef HAVE_INT64_TIMESTAMP
	*hour = time / USECS_PER_HOUR;
	time -= (*hour) * USECS_PER_HOUR;
	*min = time / USECS_PER_MINUTE;
	time -= (*min) * USECS_PER_MINUTE;
	*sec = time / USECS_PER_SEC;
	*fsec = time - (*sec * USECS_PER_SEC);
#else
	*hour = time / SECS_PER_HOUR;
	time -= (*hour) * SECS_PER_HOUR;
	*min = time / SECS_PER_MINUTE;
	time -= (*min) * SECS_PER_MINUTE;
	*sec = time;
	*fsec = time - *sec;
#endif
}	/* dt2time() */


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
timestamp2tm(Timestamp dt, int *tzp, struct pg_tm * tm, fsec_t *fsec, const char **tzn, pg_tz *attimezone)
{
	Timestamp	date;
	Timestamp	time;
	pg_time_t	utime;

	/* Use session timezone if caller asks for default */
	if (attimezone == NULL)
		attimezone = session_timezone;

#ifdef HAVE_INT64_TIMESTAMP
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
#else
	time = dt;
	TMODULO(time, date, (double) SECS_PER_DAY);

	if (time < 0)
	{
		time += SECS_PER_DAY;
		date -= 1;
	}

	/* add offset to go from J2000 back to standard Julian date */
	date += POSTGRES_EPOCH_JDATE;

recalc_d:
	/* Julian day routine does not work for negative Julian days */
	if (date < 0 || date > (Timestamp) INT_MAX)
		return -1;

	j2date((int) date, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
recalc_t:
	dt2time(time, &tm->tm_hour, &tm->tm_min, &tm->tm_sec, fsec);

	*fsec = TSROUND(*fsec);
	/* roundoff may need to propagate to higher-order fields */
	if (*fsec >= 1.0)
	{
		time = ceil(time);
		if (time >= (double) SECS_PER_DAY)
		{
			time = 0;
			date += 1;
			goto recalc_d;
		}
		goto recalc_t;
	}
#endif

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
#ifdef HAVE_INT64_TIMESTAMP
	dt = (dt - *fsec) / USECS_PER_SEC +
		(POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY;
#else
	dt = rint(dt - *fsec +
			  (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY);
#endif
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
tm2timestamp(struct pg_tm * tm, fsec_t fsec, int *tzp, Timestamp *result)
{
	TimeOffset	date;
	TimeOffset	time;

	/* Julian day routines are not correct for negative Julian days */
	if (!IS_VALID_JULIAN(tm->tm_year, tm->tm_mon, tm->tm_mday))
	{
		*result = 0;			/* keep compiler quiet */
		return -1;
	}

	date = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - POSTGRES_EPOCH_JDATE;
	time = time2t(tm->tm_hour, tm->tm_min, tm->tm_sec, fsec);

#ifdef HAVE_INT64_TIMESTAMP
	*result = date * USECS_PER_DAY + time;
	/* check for major overflow */
	if ((*result - time) / USECS_PER_DAY != date)
	{
		*result = 0;			/* keep compiler quiet */
		return -1;
	}
	/* check for just-barely overflow (okay except time-of-day wraps) */
	/* caution: we want to allow 1999-12-31 24:00:00 */
	if ((*result < 0 && date > 0) ||
		(*result > 0 && date < -1))
	{
		*result = 0;			/* keep compiler quiet */
		return -1;
	}
#else
	*result = date * SECS_PER_DAY + time;
#endif
	if (tzp != NULL)
		*result = dt2local(*result, -(*tzp));

	return 0;
}


/* interval2tm()
 * Convert an interval data type to a tm structure.
 */
int
interval2tm(Interval span, struct pg_tm * tm, fsec_t *fsec)
{
	TimeOffset	time;
	TimeOffset	tfrac;

	tm->tm_year = span.month / MONTHS_PER_YEAR;
	tm->tm_mon = span.month % MONTHS_PER_YEAR;
	tm->tm_mday = span.day;
	time = span.time;

#ifdef HAVE_INT64_TIMESTAMP
	tfrac = time / USECS_PER_HOUR;
	time -= tfrac * USECS_PER_HOUR;
	tm->tm_hour = tfrac;
	if (!SAMESIGN(tm->tm_hour, tfrac))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));
	tfrac = time / USECS_PER_MINUTE;
	time -= tfrac * USECS_PER_MINUTE;
	tm->tm_min = tfrac;
	tfrac = time / USECS_PER_SEC;
	*fsec = time - (tfrac * USECS_PER_SEC);
	tm->tm_sec = tfrac;
#else
recalc:
	TMODULO(time, tfrac, (double) SECS_PER_HOUR);
	tm->tm_hour = tfrac;		/* could overflow ... */
	TMODULO(time, tfrac, (double) SECS_PER_MINUTE);
	tm->tm_min = tfrac;
	TMODULO(time, tfrac, 1.0);
	tm->tm_sec = tfrac;
	time = TSROUND(time);
	/* roundoff may need to propagate to higher-order fields */
	if (time >= 1.0)
	{
		time = ceil(span.time);
		goto recalc;
	}
	*fsec = time;
#endif

	return 0;
}

int
tm2interval(struct pg_tm * tm, fsec_t fsec, Interval *span)
{
	double		total_months = (double) tm->tm_year * MONTHS_PER_YEAR + tm->tm_mon;

	if (total_months > INT_MAX || total_months < INT_MIN)
		return -1;
	span->month = total_months;
	span->day = tm->tm_mday;
#ifdef HAVE_INT64_TIMESTAMP
	span->time = (((((tm->tm_hour * INT64CONST(60)) +
					 tm->tm_min) * INT64CONST(60)) +
				   tm->tm_sec) * USECS_PER_SEC) + fsec;
#else
	span->time = (((tm->tm_hour * (double) MINS_PER_HOUR) +
				   tm->tm_min) * (double) SECS_PER_MINUTE) +
		tm->tm_sec + fsec;
#endif

	return 0;
}

static TimeOffset
time2t(const int hour, const int min, const int sec, const fsec_t fsec)
{
#ifdef HAVE_INT64_TIMESTAMP
	return (((((hour * MINS_PER_HOUR) + min) * SECS_PER_MINUTE) + sec) * USECS_PER_SEC) + fsec;
#else
	return (((hour * MINS_PER_HOUR) + min) * SECS_PER_MINUTE) + sec + fsec;
#endif
}

static Timestamp
dt2local(Timestamp dt, int tz)
{
#ifdef HAVE_INT64_TIMESTAMP
	dt -= (tz * USECS_PER_SEC);
#else
	dt -= tz;
#endif
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
	PG_RETURN_BOOL(true);
}


/*----------------------------------------------------------
 *	Relational operators for timestamp.
 *---------------------------------------------------------*/

void
GetEpochTime(struct pg_tm * tm)
{
	struct pg_tm *t0;
	pg_time_t	epoch = 0;

	t0 = pg_gmtime(&epoch);

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
}	/* SetEpochTimestamp() */

/*
 * We are currently sharing some code between timestamp and timestamptz.
 * The comparison functions are among them. - thomas 2001-09-25
 *
 *		timestamp_relop - is timestamp1 relop timestamp2
 *
 *		collate invalid timestamp at the end
 */
int
timestamp_cmp_internal(Timestamp dt1, Timestamp dt2)
{
#ifdef HAVE_INT64_TIMESTAMP
	return (dt1 < dt2) ? -1 : ((dt1 > dt2) ? 1 : 0);
#else

	/*
	 * When using float representation, we have to be wary of NaNs.
	 *
	 * We consider all NANs to be equal and larger than any non-NAN. This is
	 * somewhat arbitrary; the important thing is to have a consistent sort
	 * order.
	 */
	if (isnan(dt1))
	{
		if (isnan(dt2))
			return 0;			/* NAN = NAN */
		else
			return 1;			/* NAN > non-NAN */
	}
	else if (isnan(dt2))
	{
		return -1;				/* non-NAN < NAN */
	}
	else
	{
		if (dt1 > dt2)
			return 1;
		else if (dt1 < dt2)
			return -1;
		else
			return 0;
	}
#endif
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

/* note: this is used for timestamptz also */
static int
timestamp_fastcmp(Datum x, Datum y, SortSupport ssup)
{
	Timestamp	a = DatumGetTimestamp(x);
	Timestamp	b = DatumGetTimestamp(y);

	return timestamp_cmp_internal(a, b);
}

Datum
timestamp_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	ssup->comparator = timestamp_fastcmp;
	PG_RETURN_VOID();
}

Datum
timestamp_hash(PG_FUNCTION_ARGS)
{
	/* We can use either hashint8 or hashfloat8 directly */
#ifdef HAVE_INT64_TIMESTAMP
	return hashint8(fcinfo);
#else
	return hashfloat8(fcinfo);
#endif
}


/*
 * Crosstype comparison functions for timestamp vs timestamptz
 */

Datum
timestamp_eq_timestamptz(PG_FUNCTION_ARGS)
{
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(0);
	TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);
	TimestampTz dt1;

	dt1 = timestamp2timestamptz(timestampVal);

	PG_RETURN_BOOL(timestamp_cmp_internal(dt1, dt2) == 0);
}

Datum
timestamp_ne_timestamptz(PG_FUNCTION_ARGS)
{
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(0);
	TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);
	TimestampTz dt1;

	dt1 = timestamp2timestamptz(timestampVal);

	PG_RETURN_BOOL(timestamp_cmp_internal(dt1, dt2) != 0);
}

Datum
timestamp_lt_timestamptz(PG_FUNCTION_ARGS)
{
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(0);
	TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);
	TimestampTz dt1;

	dt1 = timestamp2timestamptz(timestampVal);

	PG_RETURN_BOOL(timestamp_cmp_internal(dt1, dt2) < 0);
}

Datum
timestamp_gt_timestamptz(PG_FUNCTION_ARGS)
{
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(0);
	TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);
	TimestampTz dt1;

	dt1 = timestamp2timestamptz(timestampVal);

	PG_RETURN_BOOL(timestamp_cmp_internal(dt1, dt2) > 0);
}

Datum
timestamp_le_timestamptz(PG_FUNCTION_ARGS)
{
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(0);
	TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);
	TimestampTz dt1;

	dt1 = timestamp2timestamptz(timestampVal);

	PG_RETURN_BOOL(timestamp_cmp_internal(dt1, dt2) <= 0);
}

Datum
timestamp_ge_timestamptz(PG_FUNCTION_ARGS)
{
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(0);
	TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);
	TimestampTz dt1;

	dt1 = timestamp2timestamptz(timestampVal);

	PG_RETURN_BOOL(timestamp_cmp_internal(dt1, dt2) >= 0);
}

Datum
timestamp_cmp_timestamptz(PG_FUNCTION_ARGS)
{
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(0);
	TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);
	TimestampTz dt1;

	dt1 = timestamp2timestamptz(timestampVal);

	PG_RETURN_INT32(timestamp_cmp_internal(dt1, dt2));
}

Datum
timestamptz_eq_timestamp(PG_FUNCTION_ARGS)
{
	TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(1);
	TimestampTz dt2;

	dt2 = timestamp2timestamptz(timestampVal);

	PG_RETURN_BOOL(timestamp_cmp_internal(dt1, dt2) == 0);
}

Datum
timestamptz_ne_timestamp(PG_FUNCTION_ARGS)
{
	TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(1);
	TimestampTz dt2;

	dt2 = timestamp2timestamptz(timestampVal);

	PG_RETURN_BOOL(timestamp_cmp_internal(dt1, dt2) != 0);
}

Datum
timestamptz_lt_timestamp(PG_FUNCTION_ARGS)
{
	TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(1);
	TimestampTz dt2;

	dt2 = timestamp2timestamptz(timestampVal);

	PG_RETURN_BOOL(timestamp_cmp_internal(dt1, dt2) < 0);
}

Datum
timestamptz_gt_timestamp(PG_FUNCTION_ARGS)
{
	TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(1);
	TimestampTz dt2;

	dt2 = timestamp2timestamptz(timestampVal);

	PG_RETURN_BOOL(timestamp_cmp_internal(dt1, dt2) > 0);
}

Datum
timestamptz_le_timestamp(PG_FUNCTION_ARGS)
{
	TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(1);
	TimestampTz dt2;

	dt2 = timestamp2timestamptz(timestampVal);

	PG_RETURN_BOOL(timestamp_cmp_internal(dt1, dt2) <= 0);
}

Datum
timestamptz_ge_timestamp(PG_FUNCTION_ARGS)
{
	TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(1);
	TimestampTz dt2;

	dt2 = timestamp2timestamptz(timestampVal);

	PG_RETURN_BOOL(timestamp_cmp_internal(dt1, dt2) >= 0);
}

Datum
timestamptz_cmp_timestamp(PG_FUNCTION_ARGS)
{
	TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
	Timestamp	timestampVal = PG_GETARG_TIMESTAMP(1);
	TimestampTz dt2;

	dt2 = timestamp2timestamptz(timestampVal);

	PG_RETURN_INT32(timestamp_cmp_internal(dt1, dt2));
}


/*
 *		interval_relop	- is interval1 relop interval2
 *
 *		collate invalid interval at the end
 */
static inline TimeOffset
interval_cmp_value(const Interval *interval)
{
	TimeOffset	span;

	span = interval->time;

#ifdef HAVE_INT64_TIMESTAMP
	span += interval->month * INT64CONST(30) * USECS_PER_DAY;
	span += interval->day * INT64CONST(24) * USECS_PER_HOUR;
#else
	span += interval->month * ((double) DAYS_PER_MONTH * SECS_PER_DAY);
	span += interval->day * ((double) HOURS_PER_DAY * SECS_PER_HOUR);
#endif

	return span;
}

static int
interval_cmp_internal(Interval *interval1, Interval *interval2)
{
	TimeOffset	span1 = interval_cmp_value(interval1);
	TimeOffset	span2 = interval_cmp_value(interval2);

	return ((span1 < span2) ? -1 : (span1 > span2) ? 1 : 0);
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
 * and then hash that, using either int64 or float8 hashing.
 */
Datum
interval_hash(PG_FUNCTION_ARGS)
{
	Interval   *interval = PG_GETARG_INTERVAL_P(0);
	TimeOffset	span = interval_cmp_value(interval);

#ifdef HAVE_INT64_TIMESTAMP
	return DirectFunctionCall1(hashint8, Int64GetDatumFast(span));
#else
	return DirectFunctionCall1(hashfloat8, Float8GetDatumFast(span));
#endif
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
		 * rather silly way of saying "true if both are nonnull, else null".
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

	if (TIMESTAMP_NOT_FINITE(dt1) || TIMESTAMP_NOT_FINITE(dt2))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("cannot subtract infinite timestamps")));

	result->time = dt1 - dt2;

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

#ifdef HAVE_INT64_TIMESTAMP
	TMODULO(result->time, wholeday, USECS_PER_DAY);
#else
	TMODULO(result->time, wholeday, (double) SECS_PER_DAY);
#endif
	result->day += wholeday;	/* could overflow... */

	wholemonth = result->day / DAYS_PER_MONTH;
	result->day -= wholemonth * DAYS_PER_MONTH;
	result->month += wholemonth;

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
#ifdef HAVE_INT64_TIMESTAMP
		result->time += USECS_PER_DAY;
#else
		result->time += (double) SECS_PER_DAY;
#endif
		result->day--;
	}
	else if (result->day < 0 && result->time > 0)
	{
#ifdef HAVE_INT64_TIMESTAMP
		result->time -= USECS_PER_DAY;
#else
		result->time -= (double) SECS_PER_DAY;
#endif
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

#ifdef HAVE_INT64_TIMESTAMP
	TMODULO(result->time, wholeday, USECS_PER_DAY);
#else
	TMODULO(result->time, wholeday, (double) SECS_PER_DAY);
#endif
	result->day += wholeday;	/* could overflow... */

	if (result->day > 0 && result->time < 0)
	{
#ifdef HAVE_INT64_TIMESTAMP
		result->time += USECS_PER_DAY;
#else
		result->time += (double) SECS_PER_DAY;
#endif
		result->day--;
	}
	else if (result->day < 0 && result->time > 0)
	{
#ifdef HAVE_INT64_TIMESTAMP
		result->time -= USECS_PER_DAY;
#else
		result->time -= (double) SECS_PER_DAY;
#endif
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

	wholemonth = result->day / DAYS_PER_MONTH;
	result->day -= wholemonth * DAYS_PER_MONTH;
	result->month += wholemonth;

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

	if (TIMESTAMP_NOT_FINITE(timestamp))
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

			tm->tm_mon += span->month;
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

			/* Add days by converting to and from julian */
			julian = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) + span->day;
			j2date(julian, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);

			if (tm2timestamp(tm, fsec, NULL, &timestamp) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));
		}

		timestamp += span->time;
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

	tspan.month = -span->month;
	tspan.day = -span->day;
	tspan.time = -span->time;

	return DirectFunctionCall2(timestamp_pl_interval,
							   TimestampGetDatum(timestamp),
							   PointerGetDatum(&tspan));
}


/* timestamptz_pl_interval()
 * Add an interval to a timestamp with time zone data type.
 * Note that interval has provisions for qualitative year/month
 *	units, so try to do the right thing with them.
 * To add a month, increment the month, and use the same day of month.
 * Then, if the next month has fewer days, set the day of month
 *	to the last day of month.
 * Lastly, add in the "quantitative time".
 */
Datum
timestamptz_pl_interval(PG_FUNCTION_ARGS)
{
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(0);
	Interval   *span = PG_GETARG_INTERVAL_P(1);
	TimestampTz result;
	int			tz;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		result = timestamp;
	else
	{
		if (span->month != 0)
		{
			struct pg_tm tt,
					   *tm = &tt;
			fsec_t		fsec;

			if (timestamp2tm(timestamp, &tz, tm, &fsec, NULL, NULL) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));

			tm->tm_mon += span->month;
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

			tz = DetermineTimeZoneOffset(tm, session_timezone);

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

			if (timestamp2tm(timestamp, &tz, tm, &fsec, NULL, NULL) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));

			/* Add days by converting to and from julian */
			julian = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) + span->day;
			j2date(julian, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);

			tz = DetermineTimeZoneOffset(tm, session_timezone);

			if (tm2timestamp(tm, fsec, &tz, &timestamp) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));
		}

		timestamp += span->time;
		result = timestamp;
	}

	PG_RETURN_TIMESTAMP(result);
}

Datum
timestamptz_mi_interval(PG_FUNCTION_ARGS)
{
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(0);
	Interval   *span = PG_GETARG_INTERVAL_P(1);
	Interval	tspan;

	tspan.month = -span->month;
	tspan.day = -span->day;
	tspan.time = -span->time;

	return DirectFunctionCall2(timestamptz_pl_interval,
							   TimestampGetDatum(timestamp),
							   PointerGetDatum(&tspan));
}


Datum
interval_um(PG_FUNCTION_ARGS)
{
	Interval   *interval = PG_GETARG_INTERVAL_P(0);
	Interval   *result;

	result = (Interval *) palloc(sizeof(Interval));

	result->time = -interval->time;
	/* overflow check copied from int4um */
	if (interval->time != 0 && SAMESIGN(result->time, interval->time))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));
	result->day = -interval->day;
	if (interval->day != 0 && SAMESIGN(result->day, interval->day))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));
	result->month = -interval->month;
	if (interval->month != 0 && SAMESIGN(result->month, interval->month))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));

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

Datum
interval_pl(PG_FUNCTION_ARGS)
{
	Interval   *span1 = PG_GETARG_INTERVAL_P(0);
	Interval   *span2 = PG_GETARG_INTERVAL_P(1);
	Interval   *result;

	result = (Interval *) palloc(sizeof(Interval));

	result->month = span1->month + span2->month;
	/* overflow check copied from int4pl */
	if (SAMESIGN(span1->month, span2->month) &&
		!SAMESIGN(result->month, span1->month))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));

	result->day = span1->day + span2->day;
	if (SAMESIGN(span1->day, span2->day) &&
		!SAMESIGN(result->day, span1->day))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));

	result->time = span1->time + span2->time;
	if (SAMESIGN(span1->time, span2->time) &&
		!SAMESIGN(result->time, span1->time))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));

	PG_RETURN_INTERVAL_P(result);
}

Datum
interval_mi(PG_FUNCTION_ARGS)
{
	Interval   *span1 = PG_GETARG_INTERVAL_P(0);
	Interval   *span2 = PG_GETARG_INTERVAL_P(1);
	Interval   *result;

	result = (Interval *) palloc(sizeof(Interval));

	result->month = span1->month - span2->month;
	/* overflow check copied from int4mi */
	if (!SAMESIGN(span1->month, span2->month) &&
		!SAMESIGN(result->month, span1->month))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));

	result->day = span1->day - span2->day;
	if (!SAMESIGN(span1->day, span2->day) &&
		!SAMESIGN(result->day, span1->day))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));

	result->time = span1->time - span2->time;
	if (!SAMESIGN(span1->time, span2->time) &&
		!SAMESIGN(result->time, span1->time))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));

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

	result_double = span->month * factor;
	if (result_double > INT_MAX || result_double < INT_MIN)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));
	result->month = (int32) result_double;

	result_double = span->day * factor;
	if (result_double > INT_MAX || result_double < INT_MIN)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));
	result->day = (int32) result_double;

	/*
	 * The above correctly handles the whole-number part of the month and day
	 * products, but we have to do something with any fractional part
	 * resulting when the factor is nonintegral.  We cascade the fractions
	 * down to lower units using the conversion factors DAYS_PER_MONTH and
	 * SECS_PER_DAY.  Note we do NOT cascade up, since we are not forced to do
	 * so by the representation.  The user can choose to cascade up later,
	 * using justify_hours and/or justify_days.
	 */

	/*
	 * Fractional months full days into days.
	 *
	 * Floating point calculation are inherently inprecise, so these
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
	if (Abs(sec_remainder) >= SECS_PER_DAY)
	{
		result->day += (int) (sec_remainder / SECS_PER_DAY);
		sec_remainder -= (int) (sec_remainder / SECS_PER_DAY) * SECS_PER_DAY;
	}

	/* cascade units down */
	result->day += (int32) month_remainder_days;
#ifdef HAVE_INT64_TIMESTAMP
	result_double = rint(span->time * factor + sec_remainder * USECS_PER_SEC);
	if (result_double > PG_INT64_MAX || result_double < PG_INT64_MIN)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("interval out of range")));
	result->time = (int64) result_double;
#else
	result->time = span->time * factor + sec_remainder;
#endif

	PG_RETURN_INTERVAL_P(result);
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
				sec_remainder;
	int32		orig_month = span->month,
				orig_day = span->day;
	Interval   *result;

	result = (Interval *) palloc(sizeof(Interval));

	if (factor == 0.0)
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));

	result->month = (int32) (span->month / factor);
	result->day = (int32) (span->day / factor);

	/*
	 * Fractional months full days into days.  See comment in interval_mul().
	 */
	month_remainder_days = (orig_month / factor - result->month) * DAYS_PER_MONTH;
	month_remainder_days = TSROUND(month_remainder_days);
	sec_remainder = (orig_day / factor - result->day +
		   month_remainder_days - (int) month_remainder_days) * SECS_PER_DAY;
	sec_remainder = TSROUND(sec_remainder);
	if (Abs(sec_remainder) >= SECS_PER_DAY)
	{
		result->day += (int) (sec_remainder / SECS_PER_DAY);
		sec_remainder -= (int) (sec_remainder / SECS_PER_DAY) * SECS_PER_DAY;
	}

	/* cascade units down */
	result->day += (int32) month_remainder_days;
#ifdef HAVE_INT64_TIMESTAMP
	result->time = rint(span->time / factor + sec_remainder * USECS_PER_SEC);
#else
	/* See TSROUND comment in interval_mul(). */
	result->time = span->time / factor + sec_remainder;
#endif

	PG_RETURN_INTERVAL_P(result);
}

/*
 * interval_accum, interval_accum_inv, and interval_avg implement the
 * AVG(interval) aggregate.
 *
 * The transition datatype for this aggregate is a 2-element array of
 * intervals, where the first is the running sum and the second contains
 * the number of values so far in its 'time' field.  This is a bit ugly
 * but it beats inventing a specialized datatype for the purpose.
 *
 * NOTE: The inverse transition function cannot guarantee exact results
 * when using float8 timestamps.  However, int8 timestamps are now the
 * norm, and the probable range of values is not so wide that disastrous
 * cancellation is likely even with float8, so we'll ignore the risk.
 */

Datum
interval_accum(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	Interval   *newval = PG_GETARG_INTERVAL_P(1);
	Datum	   *transdatums;
	int			ndatums;
	Interval	sumX,
				N;
	Interval   *newsum;
	ArrayType  *result;

	deconstruct_array(transarray,
					  INTERVALOID, sizeof(Interval), false, 'd',
					  &transdatums, NULL, &ndatums);
	if (ndatums != 2)
		elog(ERROR, "expected 2-element interval array");

	sumX = *(DatumGetIntervalP(transdatums[0]));
	N = *(DatumGetIntervalP(transdatums[1]));

	newsum = DatumGetIntervalP(DirectFunctionCall2(interval_pl,
												   IntervalPGetDatum(&sumX),
												 IntervalPGetDatum(newval)));
	N.time += 1;

	transdatums[0] = IntervalPGetDatum(newsum);
	transdatums[1] = IntervalPGetDatum(&N);

	result = construct_array(transdatums, 2,
							 INTERVALOID, sizeof(Interval), false, 'd');

	PG_RETURN_ARRAYTYPE_P(result);
}

Datum
interval_accum_inv(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	Interval   *newval = PG_GETARG_INTERVAL_P(1);
	Datum	   *transdatums;
	int			ndatums;
	Interval	sumX,
				N;
	Interval   *newsum;
	ArrayType  *result;

	deconstruct_array(transarray,
					  INTERVALOID, sizeof(Interval), false, 'd',
					  &transdatums, NULL, &ndatums);
	if (ndatums != 2)
		elog(ERROR, "expected 2-element interval array");

	sumX = *(DatumGetIntervalP(transdatums[0]));
	N = *(DatumGetIntervalP(transdatums[1]));

	newsum = DatumGetIntervalP(DirectFunctionCall2(interval_mi,
												   IntervalPGetDatum(&sumX),
												 IntervalPGetDatum(newval)));
	N.time -= 1;

	transdatums[0] = IntervalPGetDatum(newsum);
	transdatums[1] = IntervalPGetDatum(&N);

	result = construct_array(transdatums, 2,
							 INTERVALOID, sizeof(Interval), false, 'd');

	PG_RETURN_ARRAYTYPE_P(result);
}

Datum
interval_avg(PG_FUNCTION_ARGS)
{
	ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
	Datum	   *transdatums;
	int			ndatums;
	Interval	sumX,
				N;

	deconstruct_array(transarray,
					  INTERVALOID, sizeof(Interval), false, 'd',
					  &transdatums, NULL, &ndatums);
	if (ndatums != 2)
		elog(ERROR, "expected 2-element interval array");

	sumX = *(DatumGetIntervalP(transdatums[0]));
	N = *(DatumGetIntervalP(transdatums[1]));

	/* SQL defines AVG of no values to be NULL */
	if (N.time == 0)
		PG_RETURN_NULL();

	return DirectFunctionCall2(interval_div,
							   IntervalPGetDatum(&sumX),
							   Float8GetDatum((double) N.time));
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
	fsec_t		fsec,
				fsec1,
				fsec2;
	struct pg_tm tt,
			   *tm = &tt;
	struct pg_tm tt1,
			   *tm1 = &tt1;
	struct pg_tm tt2,
			   *tm2 = &tt2;

	result = (Interval *) palloc(sizeof(Interval));

	if (timestamp2tm(dt1, NULL, tm1, &fsec1, NULL, NULL) == 0 &&
		timestamp2tm(dt2, NULL, tm2, &fsec2, NULL, NULL) == 0)
	{
		/* form the symbolic difference */
		fsec = fsec1 - fsec2;
		tm->tm_sec = tm1->tm_sec - tm2->tm_sec;
		tm->tm_min = tm1->tm_min - tm2->tm_min;
		tm->tm_hour = tm1->tm_hour - tm2->tm_hour;
		tm->tm_mday = tm1->tm_mday - tm2->tm_mday;
		tm->tm_mon = tm1->tm_mon - tm2->tm_mon;
		tm->tm_year = tm1->tm_year - tm2->tm_year;

		/* flip sign if necessary... */
		if (dt1 < dt2)
		{
			fsec = -fsec;
			tm->tm_sec = -tm->tm_sec;
			tm->tm_min = -tm->tm_min;
			tm->tm_hour = -tm->tm_hour;
			tm->tm_mday = -tm->tm_mday;
			tm->tm_mon = -tm->tm_mon;
			tm->tm_year = -tm->tm_year;
		}

		/* propagate any negative fields into the next higher field */
		while (fsec < 0)
		{
#ifdef HAVE_INT64_TIMESTAMP
			fsec += USECS_PER_SEC;
#else
			fsec += 1.0;
#endif
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
			fsec = -fsec;
			tm->tm_sec = -tm->tm_sec;
			tm->tm_min = -tm->tm_min;
			tm->tm_hour = -tm->tm_hour;
			tm->tm_mday = -tm->tm_mday;
			tm->tm_mon = -tm->tm_mon;
			tm->tm_year = -tm->tm_year;
		}

		if (tm2interval(tm, fsec, result) != 0)
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
	fsec_t		fsec,
				fsec1,
				fsec2;
	struct pg_tm tt,
			   *tm = &tt;
	struct pg_tm tt1,
			   *tm1 = &tt1;
	struct pg_tm tt2,
			   *tm2 = &tt2;
	int			tz1;
	int			tz2;

	result = (Interval *) palloc(sizeof(Interval));

	if (timestamp2tm(dt1, &tz1, tm1, &fsec1, NULL, NULL) == 0 &&
		timestamp2tm(dt2, &tz2, tm2, &fsec2, NULL, NULL) == 0)
	{
		/* form the symbolic difference */
		fsec = fsec1 - fsec2;
		tm->tm_sec = tm1->tm_sec - tm2->tm_sec;
		tm->tm_min = tm1->tm_min - tm2->tm_min;
		tm->tm_hour = tm1->tm_hour - tm2->tm_hour;
		tm->tm_mday = tm1->tm_mday - tm2->tm_mday;
		tm->tm_mon = tm1->tm_mon - tm2->tm_mon;
		tm->tm_year = tm1->tm_year - tm2->tm_year;

		/* flip sign if necessary... */
		if (dt1 < dt2)
		{
			fsec = -fsec;
			tm->tm_sec = -tm->tm_sec;
			tm->tm_min = -tm->tm_min;
			tm->tm_hour = -tm->tm_hour;
			tm->tm_mday = -tm->tm_mday;
			tm->tm_mon = -tm->tm_mon;
			tm->tm_year = -tm->tm_year;
		}

		/* propagate any negative fields into the next higher field */
		while (fsec < 0)
		{
#ifdef HAVE_INT64_TIMESTAMP
			fsec += USECS_PER_SEC;
#else
			fsec += 1.0;
#endif
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
			fsec = -fsec;
			tm->tm_sec = -tm->tm_sec;
			tm->tm_min = -tm->tm_min;
			tm->tm_hour = -tm->tm_hour;
			tm->tm_mday = -tm->tm_mday;
			tm->tm_mon = -tm->tm_mon;
			tm->tm_year = -tm->tm_year;
		}

		if (tm2interval(tm, fsec, result) != 0)
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

	if (TIMESTAMP_NOT_FINITE(timestamp))
		PG_RETURN_TIMESTAMP(timestamp);

	lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
											VARSIZE_ANY_EXHDR(units),
											false);

	type = DecodeUnits(0, lowunits, &val);

	if (type == UNITS)
	{
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
			case DTK_CENTURY:
				/* see comments in timestamptz_trunc */
				if (tm->tm_year > 0)
					tm->tm_year = ((tm->tm_year + 99) / 100) * 100 - 99;
				else
					tm->tm_year = -((99 - (tm->tm_year - 1)) / 100) * 100 + 1;
			case DTK_DECADE:
				/* see comments in timestamptz_trunc */
				if (val != DTK_MILLENNIUM && val != DTK_CENTURY)
				{
					if (tm->tm_year > 0)
						tm->tm_year = (tm->tm_year / 10) * 10;
					else
						tm->tm_year = -((8 - (tm->tm_year - 1)) / 10) * 10;
				}
			case DTK_YEAR:
				tm->tm_mon = 1;
			case DTK_QUARTER:
				tm->tm_mon = (3 * ((tm->tm_mon - 1) / 3)) + 1;
			case DTK_MONTH:
				tm->tm_mday = 1;
			case DTK_DAY:
				tm->tm_hour = 0;
			case DTK_HOUR:
				tm->tm_min = 0;
			case DTK_MINUTE:
				tm->tm_sec = 0;
			case DTK_SECOND:
				fsec = 0;
				break;

			case DTK_MILLISEC:
#ifdef HAVE_INT64_TIMESTAMP
				fsec = (fsec / 1000) * 1000;
#else
				fsec = floor(fsec * 1000) / 1000;
#endif
				break;

			case DTK_MICROSEC:
#ifndef HAVE_INT64_TIMESTAMP
				fsec = floor(fsec * 1000000) / 1000000;
#endif
				break;

			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("timestamp units \"%s\" not supported",
								lowunits)));
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
				 errmsg("timestamp units \"%s\" not recognized",
						lowunits)));
		result = 0;
	}

	PG_RETURN_TIMESTAMP(result);
}

/* timestamptz_trunc()
 * Truncate timestamp to specified units.
 */
Datum
timestamptz_trunc(PG_FUNCTION_ARGS)
{
	text	   *units = PG_GETARG_TEXT_PP(0);
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(1);
	TimestampTz result;
	int			tz;
	int			type,
				val;
	bool		redotz = false;
	char	   *lowunits;
	fsec_t		fsec;
	struct pg_tm tt,
			   *tm = &tt;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		PG_RETURN_TIMESTAMPTZ(timestamp);

	lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
											VARSIZE_ANY_EXHDR(units),
											false);

	type = DecodeUnits(0, lowunits, &val);

	if (type == UNITS)
	{
		if (timestamp2tm(timestamp, &tz, tm, &fsec, NULL, NULL) != 0)
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
#ifdef HAVE_INT64_TIMESTAMP
				fsec = (fsec / 1000) * 1000;
#else
				fsec = floor(fsec * 1000) / 1000;
#endif
				break;
			case DTK_MICROSEC:
#ifndef HAVE_INT64_TIMESTAMP
				fsec = floor(fsec * 1000000) / 1000000;
#endif
				break;

			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("timestamp with time zone units \"%s\" not "
								"supported", lowunits)));
				result = 0;
		}

		if (redotz)
			tz = DetermineTimeZoneOffset(tm, session_timezone);

		if (tm2timestamp(tm, fsec, &tz, &result) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			   errmsg("timestamp with time zone units \"%s\" not recognized",
					  lowunits)));
		result = 0;
	}

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
	fsec_t		fsec;
	struct pg_tm tt,
			   *tm = &tt;

	result = (Interval *) palloc(sizeof(Interval));

	lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
											VARSIZE_ANY_EXHDR(units),
											false);

	type = DecodeUnits(0, lowunits, &val);

	if (type == UNITS)
	{
		if (interval2tm(*interval, tm, &fsec) == 0)
		{
			switch (val)
			{
					/* fall through */
				case DTK_MILLENNIUM:
					/* caution: C division may have negative remainder */
					tm->tm_year = (tm->tm_year / 1000) * 1000;
				case DTK_CENTURY:
					/* caution: C division may have negative remainder */
					tm->tm_year = (tm->tm_year / 100) * 100;
				case DTK_DECADE:
					/* caution: C division may have negative remainder */
					tm->tm_year = (tm->tm_year / 10) * 10;
				case DTK_YEAR:
					tm->tm_mon = 0;
				case DTK_QUARTER:
					tm->tm_mon = 3 * (tm->tm_mon / 3);
				case DTK_MONTH:
					tm->tm_mday = 0;
				case DTK_DAY:
					tm->tm_hour = 0;
				case DTK_HOUR:
					tm->tm_min = 0;
				case DTK_MINUTE:
					tm->tm_sec = 0;
				case DTK_SECOND:
					fsec = 0;
					break;

				case DTK_MILLISEC:
#ifdef HAVE_INT64_TIMESTAMP
					fsec = (fsec / 1000) * 1000;
#else
					fsec = floor(fsec * 1000) / 1000;
#endif
					break;
				case DTK_MICROSEC:
#ifndef HAVE_INT64_TIMESTAMP
					fsec = floor(fsec * 1000000) / 1000000;
#endif
					break;

				default:
					if (val == DTK_WEEK)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("interval units \"%s\" not supported "
							  "because months usually have fractional weeks",
										lowunits)));
					else
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("interval units \"%s\" not supported",
										lowunits)));
			}

			if (tm2interval(tm, fsec, result) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("interval out of range")));
		}
		else
			elog(ERROR, "could not convert interval to tm");
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("interval units \"%s\" not recognized",
						lowunits)));
	}

	PG_RETURN_INTERVAL_P(result);
}

/* isoweek2j()
 *
 *	Return the Julian day which corresponds to the first day (Monday) of the given ISO 8601 year and week.
 *	Julian days are used to convert between ISO week dates and Gregorian dates.
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

/* timestamp_part()
 * Extract specified field from timestamp.
 */
Datum
timestamp_part(PG_FUNCTION_ARGS)
{
	text	   *units = PG_GETARG_TEXT_PP(0);
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(1);
	float8		result;
	int			type,
				val;
	char	   *lowunits;
	fsec_t		fsec;
	struct pg_tm tt,
			   *tm = &tt;

	if (TIMESTAMP_NOT_FINITE(timestamp))
	{
		result = 0;
		PG_RETURN_FLOAT8(result);
	}

	lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
											VARSIZE_ANY_EXHDR(units),
											false);

	type = DecodeUnits(0, lowunits, &val);
	if (type == UNKNOWN_FIELD)
		type = DecodeSpecial(0, lowunits, &val);

	if (type == UNITS)
	{
		if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL, NULL) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));

		switch (val)
		{
			case DTK_MICROSEC:
#ifdef HAVE_INT64_TIMESTAMP
				result = tm->tm_sec * 1000000.0 + fsec;
#else
				result = (tm->tm_sec + fsec) * 1000000;
#endif
				break;

			case DTK_MILLISEC:
#ifdef HAVE_INT64_TIMESTAMP
				result = tm->tm_sec * 1000.0 + fsec / 1000.0;
#else
				result = (tm->tm_sec + fsec) * 1000;
#endif
				break;

			case DTK_SECOND:
#ifdef HAVE_INT64_TIMESTAMP
				result = tm->tm_sec + fsec / 1000000.0;
#else
				result = tm->tm_sec + fsec;
#endif
				break;

			case DTK_MINUTE:
				result = tm->tm_min;
				break;

			case DTK_HOUR:
				result = tm->tm_hour;
				break;

			case DTK_DAY:
				result = tm->tm_mday;
				break;

			case DTK_MONTH:
				result = tm->tm_mon;
				break;

			case DTK_QUARTER:
				result = (tm->tm_mon - 1) / 3 + 1;
				break;

			case DTK_WEEK:
				result = (float8) date2isoweek(tm->tm_year, tm->tm_mon, tm->tm_mday);
				break;

			case DTK_YEAR:
				if (tm->tm_year > 0)
					result = tm->tm_year;
				else
					/* there is no year 0, just 1 BC and 1 AD */
					result = tm->tm_year - 1;
				break;

			case DTK_DECADE:

				/*
				 * what is a decade wrt dates? let us assume that decade 199
				 * is 1990 thru 1999... decade 0 starts on year 1 BC, and -1
				 * is 11 BC thru 2 BC...
				 */
				if (tm->tm_year >= 0)
					result = tm->tm_year / 10;
				else
					result = -((8 - (tm->tm_year - 1)) / 10);
				break;

			case DTK_CENTURY:

				/* ----
				 * centuries AD, c>0: year in [ (c-1)* 100 + 1 : c*100 ]
				 * centuries BC, c<0: year in [ c*100 : (c+1) * 100 - 1]
				 * there is no number 0 century.
				 * ----
				 */
				if (tm->tm_year > 0)
					result = (tm->tm_year + 99) / 100;
				else
					/* caution: C division may have negative remainder */
					result = -((99 - (tm->tm_year - 1)) / 100);
				break;

			case DTK_MILLENNIUM:
				/* see comments above. */
				if (tm->tm_year > 0)
					result = (tm->tm_year + 999) / 1000;
				else
					result = -((999 - (tm->tm_year - 1)) / 1000);
				break;

			case DTK_JULIAN:
				result = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday);
#ifdef HAVE_INT64_TIMESTAMP
				result += ((((tm->tm_hour * MINS_PER_HOUR) + tm->tm_min) * SECS_PER_MINUTE) +
					tm->tm_sec + (fsec / 1000000.0)) / (double) SECS_PER_DAY;
#else
				result += ((((tm->tm_hour * MINS_PER_HOUR) + tm->tm_min) * SECS_PER_MINUTE) +
						   tm->tm_sec + fsec) / (double) SECS_PER_DAY;
#endif
				break;

			case DTK_ISOYEAR:
				result = date2isoyear(tm->tm_year, tm->tm_mon, tm->tm_mday);
				break;

			case DTK_DOW:
			case DTK_ISODOW:
				if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL, NULL) != 0)
					ereport(ERROR,
							(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
							 errmsg("timestamp out of range")));
				result = j2day(date2j(tm->tm_year, tm->tm_mon, tm->tm_mday));
				if (val == DTK_ISODOW && result == 0)
					result = 7;
				break;

			case DTK_DOY:
				if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL, NULL) != 0)
					ereport(ERROR,
							(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
							 errmsg("timestamp out of range")));
				result = (date2j(tm->tm_year, tm->tm_mon, tm->tm_mday)
						  - date2j(tm->tm_year, 1, 1) + 1);
				break;

			case DTK_TZ:
			case DTK_TZ_MINUTE:
			case DTK_TZ_HOUR:
			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("timestamp units \"%s\" not supported",
								lowunits)));
				result = 0;
		}
	}
	else if (type == RESERV)
	{
		switch (val)
		{
			case DTK_EPOCH:
#ifdef HAVE_INT64_TIMESTAMP
				result = (timestamp - SetEpochTimestamp()) / 1000000.0;
#else
				result = timestamp - SetEpochTimestamp();
#endif
				break;

			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("timestamp units \"%s\" not supported",
								lowunits)));
				result = 0;
		}

	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("timestamp units \"%s\" not recognized", lowunits)));
		result = 0;
	}

	PG_RETURN_FLOAT8(result);
}

/* timestamptz_part()
 * Extract specified field from timestamp with time zone.
 */
Datum
timestamptz_part(PG_FUNCTION_ARGS)
{
	text	   *units = PG_GETARG_TEXT_PP(0);
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(1);
	float8		result;
	int			tz;
	int			type,
				val;
	char	   *lowunits;
	double		dummy;
	fsec_t		fsec;
	struct pg_tm tt,
			   *tm = &tt;

	if (TIMESTAMP_NOT_FINITE(timestamp))
	{
		result = 0;
		PG_RETURN_FLOAT8(result);
	}

	lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
											VARSIZE_ANY_EXHDR(units),
											false);

	type = DecodeUnits(0, lowunits, &val);
	if (type == UNKNOWN_FIELD)
		type = DecodeSpecial(0, lowunits, &val);

	if (type == UNITS)
	{
		if (timestamp2tm(timestamp, &tz, tm, &fsec, NULL, NULL) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));

		switch (val)
		{
			case DTK_TZ:
				result = -tz;
				break;

			case DTK_TZ_MINUTE:
				result = -tz;
				result /= MINS_PER_HOUR;
				FMODULO(result, dummy, (double) MINS_PER_HOUR);
				break;

			case DTK_TZ_HOUR:
				dummy = -tz;
				FMODULO(dummy, result, (double) SECS_PER_HOUR);
				break;

			case DTK_MICROSEC:
#ifdef HAVE_INT64_TIMESTAMP
				result = tm->tm_sec * 1000000.0 + fsec;
#else
				result = (tm->tm_sec + fsec) * 1000000;
#endif
				break;

			case DTK_MILLISEC:
#ifdef HAVE_INT64_TIMESTAMP
				result = tm->tm_sec * 1000.0 + fsec / 1000.0;
#else
				result = (tm->tm_sec + fsec) * 1000;
#endif
				break;

			case DTK_SECOND:
#ifdef HAVE_INT64_TIMESTAMP
				result = tm->tm_sec + fsec / 1000000.0;
#else
				result = tm->tm_sec + fsec;
#endif
				break;

			case DTK_MINUTE:
				result = tm->tm_min;
				break;

			case DTK_HOUR:
				result = tm->tm_hour;
				break;

			case DTK_DAY:
				result = tm->tm_mday;
				break;

			case DTK_MONTH:
				result = tm->tm_mon;
				break;

			case DTK_QUARTER:
				result = (tm->tm_mon - 1) / 3 + 1;
				break;

			case DTK_WEEK:
				result = (float8) date2isoweek(tm->tm_year, tm->tm_mon, tm->tm_mday);
				break;

			case DTK_YEAR:
				if (tm->tm_year > 0)
					result = tm->tm_year;
				else
					/* there is no year 0, just 1 BC and 1 AD */
					result = tm->tm_year - 1;
				break;

			case DTK_DECADE:
				/* see comments in timestamp_part */
				if (tm->tm_year > 0)
					result = tm->tm_year / 10;
				else
					result = -((8 - (tm->tm_year - 1)) / 10);
				break;

			case DTK_CENTURY:
				/* see comments in timestamp_part */
				if (tm->tm_year > 0)
					result = (tm->tm_year + 99) / 100;
				else
					result = -((99 - (tm->tm_year - 1)) / 100);
				break;

			case DTK_MILLENNIUM:
				/* see comments in timestamp_part */
				if (tm->tm_year > 0)
					result = (tm->tm_year + 999) / 1000;
				else
					result = -((999 - (tm->tm_year - 1)) / 1000);
				break;

			case DTK_JULIAN:
				result = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday);
#ifdef HAVE_INT64_TIMESTAMP
				result += ((((tm->tm_hour * MINS_PER_HOUR) + tm->tm_min) * SECS_PER_MINUTE) +
					tm->tm_sec + (fsec / 1000000.0)) / (double) SECS_PER_DAY;
#else
				result += ((((tm->tm_hour * MINS_PER_HOUR) + tm->tm_min) * SECS_PER_MINUTE) +
						   tm->tm_sec + fsec) / (double) SECS_PER_DAY;
#endif
				break;

			case DTK_ISOYEAR:
				result = date2isoyear(tm->tm_year, tm->tm_mon, tm->tm_mday);
				break;

			case DTK_DOW:
			case DTK_ISODOW:
				if (timestamp2tm(timestamp, &tz, tm, &fsec, NULL, NULL) != 0)
					ereport(ERROR,
							(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
							 errmsg("timestamp out of range")));
				result = j2day(date2j(tm->tm_year, tm->tm_mon, tm->tm_mday));
				if (val == DTK_ISODOW && result == 0)
					result = 7;
				break;

			case DTK_DOY:
				if (timestamp2tm(timestamp, &tz, tm, &fsec, NULL, NULL) != 0)
					ereport(ERROR,
							(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
							 errmsg("timestamp out of range")));
				result = (date2j(tm->tm_year, tm->tm_mon, tm->tm_mday)
						  - date2j(tm->tm_year, 1, 1) + 1);
				break;

			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("timestamp with time zone units \"%s\" not supported",
					   lowunits)));
				result = 0;
		}

	}
	else if (type == RESERV)
	{
		switch (val)
		{
			case DTK_EPOCH:
#ifdef HAVE_INT64_TIMESTAMP
				result = (timestamp - SetEpochTimestamp()) / 1000000.0;
#else
				result = timestamp - SetEpochTimestamp();
#endif
				break;

			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("timestamp with time zone units \"%s\" not supported",
					   lowunits)));
				result = 0;
		}
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			   errmsg("timestamp with time zone units \"%s\" not recognized",
					  lowunits)));

		result = 0;
	}

	PG_RETURN_FLOAT8(result);
}


/* interval_part()
 * Extract specified field from interval.
 */
Datum
interval_part(PG_FUNCTION_ARGS)
{
	text	   *units = PG_GETARG_TEXT_PP(0);
	Interval   *interval = PG_GETARG_INTERVAL_P(1);
	float8		result;
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

	if (type == UNITS)
	{
		if (interval2tm(*interval, tm, &fsec) == 0)
		{
			switch (val)
			{
				case DTK_MICROSEC:
#ifdef HAVE_INT64_TIMESTAMP
					result = tm->tm_sec * 1000000.0 + fsec;
#else
					result = (tm->tm_sec + fsec) * 1000000;
#endif
					break;

				case DTK_MILLISEC:
#ifdef HAVE_INT64_TIMESTAMP
					result = tm->tm_sec * 1000.0 + fsec / 1000.0;
#else
					result = (tm->tm_sec + fsec) * 1000;
#endif
					break;

				case DTK_SECOND:
#ifdef HAVE_INT64_TIMESTAMP
					result = tm->tm_sec + fsec / 1000000.0;
#else
					result = tm->tm_sec + fsec;
#endif
					break;

				case DTK_MINUTE:
					result = tm->tm_min;
					break;

				case DTK_HOUR:
					result = tm->tm_hour;
					break;

				case DTK_DAY:
					result = tm->tm_mday;
					break;

				case DTK_MONTH:
					result = tm->tm_mon;
					break;

				case DTK_QUARTER:
					result = (tm->tm_mon / 3) + 1;
					break;

				case DTK_YEAR:
					result = tm->tm_year;
					break;

				case DTK_DECADE:
					/* caution: C division may have negative remainder */
					result = tm->tm_year / 10;
					break;

				case DTK_CENTURY:
					/* caution: C division may have negative remainder */
					result = tm->tm_year / 100;
					break;

				case DTK_MILLENNIUM:
					/* caution: C division may have negative remainder */
					result = tm->tm_year / 1000;
					break;

				default:
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("interval units \"%s\" not supported",
									lowunits)));
					result = 0;
			}

		}
		else
		{
			elog(ERROR, "could not convert interval to tm");
			result = 0;
		}
	}
	else if (type == RESERV && val == DTK_EPOCH)
	{
#ifdef HAVE_INT64_TIMESTAMP
		result = interval->time / 1000000.0;
#else
		result = interval->time;
#endif
		result += ((double) DAYS_PER_YEAR * SECS_PER_DAY) * (interval->month / MONTHS_PER_YEAR);
		result += ((double) DAYS_PER_MONTH * SECS_PER_DAY) * (interval->month % MONTHS_PER_YEAR);
		result += ((double) SECS_PER_DAY) * interval->day;
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("interval units \"%s\" not recognized",
						lowunits)));
		result = 0;
	}

	PG_RETURN_FLOAT8(result);
}


/* timestamp_zone_transform()
 * If the zone argument of a timestamp_zone() or timestamptz_zone() call is a
 * plan-time constant denoting a zone equivalent to UTC, the call will always
 * return its second argument unchanged.  Simplify the expression tree
 * accordingly.  Civil time zones almost never qualify, because jurisdictions
 * that follow UTC today have not done so continuously.
 */
Datum
timestamp_zone_transform(PG_FUNCTION_ARGS)
{
	Node	   *func_node = (Node *) PG_GETARG_POINTER(0);
	FuncExpr   *expr = (FuncExpr *) func_node;
	Node	   *ret = NULL;
	Node	   *zone_node;

	Assert(IsA(expr, FuncExpr));
	Assert(list_length(expr->args) == 2);

	zone_node = (Node *) linitial(expr->args);

	if (IsA(zone_node, Const) &&!((Const *) zone_node)->constisnull)
	{
		text	   *zone = DatumGetTextPP(((Const *) zone_node)->constvalue);
		char		tzname[TZ_STRLEN_MAX + 1];
		char	   *lowzone;
		int			type,
					abbrev_offset;
		pg_tz	   *tzp;
		bool		noop = false;

		/*
		 * If the timezone is forever UTC+0, the FuncExpr function call is a
		 * no-op for all possible timestamps.  This passage mirrors code in
		 * timestamp_zone().
		 */
		text_to_cstring_buffer(zone, tzname, sizeof(tzname));
		lowzone = downcase_truncate_identifier(tzname,
											   strlen(tzname),
											   false);
		type = DecodeTimezoneAbbrev(0, lowzone, &abbrev_offset, &tzp);
		if (type == TZ || type == DTZ)
			noop = (abbrev_offset == 0);
		else if (type == DYNTZ)
		{
			/*
			 * An abbreviation of a single-offset timezone ought not to be
			 * configured as a DYNTZ, so don't bother checking.
			 */
		}
		else
		{
			long		tzname_offset;

			tzp = pg_tzset(tzname);
			if (tzp && pg_get_timezone_offset(tzp, &tzname_offset))
				noop = (tzname_offset == 0);
		}

		if (noop)
		{
			Node	   *timestamp = (Node *) lsecond(expr->args);

			/* Strip any existing RelabelType node(s) */
			while (timestamp && IsA(timestamp, RelabelType))
				timestamp = (Node *) ((RelabelType *) timestamp)->arg;

			/*
			 * Replace the FuncExpr with its timestamp argument, relabeled as
			 * though the function call had computed it.
			 */
			ret = (Node *) makeRelabelType((Expr *) timestamp,
										   exprType(func_node),
										   exprTypmod(func_node),
										   exprCollation(func_node),
										   COERCE_EXPLICIT_CAST);
		}
	}

	PG_RETURN_POINTER(ret);
}

/*	timestamp_zone()
 *	Encode timestamp type with specified time zone.
 *	This function is just timestamp2timestamptz() except instead of
 *	shifting to the global timezone, we shift to the specified timezone.
 *	This is different from the other AT TIME ZONE cases because instead
 *	of shifting to a _to_ a new time zone, it sets the time to _be_ the
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
	char	   *lowzone;
	int			type,
				val;
	pg_tz	   *tzp;
	struct pg_tm tm;
	fsec_t		fsec;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		PG_RETURN_TIMESTAMPTZ(timestamp);

	/*
	 * Look up the requested timezone.  First we look in the timezone
	 * abbreviation table (to handle cases like "EST"), and if that fails, we
	 * look in the timezone database (to handle cases like
	 * "America/New_York").  (This matches the order in which timestamp input
	 * checks the cases; it's important because the timezone database unwisely
	 * uses a few zone names that are identical to offset abbreviations.)
	 */
	text_to_cstring_buffer(zone, tzname, sizeof(tzname));

	/* DecodeTimezoneAbbrev requires lowercase input */
	lowzone = downcase_truncate_identifier(tzname,
										   strlen(tzname),
										   false);

	type = DecodeTimezoneAbbrev(0, lowzone, &val, &tzp);

	if (type == TZ || type == DTZ)
	{
		/* fixed-offset abbreviation */
		tz = val;
		result = dt2local(timestamp, tz);
	}
	else if (type == DYNTZ)
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
		/* try it as a full zone name */
		tzp = pg_tzset(tzname);
		if (tzp)
		{
			/* Apply the timezone change */
			if (timestamp2tm(timestamp, NULL, &tm, &fsec, NULL, tzp) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));
			tz = DetermineTimeZoneOffset(&tm, tzp);
			if (tm2timestamp(&tm, fsec, &tz, &result) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("could not convert to time zone \"%s\"",
								tzname)));
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("time zone \"%s\" not recognized", tzname)));
			result = 0;			/* keep compiler quiet */
		}
	}

	PG_RETURN_TIMESTAMPTZ(result);
}

/* timestamp_izone_transform()
 * If we deduce at plan time that a particular timestamp_izone() or
 * timestamptz_izone() call can only compute tz=0, the call will always return
 * its second argument unchanged.  Simplify the expression tree accordingly.
 */
Datum
timestamp_izone_transform(PG_FUNCTION_ARGS)
{
	Node	   *func_node = (Node *) PG_GETARG_POINTER(0);
	FuncExpr   *expr = (FuncExpr *) func_node;
	Node	   *ret = NULL;
	Node	   *zone_node;

	Assert(IsA(expr, FuncExpr));
	Assert(list_length(expr->args) == 2);

	zone_node = (Node *) linitial(expr->args);

	if (IsA(zone_node, Const) &&!((Const *) zone_node)->constisnull)
	{
		Interval   *zone;

		zone = DatumGetIntervalP(((Const *) zone_node)->constvalue);
		if (zone->month == 0 && zone->day == 0 && zone->time == 0)
		{
			Node	   *timestamp = (Node *) lsecond(expr->args);

			/* Strip any existing RelabelType node(s) */
			while (timestamp && IsA(timestamp, RelabelType))
				timestamp = (Node *) ((RelabelType *) timestamp)->arg;

			/*
			 * Replace the FuncExpr with its timestamp argument, relabeled as
			 * though the function call had computed it.
			 */
			ret = (Node *) makeRelabelType((Expr *) timestamp,
										   exprType(func_node),
										   exprTypmod(func_node),
										   exprCollation(func_node),
										   COERCE_EXPLICIT_CAST);
		}
	}

	PG_RETURN_POINTER(ret);
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

	if (zone->month != 0 || zone->day != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		  errmsg("interval time zone \"%s\" must not include months or days",
				 DatumGetCString(DirectFunctionCall1(interval_out,
												  PointerGetDatum(zone))))));

#ifdef HAVE_INT64_TIMESTAMP
	tz = zone->time / USECS_PER_SEC;
#else
	tz = zone->time;
#endif

	result = dt2local(timestamp, tz);

	PG_RETURN_TIMESTAMPTZ(result);
}	/* timestamp_izone() */

/* timestamp_timestamptz()
 * Convert local timestamp to timestamp at GMT
 */
Datum
timestamp_timestamptz(PG_FUNCTION_ARGS)
{
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(0);

	PG_RETURN_TIMESTAMPTZ(timestamp2timestamptz(timestamp));
}

static TimestampTz
timestamp2timestamptz(Timestamp timestamp)
{
	TimestampTz result;
	struct pg_tm tt,
			   *tm = &tt;
	fsec_t		fsec;
	int			tz;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		result = timestamp;
	else
	{
		if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL, NULL) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));

		tz = DetermineTimeZoneOffset(tm, session_timezone);

		if (tm2timestamp(tm, fsec, &tz, &result) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
	}

	return result;
}

/* timestamptz_timestamp()
 * Convert timestamp at GMT to local timestamp
 */
Datum
timestamptz_timestamp(PG_FUNCTION_ARGS)
{
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(0);
	Timestamp	result;
	struct pg_tm tt,
			   *tm = &tt;
	fsec_t		fsec;
	int			tz;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		result = timestamp;
	else
	{
		if (timestamp2tm(timestamp, &tz, tm, &fsec, NULL, NULL) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
		if (tm2timestamp(tm, fsec, NULL, &result) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
	}
	PG_RETURN_TIMESTAMP(result);
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
	char	   *lowzone;
	int			type,
				val;
	pg_tz	   *tzp;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		PG_RETURN_TIMESTAMP(timestamp);

	/*
	 * Look up the requested timezone.  First we look in the timezone
	 * abbreviation table (to handle cases like "EST"), and if that fails, we
	 * look in the timezone database (to handle cases like
	 * "America/New_York").  (This matches the order in which timestamp input
	 * checks the cases; it's important because the timezone database unwisely
	 * uses a few zone names that are identical to offset abbreviations.)
	 */
	text_to_cstring_buffer(zone, tzname, sizeof(tzname));

	/* DecodeTimezoneAbbrev requires lowercase input */
	lowzone = downcase_truncate_identifier(tzname,
										   strlen(tzname),
										   false);

	type = DecodeTimezoneAbbrev(0, lowzone, &val, &tzp);

	if (type == TZ || type == DTZ)
	{
		/* fixed-offset abbreviation */
		tz = -val;
		result = dt2local(timestamp, tz);
	}
	else if (type == DYNTZ)
	{
		/* dynamic-offset abbreviation, resolve using specified time */
		int			isdst;

		tz = DetermineTimeZoneAbbrevOffsetTS(timestamp, tzname, tzp, &isdst);
		result = dt2local(timestamp, tz);
	}
	else
	{
		/* try it as a full zone name */
		tzp = pg_tzset(tzname);
		if (tzp)
		{
			/* Apply the timezone change */
			struct pg_tm tm;
			fsec_t		fsec;

			if (timestamp2tm(timestamp, &tz, &tm, &fsec, NULL, tzp) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));
			if (tm2timestamp(&tm, fsec, NULL, &result) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("could not convert to time zone \"%s\"",
								tzname)));
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("time zone \"%s\" not recognized", tzname)));
			result = 0;			/* keep compiler quiet */
		}
	}

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

	if (zone->month != 0 || zone->day != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		  errmsg("interval time zone \"%s\" must not include months or days",
				 DatumGetCString(DirectFunctionCall1(interval_out,
												  PointerGetDatum(zone))))));

#ifdef HAVE_INT64_TIMESTAMP
	tz = -(zone->time / USECS_PER_SEC);
#else
	tz = -zone->time;
#endif

	result = dt2local(timestamp, tz);

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
		Interval	interval_zero;

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
		MemSet(&interval_zero, 0, sizeof(Interval));
		fctx->step_sign = interval_cmp_internal(&fctx->step, &interval_zero);

		if (fctx->step_sign == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("step size cannot equal zero")));

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
		fctx->current = DatumGetTimestamp(
								   DirectFunctionCall2(timestamp_pl_interval,
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
 * Generate the set of timestamps from start to finish by step
 */
Datum
generate_series_timestamptz(PG_FUNCTION_ARGS)
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
		MemoryContext oldcontext;
		Interval	interval_zero;

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

		/* Determine sign of the interval */
		MemSet(&interval_zero, 0, sizeof(Interval));
		fctx->step_sign = interval_cmp_internal(&fctx->step, &interval_zero);

		if (fctx->step_sign == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("step size cannot equal zero")));

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
		fctx->current = DatumGetTimestampTz(
								 DirectFunctionCall2(timestamptz_pl_interval,
										  TimestampTzGetDatum(fctx->current),
											  PointerGetDatum(&fctx->step)));

		/* do when there is more left to send */
		SRF_RETURN_NEXT(funcctx, TimestampTzGetDatum(result));
	}
	else
	{
		/* do when there is no more left */
		SRF_RETURN_DONE(funcctx);
	}
}
