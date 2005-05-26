/*-------------------------------------------------------------------------
 *
 * timestamp.c
 *	  Functions for the built-in SQL92 types "timestamp" and "interval".
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/timestamp.c,v 1.96.2.3 2005/05/26 02:14:31 neilc Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>
#include <math.h>
#include <float.h>
#include <limits.h>
/* for finite() on Solaris */
#ifdef HAVE_IEEEFP_H
#include <ieeefp.h>
#endif

#include "access/hash.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"

/*
 * gcc's -ffast-math switch breaks routines that expect exact results from
 * expressions like timeval / 3600, where timeval is double.
 */
#ifdef __FAST_MATH__
#error -ffast-math is known to break this code
#endif


#ifdef HAVE_INT64_TIMESTAMP
static int64 time2t(const int hour, const int min, const int sec, const fsec_t fsec);

#else
static double time2t(const int hour, const int min, const int sec, const fsec_t fsec);
#endif
static int	EncodeSpecialTimestamp(Timestamp dt, char *str);
static Timestamp dt2local(Timestamp dt, int timezone);
static void AdjustTimestampForTypmod(Timestamp *time, int32 typmod);
static void AdjustIntervalForTypmod(Interval *interval, int32 typmod);


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
	struct tm	tt,
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
	struct tm	tt,
			   *tm = &tt;
	fsec_t		fsec;
	char	   *tzn = NULL;
	char		buf[MAXDATELEN + 1];

	if (TIMESTAMP_NOT_FINITE(timestamp))
		EncodeSpecialTimestamp(timestamp, buf);
	else if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL) == 0)
		EncodeDateTime(tm, fsec, NULL, &tzn, DateStyle, buf);
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

#ifdef HAVE_INT64_TIMESTAMP
	PG_RETURN_TIMESTAMP((Timestamp) pq_getmsgint64(buf));
#else
	PG_RETURN_TIMESTAMP((Timestamp) pq_getmsgfloat8(buf));
#endif
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
		if ((typmod < 0) || (typmod > MAX_TIMESTAMP_PRECISION))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			  errmsg("timestamp(%d) precision must be between %d and %d",
					 typmod, 0, MAX_TIMESTAMP_PRECISION)));

		/*
		 * Note: this round-to-nearest code is not completely consistent
		 * about rounding values that are exactly halfway between integral
		 * values.	On most platforms, rint() will implement
		 * round-to-nearest-even, but the integer code always rounds up
		 * (away from zero).  Is it worth trying to be consistent?
		 */
#ifdef HAVE_INT64_TIMESTAMP
		if (*time >= INT64CONST(0))
		{
			*time = (((*time + TimestampOffsets[typmod]) / TimestampScales[typmod])
					 * TimestampScales[typmod]);
		}
		else
		{
			*time = -((((-*time) + TimestampOffsets[typmod]) / TimestampScales[typmod])
					  * TimestampScales[typmod]);
		}
#else
		*time = (rint(((double) *time) * TimestampScales[typmod])
				 / TimestampScales[typmod]);
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
	struct tm	tt,
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

/* timestamptz_out()
 * Convert a timestamp to external form.
 */
Datum
timestamptz_out(PG_FUNCTION_ARGS)
{
	TimestampTz dt = PG_GETARG_TIMESTAMPTZ(0);
	char	   *result;
	int			tz;
	struct tm	tt,
			   *tm = &tt;
	fsec_t		fsec;
	char	   *tzn;
	char		buf[MAXDATELEN + 1];

	if (TIMESTAMP_NOT_FINITE(dt))
		EncodeSpecialTimestamp(dt, buf);
	else if (timestamp2tm(dt, &tz, tm, &fsec, &tzn) == 0)
		EncodeDateTime(tm, fsec, &tz, &tzn, DateStyle, buf);
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

#ifdef HAVE_INT64_TIMESTAMP
	PG_RETURN_TIMESTAMPTZ((TimestampTz) pq_getmsgint64(buf));
#else
	PG_RETURN_TIMESTAMPTZ((TimestampTz) pq_getmsgfloat8(buf));
#endif
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
	struct tm	tt,
			   *tm = &tt;
	int			dtype;
	int			nf;
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

	dterr = ParseDateTime(str, workbuf, sizeof(workbuf), field,
						  ftype, MAXDATEFIELDS, &nf);
	if (dterr == 0)
		dterr = DecodeInterval(field, ftype, nf, &dtype, tm, &fsec);
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
			AdjustIntervalForTypmod(result, typmod);
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
	struct tm	tt,
			   *tm = &tt;
	fsec_t		fsec;
	char		buf[MAXDATELEN + 1];

	if (interval2tm(*span, tm, &fsec) != 0)
		elog(ERROR, "could not convert interval to tm");

	if (EncodeInterval(tm, fsec, DateStyle, buf) != 0)
		elog(ERROR, "could not format interval");

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
	Interval   *interval;

	interval = (Interval *) palloc(sizeof(Interval));

#ifdef HAVE_INT64_TIMESTAMP
	interval->time = pq_getmsgint64(buf);
#else
	interval->time = pq_getmsgfloat8(buf);
#endif
	interval->month = pq_getmsgint(buf, sizeof(interval->month));

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
	pq_sendint(&buf, interval->month, sizeof(interval->month));
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
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
	 * Unspecified range and precision? Then not necessary to adjust.
	 * Setting typmod to -1 is the convention for all types.
	 */
	if (typmod != -1)
	{
		int			range = INTERVAL_RANGE(typmod);
		int			precision = INTERVAL_PRECISION(typmod);

		if (range == INTERVAL_FULL_RANGE)
		{
			/* Do nothing... */
		}
		else if (range == INTERVAL_MASK(YEAR))
		{
			interval->month = ((interval->month / 12) * 12);
			interval->time = 0;
		}
		else if (range == INTERVAL_MASK(MONTH))
		{
			interval->month %= 12;
			interval->time = 0;
		}
		/* YEAR TO MONTH */
		else if (range == (INTERVAL_MASK(YEAR) | INTERVAL_MASK(MONTH)))
			interval->time = 0;
		else if (range == INTERVAL_MASK(DAY))
		{
			interval->month = 0;
#ifdef HAVE_INT64_TIMESTAMP
			interval->time = (((int) (interval->time / INT64CONST(86400000000)))
							  * INT64CONST(86400000000));
#else
			interval->time = (((int) (interval->time / 86400)) * 86400);
#endif
		}
		else if (range == INTERVAL_MASK(HOUR))
		{
#ifdef HAVE_INT64_TIMESTAMP
			int64		day;

#else
			double		day;
#endif

			interval->month = 0;
#ifdef HAVE_INT64_TIMESTAMP
			day = (interval->time / INT64CONST(86400000000));
			interval->time -= (day * INT64CONST(86400000000));
			interval->time = ((interval->time / INT64CONST(3600000000))
							  * INT64CONST(3600000000));
#else
			TMODULO(interval->time, day, 86400.0);
			interval->time = (((int) (interval->time / 3600)) * 3600.0);
#endif
		}
		else if (range == INTERVAL_MASK(MINUTE))
		{
#ifdef HAVE_INT64_TIMESTAMP
			int64		hour;

#else
			double		hour;
#endif

			interval->month = 0;
#ifdef HAVE_INT64_TIMESTAMP
			hour = (interval->time / INT64CONST(3600000000));
			interval->time -= (hour * INT64CONST(3600000000));
			interval->time = ((interval->time / INT64CONST(60000000))
							  * INT64CONST(60000000));
#else
			TMODULO(interval->time, hour, 3600.0);
			interval->time = (((int) (interval->time / 60)) * 60);
#endif
		}
		else if (range == INTERVAL_MASK(SECOND))
		{
#ifdef HAVE_INT64_TIMESTAMP
			int64		minute;

#else
			double		minute;
#endif

			interval->month = 0;
#ifdef HAVE_INT64_TIMESTAMP
			minute = (interval->time / INT64CONST(60000000));
			interval->time -= (minute * INT64CONST(60000000));
#else
			TMODULO(interval->time, minute, 60.0);
/*			interval->time = (int)(interval->time); */
#endif
		}
		/* DAY TO HOUR */
		else if (range == (INTERVAL_MASK(DAY) |
						   INTERVAL_MASK(HOUR)))
		{
			interval->month = 0;
#ifdef HAVE_INT64_TIMESTAMP
			interval->time = ((interval->time / INT64CONST(3600000000))
							  * INT64CONST(3600000000));
#else
			interval->time = (((int) (interval->time / 3600)) * 3600);
#endif
		}
		/* DAY TO MINUTE */
		else if (range == (INTERVAL_MASK(DAY) |
						   INTERVAL_MASK(HOUR) |
						   INTERVAL_MASK(MINUTE)))
		{
			interval->month = 0;
#ifdef HAVE_INT64_TIMESTAMP
			interval->time = ((interval->time / INT64CONST(60000000))
							  * INT64CONST(60000000));
#else
			interval->time = (((int) (interval->time / 60)) * 60);
#endif
		}
		/* DAY TO SECOND */
		else if (range == (INTERVAL_MASK(DAY) |
						   INTERVAL_MASK(HOUR) |
						   INTERVAL_MASK(MINUTE) |
						   INTERVAL_MASK(SECOND)))
			interval->month = 0;
		/* HOUR TO MINUTE */
		else if (range == (INTERVAL_MASK(HOUR) |
						   INTERVAL_MASK(MINUTE)))
		{
#ifdef HAVE_INT64_TIMESTAMP
			int64		day;

#else
			double		day;
#endif

			interval->month = 0;
#ifdef HAVE_INT64_TIMESTAMP
			day = (interval->time / INT64CONST(86400000000));
			interval->time -= (day * INT64CONST(86400000000));
			interval->time = ((interval->time / INT64CONST(60000000))
							  * INT64CONST(60000000));
#else
			TMODULO(interval->time, day, 86400.0);
			interval->time = (((int) (interval->time / 60)) * 60);
#endif
		}
		/* HOUR TO SECOND */
		else if (range == (INTERVAL_MASK(HOUR) |
						   INTERVAL_MASK(MINUTE) |
						   INTERVAL_MASK(SECOND)))
		{
#ifdef HAVE_INT64_TIMESTAMP
			int64		day;

#else
			double		day;
#endif

			interval->month = 0;
#ifdef HAVE_INT64_TIMESTAMP
			day = (interval->time / INT64CONST(86400000000));
			interval->time -= (day * INT64CONST(86400000000));
#else
			TMODULO(interval->time, day, 86400.0);
#endif
		}
		/* MINUTE TO SECOND */
		else if (range == (INTERVAL_MASK(MINUTE) |
						   INTERVAL_MASK(SECOND)))
		{
#ifdef HAVE_INT64_TIMESTAMP
			int64		hour;

#else
			double		hour;
#endif

			interval->month = 0;
#ifdef HAVE_INT64_TIMESTAMP
			hour = (interval->time / INT64CONST(3600000000));
			interval->time -= (hour * INT64CONST(3600000000));
#else
			TMODULO(interval->time, hour, 3600.0);
#endif
		}
		else
			elog(ERROR, "unrecognized interval typmod: %d", typmod);

		/* Need to adjust precision? If not, don't even try! */
		if (precision != INTERVAL_FULL_PRECISION)
		{
			if ((precision < 0) || (precision > MAX_INTERVAL_PRECISION))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("interval(%d) precision must be between %d and %d",
					   precision, 0, MAX_INTERVAL_PRECISION)));

			/*
			 * Note: this round-to-nearest code is not completely
			 * consistent about rounding values that are exactly halfway
			 * between integral values.  On most platforms, rint() will
			 * implement round-to-nearest-even, but the integer code
			 * always rounds up (away from zero).  Is it worth trying to
			 * be consistent?
			 */
#ifdef HAVE_INT64_TIMESTAMP
			if (interval->time >= INT64CONST(0))
			{
				interval->time = (((interval->time + IntervalOffsets[precision]) / IntervalScales[precision])
								  * IntervalScales[precision]);
			}
			else
			{
				interval->time = -(((-interval->time + IntervalOffsets[precision]) / IntervalScales[precision])
								   * IntervalScales[precision]);
			}
#else
			interval->time = (rint(((double) interval->time) * IntervalScales[precision])
							  / IntervalScales[precision]);
#endif
		}
	}

	return;
}


/* EncodeSpecialTimestamp()
 * Convert reserved timestamp data type to string.
 */
static int
EncodeSpecialTimestamp(Timestamp dt, char *str)
{
	if (TIMESTAMP_IS_NOBEGIN(dt))
		strcpy(str, EARLY);
	else if (TIMESTAMP_IS_NOEND(dt))
		strcpy(str, LATE);
	else
		return FALSE;

	return TRUE;
}	/* EncodeSpecialTimestamp() */

Datum
now(PG_FUNCTION_ARGS)
{
	TimestampTz result;
	AbsoluteTime sec;
	int			usec;

	sec = GetCurrentTransactionStartTimeUsec(&usec);

	result = AbsoluteTimeUsecToTimestampTz(sec, usec);

	PG_RETURN_TIMESTAMPTZ(result);
}

void
dt2time(Timestamp jd, int *hour, int *min, int *sec, fsec_t *fsec)
{
#ifdef HAVE_INT64_TIMESTAMP
	int64		time;

#else
	double		time;
#endif

	time = jd;

#ifdef HAVE_INT64_TIMESTAMP
	*hour = (time / INT64CONST(3600000000));
	time -= ((*hour) * INT64CONST(3600000000));
	*min = (time / INT64CONST(60000000));
	time -= ((*min) * INT64CONST(60000000));
	*sec = (time / INT64CONST(1000000));
	*fsec = (time - (*sec * INT64CONST(1000000)));
#else
	*hour = (time / 3600);
	time -= ((*hour) * 3600);
	*min = (time / 60);
	time -= ((*min) * 60);
	*sec = time;
	*fsec = JROUND(time - *sec);
#endif

	return;
}	/* dt2time() */


/* timestamp2tm()
 * Convert timestamp data type to POSIX time structure.
 * Note that year is _not_ 1900-based, but is an explicit full value.
 * Also, month is one-based, _not_ zero-based.
 * Returns:
 *	 0 on success
 *	-1 on out of range
 *
 * For dates within the system-supported time_t range, convert to the
 *	local time zone. If out of this range, leave as GMT. - tgl 97/05/27
 */
int
timestamp2tm(Timestamp dt, int *tzp, struct tm *tm, fsec_t *fsec, char **tzn)
{
#ifdef HAVE_INT64_TIMESTAMP
	int			date;
	int64		time;

#else
	double		date;
	double		time;
#endif
	time_t		utime;

#if defined(HAVE_TM_ZONE) || defined(HAVE_INT_TIMEZONE)
	struct tm  *tx;
#endif

	/*
	 * If HasCTZSet is true then we have a brute force time zone
	 * specified. Go ahead and rotate to the local time zone since we will
	 * later bypass any calls which adjust the tm fields.
	 */
	if (HasCTZSet && (tzp != NULL))
	{
#ifdef HAVE_INT64_TIMESTAMP
		dt -= (CTimeZone * INT64CONST(1000000));
#else
		dt -= CTimeZone;
#endif
	}

	time = dt;
#ifdef HAVE_INT64_TIMESTAMP
	TMODULO(time, date, INT64CONST(86400000000));

	if (time < INT64CONST(0))
	{
		time += INT64CONST(86400000000);
		date -= 1;
	}
#else
	TMODULO(time, date, 86400e0);

	if (time < 0)
	{
		time += 86400;
		date -= 1;
	}
#endif

	/* Julian day routine does not work for negative Julian days */
	if (date < -POSTGRES_EPOCH_JDATE)
		return -1;

	/* add offset to go from J2000 back to standard Julian date */
	date += POSTGRES_EPOCH_JDATE;

	j2date((int) date, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
	dt2time(time, &tm->tm_hour, &tm->tm_min, &tm->tm_sec, fsec);

	if (tzp != NULL)
	{
		/*
		 * We have a brute force time zone per SQL99? Then use it without
		 * change since we have already rotated to the time zone.
		 */
		if (HasCTZSet)
		{
			*tzp = CTimeZone;
			tm->tm_isdst = 0;
#if defined(HAVE_TM_ZONE)
			tm->tm_gmtoff = CTimeZone;
			tm->tm_zone = NULL;
#endif
			if (tzn != NULL)
				*tzn = NULL;
		}

		/*
		 * Does this fall within the capabilities of the localtime()
		 * interface? Then use this to rotate to the local time zone.
		 */
		else if (IS_VALID_UTIME(tm->tm_year, tm->tm_mon, tm->tm_mday))
		{
			/*
			 * Convert to integer, avoiding platform-specific
			 * roundoff-in-wrong-direction errors, and adjust to
			 * Unix epoch.  Note we have to do this in one step
			 * because the intermediate result before adjustment
			 * won't necessarily fit in an int32.
			 */
#ifdef HAVE_INT64_TIMESTAMP
			utime = (dt - *fsec) / INT64CONST(1000000) +
				(POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * 86400;
#else
			utime = rint(dt - *fsec +
						 (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * 86400);
#endif

#if defined(HAVE_TM_ZONE) || defined(HAVE_INT_TIMEZONE)
			tx = localtime(&utime);
			tm->tm_year = tx->tm_year + 1900;
			tm->tm_mon = tx->tm_mon + 1;
			tm->tm_mday = tx->tm_mday;
			tm->tm_hour = tx->tm_hour;
			tm->tm_min = tx->tm_min;
#if NOT_USED
/* XXX HACK
 * Argh! My Linux box puts in a 1 second offset for dates less than 1970
 *	but only if the seconds field was non-zero. So, don't copy the seconds
 *	field and instead carry forward from the original - thomas 97/06/18
 * Note that Linux uses the standard freeware zic package as do
 *	many other platforms so this may not be Linux/ix86-specific.
 * Still shows a problem on my up to date Linux box - thomas 2001-01-17
 */
			tm->tm_sec = tx->tm_sec;
#endif
			tm->tm_isdst = tx->tm_isdst;

#if defined(HAVE_TM_ZONE)
			tm->tm_gmtoff = tx->tm_gmtoff;
			tm->tm_zone = tx->tm_zone;

			*tzp = -(tm->tm_gmtoff);	/* tm_gmtoff is Sun/DEC-ism */
			if (tzn != NULL)
				*tzn = (char *) tm->tm_zone;
#elif defined(HAVE_INT_TIMEZONE)
			*tzp = ((tm->tm_isdst > 0) ? (TIMEZONE_GLOBAL - 3600) : TIMEZONE_GLOBAL);
			if (tzn != NULL)
				*tzn = tzname[(tm->tm_isdst > 0)];
#endif

#else							/* not (HAVE_TM_ZONE || HAVE_INT_TIMEZONE) */
			*tzp = 0;
			/* Mark this as *no* time zone available */
			tm->tm_isdst = -1;
			if (tzn != NULL)
				*tzn = NULL;
#endif
		}
		else
		{
			*tzp = 0;
			/* Mark this as *no* time zone available */
			tm->tm_isdst = -1;
			if (tzn != NULL)
				*tzn = NULL;
		}
	}
	else
	{
		tm->tm_isdst = -1;
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
tm2timestamp(struct tm * tm, fsec_t fsec, int *tzp, Timestamp *result)
{
#ifdef HAVE_INT64_TIMESTAMP
	int			date;
	int64		time;

#else
	double		date,
				time;
#endif

	/* Julian day routines are not correct for negative Julian days */
	if (!IS_VALID_JULIAN(tm->tm_year, tm->tm_mon, tm->tm_mday))
		return -1;

	date = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - POSTGRES_EPOCH_JDATE;
	time = time2t(tm->tm_hour, tm->tm_min, tm->tm_sec, fsec);
#ifdef HAVE_INT64_TIMESTAMP
	*result = (date * INT64CONST(86400000000)) + time;
	/* check for major overflow */
	if ((*result - time) / INT64CONST(86400000000) != date)
		return -1;
	/* check for just-barely overflow (okay except time-of-day wraps) */
	if ((*result < 0) ? (date >= 0) : (date < 0))
		return -1;
#else
	*result = ((date * 86400) + time);
#endif
	if (tzp != NULL)
		*result = dt2local(*result, -(*tzp));

	return 0;
}


/* interval2tm()
 * Convert a interval data type to a tm structure.
 */
int
interval2tm(Interval span, struct tm * tm, fsec_t *fsec)
{
#ifdef HAVE_INT64_TIMESTAMP
	int64		time;

#else
	double		time;
#endif

	if (span.month != 0)
	{
		tm->tm_year = span.month / 12;
		tm->tm_mon = span.month % 12;

	}
	else
	{
		tm->tm_year = 0;
		tm->tm_mon = 0;
	}

	time = span.time;

#ifdef HAVE_INT64_TIMESTAMP
	tm->tm_mday = (time / INT64CONST(86400000000));
	time -= (tm->tm_mday * INT64CONST(86400000000));
	tm->tm_hour = (time / INT64CONST(3600000000));
	time -= (tm->tm_hour * INT64CONST(3600000000));
	tm->tm_min = (time / INT64CONST(60000000));
	time -= (tm->tm_min * INT64CONST(60000000));
	tm->tm_sec = (time / INT64CONST(1000000));
	*fsec = (time - (tm->tm_sec * INT64CONST(1000000)));
#else
	TMODULO(time, tm->tm_mday, 86400e0);
	TMODULO(time, tm->tm_hour, 3600e0);
	TMODULO(time, tm->tm_min, 60e0);
	TMODULO(time, tm->tm_sec, 1e0);
	*fsec = time;
#endif

	return 0;
}

int
tm2interval(struct tm * tm, fsec_t fsec, Interval *span)
{
	span->month = ((tm->tm_year * 12) + tm->tm_mon);
#ifdef HAVE_INT64_TIMESTAMP
	span->time = ((((((((tm->tm_mday * INT64CONST(24))
						+ tm->tm_hour) * INT64CONST(60))
					  + tm->tm_min) * INT64CONST(60))
					+ tm->tm_sec) * INT64CONST(1000000)) + fsec);
#else
	span->time = ((((((tm->tm_mday * 24.0)
					  + tm->tm_hour) * 60.0)
					+ tm->tm_min) * 60.0)
				  + tm->tm_sec);
	span->time = JROUND(span->time + fsec);
#endif

	return 0;
}

#ifdef HAVE_INT64_TIMESTAMP
static int64
time2t(const int hour, const int min, const int sec, const fsec_t fsec)
{
	return ((((((hour * 60) + min) * 60) + sec) * INT64CONST(1000000)) + fsec);
}	/* time2t() */

#else
static double
time2t(const int hour, const int min, const int sec, const fsec_t fsec)
{
	return ((((hour * 60) + min) * 60) + sec + fsec);
}	/* time2t() */
#endif

static Timestamp
dt2local(Timestamp dt, int tz)
{
#ifdef HAVE_INT64_TIMESTAMP
	dt -= (tz * INT64CONST(1000000));
#else
	dt -= tz;
	dt = JROUND(dt);
#endif
	return dt;
}	/* dt2local() */


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
GetEpochTime(struct tm * tm)
{
	struct tm  *t0;
	time_t		epoch = 0;

	t0 = gmtime(&epoch);

	tm->tm_year = t0->tm_year;
	tm->tm_mon = t0->tm_mon;
	tm->tm_mday = t0->tm_mday;
	tm->tm_hour = t0->tm_hour;
	tm->tm_min = t0->tm_min;
	tm->tm_sec = t0->tm_sec;

	if (tm->tm_year < 1900)
		tm->tm_year += 1900;
	tm->tm_mon++;

	return;
}	/* GetEpochTime() */

Timestamp
SetEpochTimestamp(void)
{
	Timestamp	dt;
	struct tm	tt,
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
static int
timestamp_cmp_internal(Timestamp dt1, Timestamp dt2)
{
#ifdef HAVE_INT64_TIMESTAMP
	return ((dt1 < dt2) ? -1 : ((dt1 > dt2) ? 1 : 0));
#else

	/*
	 * When using float representation, we have to be wary of NaNs.
	 *
	 * We consider all NANs to be equal and larger than any non-NAN. This is
	 * somewhat arbitrary; the important thing is to have a consistent
	 * sort order.
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


/*
 *		interval_relop	- is interval1 relop interval2
 *
 *		collate invalid interval at the end
 */
static int
interval_cmp_internal(Interval *interval1, Interval *interval2)
{
#ifdef HAVE_INT64_TIMESTAMP
	int64		span1,
				span2;

#else
	double		span1,
				span2;
#endif

	span1 = interval1->time;
	span2 = interval2->time;

#ifdef HAVE_INT64_TIMESTAMP
	if (interval1->month != 0)
		span1 += ((interval1->month * INT64CONST(30) * INT64CONST(86400000000)));
	if (interval2->month != 0)
		span2 += ((interval2->month * INT64CONST(30) * INT64CONST(86400000000)));
#else
	if (interval1->month != 0)
		span1 += (interval1->month * (30.0 * 86400));
	if (interval2->month != 0)
		span2 += (interval2->month * (30.0 * 86400));
#endif

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
 * interval, being an unusual size, needs a specialized hash function.
 */
Datum
interval_hash(PG_FUNCTION_ARGS)
{
	Interval   *key = PG_GETARG_INTERVAL_P(0);

	/*
	 * Specify hash length as sizeof(double) + sizeof(int4), not as
	 * sizeof(Interval), so that any garbage pad bytes in the structure
	 * won't be included in the hash!
	 */
	return hash_any((unsigned char *) key, sizeof(key->time) + sizeof(key->month));
}

/* overlaps_timestamp() --- implements the SQL92 OVERLAPS operator.
 *
 * Algorithm is per SQL92 spec.  This is much harder than you'd think
 * because the spec requires us to deliver a non-null answer in some cases
 * where some of the inputs are null.
 */
Datum
overlaps_timestamp(PG_FUNCTION_ARGS)
{
	/*
	 * The arguments are Timestamps, but we leave them as generic Datums
	 * to avoid unnecessary conversions between value and reference forms
	 * --- not to mention possible dereferences of null pointers.
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
	 * If both endpoints of interval 1 are null, the result is null
	 * (unknown). If just one endpoint is null, take ts1 as the non-null
	 * one. Otherwise, take ts1 as the lesser endpoint.
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
		 * This case is ts1 < te2 OR te1 < te2, which may look redundant
		 * but in the presence of nulls it's not quite completely so.
		 */
		if (te2IsNull)
			PG_RETURN_NULL();
		if (TIMESTAMP_LT(ts1, te2))
			PG_RETURN_BOOL(true);
		if (te1IsNull)
			PG_RETURN_NULL();

		/*
		 * If te1 is not null then we had ts1 <= te1 above, and we just
		 * found ts1 >= te2, hence te1 >= te2.
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
		 * If te2 is not null then we had ts2 <= te2 above, and we just
		 * found ts2 >= te1, hence te2 >= te1.
		 */
		PG_RETURN_BOOL(false);
	}
	else
	{
		/*
		 * For ts1 = ts2 the spec says te1 <> te2 OR te1 = te2, which is a
		 * rather silly way of saying "true if both are nonnull, else
		 * null".
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
	{
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("cannot subtract infinite timestamps")));

		result->time = 0;
	}
	else
#ifdef HAVE_INT64_TIMESTAMP
		result->time = (dt1 - dt2);
#else
		result->time = JROUND(dt1 - dt2);
#endif

	result->month = 0;

	PG_RETURN_INTERVAL_P(result);
}


/* timestamp_pl_span()
 * Add a interval to a timestamp data type.
 * Note that interval has provisions for qualitative year/month
 *	units, so try to do the right thing with them.
 * To add a month, increment the month, and use the same day of month.
 * Then, if the next month has fewer days, set the day of month
 *	to the last day of month.
 * Lastly, add in the "quantitative time".
 */
Datum
timestamp_pl_span(PG_FUNCTION_ARGS)
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
			struct tm	tt,
					   *tm = &tt;
			fsec_t		fsec;

			if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));

			tm->tm_mon += span->month;
			if (tm->tm_mon > 12)
			{
				tm->tm_year += ((tm->tm_mon - 1) / 12);
				tm->tm_mon = (((tm->tm_mon - 1) % 12) + 1);
			}
			else if (tm->tm_mon < 1)
			{
				tm->tm_year += ((tm->tm_mon / 12) - 1);
				tm->tm_mon = ((tm->tm_mon % 12) + 12);
			}

			/* adjust for end of month boundary problems... */
			if (tm->tm_mday > day_tab[isleap(tm->tm_year)][tm->tm_mon - 1])
				tm->tm_mday = (day_tab[isleap(tm->tm_year)][tm->tm_mon - 1]);

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
timestamp_mi_span(PG_FUNCTION_ARGS)
{
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(0);
	Interval   *span = PG_GETARG_INTERVAL_P(1);
	Interval	tspan;

	tspan.month = -span->month;
	tspan.time = -span->time;

	return DirectFunctionCall2(timestamp_pl_span,
							   TimestampGetDatum(timestamp),
							   PointerGetDatum(&tspan));
}


/* timestamptz_pl_span()
 * Add a interval to a timestamp with time zone data type.
 * Note that interval has provisions for qualitative year/month
 *	units, so try to do the right thing with them.
 * To add a month, increment the month, and use the same day of month.
 * Then, if the next month has fewer days, set the day of month
 *	to the last day of month.
 * Lastly, add in the "quantitative time".
 */
Datum
timestamptz_pl_span(PG_FUNCTION_ARGS)
{
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(0);
	Interval   *span = PG_GETARG_INTERVAL_P(1);
	TimestampTz result;
	int			tz;
	char	   *tzn;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		result = timestamp;
	else
	{
		if (span->month != 0)
		{
			struct tm	tt,
					   *tm = &tt;
			fsec_t		fsec;

			if (timestamp2tm(timestamp, &tz, tm, &fsec, &tzn) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));

			tm->tm_mon += span->month;
			if (tm->tm_mon > 12)
			{
				tm->tm_year += ((tm->tm_mon - 1) / 12);
				tm->tm_mon = (((tm->tm_mon - 1) % 12) + 1);
			}
			else if (tm->tm_mon < 1)
			{
				tm->tm_year += ((tm->tm_mon / 12) - 1);
				tm->tm_mon = ((tm->tm_mon % 12) + 12);
			}

			/* adjust for end of month boundary problems... */
			if (tm->tm_mday > day_tab[isleap(tm->tm_year)][tm->tm_mon - 1])
				tm->tm_mday = (day_tab[isleap(tm->tm_year)][tm->tm_mon - 1]);

			tz = DetermineLocalTimeZone(tm);

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
timestamptz_mi_span(PG_FUNCTION_ARGS)
{
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(0);
	Interval   *span = PG_GETARG_INTERVAL_P(1);
	Interval	tspan;

	tspan.month = -span->month;
	tspan.time = -span->time;

	return DirectFunctionCall2(timestamptz_pl_span,
							   TimestampGetDatum(timestamp),
							   PointerGetDatum(&tspan));
}


Datum
interval_um(PG_FUNCTION_ARGS)
{
	Interval   *interval = PG_GETARG_INTERVAL_P(0);
	Interval   *result;

	result = (Interval *) palloc(sizeof(Interval));

	result->time = -(interval->time);
	result->month = -(interval->month);

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

	result->month = (span1->month + span2->month);
#ifdef HAVE_INT64_TIMESTAMP
	result->time = (span1->time + span2->time);
#else
	result->time = JROUND(span1->time + span2->time);
#endif

	PG_RETURN_INTERVAL_P(result);
}

Datum
interval_mi(PG_FUNCTION_ARGS)
{
	Interval   *span1 = PG_GETARG_INTERVAL_P(0);
	Interval   *span2 = PG_GETARG_INTERVAL_P(1);
	Interval   *result;

	result = (Interval *) palloc(sizeof(Interval));

	result->month = (span1->month - span2->month);
#ifdef HAVE_INT64_TIMESTAMP
	result->time = (span1->time - span2->time);
#else
	result->time = JROUND(span1->time - span2->time);
#endif

	PG_RETURN_INTERVAL_P(result);
}

Datum
interval_mul(PG_FUNCTION_ARGS)
{
	Interval   *span1 = PG_GETARG_INTERVAL_P(0);
	float8		factor = PG_GETARG_FLOAT8(1);
	Interval   *result;

#ifdef HAVE_INT64_TIMESTAMP
	int64		months;

#else
	double		months;
#endif

	result = (Interval *) palloc(sizeof(Interval));

	months = (span1->month * factor);
#ifdef HAVE_INT64_TIMESTAMP
	result->month = months;
	result->time = (span1->time * factor);
	result->time += ((months - result->month) * INT64CONST(30)
					 * INT64CONST(86400000000));
#else
	result->month = rint(months);
	result->time = JROUND(span1->time * factor);
	/* evaluate fractional months as 30 days */
	result->time += JROUND((months - result->month) * 30 * 86400);
#endif

	PG_RETURN_INTERVAL_P(result);
}

Datum
mul_d_interval(PG_FUNCTION_ARGS)
{
	/* Args are float8 and Interval *, but leave them as generic Datum */
	Datum		factor = PG_GETARG_DATUM(0);
	Datum		span1 = PG_GETARG_DATUM(1);

	return DirectFunctionCall2(interval_mul, span1, factor);
}

Datum
interval_div(PG_FUNCTION_ARGS)
{
	Interval   *span = PG_GETARG_INTERVAL_P(0);
	float8		factor = PG_GETARG_FLOAT8(1);
	Interval   *result;

#ifndef HAVE_INT64_TIMESTAMP
	double		months;
#endif

	result = (Interval *) palloc(sizeof(Interval));

	if (factor == 0.0)
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));

#ifdef HAVE_INT64_TIMESTAMP
	result->month = (span->month / factor);
	result->time = (span->time / factor);
	/* evaluate fractional months as 30 days */
	result->time += (((span->month - (result->month * factor))
				   * INT64CONST(30) * INT64CONST(86400000000)) / factor);
#else
	months = (span->month / factor);
	result->month = rint(months);
	result->time = JROUND(span->time / factor);
	/* evaluate fractional months as 30 days */
	result->time += JROUND((months - result->month) * 30 * 86400);
#endif

	PG_RETURN_INTERVAL_P(result);
}

/*
 * interval_accum and interval_avg implement the AVG(interval) aggregate.
 *
 * The transition datatype for this aggregate is a 2-element array of
 * intervals, where the first is the running sum and the second contains
 * the number of values so far in its 'time' field.  This is a bit ugly
 * but it beats inventing a specialized datatype for the purpose.
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

	/* We assume the input is array of interval */
	deconstruct_array(transarray,
					  INTERVALOID, 12, false, 'd',
					  &transdatums, &ndatums);
	if (ndatums != 2)
		elog(ERROR, "expected 2-element interval array");

	/*
	 * XXX memcpy, instead of just extracting a pointer, to work around
	 * buggy array code: it won't ensure proper alignment of Interval
	 * objects on machines where double requires 8-byte alignment. That
	 * should be fixed, but in the meantime...
	 *
	 * Note: must use DatumGetPointer here, not DatumGetIntervalP, else some
	 * compilers optimize into double-aligned load/store anyway.
	 */
	memcpy((void *) &sumX, DatumGetPointer(transdatums[0]), sizeof(Interval));
	memcpy((void *) &N, DatumGetPointer(transdatums[1]), sizeof(Interval));

	newsum = DatumGetIntervalP(DirectFunctionCall2(interval_pl,
												IntervalPGetDatum(&sumX),
											 IntervalPGetDatum(newval)));
	N.time += 1;

	transdatums[0] = IntervalPGetDatum(newsum);
	transdatums[1] = IntervalPGetDatum(&N);

	result = construct_array(transdatums, 2,
							 INTERVALOID, 12, false, 'd');

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

	/* We assume the input is array of interval */
	deconstruct_array(transarray,
					  INTERVALOID, 12, false, 'd',
					  &transdatums, &ndatums);
	if (ndatums != 2)
		elog(ERROR, "expected 2-element interval array");

	/*
	 * XXX memcpy, instead of just extracting a pointer, to work around
	 * buggy array code: it won't ensure proper alignment of Interval
	 * objects on machines where double requires 8-byte alignment. That
	 * should be fixed, but in the meantime...
	 *
	 * Note: must use DatumGetPointer here, not DatumGetIntervalP, else some
	 * compilers optimize into double-aligned load/store anyway.
	 */
	memcpy((void *) &sumX, DatumGetPointer(transdatums[0]), sizeof(Interval));
	memcpy((void *) &N, DatumGetPointer(transdatums[1]), sizeof(Interval));

	/* SQL92 defines AVG of no values to be NULL */
	if (N.time == 0)
		PG_RETURN_NULL();

	return DirectFunctionCall2(interval_div,
							   IntervalPGetDatum(&sumX),
							   Float8GetDatum(N.time));
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
	struct tm	tt,
			   *tm = &tt;
	struct tm	tt1,
			   *tm1 = &tt1;
	struct tm	tt2,
			   *tm2 = &tt2;

	result = (Interval *) palloc(sizeof(Interval));

	if ((timestamp2tm(dt1, NULL, tm1, &fsec1, NULL) == 0)
		&& (timestamp2tm(dt2, NULL, tm2, &fsec2, NULL) == 0))
	{
		fsec = (fsec1 - fsec2);
		tm->tm_sec = (tm1->tm_sec - tm2->tm_sec);
		tm->tm_min = (tm1->tm_min - tm2->tm_min);
		tm->tm_hour = (tm1->tm_hour - tm2->tm_hour);
		tm->tm_mday = (tm1->tm_mday - tm2->tm_mday);
		tm->tm_mon = (tm1->tm_mon - tm2->tm_mon);
		tm->tm_year = (tm1->tm_year - tm2->tm_year);

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

		while (tm->tm_sec < 0)
		{
			tm->tm_sec += 60;
			tm->tm_min--;
		}

		while (tm->tm_min < 0)
		{
			tm->tm_min += 60;
			tm->tm_hour--;
		}

		while (tm->tm_hour < 0)
		{
			tm->tm_hour += 24;
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
			tm->tm_mon += 12;
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
	struct tm	tt,
			   *tm = &tt;
	struct tm	tt1,
			   *tm1 = &tt1;
	struct tm	tt2,
			   *tm2 = &tt2;
	int			tz1;
	int			tz2;
	char	   *tzn;

	result = (Interval *) palloc(sizeof(Interval));

	if ((timestamp2tm(dt1, &tz1, tm1, &fsec1, &tzn) == 0)
		&& (timestamp2tm(dt2, &tz2, tm2, &fsec2, &tzn) == 0))
	{
		fsec = (fsec1 - fsec2);
		tm->tm_sec = (tm1->tm_sec - tm2->tm_sec);
		tm->tm_min = (tm1->tm_min - tm2->tm_min);
		tm->tm_hour = (tm1->tm_hour - tm2->tm_hour);
		tm->tm_mday = (tm1->tm_mday - tm2->tm_mday);
		tm->tm_mon = (tm1->tm_mon - tm2->tm_mon);
		tm->tm_year = (tm1->tm_year - tm2->tm_year);

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

		while (tm->tm_sec < 0)
		{
			tm->tm_sec += 60;
			tm->tm_min--;
		}

		while (tm->tm_min < 0)
		{
			tm->tm_min += 60;
			tm->tm_hour--;
		}

		while (tm->tm_hour < 0)
		{
			tm->tm_hour += 24;
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
			tm->tm_mon += 12;
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


/* timestamp_text()
 * Convert timestamp to text data type.
 */
Datum
timestamp_text(PG_FUNCTION_ARGS)
{
	/* Input is a Timestamp, but may as well leave it in Datum form */
	Datum		timestamp = PG_GETARG_DATUM(0);
	text	   *result;
	char	   *str;
	int			len;

	str = DatumGetCString(DirectFunctionCall1(timestamp_out, timestamp));

	len = (strlen(str) + VARHDRSZ);

	result = palloc(len);

	VARATT_SIZEP(result) = len;
	memmove(VARDATA(result), str, (len - VARHDRSZ));

	pfree(str);

	PG_RETURN_TEXT_P(result);
}


/* text_timestamp()
 * Convert text string to timestamp.
 * Text type is not null terminated, so use temporary string
 *	then call the standard input routine.
 */
Datum
text_timestamp(PG_FUNCTION_ARGS)
{
	text	   *str = PG_GETARG_TEXT_P(0);
	int			i;
	char	   *sp,
			   *dp,
				dstr[MAXDATELEN + 1];

	if (VARSIZE(str) - VARHDRSZ > MAXDATELEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
				 errmsg("invalid input syntax for type timestamp: \"%s\"",
						DatumGetCString(DirectFunctionCall1(textout,
											   PointerGetDatum(str))))));

	sp = VARDATA(str);
	dp = dstr;
	for (i = 0; i < (VARSIZE(str) - VARHDRSZ); i++)
		*dp++ = *sp++;
	*dp = '\0';

	return DirectFunctionCall3(timestamp_in,
							   CStringGetDatum(dstr),
							   ObjectIdGetDatum(InvalidOid),
							   Int32GetDatum(-1));
}


/* timestamptz_text()
 * Convert timestamp with time zone to text data type.
 */
Datum
timestamptz_text(PG_FUNCTION_ARGS)
{
	/* Input is a Timestamp, but may as well leave it in Datum form */
	Datum		timestamp = PG_GETARG_DATUM(0);
	text	   *result;
	char	   *str;
	int			len;

	str = DatumGetCString(DirectFunctionCall1(timestamptz_out, timestamp));

	len = (strlen(str) + VARHDRSZ);

	result = palloc(len);

	VARATT_SIZEP(result) = len;
	memmove(VARDATA(result), str, (len - VARHDRSZ));

	pfree(str);

	PG_RETURN_TEXT_P(result);
}

/* text_timestamptz()
 * Convert text string to timestamp with time zone.
 * Text type is not null terminated, so use temporary string
 *	then call the standard input routine.
 */
Datum
text_timestamptz(PG_FUNCTION_ARGS)
{
	text	   *str = PG_GETARG_TEXT_P(0);
	int			i;
	char	   *sp,
			   *dp,
				dstr[MAXDATELEN + 1];

	if (VARSIZE(str) - VARHDRSZ > MAXDATELEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
				 errmsg("invalid input syntax for type timestamp with time zone: \"%s\"",
						DatumGetCString(DirectFunctionCall1(textout,
											   PointerGetDatum(str))))));

	sp = VARDATA(str);
	dp = dstr;
	for (i = 0; i < (VARSIZE(str) - VARHDRSZ); i++)
		*dp++ = *sp++;
	*dp = '\0';

	return DirectFunctionCall3(timestamptz_in,
							   CStringGetDatum(dstr),
							   ObjectIdGetDatum(InvalidOid),
							   Int32GetDatum(-1));
}


/* interval_text()
 * Convert interval to text data type.
 */
Datum
interval_text(PG_FUNCTION_ARGS)
{
	Interval   *interval = PG_GETARG_INTERVAL_P(0);
	text	   *result;
	char	   *str;
	int			len;

	str = DatumGetCString(DirectFunctionCall1(interval_out,
										   IntervalPGetDatum(interval)));

	len = (strlen(str) + VARHDRSZ);

	result = palloc(len);

	VARATT_SIZEP(result) = len;
	memmove(VARDATA(result), str, (len - VARHDRSZ));

	pfree(str);

	PG_RETURN_TEXT_P(result);
}


/* text_interval()
 * Convert text string to interval.
 * Text type may not be null terminated, so copy to temporary string
 *	then call the standard input routine.
 */
Datum
text_interval(PG_FUNCTION_ARGS)
{
	text	   *str = PG_GETARG_TEXT_P(0);
	int			i;
	char	   *sp,
			   *dp,
				dstr[MAXDATELEN + 1];

	if (VARSIZE(str) - VARHDRSZ > MAXDATELEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
				 errmsg("invalid input syntax for type interval: \"%s\"",
						DatumGetCString(DirectFunctionCall1(textout,
											   PointerGetDatum(str))))));

	sp = VARDATA(str);
	dp = dstr;
	for (i = 0; i < (VARSIZE(str) - VARHDRSZ); i++)
		*dp++ = *sp++;
	*dp = '\0';

	return DirectFunctionCall3(interval_in,
							   CStringGetDatum(dstr),
							   ObjectIdGetDatum(InvalidOid),
							   Int32GetDatum(-1));
}

/* timestamp_trunc()
 * Truncate timestamp to specified units.
 */
Datum
timestamp_trunc(PG_FUNCTION_ARGS)
{
	text	   *units = PG_GETARG_TEXT_P(0);
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(1);
	Timestamp	result;
	int			type,
				val;
	int			i;
	char	   *up,
			   *lp,
				lowunits[MAXDATELEN + 1];
	fsec_t		fsec;
	struct tm	tt,
			   *tm = &tt;

	if (VARSIZE(units) - VARHDRSZ > MAXDATELEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("timestamp units \"%s\" not recognized",
						DatumGetCString(DirectFunctionCall1(textout,
											 PointerGetDatum(units))))));

	up = VARDATA(units);
	lp = lowunits;
	for (i = 0; i < (VARSIZE(units) - VARHDRSZ); i++)
		*lp++ = tolower((unsigned char) *up++);
	*lp = '\0';

	type = DecodeUnits(0, lowunits, &val);

	if (TIMESTAMP_NOT_FINITE(timestamp))
		PG_RETURN_TIMESTAMP(timestamp);

	if (type == UNITS)
	{
		if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));

		switch (val)
		{
			case DTK_MILLENNIUM:
				tm->tm_year = (tm->tm_year / 1000) * 1000;
			case DTK_CENTURY:
				tm->tm_year = (tm->tm_year / 100) * 100;
			case DTK_DECADE:
				tm->tm_year = (tm->tm_year / 10) * 10;
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
				fsec = ((fsec / 1000) * 1000);
#else
				fsec = rint(fsec * 1000) / 1000;
#endif
				break;

			case DTK_MICROSEC:
#ifndef HAVE_INT64_TIMESTAMP
				fsec = rint(fsec * 1000000) / 1000000;
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
	text	   *units = PG_GETARG_TEXT_P(0);
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(1);
	TimestampTz result;
	int			tz;
	int			type,
				val;
	int			i;
	char	   *up,
			   *lp,
				lowunits[MAXDATELEN + 1];
	fsec_t		fsec;
	char	   *tzn;
	struct tm	tt,
			   *tm = &tt;

	if (VARSIZE(units) - VARHDRSZ > MAXDATELEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		   errmsg("timestamp with time zone units \"%s\" not recognized",
				  DatumGetCString(DirectFunctionCall1(textout,
											 PointerGetDatum(units))))));
	up = VARDATA(units);
	lp = lowunits;
	for (i = 0; i < (VARSIZE(units) - VARHDRSZ); i++)
		*lp++ = tolower((unsigned char) *up++);
	*lp = '\0';

	type = DecodeUnits(0, lowunits, &val);

	if (TIMESTAMP_NOT_FINITE(timestamp))
		PG_RETURN_TIMESTAMPTZ(timestamp);

	if (type == UNITS)
	{
		if (timestamp2tm(timestamp, &tz, tm, &fsec, &tzn) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));

		switch (val)
		{
			case DTK_MILLENNIUM:
				tm->tm_year = (tm->tm_year / 1000) * 1000;
			case DTK_CENTURY:
				tm->tm_year = (tm->tm_year / 100) * 100;
			case DTK_DECADE:
				tm->tm_year = (tm->tm_year / 10) * 10;
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
				fsec = ((fsec / 1000) * 1000);
#else
				fsec = rint(fsec * 1000) / 1000;
#endif
				break;
			case DTK_MICROSEC:
#ifndef HAVE_INT64_TIMESTAMP
				fsec = rint(fsec * 1000000) / 1000000;
#endif
				break;

			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					  errmsg("timestamp with time zone units \"%s\" not "
							 "supported", lowunits)));
				result = 0;
		}

		tz = DetermineLocalTimeZone(tm);

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
	text	   *units = PG_GETARG_TEXT_P(0);
	Interval   *interval = PG_GETARG_INTERVAL_P(1);
	Interval   *result;
	int			type,
				val;
	int			i;
	char	   *up,
			   *lp,
				lowunits[MAXDATELEN + 1];
	fsec_t		fsec;
	struct tm	tt,
			   *tm = &tt;

	result = (Interval *) palloc(sizeof(Interval));

	if (VARSIZE(units) - VARHDRSZ > MAXDATELEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("interval units \"%s\" not recognized",
						DatumGetCString(DirectFunctionCall1(textout,
											 PointerGetDatum(units))))));
	up = VARDATA(units);
	lp = lowunits;
	for (i = 0; i < (VARSIZE(units) - VARHDRSZ); i++)
		*lp++ = tolower((unsigned char) *up++);
	*lp = '\0';

	type = DecodeUnits(0, lowunits, &val);

	if (type == UNITS)
	{
		if (interval2tm(*interval, tm, &fsec) == 0)
		{
			switch (val)
			{
				case DTK_MILLENNIUM:
					tm->tm_year = (tm->tm_year / 1000) * 1000;
				case DTK_CENTURY:
					tm->tm_year = (tm->tm_year / 100) * 100;
				case DTK_DECADE:
					tm->tm_year = (tm->tm_year / 10) * 10;
				case DTK_YEAR:
					tm->tm_mon = 0;
				case DTK_QUARTER:
					tm->tm_mon = (3 * (tm->tm_mon / 3));
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
					fsec = ((fsec / 1000) * 1000);
#else
					fsec = rint(fsec * 1000) / 1000;
#endif
					break;
				case DTK_MICROSEC:
#ifndef HAVE_INT64_TIMESTAMP
					fsec = rint(fsec * 1000000) / 1000000;
#endif
					break;

				default:
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
						DatumGetCString(DirectFunctionCall1(textout,
											 PointerGetDatum(units))))));
		*result = *interval;
	}

	PG_RETURN_INTERVAL_P(result);
}

/* isoweek2date()
 * Convert ISO week of year number to date.
 * The year field must be specified!
 * karel 2000/08/07
 */
void
isoweek2date(int woy, int *year, int *mon, int *mday)
{
	int			day0,
				day4,
				dayn;

	if (!*year)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		 errmsg("cannot calculate week number without year information")));

	/* fourth day of current year */
	day4 = date2j(*year, 1, 4);

	/* day0 == offset to first day of week (Monday) */
	day0 = j2day(day4 - 1);

	dayn = ((woy - 1) * 7) + (day4 - day0);

	j2date(dayn, year, mon, mday);
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
	 * We need the first week containing a Thursday, otherwise this day
	 * falls into the previous year for purposes of counting weeks
	 */
	if (dayn < (day4 - day0))
	{
		day4 = date2j(year - 1, 1, 4);

		/* day0 == offset to first day of week (Monday) */
		day0 = j2day(day4 - 1);
	}

	result = (((dayn - (day4 - day0)) / 7) + 1);

	/*
	 * Sometimes the last few days in a year will fall into the first week
	 * of the next year, so check for this.
	 */
	if (result >= 53)
	{
		day4 = date2j(year + 1, 1, 4);

		/* day0 == offset to first day of week (Monday) */
		day0 = j2day(day4 - 1);

		if (dayn >= (day4 - day0))
			result = (((dayn - (day4 - day0)) / 7) + 1);
	}

	return (int) result;
}


/* timestamp_part()
 * Extract specified field from timestamp.
 */
Datum
timestamp_part(PG_FUNCTION_ARGS)
{
	text	   *units = PG_GETARG_TEXT_P(0);
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(1);
	float8		result;
	int			type,
				val;
	int			i;
	char	   *up,
			   *lp,
				lowunits[MAXDATELEN + 1];
	fsec_t		fsec;
	struct tm	tt,
			   *tm = &tt;

	if (VARSIZE(units) - VARHDRSZ > MAXDATELEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("timestamp units \"%s\" not recognized",
						DatumGetCString(DirectFunctionCall1(textout,
											 PointerGetDatum(units))))));
	up = VARDATA(units);
	lp = lowunits;
	for (i = 0; i < (VARSIZE(units) - VARHDRSZ); i++)
		*lp++ = tolower((unsigned char) *up++);
	*lp = '\0';

	type = DecodeUnits(0, lowunits, &val);
	if (type == UNKNOWN_FIELD)
		type = DecodeSpecial(0, lowunits, &val);

	if (TIMESTAMP_NOT_FINITE(timestamp))
	{
		result = 0;
		PG_RETURN_FLOAT8(result);
	}

	if (type == UNITS)
	{
		if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));

		switch (val)
		{
			case DTK_MICROSEC:
#ifdef HAVE_INT64_TIMESTAMP
				result = ((tm->tm_sec * 1000000e0) + fsec);
#else
				result = (tm->tm_sec + fsec) * 1000000;
#endif
				break;

			case DTK_MILLISEC:
#ifdef HAVE_INT64_TIMESTAMP
				result = ((tm->tm_sec * 1000e0) + (fsec / 1000e0));
#else
				result = (tm->tm_sec + fsec) * 1000;
#endif
				break;

			case DTK_SECOND:
#ifdef HAVE_INT64_TIMESTAMP
				result = (tm->tm_sec + (fsec / 1000000e0));
#else
				result = (tm->tm_sec + fsec);
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
				result = ((tm->tm_mon - 1) / 3) + 1;
				break;

			case DTK_WEEK:
				result = (float8) date2isoweek(tm->tm_year, tm->tm_mon, tm->tm_mday);
				break;

			case DTK_YEAR:
				result = tm->tm_year;
				break;

			case DTK_DECADE:
				result = (tm->tm_year / 10);
				break;

			case DTK_CENTURY:
				result = (tm->tm_year / 100);
				break;

			case DTK_MILLENNIUM:
				result = (tm->tm_year / 1000);
				break;

			case DTK_JULIAN:
				result = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday);
#ifdef HAVE_INT64_TIMESTAMP
				result += (((((tm->tm_hour * 60) + tm->tm_min) * 60)
							+ tm->tm_sec + (fsec / 1000000e0)) / 86400e0);
#else
				result += (((((tm->tm_hour * 60) + tm->tm_min) * 60)
							+ tm->tm_sec + fsec) / 86400e0);
#endif
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
				{
					int			tz;
					TimestampTz timestamptz;

					/*
					 * convert to timestamptz to produce consistent
					 * results
					 */
					if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL) != 0)
						ereport(ERROR,
						   (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
							errmsg("timestamp out of range")));

					tz = DetermineLocalTimeZone(tm);

					if (tm2timestamp(tm, fsec, &tz, &timestamptz) != 0)
						ereport(ERROR,
						   (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
							errmsg("timestamp out of range")));

#ifdef HAVE_INT64_TIMESTAMP
					result = ((timestamptz - SetEpochTimestamp()) / 1000000e0);
#else
					result = timestamptz - SetEpochTimestamp();
#endif
					break;
				}
			case DTK_DOW:
				if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL) != 0)
					ereport(ERROR,
							(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
							 errmsg("timestamp out of range")));
				result = j2day(date2j(tm->tm_year, tm->tm_mon, tm->tm_mday));
				break;

			case DTK_DOY:
				if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL) != 0)
					ereport(ERROR,
							(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
							 errmsg("timestamp out of range")));
				result = (date2j(tm->tm_year, tm->tm_mon, tm->tm_mday)
						  - date2j(tm->tm_year, 1, 1) + 1);
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
	text	   *units = PG_GETARG_TEXT_P(0);
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(1);
	float8		result;
	int			tz;
	int			type,
				val;
	int			i;
	char	   *up,
			   *lp,
				lowunits[MAXDATELEN + 1];
	double		dummy;
	fsec_t		fsec;
	char	   *tzn;
	struct tm	tt,
			   *tm = &tt;

	if (VARSIZE(units) - VARHDRSZ > MAXDATELEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		   errmsg("timestamp with time zone units \"%s\" not recognized",
				  DatumGetCString(DirectFunctionCall1(textout,
											 PointerGetDatum(units))))));
	up = VARDATA(units);
	lp = lowunits;
	for (i = 0; i < (VARSIZE(units) - VARHDRSZ); i++)
		*lp++ = tolower((unsigned char) *up++);
	*lp = '\0';

	type = DecodeUnits(0, lowunits, &val);
	if (type == UNKNOWN_FIELD)
		type = DecodeSpecial(0, lowunits, &val);

	if (TIMESTAMP_NOT_FINITE(timestamp))
	{
		result = 0;
		PG_RETURN_FLOAT8(result);
	}

	if (type == UNITS)
	{
		if (timestamp2tm(timestamp, &tz, tm, &fsec, &tzn) != 0)
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
				result /= 60;
				FMODULO(result, dummy, 60e0);
				break;

			case DTK_TZ_HOUR:
				dummy = -tz;
				FMODULO(dummy, result, 3600e0);
				break;

			case DTK_MICROSEC:
#ifdef HAVE_INT64_TIMESTAMP
				result = ((tm->tm_sec * 1000000e0) + fsec);
#else
				result = (tm->tm_sec + fsec) * 1000000;
#endif
				break;

			case DTK_MILLISEC:
#ifdef HAVE_INT64_TIMESTAMP
				result = ((tm->tm_sec * 1000e0) + (fsec / 1000e0));
#else
				result = (tm->tm_sec + fsec) * 1000;
#endif
				break;

			case DTK_SECOND:
#ifdef HAVE_INT64_TIMESTAMP
				result = (tm->tm_sec + (fsec / 1000000e0));
#else
				result = (tm->tm_sec + fsec);
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
				result = ((tm->tm_mon - 1) / 3) + 1;
				break;

			case DTK_WEEK:
				result = (float8) date2isoweek(tm->tm_year, tm->tm_mon, tm->tm_mday);
				break;

			case DTK_YEAR:
				result = tm->tm_year;
				break;

			case DTK_DECADE:
				result = (tm->tm_year / 10);
				break;

			case DTK_CENTURY:
				result = (tm->tm_year / 100);
				break;

			case DTK_MILLENNIUM:
				result = (tm->tm_year / 1000);
				break;

			case DTK_JULIAN:
				result = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday);
#ifdef HAVE_INT64_TIMESTAMP
				result += (((((tm->tm_hour * 60) + tm->tm_min) * 60)
							+ tm->tm_sec + (fsec / 1000000e0)) / 86400e0);
#else
				result += (((((tm->tm_hour * 60) + tm->tm_min) * 60)
							+ tm->tm_sec + fsec) / 86400e0);
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
	else if (type == RESERV)
	{
		switch (val)
		{
			case DTK_EPOCH:
#ifdef HAVE_INT64_TIMESTAMP
				result = ((timestamp - SetEpochTimestamp()) / 1000000e0);
#else
				result = timestamp - SetEpochTimestamp();
#endif
				break;

			case DTK_DOW:
				if (timestamp2tm(timestamp, &tz, tm, &fsec, &tzn) != 0)
					ereport(ERROR,
							(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
							 errmsg("timestamp out of range")));
				result = j2day(date2j(tm->tm_year, tm->tm_mon, tm->tm_mday));
				break;

			case DTK_DOY:
				if (timestamp2tm(timestamp, &tz, tm, &fsec, &tzn) != 0)
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
	text	   *units = PG_GETARG_TEXT_P(0);
	Interval   *interval = PG_GETARG_INTERVAL_P(1);
	float8		result;
	int			type,
				val;
	int			i;
	char	   *up,
			   *lp,
				lowunits[MAXDATELEN + 1];
	fsec_t		fsec;
	struct tm	tt,
			   *tm = &tt;

	if (VARSIZE(units) - VARHDRSZ > MAXDATELEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("interval units \"%s\" not recognized",
						DatumGetCString(DirectFunctionCall1(textout,
											 PointerGetDatum(units))))));
	up = VARDATA(units);
	lp = lowunits;
	for (i = 0; i < (VARSIZE(units) - VARHDRSZ); i++)
		*lp++ = tolower((unsigned char) *up++);
	*lp = '\0';

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
					result = ((tm->tm_sec * 1000000e0) + fsec);
#else
					result = (tm->tm_sec + fsec) * 1000000;
#endif
					break;

				case DTK_MILLISEC:
#ifdef HAVE_INT64_TIMESTAMP
					result = ((tm->tm_sec * 1000e0) + (fsec / 1000e0));
#else
					result = (tm->tm_sec + fsec) * 1000;
#endif
					break;

				case DTK_SECOND:
#ifdef HAVE_INT64_TIMESTAMP
					result = (tm->tm_sec + (fsec / 1000000e0));
#else
					result = (tm->tm_sec + fsec);
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
					result = (tm->tm_year / 10);
					break;

				case DTK_CENTURY:
					result = (tm->tm_year / 100);
					break;

				case DTK_MILLENNIUM:
					result = (tm->tm_year / 1000);
					break;

				default:
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("interval units \"%s\" not supported",
							 DatumGetCString(DirectFunctionCall1(textout,
											 PointerGetDatum(units))))));
					result = 0;
			}

		}
		else
		{
			elog(ERROR, "could not convert interval to tm");
			result = 0;
		}
	}
	else if ((type == RESERV) && (val == DTK_EPOCH))
	{
#ifdef HAVE_INT64_TIMESTAMP
		result = (interval->time / 1000000e0);
#else
		result = interval->time;
#endif
		if (interval->month != 0)
		{
			result += ((365.25 * 86400) * (interval->month / 12));
			result += ((30.0 * 86400) * (interval->month % 12));
		}
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("interval units \"%s\" not recognized",
						DatumGetCString(DirectFunctionCall1(textout,
											 PointerGetDatum(units))))));
		result = 0;
	}

	PG_RETURN_FLOAT8(result);
}


/* timestamp_zone()
 * Encode timestamp type with specified time zone.
 * Returns timestamp with time zone, with the input
 *	rotated from local time to the specified zone.
 */
Datum
timestamp_zone(PG_FUNCTION_ARGS)
{
	text	   *zone = PG_GETARG_TEXT_P(0);
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(1);
	TimestampTz result;
	int			tz;
	int			type,
				val;
	int			i;
	char	   *up,
			   *lp,
				lowzone[MAXDATELEN + 1];

	if (VARSIZE(zone) - VARHDRSZ > MAXDATELEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("time zone \"%s\" not recognized",
						DatumGetCString(DirectFunctionCall1(textout,
											  PointerGetDatum(zone))))));

	if (TIMESTAMP_NOT_FINITE(timestamp))
		PG_RETURN_TIMESTAMPTZ(timestamp);

	up = VARDATA(zone);
	lp = lowzone;
	for (i = 0; i < (VARSIZE(zone) - VARHDRSZ); i++)
		*lp++ = tolower((unsigned char) *up++);
	*lp = '\0';

	type = DecodeSpecial(0, lowzone, &val);

	if ((type == TZ) || (type == DTZ))
	{
		tz = -(val * 60);

		result = dt2local(timestamp, tz);
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("time zone \"%s\" not recognized",
						lowzone)));

		PG_RETURN_NULL();
	}

	PG_RETURN_TIMESTAMPTZ(result);
}	/* timestamp_zone() */

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

	if (zone->month != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			   errmsg("interval time zone \"%s\" must not specify month",
					  DatumGetCString(DirectFunctionCall1(interval_out,
											  PointerGetDatum(zone))))));

#ifdef HAVE_INT64_TIMESTAMP
	tz = (zone->time / INT64CONST(1000000));
#else
	tz = (zone->time);
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
	TimestampTz result;
	struct tm	tt,
			   *tm = &tt;
	fsec_t		fsec;
	int			tz;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		result = timestamp;
	else
	{
		if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));

		tz = DetermineLocalTimeZone(tm);

		if (tm2timestamp(tm, fsec, &tz, &result) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
	}

	PG_RETURN_TIMESTAMPTZ(result);
}

/* timestamptz_timestamp()
 * Convert timestamp at GMT to local timestamp
 */
Datum
timestamptz_timestamp(PG_FUNCTION_ARGS)
{
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(0);
	Timestamp	result;
	struct tm	tt,
			   *tm = &tt;
	fsec_t		fsec;
	char	   *tzn;
	int			tz;

	if (TIMESTAMP_NOT_FINITE(timestamp))
		result = timestamp;
	else
	{
		if (timestamp2tm(timestamp, &tz, tm, &fsec, &tzn) != 0)
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
	text	   *zone = PG_GETARG_TEXT_P(0);
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(1);
	Timestamp	result;

	int			tz;
	int			type,
				val;
	int			i;
	char	   *up,
			   *lp,
				lowzone[MAXDATELEN + 1];

	if (VARSIZE(zone) - VARHDRSZ > MAXDATELEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("time zone \"%s\" not recognized",
						DatumGetCString(DirectFunctionCall1(textout,
											  PointerGetDatum(zone))))));
	up = VARDATA(zone);
	lp = lowzone;
	for (i = 0; i < (VARSIZE(zone) - VARHDRSZ); i++)
		*lp++ = tolower((unsigned char) *up++);
	*lp = '\0';

	type = DecodeSpecial(0, lowzone, &val);

	if (TIMESTAMP_NOT_FINITE(timestamp))
		PG_RETURN_NULL();

	if ((type == TZ) || (type == DTZ))
	{
		tz = val * 60;

		result = dt2local(timestamp, tz);
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("time zone \"%s\" not recognized", lowzone)));

		PG_RETURN_NULL();
	}

	PG_RETURN_TIMESTAMP(result);
}	/* timestamptz_zone() */

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
		PG_RETURN_NULL();

	if (zone->month != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			   errmsg("interval time zone \"%s\" must not specify month",
					  DatumGetCString(DirectFunctionCall1(interval_out,
											  PointerGetDatum(zone))))));

#ifdef HAVE_INT64_TIMESTAMP
	tz = -(zone->time / INT64CONST(1000000));
#else
	tz = -(zone->time);
#endif

	result = dt2local(timestamp, tz);

	PG_RETURN_TIMESTAMP(result);
}	/* timestamptz_izone() */
